#include "LyricStripComponent.h"

#include <algorithm>

namespace vt
{

LyricStripComponent::LyricStripComponent (Theme& t)
    : theme (t)
{
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
}

void LyricStripComponent::applyTheme()
{
    repaint();
}

void LyricStripComponent::setChart (const Chart* c)
{
    chart = c;
    textNotes.clear();
    lastActiveIndex = -1;
    anyLyrics = false;

    if (chart != nullptr)
    {
        const auto& notes = chart->getNotes();
        textNotes.reserve (notes.size());

        for (size_t i = 0; i < notes.size(); ++i)
            if (notes[i].syllable.isNotEmpty())
                textNotes.push_back ((int) i);

        anyLyrics = ! textNotes.empty();
    }

    repaint();
}

void LyricStripComponent::setNowTime (double seconds) noexcept
{
    nowTime = seconds;
}

//==============================================================================
void LyricStripComponent::paint (juce::Graphics& g)
{
    if (chart == nullptr || textNotes.empty())
        return;

    const auto& v = theme.get();
    const auto& notes = chart->getNotes();
    const auto area = getLocalBounds().toFloat().reduced (12.0f, 2.0f);

    // Which text-bearing note is being sung? The last one whose start has
    // passed. Binary search over the cached index list, not a scan of the song.
    const auto it = std::upper_bound (textNotes.begin(), textNotes.end(), nowTime,
                                      [&notes] (double t, int idx)
                                      { return t < notes[(size_t) idx].startSeconds; });

    const int active = (int) std::distance (textNotes.begin(), it) - 1;

    const int firstIdx = juce::jmax (0, active - kBefore);
    const int lastIdx  = juce::jmin ((int) textNotes.size() - 1,
                                     juce::jmax (active, 0) + kAfter);

    if (lastIdx < firstIdx)
        return;

    //--------------------------------------------------------------- layout
    const auto fontHeight = juce::jlimit (13.0f, 30.0f, area.getHeight() * 0.62f);

    // Braces, not parentheses. `juce::Font font (juce::FontOptions (h));` is a
    // function declaration, not a variable — the most vexing parse.
    juce::Font font { juce::FontOptions (fontHeight) };

    if (autoTranscribed)
        font = font.withStyle (juce::Font::italic);

    g.setFont (font);

    // Measure first so the ACTIVE syllable can be centred. A block that jumps
    // around as words change length is unreadable; anchoring the sung syllable
    // keeps the eye still.
    float widthBeforeActive = 0.0f, totalWidth = 0.0f;
    const auto spaceWidth = juce::GlyphArrangement::getStringWidth (font, " ");

    for (int i = firstIdx; i <= lastIdx; ++i)
    {
        const auto& n = notes[(size_t) textNotes[(size_t) i]];
        const auto w = juce::GlyphArrangement::getStringWidth (font, n.syllable) + spaceWidth;

        if (i < active)
            widthBeforeActive += w;

        totalWidth += w;
    }

    const auto anchorX = area.getX() + area.getWidth() * 0.32f;
    auto x = anchorX - widthBeforeActive;

    // Never leave dead space at the left edge when the line is short.
    x = juce::jmin (x, area.getX());
    x = juce::jmax (x, area.getRight() - totalWidth);
    x = juce::jmin (x, area.getX() + area.getWidth() * 0.32f);

    const auto baselineY = area.getCentreY();

    //---------------------------------------------------------------- draw
    for (int i = firstIdx; i <= lastIdx; ++i)
    {
        const auto& n = notes[(size_t) textNotes[(size_t) i]];
        const auto w = juce::GlyphArrangement::getStringWidth (font, n.syllable);

        juce::Colour colour = v.lyricPending;

        if (i < active)
            colour = v.lyricSung.withAlpha (0.45f);      // already sung, receding
        else if (i == active)
            colour = v.lyricActiveWipe;                  // being sung now
        else if (i == active + 1)
            colour = v.lyricPending.brighter (0.35f);    // up next

        g.setColour (colour);

        const juce::Rectangle<float> box (x, baselineY - fontHeight * 0.75f,
                                          w, fontHeight * 1.5f);

        g.drawText (n.syllable, box, juce::Justification::centred, false);

        // Underline the active syllable: colour alone is not enough, and this
        // survives every form of colour blindness (DESIGN.md section 7 —
        // encode through geometry first, colour last).
        if (i == active)
        {
            g.setColour (v.lyricActiveWipe.withAlpha (0.85f));
            g.fillRect (x, baselineY + fontHeight * 0.62f, w, 2.0f);
        }

        x += w + spaceWidth;

        if (x > area.getRight())
            break;
    }

    //-------------------------------------------------- auto-transcribed badge
    if (autoTranscribed)
    {
        g.setColour (v.textDim.withAlpha (0.75f));
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("auto-transcribed",
                    juce::Rectangle<float> (area.getRight() - 108.0f, area.getY(),
                                            108.0f, area.getHeight()),
                    juce::Justification::centredRight, false);
    }
}

} // namespace vt
