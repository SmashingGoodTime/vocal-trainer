/*
    ==========================================================================

    test_render.cpp — renders the highway OFFSCREEN and inspects the pixels.

    WHY THIS EXISTS
    ---------------
    Every layer of the highway was unit-tested and correct, and the thing was
    still completely blank on screen. The grid lives in a CHILD component, JUCE
    paints children AFTER the parent, and that child does a full-bounds
    fillAll() — so the notes, the trace, the dot and even the now-line were all
    drawn correctly and then painted over. Only moving the content into
    paintOverChildren() fixed it.

    No amount of logic testing catches that. Rendering to an Image and counting
    pixels does. This is the cheapest possible substitute for a human looking at
    the window, and it guards the entire class of "it computes fine but you
    cannot see it" bug.

    LOOKING AT THE FRAMES
    ---------------------
    Set VT_RENDER_DUMP_DIR to a directory and every frame this file renders is
    written there as a PNG. The checks below are the automated half; when one of
    them fails, or when a theme is being tuned, the picture is what actually
    answers the question.

    ==========================================================================
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

#include "app/AppSettings.h"
#include "chart/Chart.h"
#include "import/SongImporter.h"
#include "score/TakeScorer.h"
#include "ui/ImportOverlayComponent.h"
#include "ui/NoteHighwayComponent.h"
#include "ui/PianoKeyboardComponent.h"
#include "ui/Theme.h"
#include "ui/TransportBarComponent.h"

namespace
{
    int failures = 0, checks = 0;

    /** Renders a component and, if VT_RENDER_DUMP_DIR is set, writes the frame
        there as a PNG. Named so the files are obvious in a folder listing. */
    juce::Image renderFrame (juce::Component& c, const juce::String& name)
    {
        juce::Image img (juce::Image::ARGB, juce::jmax (1, c.getWidth()),
                         juce::jmax (1, c.getHeight()), true);
        {
            juce::Graphics g (img);
            c.paintEntireComponent (g, false);
        }

        const auto dir = juce::SystemStats::getEnvironmentVariable ("VT_RENDER_DUMP_DIR", {});

        if (dir.isNotEmpty())
        {
            const juce::File out = juce::File (dir).getChildFile (name + ".png");
            out.getParentDirectory().createDirectory();
            out.deleteFile();

            juce::PNGImageFormat png;

            if (auto stream = std::unique_ptr<juce::FileOutputStream> (out.createOutputStream()))
                png.writeImageToStream (img, *stream);
        }

        return img;
    }

    void check (bool ok, const juce::String& name, const juce::String& detail = {})
    {
        ++checks;
        if (! ok) ++failures;

        std::cout << (ok ? "[PASS] " : "[FAIL] ")
                  << name.paddedRight (' ', 52).toStdString();

        if (detail.isNotEmpty())
            std::cout << detail.toStdString();

        std::cout << std::endl;
    }

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

    struct PixelStats
    {
        int total = 0;
        int nonBackground = 0;
        int blueish = 0;     // target note bars
        int greenish = 0;    // in-tune trace / dot
        int reddish = 0;     // a note scored as out of tune (#D55E00)
        int brightest = 0;
    };

    PixelStats analyse (const juce::Image& img, juce::Colour background)
    {
        PixelStats s;
        const juce::Image::BitmapData bmp (img, juce::Image::BitmapData::readOnly);

        for (int y = 0; y < img.getHeight(); ++y)
            for (int x = 0; x < img.getWidth(); ++x)
            {
                const auto c = bmp.getPixelColour (x, y);
                ++s.total;

                const auto dr = std::abs ((int) c.getRed()   - (int) background.getRed());
                const auto dg = std::abs ((int) c.getGreen() - (int) background.getGreen());
                const auto db = std::abs ((int) c.getBlue()  - (int) background.getBlue());
                const auto diff = dr + dg + db;

                if (diff > 24)
                    ++s.nonBackground;

                s.brightest = juce::jmax (s.brightest, (int) c.getBlue() + (int) c.getGreen());

                // The palette is Okabe-Ito: target bars #56B4E9 / #0072B2 are
                // strongly blue-dominant; the in-tune trace #009E73 is
                // green-dominant with almost no red.
                if (c.getBlue() > 90 && c.getBlue() > c.getRed() + 40 && c.getBlue() > c.getGreen() + 15)
                    ++s.blueish;

                if (c.getGreen() > 80 && c.getGreen() > c.getRed() + 40 && c.getGreen() > c.getBlue() + 15)
                    ++s.greenish;

                // Vermilion #D55E00 — the out-of-tune end of the ramp. Strongly
                // red-dominant with essentially no blue, which nothing else in
                // the Okabe-Ito palette used on the highway is.
                if (c.getRed() > 90 && c.getRed() > c.getGreen() + 45 && c.getRed() > c.getBlue() + 70)
                    ++s.reddish;
            }

        return s;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::cout << "vocal-trainer offscreen render tests\n"
              << "------------------------------------" << std::endl;

    const auto root = findRepoRoot();
    const auto chartFile = root.getChildFile ("songs/synth-demo/chart.json");

    if (! chartFile.existsAsFile())
    {
        std::cout << "[SKIP] songs/synth-demo/chart.json missing - build it first." << std::endl;
        return 0;
    }

    juce::String error;
    auto chart = vt::Chart::loadFrom (chartFile, error);

    if (chart == nullptr)
    {
        std::cout << "[FAIL] could not load chart: " << error.toStdString() << std::endl;
        return 1;
    }

    juce::SharedResourcePointer<vt::Theme> theme;
    const auto background = theme->get().background;

    vt::NoteHighwayComponent highway (*theme);
    highway.setBounds (0, 0, 1200, 600);
    highway.setChart (chart.get());

    //==========================================================================
    // Park the now-line inside the first phrase so notes are definitely in view,
    // and feed a short run of live samples right on the first note's pitch.
    //==========================================================================
    const auto& notes = chart->getNotes();
    const auto& first = notes.front();
    const auto chartNow = first.startSeconds + first.durationSeconds * 0.5;

    constexpr double streamNow = 2400.0;      // a long-running audio device
    constexpr double hop = 1.0 / 125.0;

    for (int i = 60; i >= 0; --i)
    {
        vt::NoteHighwayComponent::Sample s;
        s.timeSeconds  = streamNow - i * hop;
        s.rawMidi      = (float) first.midi + 0.1f * std::sin ((float) i * 0.4f);
        s.smoothedMidi = (float) first.midi;
        s.clarity      = 0.95f;
        s.rmsDb        = -20.0f;
        highway.pushSample (s);
    }

    highway.setClock (streamNow, chartNow);
    highway.tickAnimation (0.016);

    juce::Image img (juce::Image::ARGB, 1200, 600, true);
    {
        juce::Graphics g (img);
        highway.paintEntireComponent (g, false);
    }

    const auto stats = analyse (img, background);
    const auto pct = [&] (int n) { return juce::String (100.0 * n / juce::jmax (1, stats.total), 2) + "%"; };

    check (stats.nonBackground > stats.total / 200,
           "render: the highway is not blank",
           pct (stats.nonBackground) + " of pixels differ from the background");

    check (stats.blueish > 500,
           "render: target note bars are visible",
           juce::String (stats.blueish) + " blue-dominant pixels");

    // The trail fades to zero alpha by design, so only its newest segments read
    // as saturated green. The DOT, however, is drawn at full alpha and is the
    // single most important pixel on screen — it is where the singer looks. So
    // probe for it specifically, at its predicted position, rather than relying
    // on a whole-image colour count that a faint trail can fail on its own.
    {
        const auto nowX = juce::roundToInt (1200.0 * (2.0 / 5.5));   // history / total span
        int dotPixels = 0;
        int brightestNearNow = 0;

        const juce::Image::BitmapData bmp (img, juce::Image::BitmapData::readOnly);

        for (int y = 0; y < img.getHeight(); ++y)
            for (int x = juce::jmax (0, nowX - 14); x < juce::jmin (img.getWidth(), nowX + 14); ++x)
            {
                const auto c = bmp.getPixelColour (x, y);

                if (c.getGreen() > 70 && c.getGreen() > c.getRed() + 30)
                {
                    ++dotPixels;
                    brightestNearNow = juce::jmax (brightestNearNow, (int) c.getGreen());
                }
            }

        check (dotPixels >= 20,
               "render: the live pitch DOT is drawn at the now-line",
               juce::String (dotPixels) + " px, brightest green "
                 + juce::String (brightestNearNow));
    }

    check (stats.greenish > 20,
           "render: the live pitch trace is visible",
           juce::String (stats.greenish) + " green-dominant pixels (trail fades by design)");

    //==========================================================================
    // The regression that motivated this file: content must survive the child
    // grid layer painting on top. Compare paint() alone against the full
    // parent+children pass — if the visible content were in paint(), the full
    // pass would have FEWER interesting pixels than the child-free one.
    //==========================================================================
    juce::Image parentOnly (juce::Image::ARGB, 1200, 600, true);
    {
        juce::Graphics g (parentOnly);
        highway.paint (g);         // deliberately WITHOUT children
    }

    const auto parentStats = analyse (parentOnly, background);

    check (parentStats.blueish + parentStats.greenish == 0,
           "render: paint() itself draws no content",
           "content belongs in paintOverChildren()");

    check (stats.blueish + stats.greenish > 0,
           "render: content survives the child grid layer",
           "this is the bug that made the highway look blank");

    //==========================================================================
    // M6: A SUNG NOTE IS RECOLOURED BY ITS ACCURACY.
    //
    // Same class of bug as the blank highway, and just as invisible to logic
    // tests: TakeScorer can compute a perfectly correct accuracy for every note
    // and the bars can still come out the same blue they always were, because
    // the recolouring branch is never reached (wrong "finished" comparison, a
    // null scorer, showScoredNotes off, the halo drawn over the top). The only
    // way to know the singer can SEE their result is to look at the pixels.
    //
    // The check is a DELTA rather than an absolute count: render the same frame
    // with and without a scorer attached, and require that badly-sung notes have
    // added vermilion. That cannot pass by accident, and it cannot fail because
    // of a palette change.
    //==========================================================================
    {
        // Find a playhead position with at least two FINISHED notes just behind
        // it, so the recoloured bars are inside the 2 s history window.
        int firstScored = -1;
        double scoredNow = 0.0;

        for (size_t i = 1; i < notes.size(); ++i)
        {
            if (notes[i].endSeconds() - notes[i - 1].startSeconds < 1.8)
            {
                firstScored = (int) i - 1;
                scoredNow = notes[i].endSeconds() + 0.05;
                break;
            }
        }

        if (firstScored < 0)
        {
            std::cout << "[SKIP] no two consecutive notes close enough together to "
                         "test scored colouring" << std::endl;
        }
        else
        {
            vt::TakeScorer scorer;
            scorer.setChart (chart.get());
            scorer.setTolerances (25.0f, 100.0f);
            scorer.setMedianMs (200);

            // Sung 90 cents sharp throughout: inside tZero, so it earns a little
            // credit and lands near the out-of-tune end of the ramp rather than
            // being reported as never sung.
            for (double t = notes[(size_t) firstScored].startSeconds; t < scoredNow; t += hop)
            {
                const auto idx = chart->noteIndexAt (t);

                scorer.feed (t, idx >= 0 ? (float) chart->getNotes()[(size_t) idx].midi + 0.9f
                                         : std::numeric_limits<float>::quiet_NaN());
            }

            check (scorer.hasAnyData(), "scored render: the scorer captured the take");

            const auto renderAt = [&] (const vt::TakeScorer* s)
            {
                highway.setScorer (s);
                highway.setClock (streamNow, scoredNow);
                highway.tickAnimation (0.016);

                juce::Image image (juce::Image::ARGB, 1200, 600, true);

                // The Graphics MUST be destroyed before the pixels are read. The
                // software renderer holds an Image::BitmapData write lock for its
                // whole lifetime, and opening a second, read-only BitmapData on
                // the same image while it is held hands back an empty buffer —
                // every colour count comes out zero and the highway looks blank
                // when it is in fact drawn correctly.
                {
                    juce::Graphics g (image);
                    highway.paintEntireComponent (g, false);
                }

                return analyse (image, background);
            };

            const auto unscored = renderAt (nullptr);
            const auto scored   = renderAt (&scorer);

            check (scored.reddish > unscored.reddish + 200,
                   "scored render: badly sung notes turn vermilion",
                   juce::String (unscored.reddish) + " -> " + juce::String (scored.reddish)
                     + " red-dominant px");

            check (scored.blueish < unscored.blueish,
                   "scored render: those notes stop being drawn as upcoming blue",
                   juce::String (unscored.blueish) + " -> " + juce::String (scored.blueish)
                     + " blue-dominant px");

            // And the reverse: a well-sung take must NOT go red. Without this the
            // check above would still pass if every note were painted vermilion
            // regardless of how it was actually sung.
            vt::TakeScorer goodScorer;
            goodScorer.setChart (chart.get());
            goodScorer.setTolerances (25.0f, 100.0f);
            goodScorer.setMedianMs (200);

            for (double t = notes[(size_t) firstScored].startSeconds; t < scoredNow; t += hop)
            {
                const auto idx = chart->noteIndexAt (t);

                goodScorer.feed (t, idx >= 0 ? (float) chart->getNotes()[(size_t) idx].midi
                                             : std::numeric_limits<float>::quiet_NaN());
            }

            const auto good = renderAt (&goodScorer);

            check (good.reddish < scored.reddish / 4,
                   "scored render: a well sung take does NOT go red",
                   juce::String (good.reddish) + " red px vs "
                     + juce::String (scored.reddish) + " when out of tune");

            check (good.greenish > unscored.greenish,
                   "scored render: a well sung take adds green",
                   juce::String (unscored.greenish) + " -> " + juce::String (good.greenish)
                     + " green-dominant px");

            highway.setScorer (nullptr);
        }
    }

    //==========================================================================
    // NOTE NAMES ON THE BARS.
    //
    // A pure delta: the same frame with the labels on and off. Anything that
    // silently suppresses them — a width threshold that never passes, a colour
    // that resolves to the bar's own fill, text drawn outside the clip — comes
    // out as "no difference", which no logic test would notice.
    //==========================================================================
    const auto baseValues = theme->get();

    {
        auto withNames = baseValues;
        withNames.showNoteNames = true;

        auto withoutNames = baseValues;
        withoutNames.showNoteNames = false;

        highway.setClock (streamNow, chartNow);
        highway.tickAnimation (0.016);

        theme->adoptValues (withoutNames);
        highway.applyTheme();
        highway.tickAnimation (0.016);
        const auto plain = renderFrame (highway, "highway-without-note-names");

        theme->adoptValues (withNames);
        highway.applyTheme();
        highway.tickAnimation (0.016);
        const auto labelled = renderFrame (highway, "highway-with-note-names");

        const juce::Image::BitmapData a (plain,    juce::Image::BitmapData::readOnly);
        const juce::Image::BitmapData b (labelled, juce::Image::BitmapData::readOnly);

        int differing = 0;

        for (int y = 0; y < plain.getHeight(); ++y)
            for (int x = 0; x < plain.getWidth(); ++x)
                if (a.getPixelColour (x, y) != b.getPixelColour (x, y))
                    ++differing;

        check (differing > 150,
               "render: note names are drawn on the bars",
               juce::String (differing) + " px differ with the labels on");

        //----------------------------------------------------------------------
        // ...AND THEY HAVE TO BE READABLE WHERE THEY LAND.
        //
        // A short lane makes the bars shorter than the text, so the label moves
        // to just ABOVE the bar — where the pixels are the background, not the
        // bar's fill. Taking the contrast from the fill in that case put white
        // text from a solid active bar onto pale paper: drawn, counted by the
        // delta check above, and completely invisible. The only way to catch
        // that is to measure the contrast of the pixels the labels actually
        // changed.
        //----------------------------------------------------------------------
        {
            const auto paper = vt::Theme::getPreset ("Sheet Music");

            theme->adoptValues (paper);

            vt::NoteHighwayComponent shortLane (*theme);
            shortLane.setBounds (0, 0, 1200, 150);
            shortLane.setChart (chart.get());
            shortLane.setClock (streamNow, chartNow);
            shortLane.tickAnimation (0.016);

            const auto labelled2 = renderFrame (shortLane, "highway-short-lane-labels");

            auto noNames2 = paper;
            noNames2.showNoteNames = false;

            theme->adoptValues (noNames2);
            shortLane.applyTheme();
            shortLane.tickAnimation (0.016);

            const auto plain2 = renderFrame (shortLane, "highway-short-lane-plain");

            const juce::Image::BitmapData c (plain2,    juce::Image::BitmapData::readOnly);
            const juce::Image::BitmapData d2 (labelled2, juce::Image::BitmapData::readOnly);

            // ONLY THE ACTIVE NOTE'S LABEL. Every other bar on screen is drawn
            // at upcomingBarAlpha, which blends to something pale on paper and
            // was already producing dark, readable text — averaged in with them,
            // one broken label out of six is comfortably inside any threshold
            // loose enough not to be flaky. It is the SOLID bar that breaks, so
            // that is the bar the check has to look at.
            const auto activeIndex = chart->noteIndexAt (chartNow);

            if (activeIndex < 0)
            {
                std::cout << "[SKIP] no note active at the test playhead" << std::endl;
            }
            else
            {
                const auto& active = chart->getNotes()[(size_t) activeIndex];

                const auto xForTime = [&] (double t)
                {
                    const auto span = paper.highwayHistorySeconds + paper.highwayLookaheadSeconds;
                    return (float) ((((t - chartNow) + paper.highwayHistorySeconds) / span) * 1200.0);
                };

                const auto from = juce::jmax (0, (int) xForTime (active.startSeconds));
                const auto to   = juce::jmin (1200, (int) xForTime (active.endSeconds()));

                int labelPixels = 0, legible = 0;

                for (int y = 0; y < plain2.getHeight(); ++y)
                    for (int x = from; x < to; ++x)
                    {
                        const auto before = c.getPixelColour (x, y);
                        const auto after  = d2.getPixelColour (x, y);

                        if (before == after)
                            continue;

                        ++labelPixels;

                        // Against what the label REPLACED, not against some
                        // nominal background — that is the pixel a reader sees
                        // it on.
                        if (std::abs (after.getBrightness() - before.getBrightness()) > 0.25f)
                            ++legible;
                    }

                // Half, not a higher bar: roughly a third of the pixels a glyph
                // touches are antialiasing fringe, which sits BETWEEN the text
                // colour and what is behind it by definition and can never pass
                // a contrast test. The separation is what matters and it is
                // wide — this reads ~70% with the fix and exactly 0% without.
                check (labelPixels > 20 && legible > labelPixels / 2,
                       "render: the active note's label stays legible",
                       juce::String (legible) + " of " + juce::String (labelPixels)
                         + " px contrast with what they cover");
            }
        }

        //----------------------------------------------------------------------
        // ...AND THEY HAVE TO BELONG TO THE BAR THEY NAME.
        //
        // At the size the app actually runs — one window's worth of highway
        // against one song's worth of range — the bar is around ten pixels tall,
        // which is thinner than the type. The label used to move ABOVE the bar
        // whenever that happened, and on a chart it always happens: a chart
        // spans a whole singer's range where an exercise covers a fifth, so it
        // zooms out further and the bar (a fixed +/- tFull cents) gets thinner.
        // Every name on screen then sat between its own note and the one above
        // it. The type shrinks to fit instead, so what this measures is where
        // the label's pixels LAND — inside the bar's own band.
        //----------------------------------------------------------------------
        {
            theme->adoptValues (baseValues);

            // A 960x540 window, less the toolbar, transport, lyrics and footer.
            constexpr int kW = 900, kH = 360;

            vt::NoteHighwayComponent appSized (*theme);
            appSized.setBounds (0, 0, kW, kH);
            appSized.setChart (chart.get());
            appSized.setClock (streamNow, chartNow);
            appSized.tickAnimation (0.016);

            const auto named = renderFrame (appSized, "highway-app-size-labels");

            auto noNames = baseValues;
            noNames.showNoteNames = false;

            theme->adoptValues (noNames);
            appSized.applyTheme();
            appSized.tickAnimation (0.016);

            const auto bare = renderFrame (appSized, "highway-app-size-plain");

            const auto activeIndex = chart->noteIndexAt (chartNow);

            if (activeIndex < 0)
            {
                std::cout << "[SKIP] no note active at the test playhead" << std::endl;
            }
            else
            {
                const auto& active = chart->getNotes()[(size_t) activeIndex];

                const auto low  = appSized.getViewLowMidi();
                const auto span = juce::jmax (1.0f, appSized.getViewHighMidi() - low);

                // The same three lines paintNotes() places the bar with. tFull is
                // 25 cents until setTolerances() says otherwise, and nothing here
                // says otherwise.
                const auto pxPerSemitone = (float) kH / span;
                const auto barHeight = juce::jmax (3.0f, 2.0f * (25.0f / 100.0f) * pxPerSemitone);
                const auto cy = (float) kH - (((float) active.midi - low) / span) * (float) kH;

                const auto xForTime = [&] (double t)
                {
                    const auto seconds = baseValues.highwayHistorySeconds + baseValues.highwayLookaheadSeconds;
                    return (float) ((((t - chartNow) + baseValues.highwayHistorySeconds) / seconds) * kW);
                };

                // Strictly inside the note, so the bar starting immediately after
                // this one cannot contribute its own label to the count.
                const auto from = juce::jmax (0,  (int) xForTime (active.startSeconds) + 2);
                const auto to   = juce::jmin (kW, (int) xForTime (active.endSeconds()) - 2);

                const juce::Image::BitmapData e (bare,  juce::Image::BitmapData::readOnly);
                const juce::Image::BitmapData f (named, juce::Image::BitmapData::readOnly);

                int labelPixels = 0, strayed = 0;

                for (int y = 0; y < kH; ++y)
                    for (int x = from; x < to; ++x)
                    {
                        if (e.getPixelColour (x, y) == f.getPixelColour (x, y))
                            continue;

                        ++labelPixels;

                        // A pixel of fringe either side of the fill is the type
                        // sitting on the bar, not the type leaving it.
                        if (std::abs ((float) y + 0.5f - cy) > barHeight * 0.5f + 1.5f)
                            ++strayed;
                    }

                check (labelPixels > 20 && strayed == 0,
                       "render: the note name stays inside its own bar",
                       juce::String (labelPixels) + " label px, " + juce::String (strayed)
                         + " outside a " + juce::String (barHeight, 1) + " px bar");
            }
        }

        theme->adoptValues (baseValues);
        highway.applyTheme();
    }

    //==========================================================================
    // THE SHEET-MUSIC STAFF.
    //
    // The load-bearing claim is not "some lines appear" — it is that they appear
    // at the RIGHT PITCHES. A staff drawn on evenly-spaced rows would look more
    // like notation and be a lie, because the Y axis is chromatic-linear so the
    // target bar's height can keep meaning +/- tFullCents. So: measure how much
    // ink lands on the row of each staff pitch against the rows either side of
    // it, which is a claim only the correct geometry can satisfy.
    //==========================================================================
    {
        // Park the playhead past the end so no notes, trace or dot are drawn and
        // the only thing in the frame is the background layer under test.
        highway.clearHistory();
        highway.setClock (streamNow, 20.0);

        for (int i = 0; i < 200; ++i)
            highway.tickAnimation (0.016);      // let the zoom settle

        const auto low = highway.getViewLowMidi();
        const auto high = highway.getViewHighMidi();

        const auto yForMidi = [&] (float midi)
        {
            const auto span = juce::jmax (1.0f, high - low);
            return 600.0f - ((midi - low) / span) * 600.0f;
        };

        auto staffValues = baseValues;
        staffValues.backgroundStyle = vt::BackgroundStyle::staff;

        theme->adoptValues (staffValues);
        highway.applyTheme();
        highway.tickAnimation (0.016);

        const auto staffImg = renderFrame (highway, "highway-staff-background");

        const auto rowInk = [&] (const juce::Image& img, float midi)
        {
            const auto y = juce::roundToInt (yForMidi (midi));

            if (y < 0 || y >= img.getHeight())
                return -1;

            const juce::Image::BitmapData bmp (img, juce::Image::BitmapData::readOnly);
            int ink = 0;

            // From past the clef gutter to the right edge.
            for (int x = 40; x < img.getWidth(); ++x)
            {
                const auto c = bmp.getPixelColour (x, y);

                if (std::abs ((int) c.getRed()   - (int) background.getRed())
                      + std::abs ((int) c.getGreen() - (int) background.getGreen())
                      + std::abs ((int) c.getBlue()  - (int) background.getBlue()) > 24)
                    ++ink;
            }

            return ink;
        };

        // In view for this chart (MIDI 55-64, so roughly 52-67): F3 and A3 of
        // the bass staff, E4 and G4 of the treble.
        const int staffPitches[] = { 53, 57, 64, 67 };
        const int spacePitches[] = { 55, 58, 62, 66 };

        int staffInk = 0, staffRows = 0;
        int spaceInk = 0, spaceRows = 0;

        for (const auto midi : staffPitches)
            if (const auto ink = rowInk (staffImg, (float) midi); ink >= 0) { staffInk += ink; ++staffRows; }

        for (const auto midi : spacePitches)
            if (const auto ink = rowInk (staffImg, (float) midi); ink >= 0) { spaceInk += ink; ++spaceRows; }

        if (staffRows == 0 || spaceRows == 0)
        {
            std::cout << "[SKIP] no staff and space pitches both in view" << std::endl;
        }
        else
        {
            check (staffInk / staffRows > 800 && spaceInk / spaceRows < 100,
                   "render: staff lines land on their own pitches",
                   juce::String (staffInk / staffRows) + " px on a staff line vs "
                     + juce::String (spaceInk / spaceRows) + " in a space");
        }

        // The clef markers name the line each clef is about. Without them the
        // staff is five lines with nothing to hang a pitch on.
        {
            const juce::Image::BitmapData bmp (staffImg, juce::Image::BitmapData::readOnly);
            int gutterInk = 0;

            for (int y = 0; y < staffImg.getHeight(); ++y)
                for (int x = 0; x < 18; ++x)
                {
                    const auto c = bmp.getPixelColour (x, y);

                    if (std::abs ((int) c.getRed()   - (int) background.getRed())
                          + std::abs ((int) c.getGreen() - (int) background.getGreen())
                          + std::abs ((int) c.getBlue()  - (int) background.getBlue()) > 24)
                        ++gutterInk;
                }

            check (gutterInk > 20,
                   "render: the clef markers are drawn",
                   juce::String (gutterInk) + " px in the clef gutter");
        }

        //----------------------------------------------------------------------
        // Every preset has to survive being drawn. A colour that came out equal
        // to its own background would be an invisible highway, and picking a
        // theme is the one action that can do that to every layer at once.
        //----------------------------------------------------------------------
        highway.setClock (streamNow, chartNow);

        for (const auto& name : vt::Theme::getPresetNames())
        {
            const auto preset = vt::Theme::getPreset (name);

            theme->adoptValues (preset);
            highway.applyTheme();
            highway.tickAnimation (0.016);

            const auto img2 = renderFrame (highway, "theme-" + name.toLowerCase().replace (" ", "-"));
            const auto s2 = analyse (img2, preset.background);

            check (s2.nonBackground > s2.total / 200,
                   "theme \"" + name + "\": the highway is not blank",
                   juce::String (100.0 * s2.nonBackground / juce::jmax (1, s2.total), 2) + "%");
        }

        theme->adoptValues (baseValues);
        highway.applyTheme();
        highway.setClock (streamNow, chartNow);
        highway.tickAnimation (0.016);
    }

    //==========================================================================
    // THE PIANO KEYBOARD.
    //
    // Its one job is that a key is at the same height as the pitch it names. So
    // the check is not "keys are drawn" but "clicking the pixel row the highway
    // would put note N on reports note N" — the failure this guards against is a
    // keyboard that looks perfect and is half a semitone out.
    //==========================================================================
    {
        vt::PianoKeyboardComponent keyboard (*theme);
        keyboard.setBounds (0, 0, 68, 600);
        keyboard.setViewRange (highway.getViewLowMidi(), highway.getViewHighMidi());
        keyboard.setTargetMidi (chart->getNotes().front().midi);
        keyboard.setSungMidi ((float) chart->getNotes().front().midi + 0.3f);

        const auto img2 = renderFrame (keyboard, "piano-keyboard");
        const auto s2 = analyse (img2, background);

        check (s2.nonBackground > s2.total / 4,
               "render: the keyboard draws its keys",
               juce::String (100.0 * s2.nonBackground / juce::jmax (1, s2.total), 1) + "% painted");

        // Black keys read as black keys — RELATIVE to the white ones, not
        // against an absolute brightness. The keys are drawn in the theme's own
        // colours, so on a dark theme a "white" key is a dark grey and a fixed
        // "brighter than 0.7" threshold would fail a keyboard that is perfectly
        // legible. What must hold in EVERY theme is the ordering: sample the
        // black-key strip on the centre row of each key in view and require the
        // darkest white key to still be lighter than the lightest black one.
        {
            const juce::Image::BitmapData bmp (img2, juce::Image::BitmapData::readOnly);

            const auto low  = highway.getViewLowMidi();
            const auto high = highway.getViewHighMidi();
            const auto span = juce::jmax (1.0f, high - low);

            float dimmestWhite = 1.0f, brightestBlack = 0.0f;
            int whites = 0, blacks = 0;

            for (int midi = (int) std::ceil (low) + 1; midi <= (int) std::floor (high) - 1; ++midi)
            {
                const auto y = 600.0f - (((float) midi - low) / span) * 600.0f;
                const auto b = bmp.getPixelColour (20, juce::roundToInt (y)).getBrightness();

                const auto pc = ((midi % 12) + 12) % 12;
                const bool isBlack = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);

                // The target and sung-pitch overlays recolour two rows; they are
                // not key colour and would poison the comparison.
                if (midi == chart->getNotes().front().midi)
                    continue;

                if (isBlack) { brightestBlack = juce::jmax (brightestBlack, b); ++blacks; }
                else         { dimmestWhite   = juce::jmin (dimmestWhite,   b); ++whites; }
            }

            check (whites > 3 && blacks > 2 && dimmestWhite > brightestBlack + 0.02f,
                   "render: white and black keys are distinguishable",
                   juce::String (whites) + " white (dimmest " + juce::String (dimmestWhite, 3)
                     + ") vs " + juce::String (blacks) + " black (brightest "
                     + juce::String (brightestBlack, 3) + ")");
        }

        //----------------------------------------------------- key alignment
        int reported = -1;
        keyboard.onKeyPressed = [&reported] (int midi) { reported = midi; };

        const auto low = keyboard.getBounds().isEmpty() ? 0.0f : highway.getViewLowMidi();
        const auto high = highway.getViewHighMidi();
        const auto span = juce::jmax (1.0f, high - low);

        const auto source = juce::Desktop::getInstance().getMainMouseSource();

        int mismatches = 0, tested = 0;

        for (int midi = (int) std::ceil (low) + 1; midi <= (int) std::floor (high) - 1; ++midi)
        {
            // Exactly where NoteHighwayComponent::yForMidi would put this note.
            const auto y = 600.0f - (((float) midi - low) / span) * 600.0f;

            reported = -1;

            const juce::Point<float> p { 30.0f, y };
            const juce::MouseEvent e (source, p, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &keyboard, &keyboard, juce::Time::getCurrentTime(),
                                      p, juce::Time::getCurrentTime(), 1, false);

            keyboard.mouseDown (e);
            keyboard.mouseUp (e);

            ++tested;

            if (reported != midi)
                ++mismatches;
        }

        check (tested > 4 && mismatches == 0,
               "keyboard: every key sits on its own pitch row",
               juce::String (tested) + " keys tested, " + juce::String (mismatches)
                 + " off by a row");
    }

    //==========================================================================
    // THE TRANSPORT STRIP.
    //==========================================================================
    {
        vt::TransportBarComponent bar (*theme);
        bar.setBounds (0, 0, 1200, 34);
        bar.setChart (chart.get());
        bar.setDurationSeconds (chart->getDurationSeconds());
        bar.setPosition (chart->getDurationSeconds() * 0.5);

        double seeked = -1.0;
        bar.onSeek = [&seeked] (double t) { seeked = t; };

        double loopFrom = -1.0, loopTo = -1.0;
        bar.onLoopChanged = [&] (double a2, double b2) { loopFrom = a2; loopTo = b2; };

        const auto img2 = renderFrame (bar, "transport-bar");
        const auto s2 = analyse (img2, theme->get().transportTrack);

        check (s2.nonBackground > 400,
               "render: the transport strip draws the song's shape",
               juce::String (s2.nonBackground) + " px of density, playhead and labels");

        //-------------------------------------------------------------- seek
        const auto source = juce::Desktop::getInstance().getMainMouseSource();

        const auto press = [&] (juce::Point<float> p)
        {
            return juce::MouseEvent (source, p, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &bar, &bar, juce::Time::getCurrentTime(),
                                     p, juce::Time::getCurrentTime(), 1, false);
        };

        // Three quarters of the way along the SCRUB area, well below the loop lane.
        {
            const juce::Point<float> p { 900.0f, 24.0f };
            bar.mouseDown (press (p));
            bar.mouseUp (press (p));
        }

        check (std::abs (seeked - chart->getDurationSeconds() * 0.75) < 0.05,
               "transport: clicking the strip seeks to that point",
               juce::String (seeked, 2) + " s of " + juce::String (chart->getDurationSeconds(), 2));

        //-------------------------------------------------------------- loop
        // A drag along the thin lane at the top marks a section.
        {
            const juce::Point<float> from { 300.0f, 4.0f };
            const juce::Point<float> to   { 700.0f, 4.0f };

            bar.mouseDown (press (from));

            const juce::MouseEvent drag (source, to, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                         &bar, &bar, juce::Time::getCurrentTime(),
                                         from, juce::Time::getCurrentTime(), 1, true);

            bar.mouseDrag (drag);
            bar.mouseUp (drag);
        }

        check (bar.hasLoopRegion() && bar.isLoopEnabled()
                 && std::abs (loopFrom - chart->getDurationSeconds() * 0.25) < 0.05
                 && std::abs (loopTo   - chart->getDurationSeconds() * 0.583) < 0.10,
               "transport: dragging the loop lane marks a section",
               juce::String (loopFrom, 2) + " s -> " + juce::String (loopTo, 2) + " s");

        const auto loopedImg = renderFrame (bar, "transport-bar-looping");

        // The armed region has to be VISIBLE, or the singer has no way to know
        // why the song keeps jumping backwards.
        {
            const juce::Image::BitmapData plainBmp (img2, juce::Image::BitmapData::readOnly);
            const juce::Image::BitmapData loopBmp (loopedImg, juce::Image::BitmapData::readOnly);

            int differing = 0;

            for (int y = 0; y < img2.getHeight(); ++y)
                for (int x = 300; x < 700; ++x)
                    if (plainBmp.getPixelColour (x, y) != loopBmp.getPixelColour (x, y))
                        ++differing;

            check (differing > 1000,
                   "render: the armed loop region is visible",
                   juce::String (differing) + " px changed inside the region");
        }

        // ...and a plain click in the lane clears it. The only destructive
        // gesture in the strip, deliberately not the one used for seeking.
        {
            const juce::Point<float> p { 500.0f, 4.0f };
            bar.mouseDown (press (p));
            bar.mouseUp (press (p));
        }

        check (! bar.hasLoopRegion() && ! bar.isLoopEnabled(),
               "transport: clicking the loop lane clears the loop",
               "the only destructive gesture in the strip");
    }

    //==========================================================================
    // THEME PRESETS round-trip through theme.json's own vocabulary.
    //==========================================================================
    {
        int badStyles = 0;

        for (const auto style : { vt::BackgroundStyle::chromatic, vt::BackgroundStyle::staff,
                                  vt::BackgroundStyle::staffAndGrid, vt::BackgroundStyle::plain })
            if (vt::backgroundStyleFromString (vt::backgroundStyleToString (style)) != style)
                ++badStyles;

        check (badStyles == 0,
               "theme: background styles survive a round trip",
               "written to theme.json as a name and read back");

        // An unknown name must land on the default rather than on whatever the
        // enum's zero happens to be next time someone reorders it.
        check (vt::backgroundStyleFromString ("nonsense") == vt::BackgroundStyle::chromatic,
               "theme: an unknown background style falls back",
               "a typo in theme.json must not blank the highway");

        int distinct = 0;
        juce::Array<juce::uint32> seen;

        for (const auto& name : vt::Theme::getPresetNames())
        {
            const auto argb = vt::Theme::getPreset (name).background.getARGB();

            if (! seen.contains (argb))
            {
                seen.add (argb);
                ++distinct;
            }
        }

        check (distinct >= 3,
               "theme: the presets are actually different themes",
               juce::String (distinct) + " distinct backgrounds across "
                 + juce::String (vt::Theme::getPresetNames().size()) + " presets");
    }

    //==========================================================================
    // With no chart, the empty-state message must still be legible.
    //==========================================================================
    highway.setChart (nullptr);
    highway.tickAnimation (0.016);

    juce::Image emptyImg (juce::Image::ARGB, 1200, 600, true);
    {
        juce::Graphics g (emptyImg);
        highway.paintEntireComponent (g, false);
    }

    const auto emptyStats = analyse (emptyImg, background);

    check (emptyStats.nonBackground > 200,
           "render: empty state still draws something",
           pct (emptyStats.nonBackground));

    //==========================================================================
    // M3.5 - the import overlay.
    //
    // It is the only thing on screen for the several MINUTES a song takes to
    // prepare, and it is shown from a callback rather than by a user gesture,
    // so nobody clicks their way to it during normal development. If it ever
    // renders blank the app looks frozen at exactly the moment the user is
    // least sure anything is happening.
    //==========================================================================
    {
        vt::ImportOverlayComponent overlay (*theme);
        overlay.setSize (1200, 600);
        overlay.beginImport ("Mr Brightside.mp3");
        overlay.setProgress (0.42, "Separating the vocal from the backing");

        juce::Image overlayImg (juce::Image::ARGB, 1200, 600, true);
        {
            juce::Graphics g (overlayImg);
            overlay.paintEntireComponent (g, false);
        }

        // Coverage is measured in ALPHA, not in colour distance. The scrim and
        // the panel are both deliberately close to the background colour, so
        // "differs from the background" says nothing here - but a region the
        // overlay never painted is still fully transparent.
        const auto opaqueFraction = [] (const juce::Image& img)
        {
            const juce::Image::BitmapData bmp (img, juce::Image::BitmapData::readOnly);
            int opaque = 0;

            for (int y = 0; y < img.getHeight(); ++y)
                for (int x = 0; x < img.getWidth(); ++x)
                    if (bmp.getPixelColour (x, y).getAlpha() > 200)
                        ++opaque;

            return (double) opaque / (double) (img.getWidth() * img.getHeight());
        };

        const auto coverage = opaqueFraction (overlayImg);

        check (coverage > 0.99,
               "render: import overlay covers the highway",
               juce::String (coverage * 100.0, 1) + "% of pixels painted");

        // Text and the progress bar sit well above the panel. A blank scrim
        // alone would pass the coverage check.
        check (analyse (overlayImg, background).brightest > 200,
               "render: import overlay draws its panel and text",
               "brightest " + juce::String (analyse (overlayImg, background).brightest));

        overlay.showFailure ("no separation environment found",
                             "Traceback (most recent call last):\n  ...");

        juce::Image failImg (juce::Image::ARGB, 1200, 600, true);
        {
            juce::Graphics g (failImg);
            overlay.paintEntireComponent (g, false);
        }

        check (opaqueFraction (failImg) > 0.99
                 && analyse (failImg, background).brightest > 200,
               "render: import failure state stays legible",
               "the reason must not vanish before it is read");
    }

    //==========================================================================
    // Finding the venv. The importer walks up from the executable looking for
    // .venv/Scripts/python.exe; this test binary sits at the same depth as the
    // Standalone, so if it resolves here it resolves there.
    //==========================================================================
    {
        const auto python = vt::SongImporter::findInterpreter();

        check (python.existsAsFile(),
               "import: the vocalchart interpreter is discoverable",
               python == juce::File() ? juce::String ("NOT FOUND - walked up from ")
                                          + juce::File::getSpecialLocation (
                                              juce::File::currentExecutableFile).getFullPathName()
                                      : python.getFullPathName());

        if (python.existsAsFile())
        {
            juce::String pythonError;
            check (vt::SongImporter::checkInterpreter (python, pythonError),
                   "import: 'vocalchart' is installed in it",
                   pythonError.isEmpty() ? juce::String ("import vocalchart succeeded")
                                         : pythonError);
        }
    }

    //==========================================================================
    // Finding the SONG LIBRARY, by the same walk-up-from-the-executable trick.
    //
    // This is the whole substance of "the Load chart button opens in the song
    // folder without a hardcoded address": if the detection breaks, the chooser
    // silently falls back to %APPDATA% and the user has to navigate to their
    // songs every single time. That failure is invisible in a build log and
    // annoying forever, which is exactly what a test is for.
    //==========================================================================
    {
        const auto songs = vt::AppSettings::detectRepoSongsDirectory();

        check (songs.isDirectory(),
               "library: the repo's songs/ folder is discovered from the exe",
               songs == juce::File() ? juce::String ("NOT FOUND - walked up from ")
                                         + juce::File::getSpecialLocation (
                                             juce::File::currentExecutableFile).getFullPathName()
                                     : songs.getFullPathName());

        check (songs == root.getChildFile ("songs"),
               "library: and it is the right one",
               songs.getFullPathName());

        // The documented contract of getSongLibraryRoot() is that it always
        // returns a directory that exists, whichever of the three branches wins.
        juce::SharedResourcePointer<vt::AppSettings> settings;

        check (settings->getSongLibraryRoot().isDirectory(),
               "library: the resolved root always exists",
               settings->getSongLibraryRoot().getFullPathName());

        check (settings->getLastChartDirectory().isDirectory(),
               "library: the chooser's start directory always exists",
               settings->getLastChartDirectory().getFullPathName());
    }

    std::cout << "------------------------------------" << std::endl;

    if (failures == 0)
        std::cout << "ALL " << checks << " RENDER CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " of " << checks << " RENDER CHECKS FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
