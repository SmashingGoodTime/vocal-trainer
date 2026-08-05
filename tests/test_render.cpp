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

    ==========================================================================
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

#include "chart/Chart.h"
#include "import/SongImporter.h"
#include "ui/ImportOverlayComponent.h"
#include "ui/NoteHighwayComponent.h"
#include "ui/Theme.h"

namespace
{
    int failures = 0, checks = 0;

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

    std::cout << "------------------------------------" << std::endl;

    if (failures == 0)
        std::cout << "ALL " << checks << " RENDER CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " of " << checks << " RENDER CHECKS FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
