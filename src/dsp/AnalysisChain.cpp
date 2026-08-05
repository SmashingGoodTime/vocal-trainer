#include "AnalysisChain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vt
{

//==============================================================================
AnalysisChain::AnalysisChain()
{
    firTaps.assign ((size_t) kFirTaps, 0.0f);
    firState.assign ((size_t) kFirRingSize, 0.0f);
    chunkStorage.assign ((size_t) kChunkFifoCapacity, AudioChunk {});
}

//==============================================================================
void AnalysisChain::prepare (double hostSampleRate, int maxBlockSize)
{
    hostRate   = (hostSampleRate > 0.0) ? hostSampleRate : 48000.0;
    maxBlock   = juce::jmax (1, maxBlockSize);
    speedRatio = hostRate / kAnalysisRate;

    designFilters();

    firState.assign ((size_t) kFirRingSize, 0.0f);
    resampIn .assign ((size_t) (kCarryCapacity + maxBlock), 0.0f);

    // Worst case: every staged input sample turns into an output sample when
    // the host rate is at or below 16 kHz.
    const auto maxOut = (int) std::ceil ((double) (kCarryCapacity + maxBlock)
                                         / juce::jmax (0.05, speedRatio)) + 8;
    resampOut.assign ((size_t) maxOut, 0.0f);

    chunkStorage.assign ((size_t) kChunkFifoCapacity, AudioChunk {});
    fifo.setTotalSize (kChunkFifoCapacity);

    // Deterministic front-end delay, in host samples:
    //   linear-phase FIR group delay              = (63 - 1) / 2 = 31
    //   WindowedSinc interpolator base latency     = 100 input samples
    latencyHostSamples = (kFirTaps - 1) / 2
                       + (int) juce::Interpolators::WindowedSinc::getBaseLatency();

    prepared = true;

    reset();
}

//==============================================================================
void AnalysisChain::designFilters()
{
    // --- 2nd-order Butterworth high-pass at 65 Hz, host rate --------------
    // makeHighPass(sampleRate, frequency) uses Q = 1/sqrt(2), i.e. Butterworth.
    highPass.coefficients =
        juce::dsp::IIR::Coefficients<float>::makeHighPass (hostRate, (float) kHighPassHz);
    highPass.reset();

    // --- anti-alias FIR lowpass, 7.2 kHz, 63 taps, linear phase -----------
    // Guard against absurdly low host rates (7.2 kHz must stay below Nyquist).
    const auto cutoff = juce::jmin (kAntiAliasHz, hostRate * 0.45);

    auto designed = juce::dsp::FilterDesign<float>::designFIRLowpassWindowMethod (
                        (float) cutoff,
                        hostRate,
                        (size_t) (kFirTaps - 1),                        // order 62 -> 63 taps
                        juce::dsp::WindowingFunction<float>::hamming);

    firTaps.assign ((size_t) kFirTaps, 0.0f);

    if (designed != nullptr)
    {
        jassert (designed->coefficients.size() == kFirTaps);

        const auto numToCopy = juce::jmin (kFirTaps, designed->coefficients.size());

        for (int i = 0; i < numToCopy; ++i)
            firTaps[(size_t) i] = designed->coefficients[i];
    }
    else
    {
        firTaps[0] = 1.0f;   // degenerate fallback: pass-through
    }
}

//==============================================================================
void AnalysisChain::reset() noexcept
{
    highPass.reset();

    std::fill (firState.begin(), firState.end(), 0.0f);
    firPos = 0;

    resampler.reset();
    carryCount = 0;

    pending = AudioChunk {};
    pendingCount         = 0;
    pendingFirstOutIndex = 0;

    streamBaseHostSample   = 0;
    outSampleCounter       = 0;
    expectedNextHostSample = 0;
    streamStarted          = false;

    fifo.reset();
    droppedChunks.store (0, std::memory_order_relaxed);
}

//==============================================================================
void AnalysisChain::restartStream (int64_t hostSampleOfBlockStart) noexcept
{
    // A transport locate / loop / first block. Flush the stateful parts of the
    // chain and re-base the host-time mapping. Chunks already in the FIFO carry
    // their own timestamps and stay valid, so the FIFO is deliberately not
    // touched here (a reader may be running concurrently).
    highPass.reset();

    std::fill (firState.begin(), firState.end(), 0.0f);
    firPos = 0;

    resampler.reset();
    carryCount   = 0;
    pendingCount = 0;

    streamBaseHostSample = hostSampleOfBlockStart;
    outSampleCounter     = 0;
    streamStarted        = true;
}

//==============================================================================
int64_t AnalysisChain::hostSampleForOutputIndex (int64_t outputIndex) const noexcept
{
    const auto raw = streamBaseHostSample
                   + (int64_t) std::llround ((double) outputIndex * speedRatio);

    return compensateLatency ? raw - (int64_t) latencyHostSamples : raw;
}

//==============================================================================
void AnalysisChain::pushPendingChunk() noexcept
{
    pending.numSamples             = kChunkSize;
    pending.hostSampleIndexOfFirst = hostSampleForOutputIndex (pendingFirstOutIndex);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 > 0)
    {
        chunkStorage[(size_t) start1] = pending;
        fifo.finishedWrite (1);
    }
    else
    {
        // Worker fell behind. Drop, never block, never allocate.
        droppedChunks.fetch_add (1, std::memory_order_relaxed);
    }

    pendingCount = 0;
}

//==============================================================================
void AnalysisChain::emitSample (float value) noexcept
{
    if (pendingCount == 0)
        pendingFirstOutIndex = outSampleCounter;

    pending.data[pendingCount] = value;
    ++pendingCount;
    ++outSampleCounter;

    if (pendingCount >= kChunkSize)
        pushPendingChunk();
}

//==============================================================================
void AnalysisChain::processBlock (const float* mono, int n, int64_t hostSampleOfBlockStart) noexcept
{
    if (! prepared || mono == nullptr || n <= 0)
        return;

    // Defensive: never write past the preallocated staging buffer, even if the
    // host hands us a block larger than the one it promised in prepareToPlay().
    n = juce::jmin (n, maxBlock);

    if (! streamStarted || hostSampleOfBlockStart != expectedNextHostSample)
        restartStream (hostSampleOfBlockStart);

    expectedNextHostSample = hostSampleOfBlockStart + n;

    //-------------------------------------------------- HPF -> anti-alias FIR
    float* const staged = resampIn.data() + carryCount;

    for (int i = 0; i < n; ++i)
    {
        const auto hp = highPass.processSample (mono[i]);

        firState[(size_t) firPos] = hp;

        float acc = 0.0f;
        int   idx = firPos;

        for (int t = 0; t < kFirTaps; ++t)
        {
            acc += firTaps[(size_t) t] * firState[(size_t) idx];
            idx = (idx - 1) & kFirRingMask;
        }

        firPos = (firPos + 1) & kFirRingMask;

        staged[i] = acc;
    }

    highPass.snapToZero();

    //--------------------------------------------- resample to exactly 16 kHz
    const int numAvail = carryCount + n;

    // floor() guarantees the interpolator can never read past numAvail.
    int numOut = (int) std::floor ((double) numAvail / speedRatio);
    numOut = juce::jlimit (0, (int) resampOut.size(), numOut);

    int used = 0;

    if (numOut > 0)
        used = resampler.process (speedRatio, resampIn.data(), resampOut.data(), numOut);

    used = juce::jlimit (0, numAvail, used);

    for (int i = 0; i < numOut; ++i)
        emitSample (resampOut[(size_t) i]);

    //-------------------------------------------------------------- carry over
    int leftover = numAvail - used;

    if (leftover > 0 && used > 0)
        std::memmove (resampIn.data(),
                      resampIn.data() + used,
                      sizeof (float) * (size_t) leftover);

    // leftover is bounded by speedRatio + 1; the clamp is pure paranoia.
    carryCount = juce::jlimit (0, kCarryCapacity, leftover);
}

//==============================================================================
bool AnalysisChain::popChunk (AudioChunk& dest) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (1, start1, size1, start2, size2);

    if (size1 <= 0)
        return false;

    dest = chunkStorage[(size_t) start1];
    fifo.finishedRead (1);

    return true;
}

} // namespace vt
