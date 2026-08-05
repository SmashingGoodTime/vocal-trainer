/*
  ==============================================================================

    PluginProcessor.h
    Vocal Trainer — M1

    Three threads, two lock-free SPSC channels, one seqlock, zero locks in the
    audio path (DESIGN.md §5).

        audio thread  --AudioChunk FIFO-->  worker thread  --PitchEstimate FIFO-->  GUI
        audio thread  --------------- TransportSeqlock -------------------------->  GUI

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
                       gate, AND the 3-tap median. The processor does not
                       duplicate any of that.
      OneEuroFilter    display smoothing, fixed nominal rate, NaN-aware. Runs on
                       the message thread (see PluginEditor.cpp for why).

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include "audio/BackingPlayer.h"
#include "dsp/LockFreeTypes.h"
#include "dsp/AnalysisChain.h"

namespace vt
{

//==============================================================================
class VocalTrainerProcessor final : public juce::AudioProcessor
{
public:
    VocalTrainerProcessor();
    ~VocalTrainerProcessor() override;

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

    /** Non-blocking seqlock read. Returns false if a write was in progress; just
        try again next frame. */
    bool readTransport (TransportSnapshot& out) const noexcept  { return transport.read (out); }

    /** The host sample rate that PitchEstimate::hostSampleAtWindowCentre is
        expressed in. Valid after the first prepareToPlay(). */
    double getHostSampleRate() const noexcept
    {
        return hostSampleRateAtomic.load (std::memory_order_relaxed);
    }

    /** Tells the worker whether anyone is draining the estimate FIFO. With no
        editor open the worker drops estimates on the floor rather than filling
        the FIFO with stale data that would flood the next editor that opens. */
    void setGuiActive (bool shouldBeActive) noexcept;

    /** M3: the Standalone's backing-track player.

        Owned here rather than by the editor because it has to render on the
        audio thread, and because playback must survive the editor window being
        closed and reopened. Inert in the VST3 — Ableton owns playback there. */
    BackingPlayer& getBackingPlayer() noexcept { return backingPlayer; }

    /** Whether the microphone is passed through to the OUTPUT (Standalone only).

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

    /** Free-running host-domain sample counter. Deliberately NOT reset in
        prepareToPlay(): Live calls prepareToPlay() repeatedly on VST3 during a
        plugin's lifetime, and resetting the clock mid-session would make every
        already-queued AudioChunk's timestamp a lie. */
    int64_t hostSampleCounter = 0;

    juce::AudioBuffer<float> monoScratch;      // preallocated in prepareToPlay
    double lastPreparedRate = 0.0;
    int    lastPreparedBlock = 0;

    AnalysisChain  analysisChain;
    TransportSeqlock transport;

    //== Worker -> GUI =========================================================
    static constexpr int kEstimateFifoCapacity = 512;   // ~4 s at 125 estimates/s

    juce::AbstractFifo estimateFifo { kEstimateFifoCapacity };
    std::array<PitchEstimate, (size_t) kEstimateFifoCapacity> estimateStorage {};

    juce::WaitableEvent chunkReady { false };
    std::unique_ptr<AnalysisWorker> worker;

    //== Cross-thread scalars ==================================================
    std::atomic<double>  hostSampleRateAtomic { 48000.0 };
    std::atomic<float>   clarityThreshold     { 0.85f };
    std::atomic<float>   chartTargetMidi      { -1.0f };   // <0 == no chart / no active note
    std::atomic<bool>    monitorMic           { false };   // mic -> output (Standalone)

    BackingPlayer backingPlayer;
    std::atomic<bool>    guiActive            { false };

    /** The free-running counter value at the block whose transport snapshot is
        currently published. M4 needs this pair to map free-running sample
        indices (which is what estimates are stamped with) onto arrangement time.
        TODO(M4): fold this into the offset/anchor mapping. */
    std::atomic<int64_t> freeRunAtTransportPublish { 0 };

    //== Raw parameter pointers (safe to read from any thread) =================
    std::atomic<float>* pTargetNote = nullptr;
    std::atomic<float>* pFreeSing   = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalTrainerProcessor)
};

} // namespace vt
