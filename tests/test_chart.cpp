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
#include "chart/ChartWriter.h"
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
    // LATENCY COMPENSATION — the arithmetic MainComponent::onVBlank sets up
    // when the singer is on Bluetooth headphones.
    //
    // Pinned here because the two offsets go into setClock through DIFFERENT
    // arguments and both are easy to get backwards, and getting one backwards
    // does not crash or look broken: it silently doubles the error it was meant
    // to remove, and the singer is told they are rushing when they are not.
    //==========================================================================
    {
        constexpr double kOutputLatency = 0.200;   // Bluetooth headphones
        constexpr double kInputLatency  = 0.010;   // wired interface on the mic

        // The uncompensated case, for reference: the app writes chart second
        // 5.0 to the device, and a sample stamped at that same instant maps
        // straight onto it.
        vt::TimelineMap raw;
        raw.streamNow = 2400.0;
        raw.chartNow  = 5.0;
        check (near (raw.chartTimeFor (2400.0), 5.0, 1.0e-9),
               "latency: with no offsets the newest sample maps to chartNow");

        vt::TimelineMap map;
        map.streamNow = 2400.0 + kInputLatency;    // input goes onto streamNow
        map.chartNow  = 5.0    - kOutputLatency;   // output comes off chartNow

        // THE NOW-LINE follows the OUTPUT leg alone. Chart second 5.0 has been
        // written to the device but is still in flight; what the singer is
        // hearing right now is 4.8.
        check (near (map.chartNow, 4.8, 1.0e-9),
               "latency: now-line shows what is in the HEADPHONES, not the buffer");

        // THE MICROPHONE follows the FULL ROUND TRIP. A sample arriving now was
        // sung in reply to a cue that was already 200 ms late, and then took
        // 10 ms more to get back — so it belongs at 5.0 - 0.210.
        check (near (map.chartTimeFor (2400.0), 4.79, 1.0e-9),
               "latency: newest mic sample maps back by the whole round trip",
               juce::String (map.chartTimeFor (2400.0), 4));

        // Which is the point of the whole exercise: a singer who is DEAD ON
        // scores dead on. Sung 210 ms after the cue they heard, and credited to
        // the chart position that cue actually was.
        check (near (map.chartTimeFor (2400.0), raw.chartNow - kOutputLatency - kInputLatency,
                     1.0e-9),
               "latency: a perfectly-timed note scores at its true chart position");

        // Both offsets pull EARLIER, never later. A compensation that moved a
        // sample forwards would be describing a singer who replied before the
        // cue reached them.
        check (map.chartTimeFor (2400.0) < raw.chartTimeFor (2400.0),
               "latency: compensation only ever moves a sample earlier");

        // The trace must still be on screen with the offsets applied — a
        // correction that pushes the live samples out of the visible history
        // has taken the singer's feedback away to fix their timing.
        check (map.liveSampleIsVisible (2400.0 + kInputLatency)
                 && map.liveSampleIsVisible (2400.0 + kInputLatency - 0.4),
               "latency: live trace stays visible with offsets applied");
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
    // EDITING, and the one rule that makes it safe to offer at all: saving must
    // never lose a field this build does not read.
    //
    // songs/synth-demo/chart.json is the right fixture precisely because it is
    // full of things Chart::parse ignores — $schema, the whole `timing` block,
    // song.referenceAudioSha1, song.vocalRangeMidi, six generator fields, and a
    // 772-sample contour. A writer that serialised only the parsed fields would
    // delete every one of them the first time a user corrected a single word,
    // and the damage would not surface until the offline pipeline next ran.
    //==========================================================================
    {
        const auto original = findRepoRoot().getChildFile ("songs/synth-demo/chart.json");

        juce::String error;
        auto chart = vt::Chart::loadFrom (original, error);

        if (chart == nullptr)
        {
            check (false, "edit: fixture chart loaded", error);
        }
        else
        {
            const auto notesBefore = (int) chart->getNotes().size();
            const auto firstId = chart->getNotes().front().id;
            const auto midiBefore = chart->getNotes().front().midi;

            check (! chart->isModified(), "edit: a freshly loaded chart is not modified");

            //------------------------------------------------------- pitch
            chart->setNoteMidi (0, midiBefore - 12);

            check (chart->getNotes()[0].midi == midiBefore - 12,
                   "edit: an octave correction is applied",
                   juce::String (chart->getNotes()[0].midi));
            check (chart->isModified(), "edit: the modified flag is set");
            check (chart->indexForId (firstId) == 0,
                   "edit: the note is still findable by id");

            //------------------------------------------------------- lyrics
            chart->setNoteSyllable (0, "HAND");
            chart->markLyricsManual();

            check (chart->getNotes()[0].syllable == "HAND",
                   "edit: a corrected word is applied");
            check (chart->getLyricSource() == "manual",
                   "edit: lyricSource becomes manual, so the ASR badge comes off",
                   chart->getLyricSource());

            //-------------------------------------------- the no-overlap rule
            // Ask for a start that runs straight through the following note. It
            // must be clamped, not honoured: the renderer, the syllable wipe and
            // the scorer all assume at most one note sounds at a time.
            const auto secondStart = chart->getNotes()[1].startSeconds;
            chart->setNoteTiming (0, secondStart + 5.0, 4.0);

            bool nonOverlapping = true;

            for (size_t i = 1; i < chart->getNotes().size(); ++i)
                nonOverlapping = nonOverlapping
                                   && chart->getNotes()[i].startSeconds
                                        >= chart->getNotes()[i - 1].endSeconds();

            check (nonOverlapping,
                   "edit: a timing edit cannot make notes overlap");
            check ((int) chart->getNotes().size() == notesBefore,
                   "edit: a timing edit does not add or drop notes");

            //--------------------------------------------- phrase re-flow
            // Nine tokens onto seven notes. This overwrites the "HAND" set above,
            // which is the point of a re-flow: it replaces the whole line.
            chart->setPhraseText (0, "one two three four five six seven eight nine");

            int worded = 0;

            for (const auto& n : chart->getNotes())
                if (n.syllable.isNotEmpty())
                    ++worded;

            check (worded == notesBefore,
                   "edit: a re-flowed line puts a word on every note in the phrase",
                   juce::String (worded) + " of " + juce::String (notesBefore));

            check (chart->getNotes()[0].syllable == "one"
                     && chart->getNotes()[1].syllable == "two",
                   "edit: re-flowed words land in TIME order, not noteIds order",
                   chart->getNotes()[0].syllable + " / " + chart->getNotes()[1].syllable);

            // Surplus tokens go onto the last note rather than being dropped: text
            // the user typed must never vanish without them seeing where it went.
            check (chart->getNotes()[(size_t) notesBefore - 1].syllable.containsIgnoreCase ("nine"),
                   "edit: surplus words are appended, not silently discarded",
                   chart->getNotes()[(size_t) notesBefore - 1].syllable);

            //-------------------------------------------------- round trip
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("vt_chart_writer_test")
                                 .getChildFile ("chart.json");

            temp.deleteFile();
            temp.getSiblingFile ("chart.json.bak").deleteFile();

            juce::String saveError;
            const auto saved = vt::ChartWriter::save (*chart, temp, saveError);

            check (saved, "write: the edited chart saves", saveError);

            // Every unread field, checked against the raw JSON rather than
            // against a reloaded Chart — a Chart would not show their loss.
            juce::var written;
            juce::JSON::parse (temp.loadFileAsString(), written);

            const auto has = [&written] (const char* key)
            {
                auto* o = written.getDynamicObject();
                return o != nullptr && o->hasProperty (key);
            };

            const auto child = [&written] (const char* key)
            {
                auto* o = written.getDynamicObject();
                return o != nullptr ? o->getProperty (key) : juce::var();
            };

            check (has ("$schema") && has ("timing") && has ("contour"),
                   "write: unread top-level blocks survive ($schema, timing, contour)");

            const auto contourOut = child ("contour");

            // juce::var::getProperty needs a default value; DynamicObject's
            // single-argument form is the one wanted here.
            const juce::Array<juce::var>* fracMidi = nullptr;

            if (auto* contourObj = contourOut.getDynamicObject())
                fracMidi = contourObj->getProperty ("fracMidi").getArray();

            check (fracMidi != nullptr && fracMidi->size() == 772,
                   "write: the contour is passed through intact, not regenerated",
                   fracMidi != nullptr ? juce::String (fracMidi->size()) + " samples"
                                       : juce::String ("missing"));

            const auto songOut = child ("song");
            const auto genOut  = child ("generator");

            const auto songHas = [&songOut] (const char* key)
            {
                auto* o = songOut.getDynamicObject();
                return o != nullptr && o->hasProperty (key);
            };

            check (songHas ("referenceAudioSha1") && songHas ("vocalRangeMidi")
                     && songHas ("detectedKey"),
                   "write: unread song fields survive (sha1, vocalRangeMidi, detectedKey)");

            const auto genHas = [&genOut] (const char* key)
            {
                auto* o = genOut.getDynamicObject();
                return o != nullptr && o->hasProperty (key);
            };

            check (genHas ("tool") && genHas ("version") && genHas ("pitchModel")
                     && genHas ("generatedAt"),
                   "write: unread generator fields survive");

            const auto lyricSourceOut = genOut.getDynamicObject() != nullptr
                                          ? genOut.getDynamicObject()->getProperty ("lyricSource").toString()
                                          : juce::String();

            check (lyricSourceOut == "manual",
                   "write: lyricSource IS rewritten - it is ours to own",
                   lyricSourceOut);

            //------------------------------------------------ reload the edit
            auto reloaded = vt::Chart::loadFrom (temp, error);

            check (reloaded != nullptr, "write: the saved chart parses again", error);

            if (reloaded != nullptr)
            {
                check ((int) reloaded->getNotes().size() == notesBefore,
                       "write: the note count round-trips",
                       juce::String ((int) reloaded->getNotes().size()));

                const auto* byId = reloaded->noteById (firstId);

                check (byId != nullptr && byId->midi == midiBefore - 12,
                       "write: the pitch correction round-trips");
                check (byId != nullptr && byId->syllable == "one",
                       "write: the word correction round-trips",
                       byId != nullptr ? byId->syllable : juce::String());
                check (reloaded->getLyricSource() == "manual",
                       "write: lyricSource round-trips as manual");
                check (! reloaded->getContour().isEmpty(),
                       "write: the reloaded chart still has its ghost contour");
            }

            //-------------------------------------------------- second save
            // Saving over an existing file must leave a .bak behind: hand-editing
            // a chart is exactly where one mis-click wants an undo.
            chart->setNoteSyllable (0, "AGAIN");
            vt::ChartWriter::save (*chart, temp, saveError);

            check (temp.getSiblingFile ("chart.json.bak").existsAsFile(),
                   "write: the previous version is kept as chart.json.bak");

            temp.getParentDirectory().deleteRecursively();
        }
    }

    //==========================================================================
    // A chart must never be edited down to nothing: Chart::parse refuses a
    // chart with no notes, so allowing it would let the editor save a file it
    // could never load again.
    //==========================================================================
    {
        juce::var parsed;
        juce::JSON::parse ("{\"schemaVersion\":\"1.0\",\"notes\":[{\"id\":0,\"midi\":60,"
                           "\"startSeconds\":0,\"durationSeconds\":1}]}", parsed);

        juce::String error;
        auto single = vt::Chart::parse (parsed, error);

        check (single != nullptr && ! single->removeNote (0),
               "edit: the last remaining note cannot be deleted");
    }

    //==========================================================================
    std::cout << "-------------------------" << std::endl;

    if (failures == 0)
        std::cout << "ALL " << checks << " CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " of " << checks << " CHECKS FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
