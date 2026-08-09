/*
    ==========================================================================

    TransportBarComponent.h — the whole song on one strip: where you are, where
    you can go, and which section you are drilling.

    WHY THE WHOLE SONG AND NOT JUST A SLIDER
    ----------------------------------------
    Practice is not listening. The thing a singer does over and over is "that
    bridge again", and finding the bridge on a bare progress slider means
    guessing, seeking, overshooting and guessing again. So the strip draws the
    chart's own NOTE DENSITY behind the playhead: verses, choruses and the gaps
    between them are visible as shape, and the phrase you want is somewhere you
    can point at.

    THE LOOP LANE IS SEPARATE FROM THE SCRUB AREA, ON PURPOSE
    --------------------------------------------------------
    Dragging in the body seeks. Dragging in the thin lane along the top sets the
    loop. Overloading one gesture with a modifier key would mean the two most
    common actions in a practice session — jump there, drill that — are the same
    motion, distinguished by something invisible, and the failure mode is
    destroying a loop you spent time placing by clicking to seek.

    DESIGN.md §6 lists "phrase ranking, worst-first, with one-click
    loop-and-retry" as a review-screen feature. This is the other half of it:
    the same loop, reachable without a take having finished.

    THREADING: message thread only. The Chart is a non-owning pointer owned by
    the editor and outlives this component.

    ==========================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

#include "Theme.h"
#include "../chart/Chart.h"

namespace vt
{

class TransportBarComponent : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    explicit TransportBarComponent (Theme& themeToUse);
    ~TransportBarComponent() override;

    //==========================================================================
    /** Non-owning. Pass nullptr to clear. Rebuilds the density map. */
    void setChart (const Chart*);

    /** The length the strip spans. Normally the chart's duration, but a backing
        track can outrun it, and a strip that stopped short of the audio would
        make the last few seconds of the song unreachable. */
    void setDurationSeconds (double seconds);
    double getDurationSeconds() const noexcept { return durationSeconds; }

    /** Playhead position, in chart seconds. Cheap to call every frame. */
    void setPosition (double seconds);

    //== Looping ==============================================================
    /** `end <= start` means NO REGION, which is different from a region that is
        merely disabled: the first has nothing to re-arm, the second does. */
    void setLoop (double startSeconds, double endSeconds, bool enabled);

    double getLoopStart() const noexcept { return loopStart; }
    double getLoopEnd()   const noexcept { return loopEnd; }
    bool   isLoopEnabled() const noexcept { return loopEnabled; }
    bool   hasLoopRegion() const noexcept { return loopEnd > loopStart; }

    /** A scrub, in chart seconds. Fires continuously during a drag. */
    std::function<void (double)> onSeek;

    /** The loop region changed. `end <= start` means it was cleared. The bar
        has already updated its own state; the callback is so the editor can
        arm the transport and sync the Loop button. */
    std::function<void (double, double)> onLoopChanged;

    void applyTheme();

    //==========================================================================
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    //==========================================================================
    /** Density map and the end labels: static until the chart or the width
        changes, so buffered rather than redrawn under the playhead 60 times a
        second. */
    class TrackLayer : public juce::Component
    {
    public:
        explicit TrackLayer (TransportBarComponent& o) : owner (o)
        {
            setInterceptsMouseClicks (false, false);
            setBufferedToImage (true);
        }

        void paint (juce::Graphics&) override;

    private:
        TransportBarComponent& owner;
    };

    //==========================================================================
    float  xForTime (double seconds) const noexcept;
    double timeForX (float x) const noexcept;

    juce::Rectangle<float> getLoopLane() const noexcept;
    juce::Rectangle<float> getScrubArea() const noexcept;

    /** Per-column fraction of the column's time that has a note in it. Rebuilt
        only when the chart or the width changes. */
    void rebuildDensity();

    /** Which end of the loop region the pointer is over, or none. */
    enum class Grab { none, seek, loopStart, loopEnd, loopNew };

    Grab grabAt (juce::Point<float>) const noexcept;

    void publishLoop();

    //==========================================================================
    Theme& theme;
    const Chart* chart = nullptr;

    TrackLayer track { *this };

    double durationSeconds = 0.0;
    double position = 0.0;

    double loopStart = 0.0, loopEnd = 0.0;
    bool   loopEnabled = false;

    /** Where a loop drag started, so dragging left of it still makes a region
        rather than an inverted one. */
    double loopAnchor = 0.0;

    Grab dragging = Grab::none;
    bool dragMoved = false;

    std::vector<float> density;
    int densityWidth = -1;
    const Chart* densityChart = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBarComponent)
};

} // namespace vt
