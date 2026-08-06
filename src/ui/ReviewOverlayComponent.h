/*
    ==========================================================================

    ReviewOverlayComponent.h — what the take actually told us.

    DESIGN.md section 6: "Post-take review is the primary learning surface. The
    live highway is the practice vehicle; the review screen is the teacher."

    That is not a slogan, it is the evidence. Concurrent visual feedback
    demonstrably improves beginners and demonstrably DEGRADES trained singers
    through cognitive load, and always-on feedback breeds dependency (67%
    feedback frequency outperformed 100% on both post-test and retention). The
    conclusion the design draws is that the interesting information belongs
    AFTER the take, where it costs the performance nothing.

    WHAT THIS SCREEN IS AND IS NOT
    ------------------------------
    It is not a scoreboard. There is no letter grade, no combo, no stars. The
    design is explicit that the live view carries "no score number, no combo,
    no streak, no particles", and putting one here instead would just move the
    gamification rather than avoid it.

    What it shows instead is the four things a voice teacher would actually say
    out loud:

      1. a sentence naming the ONE most actionable thing (systematic flatness
         beats a percentage every time — it is a habit, not a scoreboard);
      2. the cents-error histogram, because the SHAPE of the distribution shows
         systematic error at a glance where no single number does;
      3. which phrases went worst, in practice order, each one click from being
         looped and retried;
      4. which INTERVAL is weakest — "consistently flat on ascending 6ths" is
         something you can go and practise.

    Rows are hit-tested rather than being real Buttons: the list length depends
    on the chart, and a dozen child components that are created and destroyed
    every time a song ends is more moving parts than a rectangle and a
    mouseDown.

    ==========================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

#include "Theme.h"
#include "../score/TakeScorer.h"

namespace vt
{

class ReviewOverlayComponent final : public juce::Component
{
public:
    explicit ReviewOverlayComponent (Theme&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** Shows the review for a finished take. */
    void show (const TakeSummary&, const juce::String& songTitle);

    /** Enables the Listen back button and names the take. */
    void setTake (const juce::File& takeFile, bool hadDropout);

    void applyTheme();

    /** Chart seconds to seek to, for one-click loop-and-retry on a phrase. */
    std::function<void (double)> onRetryPhrase;

    std::function<void()> onClose;
    std::function<void()> onListenBack;

private:
    void paintHeadline (juce::Graphics&, juce::Rectangle<int>);
    void paintStats (juce::Graphics&, juce::Rectangle<int>);
    void paintHistogram (juce::Graphics&, juce::Rectangle<int>);
    void paintPhrases (juce::Graphics&, juce::Rectangle<int>);
    void paintIntervals (juce::Graphics&, juce::Rectangle<int>);
    void paintObservations (juce::Graphics&, juce::Rectangle<int>);

    /** The colour for an accuracy, on the same Oklab ramp the highway uses, so
        a green bar there and a green number here mean the same thing. */
    juce::Colour colourForAccuracy (float zeroToOne) const;

    static constexpr int kMaxPhraseRows   = 6;
    static constexpr int kMaxIntervalRows = 4;

    Theme& theme;

    TakeSummary summary;
    juce::String title;
    juce::StringArray observationLines;

    juce::File takeFile;
    bool takeHadDropout = false;

    juce::TextButton closeButton { "Back to the song" };
    juce::TextButton listenButton { "Listen back" };

    /** Clickable phrase rows, rebuilt on every layout. */
    struct PhraseRow
    {
        juce::Rectangle<int> bounds;
        double startSeconds = 0.0;
    };

    std::vector<PhraseRow> phraseRows;
    int hoveredRow = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReviewOverlayComponent)
};

} // namespace vt
