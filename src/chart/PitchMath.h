/*
    ==========================================================================

    PitchMath.h — octave-agnostic pitch comparison.

    Small enough to be header-only, and deliberately free of any JUCE GUI
    dependency so it can be unit-tested in the console test runner.

    WHY THIS EXISTS AS ITS OWN FILE
    -------------------------------
    Scoring is octave-agnostic (DESIGN.md section 6): a bass singing a soprano
    melody an octave down is singing it correctly. If the DISPLAY does not fold
    the same way the SCORE does, the two silently contradict each other — full
    marks while the trace sits nowhere near the bar — and the singer stops
    trusting the feedback. DESIGN.md section 7 calls that the most likely single
    UX bug in the project, so the fold lives in one place, used by both, and is
    covered by tests.

    ==========================================================================
*/

#pragma once

#include <cmath>
#include <limits>

namespace vt
{

/** Folds `sungMidi` onto the octave nearest `targetMidi`.

    `octaveOffset` receives how many octaves were removed: -1 means the singer
    was an octave BELOW the written note. Show that as a badge rather than
    discarding it — it is real information about what they sang.

    Non-finite input passes through untouched with an offset of 0, so an
    unvoiced NaN frame stays NaN instead of becoming a bogus number. */
inline float foldOntoTarget (float sungMidi, float targetMidi, int& octaveOffset) noexcept
{
    octaveOffset = 0;

    if (! std::isfinite (sungMidi) || ! std::isfinite (targetMidi))
        return sungMidi;

    const auto octaves = (int) std::lround ((sungMidi - targetMidi) / 12.0f);

    octaveOffset = octaves;
    return sungMidi - 12.0f * (float) octaves;
}

/** Signed cents error of `sungMidi` against `targetMidi`, octave-folded.
    Always lands in [-600, +600]. NaN in, NaN out. */
inline float centsFromTarget (float sungMidi, float targetMidi) noexcept
{
    int octaves = 0;
    const auto folded = foldOntoTarget (sungMidi, targetMidi, octaves);

    if (! std::isfinite (folded))
        return std::numeric_limits<float>::quiet_NaN();

    return (folded - targetMidi) * 100.0f;
}

/** Two-threshold linear credit ramp. DESIGN.md section 6:
    full credit inside tFullCents, zero outside tZeroCents, linear between. */
inline float pitchCredit (float cents, float tFullCents, float tZeroCents) noexcept
{
    if (! std::isfinite (cents))
        return 0.0f;

    const auto a = std::abs (cents);

    if (a <= tFullCents)
        return 1.0f;

    if (a >= tZeroCents || tZeroCents <= tFullCents)
        return 0.0f;

    return (tZeroCents - a) / (tZeroCents - tFullCents);
}

} // namespace vt
