#pragma once

/*
    OneEuroFilter.h — the 1-Euro filter, written from the published description
    in Casiez, Roussel & Vogel, "1 Euro Filter: A Simple Speed-based Low-pass
    Filter for Noisy Input in Interactive Systems", CHI 2012.

        te      = 1 / rate
        alpha(f)= 1 / (1 + tau/te),   tau = 1 / (2*pi*f)
        dx      = (x - xPrev) * rate
        dxHat   = lowpass(dx,  dCutoff)
        cutoff  = fcmin + beta * |dxHat|
        xHat    = lowpass(x,   cutoff)

    Project settings (DESIGN.md §5): fcmin = 1.0 Hz, beta = 0.5.

    IMPORTANT: operate on FRACTIONAL MIDI (log domain), never on Hz. Smoothing
    in Hz makes high notes look jumpier than low notes for identical cents error.

    Original implementation from the paper. No third-party code is copied.
    Header-only, self-contained, allocation-free, noexcept.
*/

#include <cmath>
#include <limits>

#include "LockFreeTypes.h"

namespace vt
{

class OneEuroFilter
{
public:
    static constexpr double kDefaultMinCutoffHz   = 1.0;   // fcmin
    static constexpr double kDefaultBeta          = 0.5;
    static constexpr double kDefaultDerivCutoffHz = 1.0;   // dcutoff

    OneEuroFilter() = default;

    /** @param rateHz  the rate at which process() will be called. For the
                       analysis chain this is 125.0 (1024-sample window,
                       128-sample hop, at 16 kHz). */
    void prepare (double rateHz,
                  double minCutoffHz   = kDefaultMinCutoffHz,
                  double beta_         = kDefaultBeta,
                  double derivCutoffHz = kDefaultDerivCutoffHz) noexcept
    {
        rate      = (rateHz > 0.0) ? rateHz : 125.0;
        minCutoff = (minCutoffHz > 0.0) ? minCutoffHz : kDefaultMinCutoffHz;
        beta      = beta_;
        dCutoff   = (derivCutoffHz > 0.0) ? derivCutoffHz : kDefaultDerivCutoffHz;
        reset();
    }

    void setMinCutoffHz (double hz) noexcept        { if (hz > 0.0) minCutoff = hz; }
    void setBeta (double b) noexcept                { beta = b; }
    void setDerivativeCutoffHz (double hz) noexcept { if (hz > 0.0) dCutoff = hz; }

    double getRateHz() const noexcept       { return rate; }
    double getMinCutoffHz() const noexcept  { return minCutoff; }
    double getBeta() const noexcept         { return beta; }

    /** Clears all state. The next finite sample is treated as the first. */
    void reset() noexcept
    {
        hasPrev = false;
        xHat    = 0.0;
        dxHat   = 0.0;
    }

    bool isPrimed() const noexcept  { return hasPrev; }

    /** Filters one fractional-MIDI value.

        A NaN input means "unvoiced / rejected": the filter state is cleared and
        NaN is returned, so that the next voiced run starts clean instead of
        smearing across the silence.
    */
    float process (float value) noexcept
    {
        if (! std::isfinite (value))
        {
            reset();
            return kUnvoiced;
        }

        const double x = (double) value;

        if (! hasPrev)
        {
            hasPrev = true;
            xHat    = x;
            dxHat   = 0.0;
            return (float) xHat;
        }

        const double te = 1.0 / rate;

        // Derivative, low-passed at a fixed cutoff.
        const double dx = (x - xHat) * rate;
        dxHat = lowpass (dx, dxHat, alphaFor (dCutoff, te));

        // Speed-adaptive cutoff: steady on a sustained note, instant on a scoop.
        const double cutoff = minCutoff + beta * std::abs (dxHat);
        xHat = lowpass (x, xHat, alphaFor (cutoff, te));

        return (float) xHat;
    }

private:
    static double alphaFor (double cutoffHz, double te) noexcept
    {
        if (! (cutoffHz > 0.0))
            return 1.0;

        const double tau = 1.0 / (2.0 * 3.14159265358979323846 * cutoffHz);
        return 1.0 / (1.0 + tau / te);
    }

    static double lowpass (double x, double prev, double alpha) noexcept
    {
        return alpha * x + (1.0 - alpha) * prev;
    }

    double rate      = 125.0;
    double minCutoff = kDefaultMinCutoffHz;
    double beta      = kDefaultBeta;
    double dCutoff   = kDefaultDerivCutoffHz;

    double xHat  = 0.0;
    double dxHat = 0.0;
    bool   hasPrev = false;
};

} // namespace vt
