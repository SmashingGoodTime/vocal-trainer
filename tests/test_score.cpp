/*
    ==========================================================================

    test_score.cpp — TakeScorer and ExerciseLibrary.

    WHY THIS EXISTS
    ---------------
    Two of the checks in here guard against bugs that would be INVISIBLE while
    using the app, and would quietly teach the user to sing worse:

      1. "vibrato is not punished". A singer with textbook vibrato (5.5 Hz,
         +/-50 cents) is perfectly in tune, and must score as such. If scoring
         ever reverts to the instantaneous pitch this check fails — and without
         it, the only symptom would be a slightly stingy score that a human
         would blame on their own singing rather than on the tool.

      2. "the median does not straddle a note boundary". A trailing median that
         is not cleared between notes reports a stepwise scale as flat AND late
         on every single note, which reads exactly like a latency bug and would
         be debugged in completely the wrong place.

    The exercise checks are there because an exercise is generated, so a bad
    generator produces a chart that LOOKS plausible on the highway while being
    musically wrong. Asserting the actual semitone content is the only way to
    catch a scale with a wrong step in it.

    ==========================================================================
*/

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <vector>

#include "chart/Chart.h"
#include "exercise/ExerciseLibrary.h"
#include "score/TakeScorer.h"

namespace
{
    int failures = 0;
    int checks = 0;

    void check (bool ok, const juce::String& name, const juce::String& detail = {})
    {
        ++checks;

        if (! ok)
            ++failures;

        std::cout << (ok ? "[PASS] " : "[FAIL] ")
                  << name.paddedRight (' ', 62).toStdString();

        if (detail.isNotEmpty())
            std::cout << detail.toStdString();

        std::cout << std::endl;
    }

    bool near (double a, double b, double tol) { return std::abs (a - b) <= tol; }

    constexpr double kEstimateRate = 125.0;          // the real analysis rate
    constexpr double kDt = 1.0 / kEstimateRate;

    /** A two-note chart, one second each, C4 then D4, with no gap. */
    std::unique_ptr<vt::Chart> makeTwoNoteChart()
    {
        juce::var parsed;
        juce::JSON::parse (R"({
            "schemaVersion": "1.0",
            "song": { "id": "t", "title": "t", "durationSeconds": 3.0 },
            "notes": [
                { "id": 0, "startSeconds": 0.0, "durationSeconds": 1.0, "midi": 60,
                  "type": "normal", "phraseId": 0 },
                { "id": 1, "startSeconds": 1.0, "durationSeconds": 1.0, "midi": 62,
                  "type": "normal", "phraseId": 0 }
            ],
            "phrases": [
                { "id": 0, "startSeconds": 0.0, "endSeconds": 2.0, "text": "p",
                  "noteIds": [0, 1] }
            ]
        })", parsed);

        juce::String error;
        return vt::Chart::parse (parsed, error);
    }

    /** Feeds `seconds` of estimates, calling `pitchAt(t)` for each. */
    void sing (vt::TakeScorer& scorer, double from, double to,
               const std::function<float (double)>& pitchAt)
    {
        for (double t = from; t < to; t += kDt)
            scorer.feed (t, pitchAt (t));
    }
}

//==============================================================================
int main()
{
    // No ScopedJuceInitialiser_GUI: everything under test depends on juce_core
    // only, and keeping it that way is what makes this target fast to build and
    // runnable with no display and no audio device.
    std::cout << "=== TakeScorer ===" << std::endl;

    //==========================================================================
    // The credit ramp, integrated over time.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        // Dead-on C4 for the whole first note.
        sing (scorer, 0.0, 1.0, [] (double) { return 60.0f; });

        const auto& s = scorer.getNoteScore (0);

        check (s.hasData, "perfect note: has data");
        check (near (s.accuracy, 1.0, 0.001), "perfect note: accuracy is 1.0",
               juce::String (s.accuracy, 4));
        check (near (s.meanCents, 0.0, 0.5), "perfect note: mean cents is 0",
               juce::String (s.meanCents, 2));
        check (s.coverage > 0.95f, "perfect note: coverage is complete",
               juce::String (s.coverage, 3));
        check (near (s.onsetOffsetMs, 0.0, 40.0), "perfect note: onset is on time",
               juce::String (s.onsetOffsetMs, 1) + " ms");
    }

    //==========================================================================
    // Exactly half way up the ramp: 62.5 cents sharp with 25/100 tolerances is
    // (100 - 62.5) / (100 - 25) = 0.5 credit.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        sing (scorer, 0.0, 1.0, [] (double) { return 60.625f; });

        const auto& s = scorer.getNoteScore (0);

        check (near (s.accuracy, 0.5, 0.02), "half-credit note: accuracy is 0.5",
               juce::String (s.accuracy, 4));
        check (near (s.meanCents, 62.5, 1.0), "half-credit note: mean cents is +62.5",
               juce::String (s.meanCents, 2));
    }

    //==========================================================================
    // THE VIBRATO TRAP. 5.5 Hz, +/-50 cents peak — textbook classical vibrato,
    // centred exactly on the note. This MUST score as in tune.
    //
    // Instantaneous scoring would give roughly half credit here, and would tell
    // a well-trained singer to stop using vibrato.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        sing (scorer, 0.0, 1.0, [] (double t)
        {
            return 60.0f + 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                              * 5.5f * (float) t);
        });

        const auto& s = scorer.getNoteScore (0);

        check (s.accuracy > 0.9f, "vibrato +/-50 cents at 5.5 Hz still scores in tune",
               juce::String (s.accuracy, 3));
        check (near (s.meanCents, 0.0, 12.0), "vibrato: centre is reported, not the swing",
               juce::String (s.meanCents, 1) + " cents");
    }

    //==========================================================================
    // Octave-agnostic: singing the whole thing an octave down is CORRECT, and
    // the octave used is reported rather than penalised.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        sing (scorer, 0.0, 1.0, [] (double) { return 48.0f; });     // C3

        const auto& s = scorer.getNoteScore (0);

        check (near (s.accuracy, 1.0, 0.001), "octave down: full credit",
               juce::String (s.accuracy, 3));
        check (s.octaveOffset == -1, "octave down: reported as -1 octave",
               juce::String (s.octaveOffset));
    }

    //==========================================================================
    // THE NOTE-BOUNDARY MEDIAN CLEAR.
    //
    // Sing each note perfectly, switching pitch exactly on the boundary. If the
    // trailing median straddles the boundary it drags 200 ms of C4 into D4, and
    // the second note comes out flat.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        sing (scorer, 0.0, 2.0, [] (double t) { return t < 1.0 ? 60.0f : 62.0f; });

        const auto& first  = scorer.getNoteScore (0);
        const auto& second = scorer.getNoteScore (1);

        check (near (second.meanCents, 0.0, 5.0),
               "note boundary: second note is not dragged flat",
               juce::String (second.meanCents, 2) + " cents");
        check (second.accuracy > 0.95f, "note boundary: second note scores in tune",
               juce::String (second.accuracy, 3));
        check (first.accuracy > 0.95f, "note boundary: first note unaffected",
               juce::String (first.accuracy, 3));
    }

    //==========================================================================
    // Unvoiced input is "not attempted", NOT "missed by a mile". The distinction
    // is what stops the highway painting a note red before the singer has even
    // opened their mouth.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        const auto nan = std::numeric_limits<float>::quiet_NaN();
        sing (scorer, 0.0, 1.0, [nan] (double) { return nan; });

        const auto& s = scorer.getNoteScore (0);

        check (! s.hasData, "silence: note has no data");
        check (! scorer.hasAnyData(), "silence: whole take has no data");
        check (near (s.coverage, 0.0, 0.001), "silence: coverage is zero");
        check (! std::isfinite (s.onsetOffsetMs), "silence: onset is not invented");
    }

    //==========================================================================
    // A late entry is measured and reported as late.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        const auto nan = std::numeric_limits<float>::quiet_NaN();

        // Silent for 200 ms, then a perfect C4.
        sing (scorer, 0.0, 1.0, [nan] (double t) { return t < 0.2 ? nan : 60.0f; });

        const auto& s = scorer.getNoteScore (0);

        check (s.hasData, "late entry: still scored");

        // The median needs a few samples before it reports, so the measured
        // onset is a little after the first voiced frame. That lag is a property
        // of the median, not an error, so the tolerance allows for it.
        check (s.onsetOffsetMs > 150.0f && s.onsetOffsetMs < 300.0f,
               "late entry: onset reported as ~200 ms late",
               juce::String (s.onsetOffsetMs, 1) + " ms");
        check (s.coverage < 0.85f, "late entry: coverage reflects the missing start",
               juce::String (s.coverage, 3));
    }

    //==========================================================================
    // A seek backwards must not un-score or double-score anything.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        sing (scorer, 0.0, 1.0, [] (double) { return 60.0f; });
        const auto beforeSeek = scorer.getNoteScore (0).scoredSeconds;

        scorer.feed (0.5, 60.0f);       // jump backwards
        sing (scorer, 0.5, 0.7, [] (double) { return 60.0f; });

        check (scorer.getNoteScore (0).scoredSeconds < beforeSeek,
               "seek back: the note restarts rather than accumulating twice",
               juce::String (scorer.getNoteScore (0).scoredSeconds, 3) + " s");
    }

    //==========================================================================
    // Summary: the numbers a human is shown.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        // First note 30 cents flat (partial credit), second note perfect.
        sing (scorer, 0.0, 2.0, [] (double t) { return t < 1.0 ? 59.70f : 62.0f; });

        const auto summary = scorer.buildSummary();

        check (summary.hasData, "summary: has data");
        check (summary.notesTotal == 2, "summary: counts both notes",
               juce::String (summary.notesTotal));
        check (summary.notesAttempted == 2, "summary: both attempted");
        check (summary.meanCents < 0.0f, "summary: mean cents is flat overall",
               juce::String (summary.meanCents, 1));
        check (summary.percentInTune > 0.5f && summary.percentInTune < 1.0f,
               "summary: partial credit lands between 0.5 and 1",
               juce::String (summary.percentInTune, 3));
        check (summary.phrases.size() == 1, "summary: one phrase",
               juce::String ((int) summary.phrases.size()));
        check (summary.phrases.front().percentHit > 0.9f,
               "summary: phrase percentHit counts any-credit ticks, not the integral",
               juce::String (summary.phrases.front().percentHit, 3));
        check (summary.headline().isNotEmpty(), "summary: headline is not empty");
        check (summary.histogramPeak > 0.0f, "summary: histogram was populated");
    }

    //==========================================================================
    // reset() really forgets.
    //==========================================================================
    {
        auto chart = makeTwoNoteChart();
        vt::TakeScorer scorer;
        scorer.setChart (chart.get());
        scorer.setTolerances (25.0f, 100.0f);
        scorer.setMedianMs (200);

        sing (scorer, 0.0, 1.0, [] (double) { return 60.0f; });
        check (scorer.hasAnyData(), "reset: data present before");

        scorer.reset();
        check (! scorer.hasAnyData(), "reset: data gone after");
        check (! scorer.buildSummary().hasData, "reset: summary is empty after");
    }

    //==========================================================================
    std::cout << std::endl << "=== ExerciseLibrary ===" << std::endl;

    //==========================================================================
    // Every exercise must produce a parseable chart with notes in range.
    //==========================================================================
    {
        for (const auto& def : vt::ExerciseLibrary::getAll())
        {
            auto spec = vt::ExerciseLibrary::makeDefaultSpec (def.kind, 60, 72);

            juce::String error;
            auto chart = vt::ExerciseLibrary::buildChart (spec, error);

            const bool ok = chart != nullptr && ! chart->isEmpty()
                              && chart->getLowestMidi()  >= 36
                              && chart->getHighestMidi() <= 84
                              && chart->getDurationSeconds() > 0.0
                              && ! chart->getPhrases().empty()
                              && chart->hasLyrics();

            check (ok, juce::String ("exercise builds: ") + def.name,
                   chart != nullptr
                     ? juce::String ((int) chart->getNotes().size()) + " notes, midi "
                         + juce::String (chart->getLowestMidi()) + "-"
                         + juce::String (chart->getHighestMidi()) + ", "
                         + juce::String ((int) chart->getPhrases().size()) + " phrases"
                     : error);
        }
    }

    //==========================================================================
    // The major scale really is a major scale. This is the check that catches a
    // typo in the interval table, which would otherwise just look like a scale.
    //==========================================================================
    {
        auto spec = vt::ExerciseLibrary::makeDefaultSpec (vt::ExerciseSpec::Kind::majorScale, 60, 120);
        spec.repeats = 1;

        juce::String error;
        auto chart = vt::ExerciseLibrary::buildChart (spec, error);

        check (chart != nullptr, "major scale: parses", error);

        if (chart != nullptr)
        {
            const std::vector<int> expected =
                { 60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62, 60 };

            bool match = chart->getNotes().size() == expected.size();

            for (size_t i = 0; match && i < expected.size(); ++i)
                match = chart->getNotes()[i].midi == expected[i];

            check (match, "major scale: exact semitone content up and back down",
                   juce::String ((int) chart->getNotes().size()) + " notes");

            check (chart->getNotes().front().syllable == "do"
                     && chart->getNotes()[2].syllable == "mi",
                   "major scale: solfege syllables land on the right degrees",
                   chart->getNotes()[2].syllable);

            // Notes must not overlap: the whole chart layer assumes it.
            bool ordered = true;

            for (size_t i = 1; i < chart->getNotes().size(); ++i)
                ordered = ordered && chart->getNotes()[i].startSeconds
                                       >= chart->getNotes()[i - 1].endSeconds();

            check (ordered, "major scale: notes are sorted and non-overlapping");
        }
    }

    //==========================================================================
    // The interval drill covers all twelve intervals, and its phrases name them
    // — which is what makes the review's interval grouping legible.
    //==========================================================================
    {
        auto spec = vt::ExerciseLibrary::makeDefaultSpec (vt::ExerciseSpec::Kind::intervalDrill, 60, 100);

        juce::String error;
        auto chart = vt::ExerciseLibrary::buildChart (spec, error);

        check (chart != nullptr, "interval drill: parses", error);

        if (chart != nullptr)
        {
            // A WHOLE NUMBER of passes, not a fixed twelve: the length preset
            // decides how many times round it goes, but a pass that stopped part
            // way through the intervals would leave the review's per-interval
            // breakdown comparing four attempts at a 2nd with one at a 7th.
            check (chart->getPhrases().size() >= 12
                     && chart->getPhrases().size() % 12 == 0,
                   "interval drill: whole passes of twelve intervals",
                   juce::String ((int) chart->getPhrases().size()));

            check (chart->getNotes().size() == chart->getPhrases().size() * 3,
                   "interval drill: root-interval-root for each",
                   juce::String ((int) chart->getNotes().size()));

            check (chart->getHighestMidi() - chart->getLowestMidi() == 12,
                   "interval drill: spans exactly one octave",
                   juce::String (chart->getHighestMidi() - chart->getLowestMidi()));

            check (chart->getPhrases().front().text.containsIgnoreCase ("minor 2nd"),
                   "interval drill: phrases are named after the interval",
                   chart->getPhrases().front().text);
        }
    }

    //==========================================================================
    // An out-of-range root is SHIFTED, not clipped: a scale with its top notes
    // silently deleted is a different exercise.
    //==========================================================================
    {
        auto spec = vt::ExerciseLibrary::makeDefaultSpec (vt::ExerciseSpec::Kind::majorScale, 84, 100);

        juce::String error;
        auto chart = vt::ExerciseLibrary::buildChart (spec, error);

        check (chart != nullptr && chart->getNotes().size() == 15 * (size_t) spec.repeats,
               "out-of-range root: whole scale survives",
               chart != nullptr ? juce::String ((int) chart->getNotes().size()) : error);
        check (chart != nullptr && chart->getHighestMidi() <= 84,
               "out-of-range root: shifted down into range",
               chart != nullptr ? juce::String (chart->getHighestMidi()) : juce::String());
    }

    //==========================================================================
    // The lead-in: a reference pitch rings, THEN the count-in clicks, THEN the
    // first note. The renderer lays the clicks out from the same two functions,
    // so if these ever disagreed the count-in would land on top of the reference
    // strike and the singer would come in early by the length of it.
    //==========================================================================
    {
        auto spec = vt::ExerciseLibrary::makeDefaultSpec (vt::ExerciseSpec::Kind::majorScale, 60, 60);

        juce::String error;
        auto chart = vt::ExerciseLibrary::buildChart (spec, error);

        const auto beat  = vt::ExerciseLibrary::beatSeconds (spec);
        const auto ref   = vt::ExerciseLibrary::referencePitchSeconds (spec);
        const auto lead  = vt::ExerciseLibrary::leadInSeconds (spec);

        check (spec.referenceBeats > 0, "lead-in: an exercise starts with a reference pitch",
               juce::String (spec.referenceBeats) + " beats");

        check (std::abs (ref - beat * spec.referenceBeats) < 1.0e-9
                 && std::abs (lead - ref - beat * spec.countInBeats) < 1.0e-9,
               "lead-in: reference pitch then count-in, in beats",
               juce::String (ref, 3) + " s + " + juce::String (lead - ref, 3) + " s");

        check (chart != nullptr && ! chart->getNotes().empty()
                 && std::abs (chart->getNotes().front().startSeconds - lead) < 1.0e-6,
               "lead-in: the first note starts when the count-in ends",
               chart != nullptr && ! chart->getNotes().empty()
                 ? juce::String (chart->getNotes().front().startSeconds, 3)
                 : error);
    }

    //==========================================================================
    // A long transposing exercise walks up and back DOWN. Climbing forever would
    // run off the top of the voice, and the range clamp would then quietly drop
    // the whole warm-up into a key nobody asked for.
    //==========================================================================
    {
        auto spec = vt::ExerciseLibrary::makeDefaultSpec (vt::ExerciseSpec::Kind::fiveNoteRun,
                                                          60, 72, vt::ExerciseLibrary::kLongSeconds);

        juce::String error;
        auto chart = vt::ExerciseLibrary::buildChart (spec, error);

        const auto climb = vt::ExerciseLibrary::climbSteps (spec);

        check (spec.repeats > climb + 1,
               "climb: more repetitions than the climb is tall, so it must turn round",
               juce::String (spec.repeats) + " repeats, climb " + juce::String (climb));

        if (chart != nullptr)
        {
            // The pattern tops out at +4 semitones, so the highest note in the
            // whole exercise is the root plus the climb plus that.
            check (chart->getLowestMidi() == 60
                     && chart->getHighestMidi() == 60 + climb + 4,
                   "climb: stays between the root and the turning point",
                   juce::String (chart->getLowestMidi()) + "-"
                     + juce::String (chart->getHighestMidi()));

            check (chart->getHighestMidi() <= 84 && chart->getLowestMidi() >= 36,
                   "climb: never leaves the singable range");

            //------------------------------------------------------------------
            // Up to the turning point and back to the root — checked on the
            // repetitions themselves rather than on the note range, because a
            // run that climbed to the top and STAYED there would have exactly
            // the same lowest and highest note as one that came back down.
            //------------------------------------------------------------------
            std::vector<int> repetitionRoots;

            for (const auto& phrase : chart->getPhrases())
                if (! phrase.noteIds.empty())
                    if (const auto* note = chart->noteById (phrase.noteIds.front()))
                        repetitionRoots.push_back (note->midi);

            size_t peakAt = 0;

            for (size_t i = 1; i < repetitionRoots.size(); ++i)
                if (repetitionRoots[i] > repetitionRoots[peakAt])
                    peakAt = i;

            const bool returned = std::find (repetitionRoots.begin() + (long) peakAt,
                                             repetitionRoots.end(), 60)
                                    != repetitionRoots.end();

            check (! repetitionRoots.empty()
                     && repetitionRoots[peakAt] == 60 + climb
                     && returned,
                   "climb: walks up to the turning point and back to the root",
                   juce::String ((int) repetitionRoots.size()) + " repetitions, peak "
                     + juce::String (repetitionRoots.empty() ? 0 : repetitionRoots[peakAt]));
        }
        else
        {
            check (false, "climb: builds", error);
        }
    }

    //==========================================================================
    // Length presets. The point of asking for a DURATION is that it means the
    // same thing across exercises whose repetitions differ by a factor of ten.
    //==========================================================================
    {
        struct Case { int target; const char* name; };

        for (const auto& c : { Case { vt::ExerciseLibrary::kShortSeconds,  "short" },
                               Case { vt::ExerciseLibrary::kMediumSeconds, "medium" },
                               Case { vt::ExerciseLibrary::kLongSeconds,   "long" } })
        {
            bool allWithinRange = true;
            juce::String worst;
            double worstRatio = 1.0;

            for (const auto& def : vt::ExerciseLibrary::getAll())
            {
                const auto spec = vt::ExerciseLibrary::makeDefaultSpec (def.kind, 60, 72, c.target);
                const auto seconds = vt::ExerciseLibrary::estimateDurationSeconds (spec);

                // Half to double the target. The granularity is one repetition
                // and one repetition of the interval drill is fifty seconds, so
                // anything tighter would be a promise the generator cannot keep;
                // outside it, the preset is not doing its job.
                const auto ratio = seconds / (double) c.target;

                if (ratio < 0.5 || ratio > 2.0)
                {
                    allWithinRange = false;

                    if (std::abs (std::log (ratio)) > std::abs (std::log (worstRatio)))
                    {
                        worstRatio = ratio;
                        worst = juce::String (def.name) + " " + juce::String (seconds, 0) + " s";
                    }
                }
            }

            check (allWithinRange,
                   juce::String ("length preset: ") + c.name + " lands near "
                     + juce::String (c.target) + " s for every exercise",
                   worst);
        }

        // And the presets are ordered, exercise by exercise.
        bool ordered = true;

        for (const auto& def : vt::ExerciseLibrary::getAll())
        {
            const auto shortSpec = vt::ExerciseLibrary::makeDefaultSpec (def.kind, 60, 72,
                                                                         vt::ExerciseLibrary::kShortSeconds);
            const auto longSpec  = vt::ExerciseLibrary::makeDefaultSpec (def.kind, 60, 72,
                                                                         vt::ExerciseLibrary::kLongSeconds);

            ordered = ordered && vt::ExerciseLibrary::estimateDurationSeconds (longSpec)
                                   > vt::ExerciseLibrary::estimateDurationSeconds (shortSpec);
        }

        check (ordered, "length preset: long is longer than short, for every exercise");
    }

    //==========================================================================
    // Scoring an exercise end to end. This is the integration that matters:
    // the same TakeScorer that scores a song must score a generated warm-up.
    //==========================================================================
    {
        auto spec = vt::ExerciseLibrary::makeDefaultSpec (vt::ExerciseSpec::Kind::majorScale, 60, 240);
        juce::String error;
        auto chart = vt::ExerciseLibrary::buildChart (spec, error);

        if (chart != nullptr)
        {
            vt::TakeScorer scorer;
            scorer.setChart (chart.get());
            scorer.setTolerances (25.0f, 100.0f);
            scorer.setMedianMs (200);

            // Sing every note exactly, straight off the chart.
            for (double t = 0.0; t < chart->getDurationSeconds(); t += kDt)
            {
                const auto idx = chart->noteIndexAt (t);
                scorer.feed (t, idx >= 0 ? (float) chart->getNotes()[(size_t) idx].midi
                                         : std::numeric_limits<float>::quiet_NaN());
            }

            const auto summary = scorer.buildSummary();

            check (summary.notesAttempted == (int) chart->getNotes().size(),
                   "exercise scored end to end: every note attempted",
                   juce::String (summary.notesAttempted) + "/"
                     + juce::String ((int) chart->getNotes().size()));
            check (summary.percentInTune > 0.98f,
                   "exercise scored end to end: a perfect run scores ~100%",
                   juce::String (summary.percentInTune, 4));
            check (summary.notesGood == summary.notesAttempted,
                   "exercise scored end to end: all notes rated good");

            // Consecutive scale steps are 2 semitones, so a perfect run must
            // produce interval groups with no error.
            bool intervalsClean = true;

            for (const auto& e : summary.intervalErrors)
                intervalsClean = intervalsClean && std::abs (e.meanAbsCents) < 5.0f;

            check (intervalsClean, "exercise scored end to end: interval errors are ~0");
        }
    }

    //==========================================================================
    std::cout << "-------------------------" << std::endl;

    if (failures == 0)
        std::cout << "ALL " << checks << " CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " of " << checks << " CHECKS FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
