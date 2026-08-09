/*
  ==============================================================================

    AudioEngine.h
    Vocal Trainer — M1

    Three threads, two lock-free SPSC channels, zero locks in the audio path
    (DESIGN.md §5).

        audio thread  --AudioChunk FIFO-->  worker thread  --PitchEstimate FIFO-->  GUI

  ==============================================================================
    DEPENDENCY NOTES — the dsp/ layer is owned by another agent.

    Division of labour, as actually implemented:

      AnalysisChain    HPF 65 Hz -> anti-alias FIR -> WindowedSinc resample to
                       16 kHz -> 128-sample AudioChunks tagged with host sample
                       indices -> internal SPSC FIFO. Also compensates its own
                       front-end latency in those timestamps, and re-bases the
                       stream automatically on a transport discontinuity.
      PitchDetectorMPM NSDF / key-maxima peak picking. Returns fracMidi directly.
      VoicingGate      RMS + noise floor + hysteresis gate + hold, the clarity
                       gate, AND the 3-tap median. The engine does not
                       duplicate any of that.
      OneEuroFilter    display smoothing, fixed nominal rate, NaN-aware. Runs on
                       the message thread (see MainComponent.cpp for why).

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include "audio/BackingPlayer.h"
#include "audio/ReferenceTone.h"
#include "audio/TakeRecorder.h"
#include "dsp/LockFreeTypes.h"
#include "dsp/AnalysisChain.h"

namespace vt
{

//==============================================================================
class AudioEngine final : public juce::AudioProcessor
{
public:
    AudioEngine();
    ~AudioEngine() override;

    //== AudioProcessor ========================================================
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                       { return true; }

    const juce::String getName() const override           { return "Vocal Trainer"; }
    bool acceptsMidi() const override                     { return false; }
    bool producesMidi() const override                    { return false; }
    bool isMidiEffect() const override                    { return false; }
    double getTailLengthSeconds() const override          { return 0.0; }

    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                 {}
    const juce::String getProgramName (int) override      { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //== GUI-facing API — message thread only ==================================

    /** Copies up to maxItems estimates out of the worker->GUI FIFO.
        Returns how many were written. Single consumer: only the editor may call this. */
    int drainPitchEstimates (PitchEstimate* dest, int maxItems) noexcept;

    /** The device sample rate that PitchEstimate::hostSampleAtWindowCentre is
        expressed in. Valid after the first prepareToPlay(). */
    double getHostSampleRate() const noexcept
    {
        return hostSampleRateAtomic.load (std::memory_order_relaxed);
    }

    /** Tells the worker whether anyone is draining the estimate FIFO. With no
        editor open the worker drops estimates on the floor rather than filling
        the FIFO with stale data that would flood the next editor that opens. */
    void setGuiActive (bool shouldBeActive) noexcept;

    /** M3: the backing-track player.

        Owned here rather than by the editor because it has to render on the
        audio thread, and because playback must survive the editor window being
        closed and reopened. */
    BackingPlayer& getBackingPlayer() noexcept { return backingPlayer; }

    /** Whether the microphone is passed through to the OUTPUT.

        DEFAULT OFF, and that is the important part. JUCE's own "Mute audio
        input" checkbox in Audio Settings does something quite different from
        what its label suggests: it swaps the input for an EMPTY BUFFER before
        the plugin ever sees it (juce_StandaloneFilterWindow.h, the muteInput
        branch of audioDeviceIOCallbackWithContext). That kills the analysis
        along with the sound, so the pitch display goes dead.

        Muting belongs on the OUTPUT side for an analysis plugin: the mic must
        always reach the detector. Monitoring also defaults off because the
        Apollo Console already monitors at near-zero latency, so passing the mic
        through here just adds a delayed duplicate of the singer's own voice. */
    void setMonitorMic (bool shouldMonitor) noexcept
    {
        monitorMic.store (shouldMonitor, std::memory_order_relaxed);
    }

    bool getMonitorMic() const noexcept
    {
        return monitorMic.load (std::memory_order_relaxed);
    }

    /** How LOUD the monitored microphone is, in dB.

        A level, not just on/off, because how much of your own voice you want in
        the mix against the backing track is a per-headphone, per-interface
        judgement that cannot be picked for someone else. Unity is usually far
        too loud when the Apollo Console is already monitoring.

        Capped just above unity on purpose: this is a live path from a
        microphone to a speaker, and a monitor control that goes to +24 dB is a
        feedback machine with a friendly label. Applied as a per-block RAMP, so
        moving the slider or toggling monitoring never clicks. */
    void setMonitorGainDb (float db) noexcept
    {
        monitorGain.store (juce::Decibels::decibelsToGain (juce::jlimit (kMonitorMinDb,
                                                                        kMonitorMaxDb, db),
                                                           kMonitorMinDb),
                           std::memory_order_relaxed);
    }

    static constexpr float kMonitorMinDb = -40.0f;
    static constexpr float kMonitorMaxDb = 6.0f;

    /** M6: records the microphone to disk so a take can be listened back to.

        Owned here rather than by the editor because it has to be fed from the
        audio thread, and because a recording must not stop just because the
        window was closed. */
    TakeRecorder& getTakeRecorder() noexcept { return takeRecorder; }

    /** The piano keyboard's voice: a struck note on demand, for checking a
        pitch by ear.

        Owned here for the same reason the backing player is — it renders on the
        audio thread — and mixed in AFTER the monitor gain, so turning your own
        voice down in the headphones does not also silence the reference you
        turned it down to hear. */
    ReferenceTone& getReferenceTone() noexcept { return referenceTone; }

    /** M3: the MIDI note the chart says the singer should be on right now, or a
        negative value when no chart is loaded / no note is active.

        The editor computes this on the message thread and publishes it here; the
        worker uses it to constrain the detector's tau search to +/-1 octave,
        which makes octave errors structurally impossible. Deliberately a plain
        atomic float rather than a shared Chart pointer — the chart never leaves
        the message thread, so there is no lifetime question in the hot path. */
    void setChartTargetMidi (float midiOrNegative) noexcept
    {
        chartTargetMidi.store (midiOrNegative, std::memory_order_relaxed);
    }

    /** The chart's A4 reference (song.tuningHz), bridged to the worker the same
        way the target note is. The detector converts Hz to fractional MIDI
        against THIS reference, so a chart analysed at 446 Hz scores a singer
        matching the record as in tune rather than as 26 cents sharp on every
        note. 440 when no chart is loaded; the editor resets it on unload. */
    void setChartTuningHz (float hz) noexcept
    {
        chartTuningHz.store (hz > 0.0f ? juce::jlimit (400.0f, 480.0f, hz) : 440.0f,
                             std::memory_order_relaxed);
    }

    /** theme.json owns the clarity threshold, but the worker thread must not
        touch the Theme. The editor bridges it through here. */
    void setClarityThreshold (float t) noexcept
    {
        clarityThreshold.store (juce::jlimit (0.0f, 1.0f, t), std::memory_order_relaxed);
    }

    //== Parameters ============================================================
    static constexpr const char* pidTargetNote = "targetNoteMidi";
    static constexpr const char* pidFreeSing   = "freeSingMode";
    static constexpr const char* pidTFullCents = "tFullCents";
    static constexpr const char* pidTZeroCents = "tZeroCents";

    juce::AudioProcessorValueTreeState apvts;

private:
    //==========================================================================
    class AnalysisWorker;                 // defined in the .cpp

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //== Audio thread state (touched ONLY from processBlock) ===================

    /** Free-running device-domain sample counter, and the app's only clock.
        Deliberately NOT reset in prepareToPlay(): that runs again every time the
        audio device is changed or reopened, and resetting the clock mid-session
        would make every already-queued AudioChunk's timestamp a lie. */
    int64_t hostSampleCounter = 0;

    juce::AudioBuffer<float> monoScratch;      // preallocated in prepareToPlay
    double lastPreparedRate = 0.0;
    int    lastPreparedBlock = 0;

    AnalysisChain  analysisChain;

    //== Worker -> GUI =========================================================
    // Capacity lives in LockFreeTypes.h (the shared type contract).
    juce::AbstractFifo estimateFifo { kEstimateFifoCapacity };
    std::array<PitchEstimate, (size_t) kEstimateFifoCapacity> estimateStorage {};

    juce::WaitableEvent chunkReady { false };
    std::unique_ptr<AnalysisWorker> worker;

    //== Cross-thread scalars ==================================================
    std::atomic<double>  hostSampleRateAtomic { 48000.0 };
    std::atomic<float>   clarityThreshold     { 0.85f };
    std::atomic<float>   chartTargetMidi      { -1.0f };   // <0 == no chart / no active note
    std::atomic<float>   chartTuningHz        { 440.0f };  // song.tuningHz; 440 == no chart
    std::atomic<bool>    monitorMic           { false };   // mic -> output
    std::atomic<float>   monitorGain          { 0.5f };    // linear, ~-6 dB

    /** Gain actually applied to the previous block, so the next one can ramp
        from it. AUDIO THREAD ONLY — it is the ramp's state, not a setting. */
    float lastMonitorGain = 0.0f;

    BackingPlayer backingPlayer;
    ReferenceTone referenceTone;
    TakeRecorder  takeRecorder;
    std::atomic<bool>    guiActive            { false };

    //== Raw parameter pointers (safe to read from any thread) =================
    std::atomic<float>* pTargetNote = nullptr;
    std::atomic<float>* pFreeSing   = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace vt
