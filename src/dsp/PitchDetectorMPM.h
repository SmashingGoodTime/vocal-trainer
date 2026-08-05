#pragma once

/*
    PitchDetectorMPM.h — McLeod / Wyvill "MPM" pitch detection via the
    Normalised Square Difference Function (NSDF).

        n'(tau) = 2 * r'(tau) / m'(tau)

        r'(tau) = sum_{j=0}^{N-1-tau} x[j] * x[j+tau]      (FFT autocorrelation)
        m'(tau) = sum_{j=0}^{N-1-tau} ( x[j]^2 + x[j+tau]^2 )   (running power sums, O(N))

    Peak picking is KEY-MAXIMA picking, not global max: find the positive-going
    zero crossings of n', take the local maximum inside each positive interval,
    and choose the FIRST key maximum whose value >= k * globalMax, k = 0.90.
    Taking the global max is the classic MPM octave-UP failure.

    Frequency is ALWAYS computed against kAnalysisRate (16000 Hz), never the
    host rate. Using 48000 here is a 145-cent error.

    Original implementation. Algorithm and constants are from the published
    description (McLeod & Wyvill, ICMC 2005) — no third-party code is copied.

    Header-only, self-contained. All storage is allocated in prepare();
    process() performs no allocation, no locking and no I/O.
*/

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "LockFreeTypes.h"

namespace vt
{

/** Hz -> fractional MIDI. Always smooth/compare in this (log) domain. */
inline float hzToFracMidi (double hz, double referenceHz = 440.0) noexcept
{
    if (! (hz > 0.0) || ! (referenceHz > 0.0))
        return kUnvoiced;

    return (float) (69.0 + 12.0 * std::log2 (hz / referenceHz));
}

/** Fractional MIDI -> Hz. */
inline double fracMidiToHz (double fracMidi, double referenceHz = 440.0) noexcept
{
    return referenceHz * std::pow (2.0, (fracMidi - 69.0) / 12.0);
}

/** Signed interval between two fractional-MIDI values, in cents. */
inline double centsBetween (double aFracMidi, double bFracMidi) noexcept
{
    return (aFracMidi - bFracMidi) * 100.0;
}

//==============================================================================
class PitchDetectorMPM
{
public:
    //==========================================================================
    /** FFT order 11 == 2048 points, zero-padded from the 1024-sample window so
        that the circular autocorrelation equals the linear autocorrelation. */
    static constexpr int    kFftOrder = 11;
    static constexpr int    kFftSize  = 1 << kFftOrder;   // 2048

    /** Key-maximum acceptance factor. DESIGN.md §5. */
    static constexpr float  kPeakPickK = 0.90f;

    /** Default "Free Sing" search range. */
    static constexpr double kDefaultMinHz = 80.0;
    static constexpr double kDefaultMaxHz = 1100.0;

    struct Result
    {
        bool  valid       = false;                   ///< false == no usable periodicity found
        float frequencyHz = 0.0f;
        float fracMidi    = kUnvoiced;               ///< NaN when !valid
        float clarity     = 0.0f;                    ///< interpolated NSDF peak, 0..1
        float tau         = 0.0f;                    ///< interpolated lag, in 16 kHz samples
    };

    PitchDetectorMPM() = default;

    //==========================================================================
    /** Allocates every scratch buffer. Call once, off the audio thread.
        @param windowSize   analysis window length in 16 kHz samples; must be
                            <= kFftSize / 2 so that the zero-padded FFT gives a
                            true linear autocorrelation. Defaults to
                            kAnalysisWindow (1024).
    */
    void prepare (int windowSize = kAnalysisWindow)
    {
        jassert (windowSize > 0 && windowSize <= kFftSize / 2);

        windowLength = juce::jlimit (32, kFftSize / 2, windowSize);
        maxUsableTau = windowLength / 2 - 1;          // beyond N/2 the NSDF is unreliable

        fft = std::make_unique<juce::dsp::FFT> (kFftOrder);

        fftScratch.assign ((size_t) kFftSize * 2, 0.0f);
        squares   .assign ((size_t) windowLength, 0.0f);
        nsdf      .assign ((size_t) windowLength, 0.0f);

        const auto maxMaxima = (size_t) (windowLength / 2 + 2);
        maximaPos.assign (maxMaxima, 0);
        maximaVal.assign (maxMaxima, 0.0f);

        prepared = true;
    }

    bool isPrepared() const noexcept       { return prepared; }
    int  getWindowLength() const noexcept  { return windowLength; }

    //==========================================================================
    /** Constrains the lag search. Pass the chart note +/- one octave to make
        octave errors structurally impossible; pass (80, 1100) for Free Sing.
        Cheap, allocation-free, safe to call from any thread that is not
        concurrently inside process(). */
    void setSearchRangeHz (double minHz, double maxHz) noexcept
    {
        if (minHz > maxHz)
            std::swap (minHz, maxHz);

        searchMinHz = juce::jlimit (20.0, kAnalysisRate * 0.5 - 1.0, minHz);
        searchMaxHz = juce::jlimit (searchMinHz + 1.0, kAnalysisRate * 0.5 - 1.0, maxHz);
    }

    /** Restores the full 80 - 1100 Hz range. */
    void setFullSearchRange() noexcept  { setSearchRangeHz (kDefaultMinHz, kDefaultMaxHz); }

    /** Convenience: chart-informed constraint, +/- `octaves` around a MIDI note. */
    void setSearchRangeAroundMidi (double centreMidi, double octaves = 1.0) noexcept
    {
        const auto centreHz = fracMidiToHz (centreMidi, referenceHz);
        setSearchRangeHz (centreHz * std::pow (2.0, -octaves),
                          centreHz * std::pow (2.0,  octaves));
    }

    /** A4 reference for the Hz -> fractional MIDI conversion. Charts carry
        `song.tuningHz`; never assume 440. */
    void setReferenceHz (double hz) noexcept  { if (hz > 0.0) referenceHz = hz; }
    double getReferenceHz() const noexcept    { return referenceHz; }

    double getSearchMinHz() const noexcept    { return searchMinHz; }
    double getSearchMaxHz() const noexcept    { return searchMaxHz; }

    //==========================================================================
    /** Runs one analysis window. Real-time safe: no allocation, no locks.
        @param x            windowLength contiguous samples at 16 kHz
        @param numSamples   must equal getWindowLength()
    */
    Result process (const float* x, int numSamples) noexcept
    {
        Result result;

        if (! prepared || x == nullptr || numSamples != windowLength)
            return result;

        const int n = windowLength;

        //---------------------------------------------------------------- power
        double power = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const auto v = (double) x[i];
            const auto s = v * v;
            squares[(size_t) i] = (float) s;
            power += s;
        }

        // Digital silence (or denormal mush): nothing to detect.
        if (! (power > 1.0e-12))
            return result;

        //-------------------------------------------- r'(tau) via FFT autocorr
        std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);
        std::copy (x, x + n, fftScratch.begin());

        fft->performRealOnlyForwardTransform (fftScratch.data(), false);

        for (int bin = 0; bin < kFftSize; ++bin)
        {
            const auto re = fftScratch[(size_t) (2 * bin)];
            const auto im = fftScratch[(size_t) (2 * bin + 1)];
            fftScratch[(size_t) (2 * bin)]     = re * re + im * im;
            fftScratch[(size_t) (2 * bin + 1)] = 0.0f;
        }

        fft->performRealOnlyInverseTransform (fftScratch.data());

        // Normalise out whatever scaling convention the FFT backend used, by
        // forcing r'(0) to equal the true signal power. This makes the code
        // independent of JUCE's inverse-transform scaling.
        const double acfZero = (double) fftScratch[0];

        if (! (std::abs (acfZero) > 1.0e-20))
            return result;

        const double acfScale = power / acfZero;

        //--------------------------------------------- m'(tau) via power sums
        // m'(tau) = A(tau) + B(tau)
        //   A(tau) = sum_{j=0}^{N-1-tau} x[j]^2   -> A(tau) = A(tau-1) - x[N-tau]^2
        //   B(tau) = sum_{k=tau}^{N-1}   x[k]^2   -> B(tau) = B(tau-1) - x[tau-1]^2
        double sumA = power;
        double sumB = power;

        nsdf[0] = 1.0f;

        const int tauLimit = maxUsableTau;   // inclusive

        for (int tau = 1; tau <= tauLimit; ++tau)
        {
            sumA -= (double) squares[(size_t) (n - tau)];
            sumB -= (double) squares[(size_t) (tau - 1)];

            const double m = sumA + sumB;
            const double r = (double) fftScratch[(size_t) tau] * acfScale;

            nsdf[(size_t) tau] = (m > 1.0e-12) ? (float) (2.0 * r / m) : 0.0f;
        }

        //------------------------------------------------ key-maxima detection
        // Key maxima are found over the WHOLE lag range so that the
        // positive-going-zero-crossing semantics stay correct; the search-range
        // constraint is then applied as a filter over the candidate set.
        int numMaxima = 0;
        int tau = 1;

        // Skip the positive lobe attached to tau = 0 (n'(0) == 1 always).
        while (tau <= tauLimit && nsdf[(size_t) tau] > 0.0f)
            ++tau;

        while (tau <= tauLimit)
        {
            while (tau <= tauLimit && nsdf[(size_t) tau] <= 0.0f)   // positive-going crossing
                ++tau;

            if (tau > tauLimit)
                break;

            float bestVal = nsdf[(size_t) tau];
            int   bestPos = tau;

            while (tau <= tauLimit && nsdf[(size_t) tau] > 0.0f)
            {
                if (nsdf[(size_t) tau] > bestVal)
                {
                    bestVal = nsdf[(size_t) tau];
                    bestPos = tau;
                }

                ++tau;
            }

            if (numMaxima < (int) maximaPos.size())
            {
                maximaPos[(size_t) numMaxima] = bestPos;
                maximaVal[(size_t) numMaxima] = bestVal;
                ++numMaxima;
            }
        }

        if (numMaxima == 0)
            return result;

        //------------------------------------------------------ range + k-rule
        const int tauLo = juce::jlimit (1, tauLimit, (int) std::floor (kAnalysisRate / searchMaxHz));
        const int tauHi = juce::jlimit (tauLo, tauLimit, (int) std::ceil (kAnalysisRate / searchMinHz));

        float globalMax = 0.0f;

        for (int i = 0; i < numMaxima; ++i)
            if (maximaPos[(size_t) i] >= tauLo && maximaPos[(size_t) i] <= tauHi)
                globalMax = juce::jmax (globalMax, maximaVal[(size_t) i]);

        if (! (globalMax > 0.0f))
            return result;                       // no positive periodicity in range

        const float threshold = kPeakPickK * globalMax;

        int chosen = -1;

        for (int i = 0; i < numMaxima; ++i)      // maxima are in increasing-tau order
        {
            const auto p = maximaPos[(size_t) i];

            if (p >= tauLo && p <= tauHi && maximaVal[(size_t) i] >= threshold)
            {
                chosen = i;
                break;
            }
        }

        if (chosen < 0)
            return result;

        //------------------------------------------------ parabolic refinement
        const int p = maximaPos[(size_t) chosen];

        double tauInterp = (double) p;
        double peakVal   = (double) maximaVal[(size_t) chosen];

        if (p >= 1 && p + 1 <= tauLimit)
        {
            const double y0 = nsdf[(size_t) (p - 1)];
            const double y1 = nsdf[(size_t) p];
            const double y2 = nsdf[(size_t) (p + 1)];
            const double denom = y0 - 2.0 * y1 + y2;

            if (std::abs (denom) > 1.0e-12)
            {
                const double delta = juce::jlimit (-1.0, 1.0, 0.5 * (y0 - y2) / denom);
                tauInterp = (double) p + delta;
                peakVal   = y1 - 0.25 * (y0 - y2) * delta;
            }
        }

        if (! (tauInterp > 0.5))
            return result;

        // CRITICAL: 16000.0, the ANALYSIS rate. Never the host rate.
        const double freq = kAnalysisRate / tauInterp;

        if (! std::isfinite (freq) || freq <= 0.0)
            return result;

        result.valid       = true;
        result.tau         = (float) tauInterp;
        result.frequencyHz = (float) freq;
        result.clarity     = (float) juce::jlimit (0.0, 1.0, peakVal);
        result.fracMidi    = hzToFracMidi (freq, referenceHz);

        return result;
    }

    /** Read-only view of the most recent NSDF (for debug plotting only). */
    const std::vector<float>& getNsdf() const noexcept { return nsdf; }

private:
    std::unique_ptr<juce::dsp::FFT> fft;

    std::vector<float> fftScratch;    // 2 * kFftSize floats (packed complex)
    std::vector<float> squares;       // x[i]^2
    std::vector<float> nsdf;          // n'(tau), valid for tau in [0, maxUsableTau]
    std::vector<int>   maximaPos;
    std::vector<float> maximaVal;

    int    windowLength = kAnalysisWindow;
    int    maxUsableTau = kAnalysisWindow / 2 - 1;
    double searchMinHz  = kDefaultMinHz;
    double searchMaxHz  = kDefaultMaxHz;
    double referenceHz  = 440.0;
    bool   prepared     = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchDetectorMPM)
};

} // namespace vt
