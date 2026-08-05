/*
    ==========================================================================

    NoteHighwayComponent.h — the M3 scrolling note highway.

    Draws the chart's target notes scrolling right-to-left past a fixed
    now-line, with the singer's live pitch trace over the top.

    Geometry and colour come from docs/DESIGN.md section 7 and are all
    hot-reloadable through theme.json.

    TWO DECISIONS HERE ARE LOAD-BEARING
    -----------------------------------
    1. THE TARGET BAR'S DRAWN HEIGHT *IS* THE FULL-CREDIT WINDOW
       (2 x tFullCents). Inside the bar means full points, with no cents
       readout to translate in your head. Performous does this; Harmonix
       patented it. Do not "fix" the bar to a constant height.

    2. THE TRACE IS OCTAVE-FOLDED ONTO THE ACTIVE TARGET NOTE.
       Scoring is octave-agnostic, so the display must be too. Without this a
       bass singing the melody an octave down sees full marks while the trace
       sits nowhere near the bar, and immediately stops trusting the feedback.
       The true octave is shown as a small badge instead of being lost.

    THREADING: message thread only. The Chart is a non-owning pointer owned by
    the editor and outlives this component.

    ==========================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <limits>
#include <vector>

#include "Theme.h"
#include "../chart/Chart.h"
#include "../chart/PitchMath.h"
#include "../chart/TimelineMap.h"

namespace vt
{

class NoteHighwayComponent : public juce::Component
{
public:
    explicit NoteHighwayComponent (Theme& themeToUse);
    ~NoteHighwayComponent() override;

    //==========================================================================
    /** One live pitch estimate. Mirrors PitchLaneComponent::Sample.

        timeSeconds is AUDIO-STREAM time, NOT chart time. That distinction is
        load-bearing: the chart clock can be driven by the backing track, can be
        paused, and can be seeked, none of which the microphone knows about.
        Storing chart time here made the live trace disappear the moment a chart
        was loaded, because the samples and the now-line were being measured
        with two unrelated clocks. See setClock(). */
    struct Sample
    {
        double timeSeconds  = 0.0;   ///< AUDIO-STREAM seconds (see setClock)
        float  rawMidi      = std::numeric_limits<float>::quiet_NaN();
        float  smoothedMidi = std::numeric_limits<float>::quiet_NaN();
        float  clarity      = 0.0f;
        float  rmsDb        = -100.0f;
    };

    /** What the footer shows. Recomputed each frame from the newest sample. */
    struct Readout
    {
        bool  voiced       = false;
        float cents        = 0.0f;   ///< signed, folded onto the target
        int   octaveOffset = 0;      ///< -1 == singing an octave below the chart
        float clarity      = 0.0f;
        float rmsDb        = -100.0f;
        int   targetMidi   = -1;     ///< -1 when no note is active
    };

    //==========================================================================
    /** Non-owning. Pass nullptr to clear. Resets the zoom to the new range. */
    void setChart (const Chart* newChart);
    const Chart* getChart() const noexcept { return chart; }

    void pushSample (const Sample& s) noexcept;
    void clearHistory() noexcept;

    /** Ties the two clocks together for this frame.

        `chartNowSeconds` is where the SONG is (backing-track position, or host
        transport, or a frozen value while paused). `streamNowSeconds` is the
        audio-stream time of that same instant — i.e. the stream timestamp the
        newest microphone sample corresponds to.

        Live samples are then drawn at
            chartTime = chartNow + (sample.timeSeconds - streamNow)
        so the newest sample always sits at the now-line no matter which clock
        drives the chart, and the mic trace stays visible while paused, while
        stopped, and before the song has ever been started. */
    void setClock (double streamNowSeconds, double chartNowSeconds) noexcept
    {
        streamNow = streamNowSeconds;
        nowTime   = chartNowSeconds;
    }

    /** Chart time with no stream mapping — for use before any audio arrives. */
    void setNowTime (double seconds) noexcept { nowTime = seconds; }
    double getNowTime() const noexcept { return nowTime; }

    /** Maps a stream-time sample onto the chart timeline. Delegates to
        TimelineMap so the renderer and the tested invariant cannot drift. */
    double chartTimeFor (double sampleStreamSeconds) const noexcept
    {
        return TimelineMap { streamNow, nowTime }.chartTimeFor (sampleStreamSeconds);
    }

    void setTolerances (float tFullCents, float tZeroCents) noexcept;

    /** Advances the zoom animation. Call once per vblank BEFORE repainting;
        `dt` is the elapsed wall-clock time in seconds. */
    void tickAnimation (double dt) noexcept;

    Readout getReadout() const noexcept { return readout; }

    /** MIDI note the singer should currently be aiming at, or -1. The editor
        publishes this to the processor to constrain the detector's search. */
    int getTargetMidi() const noexcept { return readout.targetMidi; }

    void applyTheme();

    //==========================================================================
    /** Only the background fallback + the staleness check for the grid layer.

        EVERYTHING VISIBLE IS DRAWN IN paintOverChildren(), NOT HERE.
        The semitone grid is a CHILD component, and JUCE paints children AFTER
        the parent's paint(). Because that child does a full-bounds fillAll(),
        anything drawn here is covered by it a moment later — which is exactly
        what happened: notes, trace, dot and even the now-line were all being
        rendered correctly and then painted over, leaving a bare grid. */
    void paint (juce::Graphics&) override;

    /** The highway proper: ghost contour, note bars, live trace, now-line. */
    void paintOverChildren (juce::Graphics&) override;

    void resized() override;

private:
    //==========================================================================
    /** The static background: semitone grid and note-name labels. Repainted
        only when the view range or the bounds change, not every frame. */
    class BackgroundLayer : public juce::Component
    {
    public:
        explicit BackgroundLayer (NoteHighwayComponent& o) : owner (o)
        {
            setInterceptsMouseClicks (false, false);
            setBufferedToImage (true);
        }

        void paint (juce::Graphics&) override;

    private:
        NoteHighwayComponent& owner;
    };

    //==========================================================================
    float yForMidi (float midi) const noexcept;
    float xForTime (double t) const noexcept;

    void updateZoomTarget() noexcept;
    void updateReadout() noexcept;
    void refreshBackgroundIfNeeded();

    void paintGhostContour (juce::Graphics&, juce::Rectangle<float> area);
    void paintNotes (juce::Graphics&, juce::Rectangle<float> area);
    void paintTrace (juce::Graphics&, juce::Rectangle<float> area);

    //==========================================================================
    Theme& theme;
    const Chart* chart = nullptr;

    BackgroundLayer background { *this };

    double nowTime = 0.0;      ///< chart seconds at the now-line
    double streamNow = 0.0;    ///< audio-stream seconds of that same instant

    // Vertical view, in MIDI. `view*` animate toward `target*`.
    float viewLowMidi = 54.0f, viewHighMidi = 68.0f;
    float targetLowMidi = 54.0f, targetHighMidi = 68.0f;

    // What the background layer was last drawn for, so we can tell when it is
    // stale without repainting it every frame.
    float bgLowMidi = -1.0f, bgHighMidi = -1.0f;
    juce::Rectangle<int> bgBounds;

    float tFull = 25.0f, tZero = 100.0f;

    //== Live sample ring ======================================================
    std::vector<Sample> ring;
    int ringWrite = 0, ringCount = 0;

    const Sample& ringAt (int i) const noexcept;

    Readout readout;

    //== Preallocated paint scratch — paint() must never allocate ==============
    juce::Path tracePath, ghostPath, notePath;
    juce::RectangleList<float> haloRects, barRects;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteHighwayComponent)
};

} // namespace vt
