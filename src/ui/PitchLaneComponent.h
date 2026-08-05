/*
  ==============================================================================

    PitchLaneComponent.h
    Vocal Trainer — M1

    A SINGLE FIXED HORIZONTAL TARGET LANE. This is not the scrolling note highway
    (that is M3) — there is no chart yet, so there is exactly one target note, it
    is always active, and it spans the full width of the lane.

    What it draws, back to front:
      1. semitone grid + note names          (child component, setBufferedToImage)
      2. partial-credit halo, +/- tZeroCents, 12% alpha
      3. the target bar, whose DRAWN HEIGHT IS 2 * tFullCents
      4. the now-line
      5. the unsmoothed 500 ms trail, alpha fading 60% -> 0% with age
      6. the 1-Euro-smoothed dot, 7 px with a 2 px ring
      7. an octave badge when the singer is folded from another octave

    THE BAR HEIGHT IS THE POINT. DESIGN.md §7 calls it the highest-value visual
    decision in the project: because the bar is exactly as tall as the full-credit
    window, "inside the bar" means "full points" with no cognitive translation and
    no need to read a cents number while singing.

    paint() IS AUDIO CODE. Nothing in the paint path may allocate:
      - every juce::Path, juce::Font and juce::String is a member
      - the in-tune -> out-of-tune Oklab ramp is a precomputed lookup table
      - the trail is batched into a fixed number of alpha bands so it costs one
        strokePath per band instead of one drawLine per sample
      - the grid is a child component with setBufferedToImage(true) and only
        repaints when the lane centre or the theme changes

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <limits>

#include "Theme.h"

namespace vt
{

class PitchLaneComponent final : public juce::Component
{
public:
    //==========================================================================
    /** One drained pitch estimate, already converted to GUI units.
        Both the raw and the smoothed value are carried: the trail must show the
        singer's honest wobble (vibrato is not error) while the dot must be calm.
    */
    struct Sample
    {
        double timeSeconds  = 0.0;
        float  rawMidi      = std::numeric_limits<float>::quiet_NaN();   // NaN == unvoiced
        float  smoothedMidi = std::numeric_limits<float>::quiet_NaN();   // NaN == unvoiced
        float  clarity      = 0.0f;
        float  rmsDb        = -100.0f;
    };

    /** What the footer needs. Cheap, no allocation. */
    struct Readout
    {
        bool  voiced        = false;
        float cents         = 0.0f;    // signed, folded onto the target (or nearest semitone in Free Sing)
        float smoothedMidi  = std::numeric_limits<float>::quiet_NaN();
        float clarity       = 0.0f;
        float rmsDb         = -100.0f;
        int   octaveOffset  = 0;       // how many octaves the true pitch is from the target
    };

    PitchLaneComponent();
    ~PitchLaneComponent() override;

    //==========================================================================
    /** Message thread only (called from the VBlankAttachment callback). */
    void pushSample (const Sample& s) noexcept;
    void clearHistory() noexcept;

    /** The right-hand edge of the lane represents this time. Set it from the
        newest estimate's timestamp so the trace never drifts against the data. */
    void setNowTime (double seconds) noexcept  { nowTime = seconds; }

    void setTargetNote (int midiNote) noexcept;
    void setFreeSing (bool shouldFreeSing) noexcept;
    void setTolerances (float tFullCents, float tZeroCents) noexcept;

    /** Re-read everything from the Theme and rebuild the cached colour ramp. */
    void applyTheme();

    Readout getReadout() const noexcept  { return readout; }

    /** True while the trail contains at least one voiced sample. */
    bool hasSignal() const noexcept  { return ringCount > 0; }

    //==========================================================================
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    //==========================================================================
    /** Static layer: semitone lines and note names. Buffered to an image and only
        repainted when the lane centre, size or theme changes. */
    class GridLayer final : public juce::Component
    {
    public:
        explicit GridLayer (PitchLaneComponent& o) : owner (o)
        {
            setInterceptsMouseClicks (false, false);
            setBufferedToImage (true);
        }

        void paint (juce::Graphics&) override;

    private:
        PitchLaneComponent& owner;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GridLayer)
    };

    friend class GridLayer;

    //==========================================================================
    float yForMidi (float midi) const noexcept;
    float xForTime (double t) const noexcept;

    /** Octave-folds a sung pitch onto the target so an octave-agnostic score and
        a literal-pitch display cannot silently contradict each other
        (DESIGN.md §7 — the single most likely UX bug in the project).
        No folding happens in Free Sing: there is no meaningful target to fold to. */
    float foldOntoTarget (float sungMidi) const noexcept;

    /** Signed cents error, folded. In Free Sing this is deviation from the
        nearest semitone, i.e. ordinary chromatic-tuner behaviour. */
    float centsErrorFor (float sungMidi) const noexcept;

    /** 0 = full credit, 1 = zero credit. Index into traceColourLut. */
    float errorFractionFor (float cents) const noexcept;

    juce::Colour colourForCents (float cents) const noexcept;

    /** Trailing median of the last colourMedianMs of raw estimates. Colouring
        from the instantaneous value flickers all the way through vibrato. */
    float trailingMedianCents() const noexcept;

    void recomputeDerived() noexcept;
    void updateLaneCentre() noexcept;
    void updateReadout() noexcept;
    void rebuildColourLut();

    int  physicalIndex (int logicalIndex) const noexcept;
    const Sample& sampleAt (int logicalIndex) const noexcept;

    //==========================================================================
    juce::SharedResourcePointer<Theme> theme;

    GridLayer grid { *this };

    // --- history ring ---------------------------------------------------
    static constexpr int kRingCapacity = 1024;   // 8.2 s at 125 estimates/s
    std::array<Sample, kRingCapacity> ring {};
    int    ringWrite = 0;
    int    ringCount = 0;
    double nowTime   = 0.0;

    // --- state ----------------------------------------------------------
    int   targetMidi       = 60;
    bool  freeSing         = true;
    float tFullCents       = 25.0f;
    float tZeroCents       = 100.0f;
    float laneCentreMidi   = 60.0f;   // always a whole semitone
    Readout readout;

    // --- cached theme-derived values (recomputed on theme change / resize) ---
    float semitonesVisible   = 14.0f;
    float historySeconds     = 0.6f;
    float nowLineFraction    = 0.92f;
    float trailSeconds       = 0.5f;
    float ringSeconds        = 0.6f;
    float traceBreakSemis    = 1.0f;
    int   trailBands         = 8;
    float pixelsPerSemitone  = 1.0f;
    float pixelsPerSecond    = 1.0f;
    float nowLineX           = 0.0f;

    // --- preallocated paint resources -----------------------------------
    static constexpr int kMaxTrailBands = 32;
    static constexpr int kColourLutSize = 33;

    std::array<juce::Path, kMaxTrailBands> trailBandPaths;
    juce::Path barPath, haloPath;
    juce::PathStrokeType trailStroke { 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded };
    std::array<juce::Colour, kColourLutSize> traceColourLut {};
    std::array<juce::String, 128> noteNames;
    juce::Font gridFont  { juce::FontOptions (11.0f) };
    juce::Font badgeFont { juce::FontOptions (12.0f) };
    juce::String octaveBadgeText;
    int lastOctaveBadgeValue = 0;

    /** Scratch used by trailingMedianCents(). Member, so no per-frame allocation. */
    mutable std::array<float, 64> medianScratch {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchLaneComponent)
};

} // namespace vt
