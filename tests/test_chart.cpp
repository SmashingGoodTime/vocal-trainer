/*
    ==========================================================================

    test_chart.cpp — Chart parsing, time queries, and octave folding.

    Runs against the REAL chart.json produced by the offline pipeline
    (songs/synth-demo), not a hand-written fixture, so a schema drift between
    the Python writer and the C++ reader fails here rather than on stage.

    ==========================================================================
*/

#include <juce_core/juce_core.h>

#include <cmath>
#include <iostream>
#include <vector>

#include "chart/Chart.h"
#include "chart/PitchMath.h"
#include "chart/TimelineMap.h"

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
                  << name.paddedRight (' ', 56).toStdString();

        if (detail.isNotEmpty())
            std::cout << detail.toStdString();

        std::cout << std::endl;
    }

    bool near (double a, double b, double tol) { return std::abs (a - b) <= tol; }

    /** Walks up from the executable to find the repo root, so the test does not
        depend on the working directory the runner happens to use. */
    juce::File findRepoRoot()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

        for (int i = 0; i < 8 && dir != juce::File(); ++i)
        {
            dir = dir.getParentDirectory();

            if (dir.getChildFile ("CMakeLists.txt").existsAsFile()
                && dir.getChildFile ("src").isDirectory())
                return dir;
        }

        return {};
    }
}

int main()
{
    std::cout << "vocal-trainer chart tests\n"
              << "-------------------------" << std::endl;

    //==========================================================================
    // Octave folding — the display/score agreement that DESIGN.md section 7
    // calls the most likely single UX bug in the project.
    //==========================================================================
    {
        // Every case computes into locals FIRST. Building the detail string
        // inline would call foldOntoTarget again inside the argument list, where
        // evaluation order is unspecified — which prints a value that disagrees
        // with the assertion and sends you hunting a bug that is not there.
        int oct = 0;
        auto folded = vt::foldOntoTarget (48.0f, 60.0f, oct);
        check (near (folded, 60.0f, 1.0e-4) && oct == -1,
               "octave fold: an octave below reads as the target",
               "folded=" + juce::String (folded) + " oct=" + juce::String (oct));

        folded = vt::foldOntoTarget (72.0f, 60.0f, oct);
        check (near (folded, 60.0f, 1.0e-4) && oct == 1,
               "octave fold: an octave above reads as the target",
               "folded=" + juce::String (folded) + " oct=" + juce::String (oct));

        folded = vt::foldOntoTarget (60.25f, 60.0f, oct);
        check (near (folded, 60.25f, 1.0e-4) && oct == 0,
               "octave fold: a small error is left alone",
               "folded=" + juce::String (folded));

        // Exactly a tritone is genuinely ambiguous — +6 and -6 are equidistant,
        // so either fold is correct. What must NOT happen is a wild result. The
        // discontinuity is inherent to octave folding, not a defect.
        folded = vt::foldOntoTarget (66.0f, 60.0f, oct);
        check (std::abs (folded - 60.0f) <= 6.0f + 1.0e-4f,
               "octave fold: tritone stays within half an octave",
               "folded=" + juce::String (folded) + " oct=" + juce::String (oct));

        const auto nan = std::numeric_limits<float>::quiet_NaN();
        folded = vt::foldOntoTarget (nan, 60.0f, oct);
        check (std::isnan (folded) && oct == 0,
               "octave fold: NaN passes through as NaN");

        // The property that actually matters: whatever the octave, the folded
        // result is always within half an octave of the target.
        bool allWithin = true;

        for (int semis = -36; semis <= 36; ++semis)
        {
            const auto f = vt::foldOntoTarget (60.0f + (float) semis, 60.0f, oct);

            if (std::abs (f - 60.0f) > 6.0f + 1.0e-4f)
                allWithin = false;
        }

        check (allWithin, "octave fold: holds across +/-3 octaves");

        check (near (vt::centsFromTarget (47.5f, 60.0f), -50.0f, 0.01f),
               "centsFromTarget: octave-folded cents are signed and correct",
               juce::String (vt::centsFromTarget (47.5f, 60.0f)) + " cents");
    }

    //==========================================================================
    // Two-clock mapping. REGRESSION GUARD for a bug found by actually running
    // the app: with a chart loaded, the live microphone trace vanished
    // completely, because samples were stamped in STREAM time while the
    // now-line ran on the backing track's CHART clock.
    //==========================================================================
    {
        vt::TimelineMap map;
        map.historySeconds = 2.0;
        map.lookaheadSeconds = 3.5;

        // The realistic failure: the audio device has been open for 40 minutes,
        // the song is 5 seconds in. Those numbers differ by three orders of
        // magnitude, which is what made the old code fail so completely.
        map.streamNow = 2400.0;
        map.chartNow  = 5.0;

        check (near (map.chartTimeFor (2400.0), 5.0, 1.0e-9),
               "clock: the newest sample maps to the now-line");
        check (near (map.chartTimeFor (2399.5), 4.5, 1.0e-9),
               "clock: a 500 ms old sample maps 500 ms earlier on the chart");
        check (near (map.ageOf (2399.5), 0.5, 1.0e-9),
               "clock: age is measured in stream time");

        check (map.liveSampleIsVisible (2400.0),
               "clock: newest sample is on screen");
        check (map.liveSampleIsVisible (2399.5),
               "clock: 500 ms of trail is on screen");
        check (! map.liveSampleIsVisible (2396.0),
               "clock: a sample older than the history window is off screen");

        // The invariant must hold for ANY offset between the clocks, including
        // a paused song (chartNow frozen while streamNow keeps advancing) and a
        // song seeked back to zero.
        bool alwaysVisible = true;

        const double chartPositions[] = { 0.0, 0.001, 5.0, 137.4, 226.6 };
        const double streamPositions[] = { 0.0, 12.5, 2400.0, 86400.0 };

        for (const auto chartT : chartPositions)
            for (const auto streamT : streamPositions)
            {
                map.streamNow = streamT;
                map.chartNow  = chartT;

                if (! map.liveSampleIsVisible (streamT)          // newest
                    || ! map.liveSampleIsVisible (streamT - 0.4)) // recent trail
                    alwaysVisible = false;
            }

        check (alwaysVisible,
               "clock: live trace is visible at EVERY clock offset",
               "20 chart/stream combinations");

        // And the chart content still scrolls independently of all that.
        map.streamNow = 100.0;
        map.chartNow  = 10.0;
        check (map.isVisible (12.0) && ! map.isVisible (30.0),
               "clock: chart notes are placed by CHART time, not stream time");
    }

    //==========================================================================
    // Credit ramp (DESIGN.md section 6).
    //==========================================================================
    {
        check (near (vt::pitchCredit (0.0f, 25.0f, 100.0f), 1.0f, 1.0e-4),
               "credit: dead on = full credit");
        check (near (vt::pitchCredit (25.0f, 25.0f, 100.0f), 1.0f, 1.0e-4),
               "credit: exactly at tFull = still full credit");
        check (near (vt::pitchCredit (62.5f, 25.0f, 100.0f), 0.5f, 1.0e-3),
               "credit: halfway between thresholds = half credit");
        check (near (vt::pitchCredit (100.0f, 25.0f, 100.0f), 0.0f, 1.0e-4),
               "credit: at tZero = no credit");
        check (near (vt::pitchCredit (-62.5f, 25.0f, 100.0f), 0.5f, 1.0e-3),
               "credit: symmetric for flat and sharp");
    }

    //==========================================================================
    // Parse the real generated chart.
    //==========================================================================
    const auto root = findRepoRoot();
    const auto chartFile = root.getChildFile ("songs/synth-demo/chart.json");

    if (! chartFile.existsAsFile())
    {
        std::cout << "[SKIP] songs/synth-demo/chart.json not found - run:\n"
                  << "         .venv/Scripts/python -m vocalchart.cli build songs/synth-demo\n"
                  << "       (chart parsing tests skipped)" << std::endl;
    }
    else
    {
        juce::String error;
        auto chart = vt::Chart::loadFrom (chartFile, error);

        check (chart != nullptr, "chart: parses the pipeline's real chart.json", error);

        if (chart != nullptr)
        {
            const auto& notes = chart->getNotes();

            // SEVEN, not six. The synthetic melody has seven notes, two of which
            // are adjacent C4s 60 ms apart. Those used to fuse into one - the
            // same syllable-fusing bug that flattened a whole real song's rhythm
            // (DESIGN.md 11.5). If this ever reads 6 again, the segmenter has
            // regressed, not the test.
            check (notes.size() == 7, "chart: note count",
                   juce::String (notes.size()) + " notes");

            const std::vector<int> expected { 55, 60, 64, 60, 60, 64, 62 };
            bool midiOk = notes.size() == expected.size();

            juce::String got;

            for (size_t i = 0; i < notes.size(); ++i)
            {
                got << notes[i].midi << (i + 1 < notes.size() ? " " : "");

                if (midiOk && notes[i].midi != expected[i])
                    midiOk = false;
            }

            check (midiOk, "chart: MIDI values match the synthesised melody", got);

            check (near (chart->getTuningHz(), 440.0, 0.01),
                   "chart: tuning was rejected and fell back to A=440",
                   juce::String (chart->getTuningHz(), 2) + " Hz");

            // Notes must come out sorted and non-overlapping — every query below
            // is a binary search that assumes it.
            bool ordered = true;

            for (size_t i = 1; i < notes.size(); ++i)
                if (notes[i].startSeconds < notes[i - 1].endSeconds() - 1.0e-9)
                    ordered = false;

            check (ordered, "chart: notes are sorted and non-overlapping");

            //---------------------------------------------------------- queries
            const auto& n0 = notes[0];
            const auto mid0 = n0.startSeconds + n0.durationSeconds * 0.5;

            check (chart->noteIndexAt (mid0) == 0,
                   "query: noteIndexAt finds the sounding note");

            check (chart->noteIndexAt (n0.startSeconds - 0.05) == -1,
                   "query: noteIndexAt returns -1 in a gap before the first note");

            check (chart->nextNoteIndexFrom (0.0) == 0,
                   "query: nextNoteIndexFrom at t=0 is the first note");

            // targetNoteIndex must pre-arm the detector slightly BEFORE the note,
            // otherwise the octave constraint arrives after the attack.
            const auto justBefore = n0.startSeconds - 0.2;

            check (justBefore < 0.0 || chart->targetNoteIndex (justBefore) == 0,
                   "query: targetNoteIndex pre-arms just before a note starts");

            check (chart->targetNoteIndex (n0.startSeconds - 5.0) == -1,
                   "query: targetNoteIndex does not pre-arm 5 s early");

            int first = 0, last = 0;
            const auto any = chart->notesOverlapping (0.0, chart->getDurationSeconds(), first, last);

            check (any && first == 0 && last == (int) notes.size(),
                   "query: notesOverlapping spans the whole chart",
                   juce::String (first) + ".." + juce::String (last));

            check (! chart->notesOverlapping (chart->getDurationSeconds() + 10.0,
                                              chart->getDurationSeconds() + 20.0, first, last),
                   "query: notesOverlapping past the end finds nothing");

            //--------------------------------------------------------- contour
            const auto& contour = chart->getContour();

            check (! contour.isEmpty(), "chart: ghost contour present",
                   juce::String (contour.fracMidi.size()) + " frames @ "
                     + juce::String (contour.rateHz, 0) + " Hz");

            if (! contour.isEmpty())
            {
                // The lead-in is silence, so those frames must be NaN, not 0.0 —
                // a 0.0 would draw the ghost line at MIDI 0 across the screen.
                check (std::isnan (contour.at (0.0)) || contour.at (0.0) > 20.0f,
                       "chart: unvoiced contour frames are NaN, not zero",
                       "at(0)=" + juce::String (contour.at (0.0)));

                const auto duringNote = contour.at (mid0);

                check (std::isfinite (duringNote)
                         && std::abs (duringNote - (float) n0.midi) < 1.5f,
                       "chart: contour agrees with the note it sits under",
                       "contour=" + juce::String (duringNote) + " note=" + juce::String (n0.midi));
            }

            //--------------------------------------------------- backing audio
            // The chart stores a RELATIVE path; resolving it against the chart's
            // own directory is what lets a song folder be moved without breaking.
            const auto backing = chart->resolveBackingAudio();

            if (chart->getBackingAudioRelative().isNotEmpty())
            {
                check (backing != juce::File() && backing.existsAsFile(),
                       "backing: relative path resolves to a real file",
                       backing.getFullPathName());

                check (backing.getParentDirectory() == chartFile.getParentDirectory(),
                       "backing: resolved against the chart's own directory");
            }
            else
            {
                std::cout << "[INFO] chart has no backingAudio - silent practice is legal"
                          << std::endl;
            }

            // syllable == null is legal and expected; it must not crash or
            // produce the literal string "null".
            bool syllablesOk = true;

            for (const auto& n : notes)
                if (n.syllable == "null")
                    syllablesOk = false;

            check (syllablesOk, "chart: JSON null syllables read as empty, not \"null\"");
        }
    }

    //==========================================================================
    // Malformed input must fail cleanly rather than crash.
    //==========================================================================
    {
        juce::String error;
        juce::var parsed;

        juce::JSON::parse ("{\"schemaVersion\":\"1.0\",\"notes\":[]}", parsed);
        check (vt::Chart::parse (parsed, error) == nullptr && error.isNotEmpty(),
               "robustness: empty note list is rejected with a message");

        juce::JSON::parse ("{\"schemaVersion\":\"9.0\",\"notes\":[{\"midi\":60,"
                           "\"startSeconds\":0,\"durationSeconds\":1}]}", parsed);
        check (vt::Chart::parse (parsed, error) == nullptr,
               "robustness: a future schemaVersion is refused, not misread");

        check (vt::Chart::parse (juce::var (42), error) == nullptr,
               "robustness: non-object JSON is rejected");
    }

    //==========================================================================
    std::cout << "-------------------------" << std::endl;

    if (failures == 0)
        std::cout << "ALL " << checks << " CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " of " << checks << " CHECKS FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
