/*
    ==========================================================================

    LyricStripComponent.h — the fixed lyric line under the highway.

    THE LYRICS DO NOT SCROLL WITH THE NOTES. DESIGN.md section 7:

        "Use Rock Band's model: the note cue moves toward the now-line while
         the lyrics sit in a FIXED position, highlighting as the cue crosses.
         This is far more readable and decouples lyric legibility from scroll
         speed."

    Text that slides sideways is much harder to read than text that stays put
    and lights up. Harmonix patented the fixed-block form (US 8,449,360 B2) and
    YARG implements it.

    A WINDOW, NOT A PHRASE. The design says show the current phrase, but phrases
    here are derived from 600 ms silence gaps and come out ~17 s long on real
    songs — far too much text for one line. So this shows a sliding window of
    syllables centred on the note being sung, which reads well at any tempo.

    ==========================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

#include "Theme.h"
#include "../chart/Chart.h"

namespace vt
{

class LyricStripComponent : public juce::Component
{
public:
    explicit LyricStripComponent (Theme& themeToUse);

    /** Non-owning; the editor owns the chart and outlives this. */
    void setChart (const Chart* newChart);

    /** Chart-time position of the now-line. */
    void setNowTime (double seconds) noexcept;

    /** True when the words came from ASR rather than a known lyrics file. They
        are then rendered in italic with a badge — never present auto-transcribed
        lyrics as authoritative, or the singer practises the wrong words. */
    void setAutoTranscribed (bool isAsr) noexcept { autoTranscribed = isAsr; }

    bool hasAnyLyrics() const noexcept { return anyLyrics; }

    void applyTheme();

    void paint (juce::Graphics&) override;

private:
    /** How many syllables of context to show either side of the current one. */
    static constexpr int kBefore = 4;
    static constexpr int kAfter  = 10;

    void rebuildWindow();

    Theme& theme;
    const Chart* chart = nullptr;

    double nowTime = 0.0;
    bool   autoTranscribed = false;
    bool   anyLyrics = false;

    /** Indices into chart->getNotes() that carry text, cached once per chart so
        the paint path never scans the whole song. */
    std::vector<int> textNotes;

    int lastActiveIndex = -1;   // index INTO textNotes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LyricStripComponent)
};

} // namespace vt
