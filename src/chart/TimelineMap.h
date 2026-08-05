/*
    ==========================================================================

    TimelineMap.h — the mapping between the two clocks on the highway.

    THERE ARE ALWAYS AT LEAST TWO CLOCKS, AND THEY ARE NOT THE SAME NUMBER.

      STREAM time  — audio-device time. Monotonic, never pauses, and it is what
                     every microphone pitch estimate is timestamped in. It has
                     been running since the audio device opened, so it can be
                     thousands of seconds while a song is 5 seconds in.

      CHART time   — position in the song. Driven by the backing track (or, from
                     M4, by Ableton's transport). Starts at 0, pauses, seeks,
                     and has no relationship to stream time whatsoever.

    Mixing them up is not a hypothetical. Chart mode originally stored chart
    time in the live samples while the now-line ran on the backing track's
    clock, and the entire microphone trace silently disappeared: every sample's
    age computed to some huge value, so the trail was skipped, and the dot was
    positioned far off-screen. It looked like the pitch detector had died.

    Keep the two apart, and convert ONLY here.

    Free of JUCE GUI dependencies on purpose, so the invariants below can be
    tested in a console runner.

    ==========================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>

namespace vt
{

struct TimelineMap
{
    double streamNow = 0.0;          ///< stream seconds at the now-line
    double chartNow  = 0.0;          ///< chart seconds at the now-line
    double historySeconds   = 2.0;   ///< visible span left of the now-line
    double lookaheadSeconds = 3.5;   ///< visible span right of the now-line

    /** Chart time a stream-stamped sample belongs at. */
    double chartTimeFor (double sampleStreamSeconds) const noexcept
    {
        return chartNow + (sampleStreamSeconds - streamNow);
    }

    /** Age of a sample, in the clock the samples are actually stamped in.
        Measuring age against chart time is the bug this file exists to stop. */
    double ageOf (double sampleStreamSeconds) const noexcept
    {
        return streamNow - sampleStreamSeconds;
    }

    double totalSpanSeconds() const noexcept
    {
        return std::max (1.0e-6, historySeconds + lookaheadSeconds);
    }

    /** 0 at the left edge, 1 at the right edge. */
    double normalisedX (double chartSeconds) const noexcept
    {
        return ((chartSeconds - chartNow) + historySeconds) / totalSpanSeconds();
    }

    bool isVisible (double chartSeconds) const noexcept
    {
        const auto x = normalisedX (chartSeconds);
        return x >= 0.0 && x <= 1.0;
    }

    /** THE INVARIANT THE LIVE TRACE DEPENDS ON.

        A sample no older than `historySeconds` must be on screen, whatever the
        offset between the two clocks happens to be. If this is ever false, the
        singer is looking at a highway with no feedback on it. */
    bool liveSampleIsVisible (double sampleStreamSeconds) const noexcept
    {
        const auto age = ageOf (sampleStreamSeconds);

        if (age < 0.0 || age > historySeconds)
            return false;           // genuinely outside the visible history

        return isVisible (chartTimeFor (sampleStreamSeconds));
    }
};

} // namespace vt
