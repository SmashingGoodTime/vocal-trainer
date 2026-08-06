#include "ReviewOverlayComponent.h"

#include <cmath>

namespace vt
{

namespace
{
    constexpr int kPad = 18;
    constexpr int kRowHeight = 22;
    constexpr int kSectionGap = 14;

    juce::String pct (float zeroToOne)
    {
        return juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, zeroToOne) * 100.0f)) + "%";
    }

    juce::String signedCents (float cents)
    {
        const auto r = juce::roundToInt (cents);
        return juce::String (r > 0 ? "+" : "") + juce::String (r) + juce::String::fromUTF8 ("\xc2\xa2");
    }
}

//==============================================================================
ReviewOverlayComponent::ReviewOverlayComponent (Theme& t)
    : theme (t)
{
    closeButton.onClick = [this] { if (onClose != nullptr) onClose(); };
    addAndMakeVisible (closeButton);

    listenButton.setTooltip ("Hear the take you just sang, against the backing track. "
                             "The numbers here are an interpretation; the recording is "
                             "what actually came out.");
    listenButton.onClick = [this] { if (onListenBack != nullptr) onListenBack(); };
    addChildComponent (listenButton);

    // The overlay swallows clicks so nothing underneath it can be operated while
    // it is up, but its own children still receive them.
    setInterceptsMouseClicks (true, true);
    setOpaque (false);
}

void ReviewOverlayComponent::applyTheme()
{
    const auto& v = theme.get();

    for (auto* b : { &closeButton, &listenButton })
    {
        b->setColour (juce::TextButton::buttonColourId, v.panel);
        b->setColour (juce::TextButton::textColourOffId, v.text);
    }

    repaint();
}

//==============================================================================
void ReviewOverlayComponent::show (const TakeSummary& newSummary, const juce::String& songTitle)
{
    summary = newSummary;
    title   = songTitle;
    observationLines = summary.observations();

    hoveredRow = -1;

    setVisible (true);
    toFront (false);
    resized();
    repaint();
}

void ReviewOverlayComponent::setTake (const juce::File& file, bool hadDropout)
{
    takeFile = file;
    takeHadDropout = hadDropout;

    listenButton.setVisible (takeFile.existsAsFile());
    resized();
    repaint();
}

//==============================================================================
void ReviewOverlayComponent::resized()
{
    auto bounds = getLocalBounds().reduced (kPad);

    auto buttonRow = bounds.removeFromBottom (30);
    closeButton.setBounds (buttonRow.removeFromRight (140));

    if (listenButton.isVisible())
    {
        buttonRow.removeFromRight (8);
        listenButton.setBounds (buttonRow.removeFromRight (120));
    }
}

//==============================================================================
juce::Colour ReviewOverlayComponent::colourForAccuracy (float zeroToOne) const
{
    const auto& v = theme.get();

    // 1 - accuracy, so 0 maps to "good": the same direction as the highway's
    // cents ramp, which goes from in-tune toward out-of-tune.
    return lerpOklab (v.noteScoredGood, v.noteScoredPoor,
                      juce::jlimit (0.0f, 1.0f, 1.0f - zeroToOne));
}

//==============================================================================
void ReviewOverlayComponent::paint (juce::Graphics& g)
{
    const auto& v = theme.get();

    // Nearly opaque rather than fully: the highway staying faintly visible
    // behind the review keeps it obvious that this is a layer over the song and
    // not a different screen you have to find your way back from.
    g.fillAll (v.background.withAlpha (0.94f));

    auto bounds = getLocalBounds().reduced (kPad);
    bounds.removeFromBottom (30 + kSectionGap);          // the button row

    //--------------------------------------------------------------- title
    g.setColour (v.textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (title.isNotEmpty() ? "Take review  -  " + title : "Take review",
                bounds.removeFromTop (18), juce::Justification::centredLeft, true);

    bounds.removeFromTop (6);

    if (! summary.hasData)
    {
        g.setColour (v.text);
        g.setFont (juce::FontOptions (16.0f));
        g.drawFittedText (summary.headline(), bounds.reduced (0, 20),
                          juce::Justification::centredTop, 4);
        return;
    }

    //-------------------------------------------------------------- headline
    paintHeadline (g, bounds.removeFromTop (66));
    bounds.removeFromTop (kSectionGap);

    paintStats (g, bounds.removeFromTop (44));
    bounds.removeFromTop (kSectionGap);

    //-------------------------------------------------------- two columns
    // Left is diagnosis in words, right is diagnosis in numbers. Both fit at
    // the 640 px minimum window width because each column is given half.
    auto left  = bounds.removeFromLeft (bounds.getWidth() / 2 - kSectionGap / 2);
    bounds.removeFromLeft (kSectionGap);
    auto right = bounds;

    paintHistogram (g, left.removeFromTop (juce::jmin (120, left.getHeight() / 2)));
    left.removeFromTop (kSectionGap);
    paintObservations (g, left);

    phraseRows.clear();

    const auto phraseHeight = juce::jmin (right.getHeight() * 3 / 5,
                                          (kMaxPhraseRows + 2) * kRowHeight);
    paintPhrases (g, right.removeFromTop (phraseHeight));
    right.removeFromTop (kSectionGap);
    paintIntervals (g, right);
}

//==============================================================================
void ReviewOverlayComponent::paintHeadline (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto& v = theme.get();

    g.setColour (v.text);
    g.setFont (juce::FontOptions (16.0f));
    g.drawFittedText (summary.headline(), area, juce::Justification::topLeft, 3, 1.0f);
}

//==============================================================================
void ReviewOverlayComponent::paintStats (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto& v = theme.get();

    struct Stat { juce::String value; juce::String label; juce::Colour colour; };

    std::vector<Stat> stats;

    stats.push_back ({ pct (summary.percentInTune), "in tune",
                       colourForAccuracy (summary.percentInTune) });

    stats.push_back ({ signedCents (summary.meanCents), "average error",
                       std::abs (summary.meanCents) <= 20.0f ? v.noteScoredGood
                                                             : colourForAccuracy (0.4f) });

    stats.push_back ({ juce::String (summary.notesGood) + "/" + juce::String (summary.notesTotal),
                       "notes solid", v.text });

    if (summary.onsetSamples >= 4)
    {
        const auto ms = juce::roundToInt (summary.meanOnsetMs);
        stats.push_back ({ juce::String (ms > 0 ? "+" : "") + juce::String (ms) + " ms",
                           ms > 0 ? "late" : "early",
                           std::abs (summary.meanOnsetMs) <= 80.0f ? v.noteScoredGood
                                                                  : colourForAccuracy (0.4f) });
    }

    if (summary.coverage < 0.95f)
        stats.push_back ({ pct (summary.coverage), "of the chart sung", v.textDim });

    const auto columnWidth = area.getWidth() / juce::jmax (1, (int) stats.size());

    for (size_t i = 0; i < stats.size(); ++i)
    {
        auto cell = area.removeFromLeft (columnWidth);

        g.setColour (stats[i].colour);
        g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
        g.drawText (stats[i].value, cell.removeFromTop (28),
                    juce::Justification::centredLeft, false);

        g.setColour (v.textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (stats[i].label, cell, juce::Justification::topLeft, false);
    }
}

//==============================================================================
void ReviewOverlayComponent::paintHistogram (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto& v = theme.get();

    g.setColour (v.textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("Where your pitch actually sat", area.removeFromTop (16),
                juce::Justification::topLeft, false);

    auto plot = area.reduced (0, 4).toFloat();
    auto labelRow = plot.removeFromBottom (13.0f);

    if (plot.getHeight() < 12.0f || summary.histogramPeak <= 0.0f)
        return;

    //-------------------------------------------------------------------------
    // Shade the full-credit window behind the bars. It gives the histogram a
    // reference: bars inside the shaded band were in tune, which is a far
    // quicker read than comparing bar positions to an axis label.
    //-------------------------------------------------------------------------
    const auto xForCents = [&plot] (float cents)
    {
        return plot.getX() + plot.getWidth() * (juce::jlimit (-175.0f, 175.0f, cents) + 175.0f) / 350.0f;
    };

    g.setColour (v.noteScoredGood.withAlpha (0.12f));
    g.fillRect (juce::Rectangle<float> (xForCents (-theme.get().tFullCents), plot.getY(),
                                        xForCents (theme.get().tFullCents)
                                          - xForCents (-theme.get().tFullCents),
                                        plot.getHeight()));

    const auto barWidth = plot.getWidth() / (float) TakeSummary::kHistogramBins;

    for (int bin = 0; bin < TakeSummary::kHistogramBins; ++bin)
    {
        const auto value = summary.centsHistogram[(size_t) bin] / summary.histogramPeak;

        if (value <= 0.0f)
            continue;

        const auto height = juce::jmax (1.0f, value * plot.getHeight());
        const auto x = plot.getX() + barWidth * (float) bin;
        const auto centre = TakeSummary::histogramBinCentreCents (bin);

        // Coloured by how far out of tune that bin is, so a lopsided
        // distribution is visible in hue as well as in position — the
        // redundant encoding DESIGN.md section 7 asks for.
        const auto t = juce::jlimit (0.0f, 1.0f,
                                     (std::abs (centre) - theme.get().tFullCents)
                                       / juce::jmax (1.0f, theme.get().tZeroCents
                                                             - theme.get().tFullCents));

        g.setColour (lerpOklab (v.noteScoredGood, v.noteScoredPoor, t));
        g.fillRect (juce::Rectangle<float> (x + 1.0f, plot.getBottom() - height,
                                            juce::jmax (1.0f, barWidth - 2.0f), height));
    }

    // Dead-centre line: "0 cents" is the only landmark that matters.
    g.setColour (v.nowLine);
    g.fillRect (juce::Rectangle<float> (xForCents (0.0f) - 0.5f, plot.getY(), 1.0f, plot.getHeight()));

    g.setColour (v.gridLabel);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("flat", labelRow, juce::Justification::centredLeft, false);
    g.drawText ("in tune", labelRow, juce::Justification::centred, false);
    g.drawText ("sharp", labelRow, juce::Justification::centredRight, false);
}

//==============================================================================
void ReviewOverlayComponent::paintObservations (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto& v = theme.get();

    if (observationLines.isEmpty() || area.getHeight() < 20)
        return;

    g.setColour (v.textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("Also worth knowing", area.removeFromTop (16),
                juce::Justification::topLeft, false);

    g.setColour (v.text);
    g.setFont (juce::FontOptions (12.0f));

    for (const auto& line : observationLines)
    {
        if (area.getHeight() < 16)
            break;

        // Two lines allowed per observation; they are written to fit.
        const auto rowHeight = juce::jmin (area.getHeight(), 32);
        g.drawFittedText (juce::String::fromUTF8 ("\xe2\x80\xa2  ") + line,
                          area.removeFromTop (rowHeight),
                          juce::Justification::topLeft, 2, 1.0f);
        area.removeFromTop (4);
    }
}

//==============================================================================
void ReviewOverlayComponent::paintPhrases (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto& v = theme.get();

    if (summary.phrases.empty() || area.getHeight() < 34)
        return;

    g.setColour (v.textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("Weakest phrases first  -  click one to loop it",
                area.removeFromTop (16), juce::Justification::topLeft, false);

    int shown = 0;

    for (const auto& phrase : summary.phrases)
    {
        if (shown >= kMaxPhraseRows || area.getHeight() < kRowHeight)
            break;

        // Nothing to learn from a phrase that scored well, and listing them all
        // buries the ones that matter.
        if (phrase.notesAttempted == 0 || phrase.percentHit > 0.9f)
            continue;

        auto row = area.removeFromTop (kRowHeight);

        phraseRows.push_back ({ row, phrase.startSeconds });

        if ((int) phraseRows.size() - 1 == hoveredRow)
        {
            g.setColour (v.panel);
            g.fillRect (row);
        }

        auto scoreCell = row.removeFromRight (52);
        auto centsCell = row.removeFromRight (52);

        // The phrase's own words are the label wherever there are any: "how
        // sweet the sound" locates a phrase in a way "phrase 14" cannot.
        const auto label = phrase.text.isNotEmpty()
                             ? phrase.text
                             : "phrase " + juce::String (phrase.phraseIndex + 1);

        g.setColour (phrase.pass ? v.text : v.noteScoredPoor.brighter (0.2f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (label, row.reduced (2, 0), juce::Justification::centredLeft, true);

        g.setColour (v.textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (signedCents (phrase.meanCents), centsCell,
                    juce::Justification::centredRight, false);

        g.setColour (colourForAccuracy (phrase.percentHit));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (pct (phrase.percentHit), scoreCell,
                    juce::Justification::centredRight, false);

        ++shown;
    }

    if (shown == 0)
    {
        g.setColour (v.noteScoredGood);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Every phrase you sang came in above the pass mark.",
                    area.removeFromTop (kRowHeight), juce::Justification::centredLeft, true);
    }
}

//==============================================================================
void ReviewOverlayComponent::paintIntervals (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto& v = theme.get();

    if (summary.intervalErrors.empty() || area.getHeight() < 34)
        return;

    g.setColour (v.textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("Errors by interval leapt into", area.removeFromTop (16),
                juce::Justification::topLeft, false);

    int shown = 0;

    for (const auto& e : summary.intervalErrors)
    {
        if (shown >= kMaxIntervalRows || area.getHeight() < kRowHeight)
            break;

        auto row = area.removeFromTop (kRowHeight);

        auto errorCell = row.removeFromRight (60);
        auto countCell = row.removeFromRight (44);

        g.setColour (v.text);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (e.describe(), row.reduced (2, 0), juce::Justification::centredLeft, true);

        g.setColour (v.textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (e.count) + juce::String::fromUTF8 ("\xc3\x97"), countCell,
                    juce::Justification::centredRight, false);

        g.setColour (e.meanAbsCents <= theme.get().tFullCents ? v.noteScoredGood
                                                              : v.noteScoredPoor.brighter (0.2f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (signedCents (e.meanCents), errorCell,
                    juce::Justification::centredRight, false);

        ++shown;
    }
}

//==============================================================================
void ReviewOverlayComponent::mouseDown (const juce::MouseEvent& e)
{
    for (const auto& row : phraseRows)
        if (row.bounds.contains (e.getPosition()))
        {
            if (onRetryPhrase != nullptr)
                onRetryPhrase (row.startSeconds);

            return;
        }
}

void ReviewOverlayComponent::mouseMove (const juce::MouseEvent& e)
{
    int found = -1;

    for (size_t i = 0; i < phraseRows.size(); ++i)
        if (phraseRows[i].bounds.contains (e.getPosition()))
        {
            found = (int) i;
            break;
        }

    if (found != hoveredRow)
    {
        hoveredRow = found;
        setMouseCursor (found >= 0 ? juce::MouseCursor::PointingHandCursor
                                   : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void ReviewOverlayComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRow != -1)
    {
        hoveredRow = -1;
        repaint();
    }
}

} // namespace vt
