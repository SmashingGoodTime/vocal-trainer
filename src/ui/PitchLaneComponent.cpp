/*
  ==============================================================================

    PitchLaneComponent.cpp
    Vocal Trainer — M1

  ==============================================================================
*/

#include "PitchLaneComponent.h"

#include <juce_audio_basics/juce_audio_basics.h>   // juce::MidiMessage::getMidiNoteName

#include <algorithm>
#include <cmath>

namespace vt
{

namespace
{
    /** Ableton Live shows middle C (MIDI 60) as C3, so match it. Anything else
        makes the note selector disagree with the DAW the user is staring at. */
    constexpr int kOctaveNumberForMiddleC = 3;

    /** A gap larger than this between consecutive estimates means the analysis
        stream itself stalled, so the polyline must break. Three hops at 8 ms. */
    constexpr double kMaxSegmentGapSeconds = 0.024 * 2.0;
}

//==============================================================================
PitchLaneComponent::PitchLaneComponent()
{
    setOpaque (true);
    setInterceptsMouseClicks (false, false);

    for (int m = 0; m < 128; ++m)
        noteNames[(size_t) m] = juce::MidiMessage::getMidiNoteName (m, true, true, kOctaveNumberForMiddleC);

    addAndMakeVisible (grid);

    applyTheme();
}

PitchLaneComponent::~PitchLaneComponent() = default;

//==============================================================================
void PitchLaneComponent::applyTheme()
{
    const auto& t = theme->get();

    semitonesVisible = t.semitonesVisible;
    historySeconds   = t.historySeconds;
    nowLineFraction  = t.nowLineFraction;
    trailSeconds     = (float) t.trailMs * 0.001f;
    ringSeconds      = (float) t.ringMs  * 0.001f;
    traceBreakSemis  = t.traceBreakSemitones;
    trailBands       = juce::jlimit (1, kMaxTrailBands, t.trailAlphaBands);

    trailStroke.setStrokeThickness (t.trailThicknessPx);

    rebuildColourLut();
    recomputeDerived();

    grid.repaint();
    repaint();
}

void PitchLaneComponent::rebuildColourLut()
{
    const auto& t = theme->get();

    for (int i = 0; i < kColourLutSize; ++i)
    {
        const float f = (float) i / (float) (kColourLutSize - 1);
        traceColourLut[(size_t) i] = lerpOklab (t.traceInTune, t.traceOutOfTune, f);
    }
}

void PitchLaneComponent::recomputeDerived() noexcept
{
    const auto w = (float) juce::jmax (1, getWidth());
    const auto h = (float) juce::jmax (1, getHeight());

    pixelsPerSemitone = h / juce::jmax (1.0f, semitonesVisible);
    nowLineX          = w * juce::jlimit (0.05f, 1.0f, nowLineFraction);
    pixelsPerSecond   = nowLineX / juce::jmax (0.05f, historySeconds);
}

void PitchLaneComponent::resized()
{
    grid.setBounds (getLocalBounds());
    recomputeDerived();

    // Enough room for every trail segment this lane can hold, so that
    // startNewSubPath()/lineTo() in paint() never reallocate.
    const int coordsPerSegment = 6;
    const int maxSegments = juce::jmax (64, (int) (ringSeconds * 130.0f));

    for (auto& p : trailBandPaths)
        p.preallocateSpace (maxSegments * coordsPerSegment);

    barPath.preallocateSpace (64);
    haloPath.preallocateSpace (64);
}

//==============================================================================
void PitchLaneComponent::setTargetNote (int midiNote) noexcept
{
    midiNote = juce::jlimit (0, 127, midiNote);

    if (midiNote == targetMidi)
        return;

    targetMidi = midiNote;
    updateLaneCentre();
    repaint();
}

void PitchLaneComponent::setFreeSing (bool shouldFreeSing) noexcept
{
    if (shouldFreeSing == freeSing)
        return;

    freeSing = shouldFreeSing;
    updateLaneCentre();
    repaint();
}

void PitchLaneComponent::setTolerances (float newFull, float newZero) noexcept
{
    newFull = juce::jmax (1.0f, newFull);
    newZero = juce::jmax (newFull + 1.0f, newZero);

    if (juce::approximatelyEqual (newFull, tFullCents) && juce::approximatelyEqual (newZero, tZeroCents))
        return;

    tFullCents = newFull;
    tZeroCents = newZero;
    repaint();
}

//==============================================================================
void PitchLaneComponent::pushSample (const Sample& s) noexcept
{
    ring[(size_t) ringWrite] = s;
    ringWrite = (ringWrite + 1) % kRingCapacity;
    ringCount = juce::jmin (ringCount + 1, kRingCapacity);

    updateLaneCentre();
    updateReadout();
}

void PitchLaneComponent::clearHistory() noexcept
{
    ringWrite = 0;
    ringCount = 0;
    readout = {};
}

int PitchLaneComponent::physicalIndex (int logicalIndex) const noexcept
{
    return (ringWrite - ringCount + logicalIndex + 2 * kRingCapacity) % kRingCapacity;
}

const PitchLaneComponent::Sample& PitchLaneComponent::sampleAt (int logicalIndex) const noexcept
{
    return ring[(size_t) physicalIndex (logicalIndex)];
}

//==============================================================================
float PitchLaneComponent::yForMidi (float midi) const noexcept
{
    const float topMidi = laneCentreMidi + semitonesVisible * 0.5f;
    return (topMidi - midi) * pixelsPerSemitone;
}

float PitchLaneComponent::xForTime (double t) const noexcept
{
    return nowLineX - (float) ((nowTime - t) * (double) pixelsPerSecond);
}

float PitchLaneComponent::foldOntoTarget (float sungMidi) const noexcept
{
    if (freeSing || std::isnan (sungMidi))
        return sungMidi;

    return (float) targetMidi + (float) std::remainder ((double) sungMidi - (double) targetMidi, 12.0);
}

float PitchLaneComponent::centsErrorFor (float sungMidi) const noexcept
{
    if (std::isnan (sungMidi))
        return 0.0f;

    if (freeSing)
        return (sungMidi - std::round (sungMidi)) * 100.0f;

    return (float) (std::remainder ((double) sungMidi - (double) targetMidi, 12.0) * 100.0);
}

float PitchLaneComponent::errorFractionFor (float cents) const noexcept
{
    const float span = juce::jmax (1.0f, tZeroCents - tFullCents);
    return juce::jlimit (0.0f, 1.0f, (std::abs (cents) - tFullCents) / span);
}

juce::Colour PitchLaneComponent::colourForCents (float cents) const noexcept
{
    const int i = juce::jlimit (0, kColourLutSize - 1,
                                juce::roundToInt (errorFractionFor (cents) * (float) (kColourLutSize - 1)));
    return traceColourLut[(size_t) i];
}

float PitchLaneComponent::trailingMedianCents() const noexcept
{
    const double window = (double) theme->get().colourMedianMs * 0.001;
    const double oldest = nowTime - juce::jmax (0.008, window);

    int n = 0;

    for (int i = ringCount - 1; i >= 0 && n < (int) medianScratch.size(); --i)
    {
        const auto& s = sampleAt (i);

        if (s.timeSeconds < oldest)
            break;

        if (! std::isnan (s.rawMidi))
            medianScratch[(size_t) n++] = centsErrorFor (s.rawMidi);
    }

    if (n == 0)
        return 0.0f;

    const auto mid = medianScratch.begin() + n / 2;
    std::nth_element (medianScratch.begin(), mid, medianScratch.begin() + n);
    return *mid;
}

//==============================================================================
void PitchLaneComponent::updateLaneCentre() noexcept
{
    float wanted = laneCentreMidi;

    if (! freeSing)
    {
        wanted = (float) targetMidi;
    }
    else if (ringCount > 0)
    {
        const auto& newest = sampleAt (ringCount - 1);

        if (! std::isnan (newest.smoothedMidi))
        {
            // Snap with hysteresis rather than easing continuously: the grid is a
            // buffered image, and a centre that moved every frame would re-render
            // it every frame, which is exactly what buffering is meant to avoid.
            const float drift = std::abs (newest.smoothedMidi - laneCentreMidi);

            if (drift > theme->get().freeSingRecentreSemis)
                wanted = std::round (newest.smoothedMidi);
        }
    }

    if (! juce::approximatelyEqual (wanted, laneCentreMidi))
    {
        laneCentreMidi = wanted;
        grid.repaint();
    }
}

void PitchLaneComponent::updateReadout() noexcept
{
    Readout r;

    if (ringCount > 0)
    {
        const auto& newest = sampleAt (ringCount - 1);

        r.clarity = newest.clarity;
        r.rmsDb   = newest.rmsDb;

        if (! std::isnan (newest.smoothedMidi))
        {
            r.voiced       = true;
            r.smoothedMidi = newest.smoothedMidi;
            r.cents        = trailingMedianCents();

            if (! freeSing)
                r.octaveOffset = juce::roundToInt ((newest.smoothedMidi - foldOntoTarget (newest.smoothedMidi)) / 12.0f);
        }
    }

    readout = r;

    if (r.octaveOffset != lastOctaveBadgeValue)
    {
        lastOctaveBadgeValue = r.octaveOffset;

        // Built here, never in paint(). juce::String construction allocates.
        octaveBadgeText = r.octaveOffset == 0
                            ? juce::String()
                            : (r.octaveOffset > 0 ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92"))
                                  + juce::String (std::abs (r.octaveOffset)) + " 8ve";
    }
}

//==============================================================================
void PitchLaneComponent::paint (juce::Graphics& g)
{
    // Background only. The grid child paints on top of this, and everything
    // dynamic goes in paintOverChildren() so it sits above the grid.
    g.fillAll (theme->get().background);
}

void PitchLaneComponent::GridLayer::paint (juce::Graphics& g)
{
    const auto& t = owner.theme->get();

    const auto w = (float) getWidth();

    const int lowest  = (int) std::floor (owner.laneCentreMidi - owner.semitonesVisible * 0.5f);
    const int highest = (int) std::ceil  (owner.laneCentreMidi + owner.semitonesVisible * 0.5f);

    g.setFont (owner.gridFont);

    for (int m = lowest; m <= highest; ++m)
    {
        if (m < 0 || m > 127)
            continue;

        const bool isC = (m % 12) == 0;
        const float y  = owner.yForMidi ((float) m);
        const float th = isC ? t.gridLineOctaveThickPx : t.gridLineThicknessPx;

        g.setColour (isC ? t.gridLineOctave : t.gridLine);
        g.fillRect (0.0f, y - th * 0.5f, w, th);

        g.setColour (t.gridLabel);
        g.drawText (owner.noteNames[(size_t) m],
                    juce::Rectangle<float> (w - 46.0f, y - t.gridLabelHeightPx, 42.0f, t.gridLabelHeightPx * 2.0f),
                    juce::Justification::centredRight,
                    false);
    }
}

void PitchLaneComponent::paintOverChildren (juce::Graphics& g)
{
    const auto& t = theme->get();

    const auto w = (float) getWidth();
    const auto h = (float) getHeight();

    if (w <= 1.0f || h <= 1.0f)
        return;

    const float bandCents = trailingMedianCents();
    const juce::Colour traceColour = colourForCents (bandCents);

    //--------------------------------------------------------------------
    // 1. Partial-credit halo, then 2. the target bar.
    //    The bar's drawn height IS 2 * tFullCents. Do not decouple these.
    //--------------------------------------------------------------------
    if (! freeSing)
    {
        const float centreY   = yForMidi ((float) targetMidi);
        const float fullPx    = (tFullCents / 100.0f) * pixelsPerSemitone;   // half-height
        const float zeroPx    = (tZeroCents / 100.0f) * pixelsPerSemitone;   // half-height
        const float radius    = t.noteBarCornerRadiusPx;

        haloPath.clear();
        haloPath.addRoundedRectangle (juce::Rectangle<float> (0.0f, centreY - zeroPx, w, zeroPx * 2.0f), radius);
        g.setColour (t.targetBarActive.withAlpha (t.haloAlpha));
        g.fillPath (haloPath);

        barPath.clear();
        barPath.addRoundedRectangle (juce::Rectangle<float> (0.0f, centreY - fullPx, w, fullPx * 2.0f), radius);
        g.setColour (t.targetBarActive);
        g.fillPath (barPath);
    }

    //--------------------------------------------------------------------
    // 3. Now-line.
    //--------------------------------------------------------------------
    g.setColour (t.nowLine);
    g.fillRect (nowLineX - 0.5f, 0.0f, 1.0f, h);

    //--------------------------------------------------------------------
    // 4. Unsmoothed trail, batched into alpha bands.
    //
    //    BREAK the polyline on NaN, on a jump larger than traceBreakSemitones,
    //    and on a stall in the estimate stream. Without this, consonants and
    //    octave flips draw vertical streaks that the singer reads as error.
    //--------------------------------------------------------------------
    const int bands = juce::jlimit (1, kMaxTrailBands, trailBands);

    for (int b = 0; b < bands; ++b)
        trailBandPaths[(size_t) b].clear();

    const double oldestVisible = nowTime - (double) trailSeconds;

    for (int i = 1; i < ringCount; ++i)
    {
        const auto& a = sampleAt (i - 1);
        const auto& b = sampleAt (i);

        if (b.timeSeconds < oldestVisible)
            continue;

        if (std::isnan (a.rawMidi) || std::isnan (b.rawMidi))
            continue;

        if (std::abs (b.rawMidi - a.rawMidi) > traceBreakSemis)
            continue;

        if (b.timeSeconds - a.timeSeconds > kMaxSegmentGapSeconds)
            continue;

        const double age = juce::jmax (0.0, nowTime - b.timeSeconds);
        const int band = juce::jlimit (0, bands - 1,
                                       (int) (age / juce::jmax (1.0e-3, (double) trailSeconds) * (double) bands));

        auto& p = trailBandPaths[(size_t) band];
        p.startNewSubPath (xForTime (a.timeSeconds), yForMidi (foldOntoTarget (a.rawMidi)));
        p.lineTo          (xForTime (b.timeSeconds), yForMidi (foldOntoTarget (b.rawMidi)));
    }

    for (int b = 0; b < bands; ++b)
    {
        auto& p = trailBandPaths[(size_t) b];

        if (p.isEmpty())
            continue;

        const float ageFraction = ((float) b + 0.5f) / (float) bands;
        const float alpha = t.trailStartAlpha * (1.0f - ageFraction);

        if (alpha <= 0.004f)
            continue;

        g.setColour (traceColour.withMultipliedAlpha (alpha));
        g.strokePath (p, trailStroke);
    }

    //--------------------------------------------------------------------
    // 5. Smoothed dot.
    //--------------------------------------------------------------------
    if (ringCount > 0)
    {
        const auto& newest = sampleAt (ringCount - 1);

        if (! std::isnan (newest.smoothedMidi))
        {
            const float cx = xForTime (newest.timeSeconds);
            const float cy = yForMidi (foldOntoTarget (newest.smoothedMidi));
            const float d  = t.dotDiameterPx;

            g.setColour (traceColour);
            g.fillEllipse (cx - d * 0.5f, cy - d * 0.5f, d, d);

            if (t.dotRingPx > 0.0f)
            {
                const float rd = d + t.dotRingPx * 2.0f;
                g.setColour (t.text.withAlpha (0.9f));
                g.drawEllipse (cx - rd * 0.5f, cy - rd * 0.5f, rd, rd, t.dotRingPx);
            }
        }
    }

    //--------------------------------------------------------------------
    // 6. Octave badge. The trace is folded, so the true octave would otherwise
    //    be lost information.
    //--------------------------------------------------------------------
    if (octaveBadgeText.isNotEmpty())
    {
        g.setFont (badgeFont);
        g.setColour (t.textDim);
        g.drawText (octaveBadgeText,
                    juce::Rectangle<float> (8.0f, 6.0f, 90.0f, 16.0f),
                    juce::Justification::centredLeft,
                    false);
    }
}

} // namespace vt
