#include "TakeScorer.h"

#include "../chart/PitchMath.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace vt
{

namespace
{
    constexpr float  kNaNf = std::numeric_limits<float>::quiet_NaN();
    constexpr double kNaN  = std::numeric_limits<double>::quiet_NaN();

    /** Below this much voiced time a note is "not attempted" rather than
        "missed". Eight estimates at 125 Hz — enough that a single stray frame
        cannot paint a note, few enough that a real staccato note still counts. */
    constexpr double kMinScoredSeconds = 0.064;

    /** Ignored when measuring steadiness: a deliberate scoop into a note is
        good singing, and including the attack would report it as wobble.
        DESIGN.md section 6. */
    constexpr double kAttackSeconds = 0.15;

    /** A chart clock delta larger than this is a discontinuity, not a frame.
        Clamping rather than trusting it stops one stalled audio device or one
        window-drag from crediting a second of perfect pitch that never
        happened. */
    constexpr double kMaxFrameSeconds = 0.05;

    /** Two notes further apart than this are not a leap, they are two separate
        entries, and grouping the second one's error under "ascending 5th" would
        be meaningless. */
    constexpr double kMaxIntervalGapSeconds = 0.5;

    /** DESIGN.md section 6. Looks low; is not. See the header. */
    constexpr float kPhrasePassRatio = 0.45f;

    constexpr float kGoodAccuracy = 0.80f;
    constexpr float kFairAccuracy = 0.45f;

    int histogramBinFor (float cents) noexcept
    {
        if (! std::isfinite (cents))
            return -1;

        if (cents < -150.0f) return 0;
        if (cents >= 150.0f) return TakeSummary::kHistogramBins - 1;

        return 1 + (int) std::floor ((cents + 150.0f) / 25.0f);
    }

    juce::String centsText (float cents)
    {
        const auto rounded = juce::roundToInt (cents);
        return juce::String (rounded > 0 ? "+" : "") + juce::String (rounded) + " cents";
    }
}

//==============================================================================
// MedianWindow
//==============================================================================
float MedianWindow::push (float value, int minimumSamples) noexcept
{
    if (! std::isfinite (value))
    {
        // A voicing gap must CLEAR the history, not be skipped over. Bridging a
        // gap would let the pitch from before a breath keep scoring through it.
        clear();
        return kNaNf;
    }

    samples[(size_t) write] = value;
    write = (write + 1) % length;

    if (count < length)
        ++count;

    if (count < juce::jmax (1, minimumSamples))
        return kNaNf;

    std::copy (samples.begin(), samples.begin() + count, scratch.begin());

    const auto mid = scratch.begin() + count / 2;
    std::nth_element (scratch.begin(), mid, scratch.begin() + count);

    if ((count % 2) == 1)
        return *mid;

    // Even count: average the two central values. std::nth_element has already
    // partitioned, so the lower neighbour is the max of the left partition.
    const auto lower = *std::max_element (scratch.begin(), mid);
    return 0.5f * (lower + *mid);
}

//==============================================================================
// IntervalError
//==============================================================================
juce::String IntervalError::describe() const
{
    static const char* const names[13] =
    {
        "same note", "minor 2nd", "major 2nd", "minor 3rd", "major 3rd",
        "4th", "tritone", "5th", "minor 6th", "major 6th",
        "minor 7th", "major 7th", "octave"
    };

    if (semitones == 0)
        return "repeated notes";

    const auto name = semitones <= 12 ? juce::String (names[semitones])
                                      : juce::String (semitones) + " semitones";

    return juce::String (ascending ? "ascending " : "descending ") + name;
}

//==============================================================================
// TakeSummary
//==============================================================================
float TakeSummary::histogramBinCentreCents (int bin) noexcept
{
    if (bin <= 0)                    return -175.0f;   // "worse than -150"
    if (bin >= kHistogramBins - 1)   return  175.0f;   // "worse than +150"

    return -150.0f + 25.0f * (float) (bin - 1) + 12.5f;
}

juce::String TakeSummary::headline() const
{
    if (! hasData)
        return "No singing was picked up on this take. Check the input device and "
               "the level meter, then try again.";

    juce::String s;

    const auto pct = juce::roundToInt (percentInTune * 100.0f);

    //-------------------------------------------------------------------------
    // Lead with the honest overall number, then IMMEDIATELY with the most
    // actionable thing. A systematic direction is worth far more to a singer
    // than a percentage: "you are singing flat" is a habit they can work on,
    // where "82%" is a scoreboard.
    //-------------------------------------------------------------------------
    s << "You were in tune for " << pct << "% of the time you were singing";

    if (coverage < 0.6f)
        s << ", and you sang " << juce::roundToInt (coverage * 100.0f)
          << "% of the notes in the chart";

    s << ".";

    const auto absMean = std::abs (meanCents);
    const bool systematicallyFlat  = fractionFlat  >= 0.5f && meanCents < -8.0f;
    const bool systematicallySharp = fractionSharp >= 0.5f && meanCents >  8.0f;

    if (systematicallyFlat || systematicallySharp)
    {
        s << " You sat consistently " << juce::roundToInt (absMean) << " cents "
          << (systematicallyFlat ? "FLAT" : "SHARP")
          << " — that is a single habit to fix, not "
          << (notesPoor + notesMissed) << " separate mistakes.";
    }
    else if (meanAbsCents > 35.0f)
    {
        s << " Your errors go both ways (average " << juce::roundToInt (meanAbsCents)
          << " cents off, no consistent direction), which points at pitch "
             "accuracy rather than at one bad habit.";
    }
    else if (percentInTune >= 0.85f && coverage >= 0.8f)
    {
        s << " That is solid — tighten the tolerance in theme.json if you want "
             "this to stay hard.";
    }

    return s;
}

juce::StringArray TakeSummary::observations() const
{
    juce::StringArray lines;

    if (! hasData)
        return lines;

    //---------------------------------------------------------------- timing
    if (onsetSamples >= 4)
    {
        const auto onset = juce::roundToInt (meanOnsetMs);

        if (std::abs (meanOnsetMs) > 80.0f)
            lines.add (juce::String ("Timing: you came in ") + juce::String (std::abs (onset))
                         + " ms " + (meanOnsetMs > 0 ? "LATE" : "EARLY")
                         + " on average. Singers are usually late; listen to the note "
                           "before the one you keep missing.");
        else if (std::abs (meanOnsetMs) > 30.0f)
            lines.add (juce::String ("Timing: ") + juce::String (std::abs (onset)) + " ms "
                         + (meanOnsetMs > 0 ? "late" : "early") + " on average, which is fine.");
        else
            lines.add ("Timing: tight, within 30 ms on average.");
    }

    //--------------------------------------------------------------- octave
    if (fractionOctaveShifted > 0.3f && dominantOctaveOffset != 0)
        lines.add (juce::String ("You sang ") + juce::String (juce::roundToInt (fractionOctaveShifted * 100.0f))
                     + "% of this " + (dominantOctaveOffset < 0 ? "an octave down" : "an octave up")
                     + ". That is scored as correct, but the chart may simply be in the "
                       "wrong range for your voice.");

    //------------------------------------------------------------ steadiness
    if (std::isfinite (meanSteadinessCents))
    {
        if (meanSteadinessCents > 45.0f)
            lines.add (juce::String ("Sustained notes wandered by ")
                         + juce::String (juce::roundToInt (meanSteadinessCents))
                         + " cents after the attack. Breath support is usually the cause, "
                           "not the ear.");
        else if (meanSteadinessCents < 20.0f)
            lines.add ("Sustained notes were steady once you arrived on them.");
    }

    //-------------------------------------------------------------- intervals
    if (! intervalErrors.empty())
    {
        const auto& worst = intervalErrors.front();

        if (worst.count >= 2 && worst.meanAbsCents > 25.0f)
            lines.add (juce::String ("Weakest interval: ") + worst.describe() + ", "
                         + centsText (worst.meanCents) + " over " + juce::String (worst.count)
                         + " attempts. Drill that interval on its own with the exercises.");
    }

    //---------------------------------------------------------------- notes
    if (notesMissed > 0 && notesTotal > 0)
        lines.add (juce::String (notesMissed) + " of " + juce::String (notesTotal)
                     + " notes were never sung at all.");

    return lines;
}

//==============================================================================
// TakeScorer
//==============================================================================
void TakeScorer::setChart (const Chart* newChart)
{
    chart = newChart;

    scores.assign (chart != nullptr ? chart->getNotes().size() : 0, NoteScore {});

    if (chart != nullptr)
        for (size_t i = 0; i < scores.size(); ++i)
            scores[i].pitchScored = isPitchScored (chart->getNotes()[i].type);

    reset();
}

void TakeScorer::setTolerances (float tFullCents, float tZeroCents) noexcept
{
    tFull = juce::jmax (1.0f, tFullCents);
    tZero = juce::jmax (tFull + 1.0f, tZeroCents);
}

void TakeScorer::setMedianMs (int ms) noexcept
{
    medianMs = juce::jlimit (0, 500, ms);

    // 125 estimates per second. A zero-length window is not offered: scoring
    // instantaneous pitch is the one thing this class exists to prevent, so the
    // floor is 5 samples (40 ms) even if theme.json asks for none.
    const auto samples = juce::jlimit (5, MedianWindow::kMaxLength,
                                       (int) std::lround (medianMs * 0.125));
    median.setLength (samples);
}

void TakeScorer::reset()
{
    for (auto& s : scores)
    {
        const auto pitchScored = s.pitchScored;
        s = NoteScore {};
        s.pitchScored = pitchScored;
    }

    live.clear();
    median.clear();
    histogram.fill (0.0f);

    lastChartTime = kNaN;
    notesWithData = 0;
}

//==============================================================================
void TakeScorer::feed (double chartSeconds, float rawFracMidi) noexcept
{
    if (chart == nullptr || scores.empty())
        return;

    //-------------------------------------------------------------------------
    // Frame duration from the CHART CLOCK, not from a compiled-in 8 ms.
    //
    // The chart clock is the backing track's position when there is one, so it
    // is what the singer is actually hearing. Assuming the nominal estimate
    // rate would silently mis-integrate as soon as anything drives that clock
    // at a different rate — which the slow-down practice mode eventually will.
    //-------------------------------------------------------------------------
    double dt = 0.0;

    if (std::isfinite (lastChartTime))
    {
        const auto delta = chartSeconds - lastChartTime;

        if (delta < 0.0)
        {
            // Backwards: a seek. Drop the median (its contents belong to a
            // different part of the song) and resynchronise without scoring.
            median.clear();
            live.clear();
            lastChartTime = chartSeconds;
            return;
        }

        dt = juce::jlimit (0.0, kMaxFrameSeconds, delta);
    }

    lastChartTime = chartSeconds;

    if (dt <= 0.0)
        return;

    //-------------------------------------------------------------------------
    // Only the SOUNDING note is scored — deliberately noteIndexAt() rather than
    // targetNoteIndex(), which looks ahead. The lookahead exists to prime the
    // detector's octave search before a note arrives; crediting time against a
    // note that has not started yet would score the singer for anticipating.
    //-------------------------------------------------------------------------
    const auto noteIndex = chart->noteIndexAt (chartSeconds);

    if (noteIndex != live.noteIndex)
    {
        // Note boundary: the trailing median must not straddle it. See the
        // MedianWindow docs — this is why a stepwise scale does not read as
        // "flat and late on every note".
        median.clear();
        live.clear();
        live.noteIndex = noteIndex;
    }

    if (noteIndex < 0 || noteIndex >= (int) scores.size())
        return;

    const auto& note = chart->getNotes()[(size_t) noteIndex];

    live.noteSeconds += dt;
    ++live.frames;

    // Onset is measured from the FIRST voiced frame on the note — the raw
    // (post-gate) estimate, not the median's first output, which lags it by
    // three hops (~24 ms) and would report every singer as later than they are.
    if (std::isfinite (rawFracMidi) && ! std::isfinite (live.firstVoicedChartTime))
        live.firstVoicedChartTime = chartSeconds;

    const auto medianMidi = median.push (rawFracMidi);

    if (std::isfinite (medianMidi))
    {
        int octaveOffset = 0;
        const auto folded = foldOntoTarget (medianMidi, (float) note.midi, octaveOffset);
        const auto cents  = (folded - (float) note.midi) * 100.0f;

        // Notes whose pitch is not scored still count as sung (DESIGN.md
        // section 6 gives them power 1.0) — a rap line or a breath mark is not
        // a pitch failure.
        const auto power = scores[(size_t) noteIndex].pitchScored
                             ? pitchCredit (cents, tFull, tZero)
                             : 1.0f;

        live.scoredSeconds += dt;
        live.creditSeconds += (double) power * dt;

        if (power > 0.0f)
            live.hitSeconds += dt;

        // Time-weighted, so a three-second held note is not outvoted by four
        // passing frames on a grace note.
        live.centsSum    += (double) cents * dt;
        live.absCentsSum += (double) std::abs (cents) * dt;

        if (chartSeconds - note.startSeconds >= kAttackSeconds)
        {
            live.postAttackSum   += cents;
            live.postAttackSumSq += (double) cents * cents;
            ++live.postAttackCount;
        }

        const auto vote = juce::jlimit (0, 4, octaveOffset + 2);
        ++live.octaveVotes[(size_t) vote];

        if (scores[(size_t) noteIndex].pitchScored)
            if (const auto bin = histogramBinFor (cents); bin >= 0)
                histogram[(size_t) bin] += (float) dt;
    }

    // Published every frame rather than only at the note boundary, so the
    // highway and the review always see the current state. Without this the
    // last note of a take — the one the singer just finished, and the one they
    // are looking at — would be permanently absent from both.
    finaliseNote (noteIndex);
}

//==============================================================================
void TakeScorer::finaliseNote (int noteIndex) noexcept
{
    if (chart == nullptr || noteIndex < 0 || noteIndex >= (int) scores.size())
        return;

    const auto& note = chart->getNotes()[(size_t) noteIndex];
    auto& s = scores[(size_t) noteIndex];

    const bool hadData = s.hasData;

    s.scoredSeconds = live.scoredSeconds;
    s.creditSeconds = live.creditSeconds;
    s.hitSeconds    = live.hitSeconds;
    s.noteSeconds   = live.noteSeconds;
    s.hasData       = live.scoredSeconds >= kMinScoredSeconds;

    s.accuracy = live.scoredSeconds > 0.0
                   ? (float) juce::jlimit (0.0, 1.0, live.creditSeconds / live.scoredSeconds)
                   : 0.0f;

    // Denominator is the time the note was actually UNDER THE PLAYHEAD, not its
    // written duration. Otherwise a take stopped halfway through a long note
    // reports that note as barely sung, which is not what happened.
    const auto denominator = juce::jmax (live.noteSeconds, 1.0e-6);
    s.coverage = (float) juce::jlimit (0.0, 1.0, live.scoredSeconds / denominator);

    s.meanCents = live.scoredSeconds > 0.0
                    ? (float) (live.centsSum / live.scoredSeconds)
                    : 0.0f;

    if (live.postAttackCount >= 8)
    {
        const auto n    = (double) live.postAttackCount;
        const auto mean = live.postAttackSum / n;
        const auto var  = juce::jmax (0.0, live.postAttackSumSq / n - mean * mean);
        s.steadinessCents = (float) std::sqrt (var);
    }
    else
    {
        s.steadinessCents = kNaNf;
    }

    s.onsetOffsetMs = std::isfinite (live.firstVoicedChartTime)
                        ? (float) ((live.firstVoicedChartTime - note.startSeconds) * 1000.0)
                        : kNaNf;

    const auto winner = (int) std::distance (live.octaveVotes.begin(),
                                             std::max_element (live.octaveVotes.begin(),
                                                               live.octaveVotes.end()));
    s.octaveOffset = live.octaveVotes[(size_t) winner] > 0 ? winner - 2 : 0;

    if (s.hasData && ! hadData)
        ++notesWithData;
    else if (! s.hasData && hadData)
        --notesWithData;
}

//==============================================================================
const NoteScore& TakeScorer::getNoteScore (int noteIndex) const noexcept
{
    if (noteIndex < 0 || noteIndex >= (int) scores.size())
        return emptyScore;

    return scores[(size_t) noteIndex];
}

//==============================================================================
TakeSummary TakeScorer::buildSummary() const
{
    TakeSummary out;

    if (chart == nullptr || scores.empty())
        return out;

    const auto& notes = chart->getNotes();

    double centsWeighted = 0.0, absCentsWeighted = 0.0;
    double steadinessSum = 0.0;
    int    steadinessCount = 0;
    double onsetSum = 0.0, absOnsetSum = 0.0;
    double octaveShiftedSeconds = 0.0;
    std::array<double, 5> octaveSeconds { 0.0, 0.0, 0.0, 0.0, 0.0 };
    int    flat = 0, sharp = 0;

    for (size_t i = 0; i < scores.size(); ++i)
    {
        const auto& s = scores[i];

        if (! s.pitchScored)
            continue;

        ++out.notesTotal;
        out.chartNoteSeconds += notes[i].durationSeconds;

        if (! s.hasData)
        {
            ++out.notesMissed;
            continue;
        }

        ++out.notesAttempted;

        out.scoredSeconds += s.scoredSeconds;
        out.creditSeconds += s.creditSeconds;

        centsWeighted    += (double) s.meanCents * s.scoredSeconds;
        absCentsWeighted += (double) std::abs (s.meanCents) * s.scoredSeconds;

        if (s.accuracy >= kGoodAccuracy)      ++out.notesGood;
        else if (s.accuracy >= kFairAccuracy) ++out.notesFair;
        else                                  ++out.notesPoor;

        if (s.meanCents < -tFull) ++flat;
        if (s.meanCents >  tFull) ++sharp;

        if (std::isfinite (s.steadinessCents))
        {
            steadinessSum += s.steadinessCents;
            ++steadinessCount;
        }

        if (std::isfinite (s.onsetOffsetMs))
        {
            onsetSum    += s.onsetOffsetMs;
            absOnsetSum += std::abs (s.onsetOffsetMs);
            ++out.onsetSamples;
        }

        const auto vote = juce::jlimit (0, 4, s.octaveOffset + 2);
        octaveSeconds[(size_t) vote] += s.scoredSeconds;

        if (s.octaveOffset != 0)
            octaveShiftedSeconds += s.scoredSeconds;
    }

    out.hasData = out.notesAttempted > 0 && out.scoredSeconds > 0.0;

    if (! out.hasData)
        return out;

    out.percentInTune = (float) juce::jlimit (0.0, 1.0, out.creditSeconds / out.scoredSeconds);
    out.coverage      = out.chartNoteSeconds > 0.0
                          ? (float) juce::jlimit (0.0, 1.0, out.scoredSeconds / out.chartNoteSeconds)
                          : 0.0f;

    out.meanCents    = (float) (centsWeighted    / out.scoredSeconds);
    out.meanAbsCents = (float) (absCentsWeighted / out.scoredSeconds);

    out.fractionFlat  = (float) flat  / (float) out.notesAttempted;
    out.fractionSharp = (float) sharp / (float) out.notesAttempted;

    if (out.onsetSamples > 0)
    {
        out.meanOnsetMs    = (float) (onsetSum    / out.onsetSamples);
        out.meanAbsOnsetMs = (float) (absOnsetSum / out.onsetSamples);
    }

    if (steadinessCount > 0)
        out.meanSteadinessCents = (float) (steadinessSum / steadinessCount);

    out.fractionOctaveShifted = (float) (octaveShiftedSeconds / out.scoredSeconds);

    const auto dominant = (int) std::distance (octaveSeconds.begin(),
                                               std::max_element (octaveSeconds.begin(),
                                                                 octaveSeconds.end()));
    out.dominantOctaveOffset = dominant - 2;

    //------------------------------------------------------------- histogram
    out.centsHistogram = histogram;
    out.histogramPeak  = *std::max_element (histogram.begin(), histogram.end());

    //--------------------------------------------------------------- phrases
    const auto& phrases = chart->getPhrases();

    for (size_t p = 0; p < phrases.size(); ++p)
    {
        const auto& phrase = phrases[p];

        PhraseScore ps;
        ps.phraseIndex  = (int) p;
        ps.startSeconds = phrase.startSeconds;
        ps.text         = phrase.text;

        double scored = 0.0, hit = 0.0, cents = 0.0, onset = 0.0;
        int onsetCount = 0;

        const auto consider = [&] (size_t noteIdx)
        {
            if (noteIdx >= scores.size() || ! scores[noteIdx].pitchScored)
                return;

            const auto& s = scores[noteIdx];
            ++ps.notesTotal;

            if (! s.hasData)
                return;

            ++ps.notesAttempted;
            scored += s.scoredSeconds;
            hit    += s.hitSeconds;
            cents  += (double) s.meanCents * s.scoredSeconds;

            if (std::isfinite (s.onsetOffsetMs))
            {
                onset += s.onsetOffsetMs;
                ++onsetCount;
            }
        };

        // noteIds when the pipeline supplied them, time overlap otherwise —
        // exercises and hand-edited charts do not always carry them.
        if (! phrase.noteIds.empty())
        {
            for (const auto id : phrase.noteIds)
                for (size_t i = 0; i < notes.size(); ++i)
                    if (notes[i].id == id)
                    {
                        consider (i);
                        break;
                    }
        }
        else
        {
            int first = 0, last = 0;

            if (chart->notesOverlapping (phrase.startSeconds, phrase.endSeconds, first, last))
                for (int i = first; i < last; ++i)
                    consider ((size_t) i);
        }

        if (ps.notesTotal == 0)
            continue;

        // percentHit counts every tick that earned ANY credit, which is what
        // the 0.45 pass threshold was calibrated against — not the stricter
        // credit integral. See the header.
        ps.percentHit  = scored > 0.0 ? (float) (hit / scored) : 0.0f;
        ps.meanCents   = scored > 0.0 ? (float) (cents / scored) : 0.0f;
        ps.meanOnsetMs = onsetCount > 0 ? (float) (onset / onsetCount) : 0.0f;
        ps.pass        = ps.notesAttempted > 0 && ps.percentHit >= kPhrasePassRatio;

        out.phrases.push_back (std::move (ps));
    }

    // Worst first: that is the practice order, and it is the whole point of
    // ranking them. An unattempted phrase sorts last — it is not a failure,
    // the singer simply never got to it.
    std::sort (out.phrases.begin(), out.phrases.end(),
               [] (const PhraseScore& a, const PhraseScore& b)
               {
                   if ((a.notesAttempted > 0) != (b.notesAttempted > 0))
                       return a.notesAttempted > 0;

                   return a.percentHit < b.percentHit;
               });

    //------------------------------------------------------- interval errors
    struct Bucket { double sum = 0.0, absSum = 0.0; int count = 0; };
    std::map<std::pair<int, bool>, Bucket> buckets;

    for (size_t i = 1; i < notes.size(); ++i)
    {
        const auto& prev = notes[i - 1];
        const auto& cur  = notes[i];

        if (! scores[i].pitchScored || ! scores[i - 1].pitchScored)
            continue;

        if (! scores[i].hasData || ! scores[i - 1].hasData)
            continue;

        if (cur.startSeconds - prev.endSeconds() > kMaxIntervalGapSeconds)
            continue;

        const auto delta = cur.midi - prev.midi;
        auto& bucket = buckets[{ std::abs (delta), delta >= 0 }];

        bucket.sum    += scores[i].meanCents;
        bucket.absSum += std::abs (scores[i].meanCents);
        ++bucket.count;
    }

    for (const auto& [key, bucket] : buckets)
    {
        if (bucket.count < 2)          // one attempt is an anecdote, not a pattern
            continue;

        IntervalError e;
        e.semitones    = key.first;
        e.ascending    = key.second;
        e.count        = bucket.count;
        e.meanCents    = (float) (bucket.sum    / bucket.count);
        e.meanAbsCents = (float) (bucket.absSum / bucket.count);

        out.intervalErrors.push_back (e);
    }

    std::sort (out.intervalErrors.begin(), out.intervalErrors.end(),
               [] (const IntervalError& a, const IntervalError& b)
               { return a.meanAbsCents > b.meanAbsCents; });

    //----------------------------------------------------------- worst notes
    for (size_t i = 0; i < scores.size(); ++i)
        if (scores[i].pitchScored && scores[i].hasData && scores[i].accuracy < 0.6f)
            out.worstNoteIndices.push_back ((int) i);

    std::sort (out.worstNoteIndices.begin(), out.worstNoteIndices.end(),
               [this] (int a, int b)
               { return scores[(size_t) a].accuracy < scores[(size_t) b].accuracy; });

    if (out.worstNoteIndices.size() > 12)
        out.worstNoteIndices.resize (12);

    return out;
}

} // namespace vt
