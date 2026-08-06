/*
  ==============================================================================

    AudioEngine.cpp
    Vocal Trainer — M1

  ==============================================================================
*/

#include "AudioEngine.h"
#include "MainComponent.h"

#include "dsp/PitchDetectorMPM.h"
#include "dsp/VoicingGate.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace vt
{

namespace
{
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

    inline float midiToHz (float midi) noexcept
    {
        return 440.0f * std::pow (2.0f, (midi - 69.0f) / 12.0f);
    }

    inline float hzToMidi (float hz) noexcept
    {
        return 69.0f + 12.0f * std::log2 (hz / 440.0f);
    }

    // Free Sing search range (DESIGN.md §5).
    constexpr float kFreeSingMinHz = 80.0f;
    constexpr float kFreeSingMaxHz = 1100.0f;
}

//==============================================================================
// Worker thread
//==============================================================================
/*
    Not the message thread, not a Timer. Woken by a WaitableEvent after each
    chunk push, with a 20 ms timeout so a missed signal can never wedge it.

    Real-time discipline applies here exactly as it does in processBlock: no
    allocation, no locks, no logging, no exceptions. Everything below is either
    a member array or a stack scalar.
*/
class AudioEngine::AnalysisWorker final : public juce::Thread
{
public:
    explicit AnalysisWorker (AudioEngine& o)
        : juce::Thread ("Vocal Trainer Analysis"), owner (o)
    {
    }

    void run() override
    {
        // [DSP-API] one-time setup, off the audio thread.
        // The detector's rate is fixed at kAnalysisRate internally; prepare()
        // only needs the window length.
        detector.prepare (kAnalysisWindow);
        gate.prepare (kAnalysisRate);

        window.fill (0.0f);
        writeIndex = 0;
        samplesSeen = 0;

        while (! threadShouldExit())
        {
            owner.chunkReady.wait (20);

            if (threadShouldExit())
                break;

            AudioChunk chunk;

            // [DSP-API] drain everything the audio thread produced since we slept.
            while (owner.analysisChain.popChunk (chunk))
            {
                handleChunk (chunk);

                if (threadShouldExit())
                    return;
            }
        }
    }

private:
    //==========================================================================
    void handleChunk (const AudioChunk& chunk) noexcept
    {
        const int n = juce::jlimit (0, kChunkSize, chunk.numSamples);

        for (int i = 0; i < n; ++i)
        {
            window[(size_t) writeIndex] = chunk.data[i];
            writeIndex = (writeIndex + 1) % kAnalysisWindow;
        }

        // The gate is driven per HOP, in order — it owns the RMS window, the
        // noise-floor tracker and the open/close state machine, so it must see
        // every hop exactly once even before the analysis window has filled.
        gate.pushHop (chunk.data, n);

        samplesSeen += n;

        if (samplesSeen < kAnalysisWindow)
            return;

        // Linearise the circular window. The oldest sample sits at writeIndex.
        const int tailCount = kAnalysisWindow - writeIndex;
        std::memcpy (linear.data(),             window.data() + writeIndex, (size_t) tailCount * sizeof (float));
        std::memcpy (linear.data() + tailCount, window.data(),              (size_t) writeIndex * sizeof (float));

        //----------------------------------------------------------------
        // Voicing gate + RMS. Both were already updated by pushHop() above;
        // read the state rather than re-deriving it. Running the detector only
        // while the gate is open is a pure CPU saving — the gate has the final
        // say further down, in gate.process().
        //----------------------------------------------------------------
        const float rmsDb  = gate.getRmsDb();
        const bool  voiced = gate.isOpen();

        //----------------------------------------------------------------
        // Chart-informed tau range. In M1 the "chart" is the single target note.
        // Constraining the search to +/- one octave makes octave errors — the
        // dominant failure mode of every autocorrelation method — structurally
        // impossible, because the wrong-octave lag is not in the candidate set.
        //----------------------------------------------------------------
        const bool freeSing = owner.pFreeSing != nullptr
                                && owner.pFreeSing->load (std::memory_order_relaxed) > 0.5f;

        float minHz = kFreeSingMinHz;
        float maxHz = kFreeSingMaxHz;

        // A loaded chart takes precedence over the manual target note AND over
        // Free Sing: it knows what the singer should be on at THIS instant,
        // which is the whole point of the constraint. Falls back to the APVTS
        // note when no chart is loaded, and to the full range in Free Sing.
        //
        // Free Sing losing to a chart is not an oversight. The editor hides the
        // toggle in chart mode — it is a free-sing-mode control — and a hidden
        // toggle left on from a previous session must not be able to switch off
        // the octave-error constraint on every song from then on, invisibly and
        // with nothing on screen to explain why the trace jumps an octave.
        const float chartTarget = owner.chartTargetMidi.load (std::memory_order_relaxed);

        if (chartTarget >= 0.0f)
        {
            minHz = midiToHz (chartTarget - 12.0f);
            maxHz = midiToHz (chartTarget + 12.0f);
        }
        else if (! freeSing && owner.pTargetNote != nullptr)
        {
            const float target = owner.pTargetNote->load (std::memory_order_relaxed);
            minHz = midiToHz (target - 12.0f);
            maxHz = midiToHz (target + 12.0f);
        }

        if (! juce::approximatelyEqual (minHz, lastMinHz) || ! juce::approximatelyEqual (maxHz, lastMaxHz))
        {
            detector.setSearchRangeHz (minHz, maxHz);   // [DSP-API]
            lastMinHz = minHz;
            lastMaxHz = maxHz;
        }

        //----------------------------------------------------------------
        // Detect.
        //----------------------------------------------------------------
        float fracMidi = kNaN;
        float clarity  = 0.0f;

        if (voiced)
        {
            const auto r = detector.process (linear.data(), kAnalysisWindow);   // [DSP-API]
            clarity = r.clarity;

            if (r.valid)
                fracMidi = r.fracMidi;
        }

        //----------------------------------------------------------------
        // The gate applies the clarity threshold and the level gate, then the
        // 3-tap median on fractional MIDI — the cheapest and most effective
        // octave-glitch killer. Costs one hop (8 ms). Returns NaN if rejected.
        //
        // theme.json can retune the clarity threshold live, so push it each
        // frame; it is a plain float store, not a reallocation.
        //----------------------------------------------------------------
        gate.setClarityThreshold (owner.clarityThreshold.load (std::memory_order_relaxed));

        const float median = gate.process (fracMidi, clarity);

        //----------------------------------------------------------------
        // Timestamp at the analysis-window CENTRE, in host samples.
        //
        //   windowFirst = chunk.hostSampleIndexOfFirst - (1024 - 128) * r
        //   centre      = windowFirst + 512 * r
        //               = chunk.hostSampleIndexOfFirst - 384 * r
        //
        // where r = hostSampleRate / 16000. This is what converts the analysis
        // lag from an alignment error into a plain feedback delay.
        //----------------------------------------------------------------
        const double hostRate = owner.hostSampleRateAtomic.load (std::memory_order_relaxed);
        const double r = hostRate / kAnalysisRate;

        PitchEstimate est;
        est.hostSampleAtWindowCentre = chunk.hostSampleIndexOfFirst
                                         - (int64_t) std::llround (384.0 * r);
        est.fracMidi = median;
        est.clarity  = clarity;
        est.rmsDb    = rmsDb;

        push (est);
    }

    // NOTE: the 3-tap median lives in VoicingGate::process(), not here — it has
    // to share the gate's notion of which frames were accepted, so that an
    // unvoiced gap clears the history instead of being bridged by stale values.

    void push (const PitchEstimate& est) noexcept
    {
        if (! owner.guiActive.load (std::memory_order_relaxed))
            return;

        int s1 = 0, b1 = 0, s2 = 0, b2 = 0;
        owner.estimateFifo.prepareToWrite (1, s1, b1, s2, b2);

        if (b1 > 0)
        {
            owner.estimateStorage[(size_t) s1] = est;
            owner.estimateFifo.finishedWrite (1);
        }
        // else: the GUI stalled. Dropping the newest estimate is correct —
        // never block, never allocate, never overwrite the consumer's data.
    }

    //==========================================================================
    AudioEngine& owner;

    PitchDetectorMPM detector;
    VoicingGate      gate;

    std::array<float, (size_t) kAnalysisWindow> window {};
    std::array<float, (size_t) kAnalysisWindow> linear {};
    int     writeIndex  = 0;
    int64_t samplesSeen = 0;

    std::array<float, 3> history { kNaN, kNaN, kNaN };
    int   medianCount = 0;

    float lastMinHz = -1.0f;
    float lastMaxHz = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalysisWorker)
};

//==============================================================================
// Parameters
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout AudioEngine::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { pidTargetNote, 1 },
        "Target Note",
        36, 84, 60,
        juce::AudioParameterIntAttributes()
            .withStringFromValueFunction ([] (int v, int)
            {
                // Middle C = C3, matching Ableton Live's display convention.
                return juce::MidiMessage::getMidiNoteName (v, true, true, 3);
            })));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pidFreeSing, 1 },
        "Free Sing",
        true));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { pidTFullCents, 1 },
        "Full Credit Tolerance",
        juce::NormalisableRange<float> (5.0f, 100.0f, 0.5f),
        25.0f,
        juce::AudioParameterFloatAttributes().withLabel ("cents")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { pidTZeroCents, 1 },
        "Zero Credit Tolerance",
        juce::NormalisableRange<float> (20.0f, 300.0f, 1.0f),
        100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("cents")));

    return layout;
}

//==============================================================================
// Construction
//==============================================================================
AudioEngine::AudioEngine()
    : juce::AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    // NEVER change this. Nothing here delays the signal — the analysis chain
    // taps the buffer, it does not sit in front of it — so reporting N > 0 would
    // make the standalone wrapper add real audible delay to a live-monitored
    // vocal for zero benefit. The analysis front end compensates its own latency
    // in the TIMESTAMPS it hands the GUI (AnalysisChain.h), which is where that
    // correction belongs.
    setLatencySamples (0);

    pTargetNote = apvts.getRawParameterValue (pidTargetNote);
    pFreeSing   = apvts.getRawParameterValue (pidFreeSing);

    worker = std::make_unique<AnalysisWorker> (*this);

    // DESIGN.md asks for "priority 7, below realtime". JUCE 9 replaced the legacy
    // 0-10 integer priorities with Thread::Priority; `highest` is the highest
    // non-realtime priority, which is exactly that intent. Do NOT promote this to
    // startRealtimeThread() — an FFT on a realtime thread can starve the audio
    // callback it is downstream of.
    worker->startThread (juce::Thread::Priority::highest);
}

AudioEngine::~AudioEngine()
{
    if (worker != nullptr)
    {
        // Order matters: raise the exit flag, THEN break the WaitableEvent.
        // Thread::signalThreadShouldExit() is not virtual and knows nothing about
        // our event, so without the explicit signal() the worker would sit in
        // chunkReady.wait() until its 20 ms timeout on every shutdown.
        worker->signalThreadShouldExit();
        chunkReady.signal();
        worker->stopThread (2000);
        worker.reset();
    }
}

//==============================================================================
// Preparation
//==============================================================================
void AudioEngine::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    // Runs again every time the audio device, rate or block size changes, which
    // the singer can do at any point from Audio Settings. So: cheap, idempotent,
    // and free of any persistent state. hostSampleCounter, the chart anchor and
    // the calibration offset all live outside this function on purpose.
    const int block = juce::jmax (64, maximumExpectedSamplesPerBlock);

    hostSampleRateAtomic.store (sampleRate, std::memory_order_relaxed);

    const bool configurationChanged = ! juce::approximatelyEqual (sampleRate, lastPreparedRate)
                                        || block != lastPreparedBlock;

    if (! configurationChanged)
        return;                     // same rate and block size: nothing to do.

    // A rate change mid-recording would append samples at the new rate to a WAV
    // whose header states the old one, i.e. a take that plays back at the wrong
    // speed. Closing the file keeps what was captured and keeps it correct;
    // silently corrupting it would be discovered much later, on playback.
    if (takeRecorder.isRecording())
        takeRecorder.stop();

    lastPreparedRate  = sampleRate;
    lastPreparedBlock = block;

    // avoidReallocating = true, so a repeated call at the same size is free.
    monoScratch.setSize (1, block, false, false, true);

    analysisChain.prepare (sampleRate, block);   // [DSP-API]

    // Renders nothing unless a track is loaded AND playing, so preparing it
    // unconditionally costs nothing.
    backingPlayer.prepare (sampleRate, block);
}

void AudioEngine::releaseResources()
{
    // Nothing to free: every buffer is sized for the worst case and kept.
    // The worker keeps running; it will simply find the chunk FIFO empty.
    //
    // The backing player DOES need telling — it holds a read-ahead thread and a
    // prepared transport source.
    backingPlayer.releaseResources();
}

bool AudioEngine::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (in.isDisabled() || out.isDisabled())
        return false;

    if (in != out)
        return false;

    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

//==============================================================================
// Audio thread
//==============================================================================
void AudioEngine::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const int numIn       = juce::jmin (getTotalNumInputChannels(),  numChannels);
    const int numOut      = juce::jmin (getTotalNumOutputChannels(), numChannels);

    //----------------------------------------------------------------------
    // 1. JUCE hands processBlock a single buffer used for both input and
    //    output, so the microphone is already in place and no copy is needed.
    //    Only the output channels the input does not reach need clearing.
    //    Do not "helpfully" copy anything.
    //----------------------------------------------------------------------
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    const int64_t blockStart = hostSampleCounter;
    hostSampleCounter += numSamples;

    if (numSamples <= 0 || numIn <= 0)
        return;

    //----------------------------------------------------------------------
    // 2. Sum to mono into the preallocated scratch buffer, then feed the
    //    analysis chain. Sliced, so an over-large block cannot overrun.
    //----------------------------------------------------------------------
    const int capacity = monoScratch.getNumSamples();

    if (capacity <= 0)
        return;

    float* const mono = monoScratch.getWritePointer (0);

    for (int offset = 0; offset < numSamples; offset += capacity)
    {
        const int n = juce::jmin (capacity, numSamples - offset);

        juce::FloatVectorOperations::copy (mono, buffer.getReadPointer (0, offset), n);

        if (numIn >= 2)
        {
            juce::FloatVectorOperations::add (mono, buffer.getReadPointer (1, offset), n);
            juce::FloatVectorOperations::multiply (mono, 0.5f, n);
        }

        analysisChain.processBlock (mono, n, blockStart + offset);   // [DSP-API]

        // Recorded from the SAME mono sum the detector just analysed, and BEFORE
        // the monitor gain and the backing track are applied below. Both halves
        // of that matter: a take that was not the analysed signal makes "the
        // score disagrees with what I hear" unresolvable, and a take with the
        // backing track bled into it is useless for hearing your own pitch.
        takeRecorder.write (mono, n);
    }

    //----------------------------------------------------------------------
    // 3. Wake the worker. signal() on an already-signalled event is a no-op and
    //    does not block, so this is safe on the audio thread.
    //----------------------------------------------------------------------
    if (analysisChain.getNumChunksReady() > 0)
        chunkReady.signal();

    //----------------------------------------------------------------------
    // 4. Output stage — strictly AFTER the analysis chain has read the input.
    //
    //    The buffer is shared between input and output, so mixing the backing
    //    track in any earlier would feed it straight into the pitch detector
    //    and the tracker would follow the record instead of the singer.
    //----------------------------------------------------------------------

    // 4a. MONITORING IS AN OUTPUT DECISION, NOT AN INPUT ONE.
    //
    //     The analysis chain has already consumed the mic above, so attenuating
    //     the buffer here changes monitoring WITHOUT touching the pitch display.
    //     That is the whole point: the singer can turn their own voice down to
    //     nothing and still see their pitch.
    //
    //     RAMPED, not switched. A jump straight from 0 to the monitor gain puts
    //     a step in the waveform, and a step in a signal already feeding
    //     headphones is an audible click on every toggle and every slider move.
    //     One ramp per block is the cheapest fix and the standard one.
    const auto monitorTarget = monitorMic.load (std::memory_order_relaxed)
                                 ? monitorGain.load (std::memory_order_relaxed)
                                 : 0.0f;

    if (juce::approximatelyEqual (monitorTarget, lastMonitorGain))
        buffer.applyGain (monitorTarget);
    else
        buffer.applyGainRamp (0, numSamples, lastMonitorGain, monitorTarget);

    lastMonitorGain = monitorTarget;

    // 4b. Backing track and guide vocal go in AFTER the mic is cleared, so they
    //     are unaffected by the monitor setting.
    backingPlayer.renderInto (buffer);
}

//==============================================================================
// GUI-facing
//==============================================================================
int AudioEngine::drainPitchEstimates (PitchEstimate* dest, int maxItems) noexcept
{
    if (dest == nullptr || maxItems <= 0)
        return 0;

    const int wanted = juce::jmin (maxItems, estimateFifo.getNumReady());

    if (wanted <= 0)
        return 0;

    int s1 = 0, b1 = 0, s2 = 0, b2 = 0;
    estimateFifo.prepareToRead (wanted, s1, b1, s2, b2);

    for (int i = 0; i < b1; ++i)
        dest[i] = estimateStorage[(size_t) (s1 + i)];

    for (int i = 0; i < b2; ++i)
        dest[b1 + i] = estimateStorage[(size_t) (s2 + i)];

    estimateFifo.finishedRead (b1 + b2);
    return b1 + b2;
}

void AudioEngine::setGuiActive (bool shouldBeActive) noexcept
{
    if (shouldBeActive)
    {
        // Discard anything left over from a previous editor.
        int s1 = 0, b1 = 0, s2 = 0, b2 = 0;
        const int ready = estimateFifo.getNumReady();

        if (ready > 0)
        {
            estimateFifo.prepareToRead (ready, s1, b1, s2, b2);
            estimateFifo.finishedRead (b1 + b2);
        }
    }

    guiActive.store (shouldBeActive, std::memory_order_relaxed);
}

//==============================================================================
// State
//==============================================================================
void AudioEngine::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AudioEngine::setStateInformation (const void* data, int sizeInBytes)
{
    // setStateInformation can be called on any thread and possibly before the
    // editor exists. Parameters only — never parse a chart synchronously here.
    // The loaded song is deliberately not part of this state: it lives in the
    // PropertiesFile (see AppSettings), which is why there is nothing to load
    // asynchronously here.
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessorEditor* AudioEngine::createEditor()
{
    return new MainComponent (*this);
}

} // namespace vt

//==============================================================================
// JUCE's standalone wrapper calls this to build the engine. The name is fixed
// by juce_audio_plugin_client and cannot be changed.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new vt::AudioEngine();
}
