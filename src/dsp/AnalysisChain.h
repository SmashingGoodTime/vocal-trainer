#pragma once

/*
    AnalysisChain.h — the audio-thread front end. DESIGN.md §5.

        mono in (host rate)
          -> 2nd-order Butterworth HPF, 65 Hz, at host rate
          -> anti-alias FIR lowpass, 7.2 kHz, 63 taps (linear phase)
          -> juce::Interpolators::WindowedSinc resample to EXACTLY 16000 Hz
             (speedRatio = hostSampleRate / 16000.0)
          -> staging buffer
          -> one AudioChunk (128 samples) pushed into a juce::AbstractFifo of
             capacity 64, tagged with the exact host sample index of data[0]

    processBlock() is real-time safe: no allocation, no locks, no I/O, no
    logging, no exceptions. Everything is preallocated in prepare().

    Nothing here touches the plugin's audio buffer — the plugin is a bit-exact
    pass-through with setLatencySamples(0).
*/

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "LockFreeTypes.h"

namespace vt
{

class AnalysisChain
{
public:
    //==========================================================================
    static constexpr double kHighPassHz   = 65.0;    // 2nd-order Butterworth
    static constexpr double kAntiAliasHz  = 7200.0;  // FIR lowpass corner
    static constexpr int    kFirTaps      = 63;      // odd -> integer group delay

    AnalysisChain();
    ~AnalysisChain() = default;

    //==========================================================================
    /** Allocates every buffer and designs both filters. Call from
        prepareToPlay(). Safe to call repeatedly (that runs again on every audio
        device, rate or block-size change) — it is idempotent apart from
        clearing the analysis state, and it holds no chart/anchor state. */
    void prepare (double hostSampleRate, int maxBlockSize);

    /** Clears all filter/resampler/FIFO state. Not real-time safe with respect
        to a concurrent reader — call it while the audio thread is stopped. */
    void reset() noexcept;

    /** Audio thread. Feeds one block of already-summed mono host-rate audio.
        @param mono                    n samples at the host rate
        @param n                       number of samples (<= maxBlockSize)
        @param hostSampleOfBlockStart  host-domain sample index of mono[0]
                                       (AudioPlayHead timeInSamples, or a free
                                       running counter in the Standalone target)

        A discontinuity in hostSampleOfBlockStart (locate, loop, transport jump)
        automatically re-bases the stream and flushes the filters. */
    void processBlock (const float* mono, int n, int64_t hostSampleOfBlockStart) noexcept;

    //==========================================================================
    /** Consumer (worker thread) side of the SPSC queue.
        @returns false when no complete chunk is available. */
    bool popChunk (AudioChunk& dest) noexcept;

    int getNumChunksReady() const noexcept { return fifo.getNumReady(); }

    /** Number of chunks dropped because the worker fell behind. Should be 0. */
    int getNumDroppedChunks() const noexcept { return droppedChunks.load (std::memory_order_relaxed); }

    //==========================================================================
    /** Host-domain sample index of the centre of an analysis window whose first
        16 kHz sample was tagged `firstSampleOfWindowHost`.

            hostSampleAtWindowCentre = firstSampleOfWindow + (1024/2) * (hostRate/16000)

        DESIGN.md §5, "timestamp-correct rendering". */
    static int64_t hostSampleAtWindowCentre (int64_t firstSampleOfWindowHost,
                                             double hostRate,
                                             int windowLength = kAnalysisWindow) noexcept
    {
        return firstSampleOfWindowHost
             + (int64_t) std::llround ((double) (windowLength / 2) * (hostRate / kAnalysisRate));
    }

    /** Same, using the prepared host rate. */
    int64_t hostSampleAtWindowCentre (int64_t firstSampleOfWindowHost,
                                      int windowLength = kAnalysisWindow) const noexcept
    {
        return hostSampleAtWindowCentre (firstSampleOfWindowHost, hostRate, windowLength);
    }

    //==========================================================================
    double getHostSampleRate() const noexcept { return hostRate; }
    double getSpeedRatio() const noexcept     { return speedRatio; }
    bool   isPrepared() const noexcept        { return prepared; }

    /** Deterministic latency of the front end, in HOST samples:
        FIR group delay ((63-1)/2 = 31) + the WindowedSinc interpolator's
        algorithmic latency (100 input samples). Subtracted from every chunk's
        timestamp when latency compensation is enabled (the default). */
    int getFrontEndLatencyHostSamples() const noexcept { return latencyHostSamples; }

    /** Turn the timestamp compensation above off to get the literal
        DESIGN.md formula (base + k * speedRatio) with no correction. */
    void setCompensateFrontEndLatency (bool shouldCompensate) noexcept
    {
        compensateLatency = shouldCompensate;
    }

    bool getCompensateFrontEndLatency() const noexcept { return compensateLatency; }

private:
    //==========================================================================
    void designFilters();
    void restartStream (int64_t hostSampleOfBlockStart) noexcept;
    int64_t hostSampleForOutputIndex (int64_t outputIndex) const noexcept;
    void emitSample (float value) noexcept;
    void pushPendingChunk() noexcept;

    //==========================================================================
    double hostRate    = 48000.0;
    double speedRatio  = 3.0;
    int    maxBlock    = 512;
    bool   prepared    = false;

    // --- filters ------------------------------------------------------------
    juce::dsp::IIR::Filter<float> highPass;
    std::vector<float> firTaps;                 // kFirTaps coefficients
    std::vector<float> firState;                // circular, size kFirRingSize
    int  firPos = 0;
    static constexpr int kFirRingSize = 64;     // power of two, >= kFirTaps
    static constexpr int kFirRingMask = kFirRingSize - 1;

    // --- resampler ----------------------------------------------------------
    juce::Interpolators::WindowedSinc resampler;
    static constexpr int kCarryCapacity = 256;  // > any plausible speedRatio + 1
    std::vector<float> resampIn;                // carry + one block
    std::vector<float> resampOut;
    int carryCount = 0;

    // --- chunk staging ------------------------------------------------------
    AudioChunk pending {};
    int     pendingCount        = 0;
    int64_t pendingFirstOutIndex = 0;

    // --- host-time mapping --------------------------------------------------
    int64_t streamBaseHostSample   = 0;   // host index of output sample 0
    int64_t outSampleCounter       = 0;   // 16 kHz samples produced since base
    int64_t expectedNextHostSample = 0;
    bool    streamStarted          = false;

    int  latencyHostSamples = 0;
    bool compensateLatency  = true;

    // --- SPSC queue ---------------------------------------------------------
    juce::AbstractFifo      fifo { kChunkFifoCapacity };
    std::vector<AudioChunk> chunkStorage;
    std::atomic<int>        droppedChunks { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalysisChain)
};

} // namespace vt
