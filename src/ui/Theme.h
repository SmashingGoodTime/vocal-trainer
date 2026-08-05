/*
  ==============================================================================

    Theme.h
    Vocal Trainer — M1

    Every tunable visual / perceptual constant in the runtime lives here and is
    loaded from  %APPDATA%/VocalTrainer/theme.json  at startup, then hot-reloaded
    by polling the file's modification time at 4 Hz.

    WHY THIS EXISTS: C++ rebuild latency is the single biggest threat to this
    project ever getting visually tuned. Every number a human would want to nudge
    while singing — a colour, a bar height, a trail length, a tolerance — must be
    changeable by saving a text file, with the change visible on the next frame.
    Nothing in the UI may hard-code a magic number that belongs here.

    Polling with a juce::Timer is correct in this one place: Theme lives entirely
    on the message thread. (The audio thread and the analysis worker never touch
    it; values they need are bridged through atomics on the processor.)

    Missing or malformed keys fall back to the compiled-in defaults, silently and
    without throwing. A theme.json that fails to parse leaves the previous values
    in place — a half-written file during an editor save must never blank the UI.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vt
{

//==============================================================================
/** The complete set of tunable constants. Defaults here are the DESIGN.md values
    and are what gets written to theme.json the first time the app runs.
*/
struct ThemeValues
{
    //== Colours — Okabe-Ito, DESIGN.md §7 ====================================
    juce::Colour background        { 0xff101014 };
    juce::Colour panel             { 0xff16161c };
    juce::Colour gridLine          { 0xff2a2a32 };
    juce::Colour gridLineOctave    { 0xff3a3a46 };
    juce::Colour gridLabel         { 0xff6a6a76 };
    juce::Colour targetBarActive   { 0xff56b4e9 };   // active note
    juce::Colour targetBarUpcoming { 0xff0072b2 };   // upcoming note (M3)
    juce::Colour goldenNote        { 0xffe69f00 };
    juce::Colour traceInTune       { 0xff009e73 };   // |cents| <= tFullCents
    juce::Colour traceOutOfTune    { 0xffd55e00 };   // |cents| >= tZeroCents
    juce::Colour ghostContour      { 0xff8a8a96 };   // M3 reference contour
    juce::Colour lyricSung         { 0xfff0e442 };   // M5
    juce::Colour lyricPending      { 0xff8a8a96 };   // M5
    juce::Colour lyricActiveWipe   { 0xffffffff };   // M5
    juce::Colour text              { 0xffe6e6ec };
    juce::Colour textDim           { 0xff8a8a96 };
    juce::Colour nowLine           { 0xff5a5a68 };

    //== Lane geometry ========================================================
    int   headerHeight            = 32;
    int   controlsHeight          = 34;
    int   footerHeight            = 28;

    /** Vertical span of the lane, in semitones. DESIGN.md: minimum 12. */
    float semitonesVisible        = 14.0f;

    /** Seconds of history shown to the LEFT of the now-line.
        M1 has no chart and therefore no lookahead, so this is deliberately short
        (see nowLineFraction). M3 restores the 2.0 s / 3.5 s split. */
    float historySeconds          = 0.60f;

    /** Horizontal position of the now-line as a fraction of lane width.
        M1: near the right edge (no lookahead content exists yet).
        M3: 0.364 (= 2.0 / 5.5) per DESIGN.md §7. */
    float nowLineFraction         = 0.92f;

    float gridLineThicknessPx     = 1.0f;
    float gridLineOctaveThickPx   = 1.6f;
    float gridLabelHeightPx       = 11.0f;
    float noteBarCornerRadiusPx   = 4.0f;
    float targetBarBorderPx       = 1.0f;
    float upcomingBarAlpha        = 0.35f;

    /** Alpha of the partial-credit halo behind the target bar. DESIGN.md: 12%. */
    float haloAlpha               = 0.12f;

    //== Highway (M3) =========================================================
    /** Seconds of history shown LEFT of the now-line on the scrolling highway.
        DESIGN.md §7: 2.0. Must be long enough that the timestamp-corrected
        trace's trailing tail (~32 ms behind the now-line) is always on screen. */
    float highwayHistorySeconds   = 2.0f;

    /** Seconds of upcoming chart shown RIGHT of the now-line. DESIGN.md §7: 3.5.
        Total span is history + lookahead = 5.5 s, putting the now-line at
        2.0/5.5 = 36.4% from the left. */
    float highwayLookaheadSeconds = 3.5f;

    /** Vertical auto-zoom. DESIGN.md §7: never show fewer than 12 semitones,
        pad 3 semitones beyond the chart's range in view. */
    float minSemitonesVisible     = 12.0f;
    float zoomPaddingSemitones    = 3.0f;

    /** Dual horizon: the zoom considers notes up to `inner` seconds ahead, and
        widens (never narrows) to also fit anything up to `outer` seconds ahead.
        This stops the view lurching when a big leap arrives. */
    float zoomHorizonInnerSeconds = 8.0f;
    float zoomHorizonOuterSeconds = 15.0f;

    /** Exponential approach rate of the animated zoom, in Hz. Higher snaps
        faster. Low enough that the highway never appears to slide under you. */
    float zoomSmoothingHz         = 1.5f;

    /** Alpha of the reference singer's ghost contour. DESIGN.md §7: 25%. */
    float ghostContourAlpha       = 0.25f;
    float ghostContourThicknessPx = 1.0f;

    //== Trace ================================================================
    /** Length of the unsmoothed trail, milliseconds. DESIGN.md: 500. */
    int   trailMs                 = 500;
    /** Length of history retained in the GUI ring buffer, milliseconds. */
    int   ringMs                  = 600;
    float trailThicknessPx        = 2.0f;
    /** Alpha of the newest trail segment; fades linearly to 0 at trailMs. */
    float trailStartAlpha         = 0.60f;
    /** Number of discrete alpha bands the trail is batched into (see
        PitchLaneComponent — this is what keeps paint() allocation-free). */
    int   trailAlphaBands         = 8;
    /** Break the polyline when consecutive estimates jump more than this many
        semitones. Stops consonants and octave flips drawing vertical streaks. */
    float traceBreakSemitones     = 1.0f;

    float dotDiameterPx           = 7.0f;
    float dotRingPx               = 2.0f;

    //== Tolerances (DESIGN.md §6, "Practice" preset) =========================
    /** Full credit inside +/- this. The target bar's DRAWN HEIGHT is 2x this. */
    float tFullCents              = 25.0f;
    /** Zero credit outside +/- this. */
    float tZeroCents              = 100.0f;
    /** NSDF clarity below this rejects the frame. Bridged to the worker thread. */
    float clarityThreshold        = 0.85f;

    /** Default guide-vocal level in dB relative to the backing track.
        NEGATIVE on purpose: the reference singer is a reference, not the
        performance. At unity it buries the singer's own voice and they end up
        listening to the record instead of to themselves. */
    float guideVocalDb            = -12.0f;
    /** Trailing median window used for the live colour band. Never colour from
        the instantaneous value — that punishes vibrato. DESIGN.md §6. */
    int   colourMedianMs          = 200;

    //== Display smoothing — 1-Euro filter (Casiez et al., CHI 2012) ==========
    float oneEuroMinCutoffHz      = 1.0f;
    float oneEuroBeta             = 0.5f;
    float oneEuroDerivCutoffHz    = 1.0f;

    //== Free Sing ============================================================
    /** In Free Sing the lane re-centres (snapping to whole semitones) once the
        voice drifts further than this from the current centre. Snapping with
        hysteresis rather than easing keeps the buffered grid layer static. */
    float freeSingRecentreSemis   = 3.0f;
};

//==============================================================================
/** Perceptually-uniform colour interpolation (Oklab, Björn Ottosson 2020).

    DESIGN.md §7 specifies Oklab for the in-tune -> out-of-tune ramp. Naive sRGB
    interpolation between #009E73 and #D55E00 dips through a muddy dark olive at
    t ~ 0.5, which reads as a third state rather than a midpoint.

    The matrices below are published constants (facts, not copyrightable code).
*/
juce::Colour lerpOklab (juce::Colour a, juce::Colour b, float t) noexcept;

//==============================================================================
/**
    Owns the ThemeValues and keeps them in sync with theme.json.

    Obtain it with juce::SharedResourcePointer<vt::Theme> — every holder sees the
    same instance and it is destroyed when the last holder goes away, which keeps
    the JUCE leak detector happy in a plugin (no process-lifetime singleton).

    Register as a juce::ChangeListener to be told when the file was reloaded.
*/
class Theme final : public juce::ChangeBroadcaster,
                    private juce::Timer
{
public:
    Theme();
    ~Theme() override;

    /** The current values. Message thread only. */
    const ThemeValues& get() const noexcept  { return values; }

    /** %APPDATA%/VocalTrainer/theme.json */
    static juce::File getThemeFile();

    /** Writes the compiled-in defaults to disk, overwriting whatever is there.
        Also used to create the file on first run. */
    bool writeDefaultsToFile() const;

    /** Force an immediate re-read. Returns true if the file parsed. */
    bool reloadNow();

    /** True if the last parse attempt failed (useful for a UI hint). */
    bool hasParseError() const noexcept { return parseError; }

private:
    void timerCallback() override;
    bool loadFrom (const juce::File&);

    static constexpr int kPollHz = 4;

    ThemeValues values;

    juce::Time  seenModTime;
    juce::int64 seenSize = -1;
    bool        changePending = false;   // one-tick debounce against partial writes
    bool        parseError = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Theme)
};

} // namespace vt
