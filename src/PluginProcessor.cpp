/*
  ==============================================================================

    PluginProcessor.cpp
    Vocal Trainer — M1

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

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
class VocalTrainerProcessor::AnalysisWorker final : public juce::Thread
{
public:
    explicit AnalysisWorker (VocalTrainerProcessor& o)
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

        // A loaded chart takes precedence over the manual target note: it knows
        // what the singer should be on at THIS instant, which is the whole point
        // of the constraint. Falls back to the APVTS note when no chart is
        // loaded, and to the full range in Free Sing.
        const float chartTarget = owner.chartTargetMidi.load (std::memory_order_relaxed);

        if (! freeSing && chartTarget >= 0.0f)
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
    VocalTrainerProcessor& owner;

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
juce::AudioProcessorValueTreeState::ParameterLayout VocalTrainerProcessor::createParameterLayout()
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

    // TODO(M4): chartAnchorSeconds, fileStartOffsetSeconds, rateFactor,
    // visualOffsetMs, monitoringMode (DESIGN.md §5 "Transport sync and offset
    // handling"). They are deliberately absent in M1 — there is no chart and no
    // transport mapping yet, and shipping unused automatable parameters would
    // bake them into saved Live sets before their ranges are settled.

    return layout;
}

//==============================================================================
// Construction
//==============================================================================
VocalTrainerProcessor::VocalTrainerProcessor()
    : juce::AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    // NEVER change this. This plugin adds no delay to the signal: processBlock
    // leaves the buffer untouched. Reporting N > 0 would make Live delay every
    // other track by N through PDC, add real audible delay to a live-monitored
    // vocal for zero benefit, interact inconsistently with "Reduced Latency When
    // Monitoring", and expose us to Live's documented dynamic-latency bugs.
    // Keeping it at zero also means the host never pre-shifts ppqPosition.
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

VocalTrainerProcessor::~VocalTrainerProcessor()
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
void VocalTrainerProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    // Live calls prepareToPlay() REPEATEDLY on VST3 during a plugin's lifetime
    // (unlike AU, where it fires once). So: cheap, idempotent, and free of any
    // persistent state. hostSampleCounter, the chart anchor and the calibration
    // offset all live outside this function on purpose.
    const int block = juce::jmax (64, maximumExpectedSamplesPerBlock);

    hostSampleRateAtomic.store (sampleRate, std::memory_order_relaxed);

    const bool configurationChanged = ! juce::approximatelyEqual (sampleRate, lastPreparedRate)
                                        || block != lastPreparedBlock;

    if (! configurationChanged)
        return;                     // same rate and block size: nothing to do.

    lastPreparedRate  = sampleRate;
    lastPreparedBlock = block;

    // avoidReallocating = true, so a repeated call at the same size is free.
    monoScratch.setSize (1, block, false, false, true);

    analysisChain.prepare (sampleRate, block);   // [DSP-API]

    // Standalone only, but preparing it unconditionally is harmless and keeps
    // this branch-free — it renders nothing unless a track is loaded AND playing.
    backingPlayer.prepare (sampleRate, block);
}

void VocalTrainerProcessor::releaseResources()
{
    // Nothing to free: every buffer is sized for the worst case and kept.
    // The worker keeps running; it will simply find the chunk FIFO empty.
    //
    // The backing player DOES need telling — it holds a read-ahead thread and a
    // prepared transport source.
    backingPlayer.releaseResources();
}

bool VocalTrainerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
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
void VocalTrainerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const int numIn       = juce::jmin (getTotalNumInputChannels(),  numChannels);
    const int numOut      = juce::jmin (getTotalNumOutputChannels(), numChannels);

    //----------------------------------------------------------------------
    // 1. PASS-THROUGH. JUCE hands processBlock a single buffer used for both
    //    input and output, so leaving it alone is already bit-exact
    //    pass-through. Do not "helpfully" copy anything.
    //----------------------------------------------------------------------
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    const int64_t blockStart = hostSampleCounter;
    hostSampleCounter += numSamples;

    //----------------------------------------------------------------------
    // 2. Read the playhead EXACTLY ONCE and publish it.
    //
    //    getPosition() may only be called from processBlock; anywhere else is
    //    undefined behaviour.
    //----------------------------------------------------------------------
    {
        TransportSnapshot snap;
        snap.sampleRate    = hostSampleRateAtomic.load (std::memory_order_relaxed);
        snap.bpm           = 120.0;
        snap.isPlaying     = false;
        snap.timeInSamples = blockStart;      // standalone fallback: our own clock

        if (auto* ph = getPlayHead())
        {
            if (const auto pos = ph->getPosition())
            {
                // EXPLICIT optional handling. NEVER write
                //     if (auto x = pos->getTimeInSamples())
                // because that is FALSE at sample 0 — i.e. exactly at song start,
                // i.e. exactly where a chart-sync bug is most visible and most
                // likely to be misblamed on Ableton.
                //
                // NOTE: JUCE 9's PositionInfo returns juce::Optional, not
                // std::optional, so it is converted here rather than assigned.
                const auto rawTs = pos->getTimeInSamples();
                const std::optional<int64_t> ts = rawTs.hasValue()
                                                    ? std::optional<int64_t> { *rawTs }
                                                    : std::nullopt;

                const auto rawBpm = pos->getBpm();
                const bool playing = pos->getIsPlaying();   // bare bool, not Optional

                snap.isPlaying = playing;
                snap.bpm       = rawBpm.hasValue() ? *rawBpm : 120.0;

                if (ts.has_value())
                    snap.timeInSamples = *ts;
            }
        }

        // Published just before the snapshot so the GUI can pair arrangement
        // time with the free-running domain that estimates are stamped in.
        // TODO(M4): this pair is the input to the chartAnchor mapping.
        freeRunAtTransportPublish.store (blockStart, std::memory_order_relaxed);
        transport.publish (snap);
    }

    if (numSamples <= 0 || numIn <= 0)
        return;

    //----------------------------------------------------------------------
    // 3. Sum to mono into the preallocated scratch buffer, then feed the
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
    }

    //----------------------------------------------------------------------
    // 4. Wake the worker. signal() on an already-signalled event is a no-op and
    //    does not block, so this is safe on the audio thread.
    //----------------------------------------------------------------------
    if (analysisChain.getNumChunksReady() > 0)
        chunkReady.signal();

    //----------------------------------------------------------------------
    // 5. Backing track — STANDALONE ONLY, and strictly AFTER the analysis
    //    chain has read the input.
    //
    //    The buffer is shared between input and output, so mixing the backing
    //    track in any earlier would feed it straight into the pitch detector
    //    and the tracker would follow the record instead of the singer.
    //
    //    In the VST3 this is skipped entirely: Ableton plays the backing track,
    //    and a plugin that also played it would double it.
    //----------------------------------------------------------------------
    if (wrapperType == wrapperType_Standalone)
    {
        // 5a. MONITORING IS AN OUTPUT DECISION, NOT AN INPUT ONE.
        //
        //     The analysis chain has already consumed the mic above, so clearing
        //     the buffer here silences monitoring WITHOUT touching the pitch
        //     display. That is the whole point: the singer can stop hearing
        //     themselves and still see their pitch.
        //
        //     (In the VST3, Live owns monitoring and routing, so this is skipped
        //     and the plugin stays a bit-exact pass-through.)
        if (! monitorMic.load (std::memory_order_relaxed))
            buffer.clear();

        // 5b. Backing track and guide vocal go in AFTER the mic is cleared, so
        //     they are unaffected by the monitor setting.
        backingPlayer.renderInto (buffer);
    }
}

//==============================================================================
// GUI-facing
//==============================================================================
int VocalTrainerProcessor::drainPitchEstimates (PitchEstimate* dest, int maxItems) noexcept
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

void VocalTrainerProcessor::setGuiActive (bool shouldBeActive) noexcept
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
void VocalTrainerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VocalTrainerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // setStateInformation can be called on any thread and possibly before the
    // editor exists. Parameters only — never parse a chart synchronously here.
    // TODO(M4): songId as a non-parameter ValueTree property, loaded async.
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessorEditor* VocalTrainerProcessor::createEditor()
{
    return new VocalTrainerEditor (*this);
}

} // namespace vt

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new vt::VocalTrainerProcessor();
}
