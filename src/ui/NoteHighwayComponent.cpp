#include "NoteHighwayComponent.h"

#include "../chart/PitchMath.h"

#include <algorithm>
#include <cmath>

namespace vt
{

namespace
{
    constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

    const char* const kNoteNames[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    juce::String midiName (int midi)
    {
        return juce::String (kNoteNames[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }
}

//==============================================================================
NoteHighwayComponent::NoteHighwayComponent (Theme& t)
    : theme (t)
{
    addAndMakeVisible (background);
    setInterceptsMouseClicks (false, false);
    setOpaque (true);

    applyTheme();
}

NoteHighwayComponent::~NoteHighwayComponent() = default;

//==============================================================================
void NoteHighwayComponent::applyTheme()
{
    const auto& v = theme.get();

    tFull = v.tFullCents;
    tZero = v.tZeroCents;

    // Ring must hold at least the visible history. Sized from the highway's
    // history window rather than the M1 lane's much shorter one.
    const auto estimatesPerSecond = 125.0;
    const auto wanted = (int) std::ceil ((v.highwayHistorySeconds + 1.0f) * estimatesPerSecond);

    if ((int) ring.size() != wanted)
    {
        ring.assign ((size_t) juce::jmax (16, wanted), Sample {});
        ringWrite = ringCount = 0;
    }

    bgLowMidi = bgHighMidi = -1.0f;    // force a background redraw
    repaint();
}

void NoteHighwayComponent::setTolerances (float full, float zero) noexcept
{
    tFull = juce::jmax (1.0f, full);
    tZero = juce::jmax (tFull + 1.0f, zero);
}

//==============================================================================
void NoteHighwayComponent::setChart (const Chart* c)
{
    chart = c;
    clearHistory();

    if (chart != nullptr && ! chart->isEmpty())
    {
        // Snap (rather than animate) to the opening view, so the first frame is
        // already framed correctly.
        updateZoomTarget();
        viewLowMidi  = targetLowMidi;
        viewHighMidi = targetHighMidi;
    }

    bgLowMidi = bgHighMidi = -1.0f;
    repaint();
}

void NoteHighwayComponent::clearHistory() noexcept
{
    ringWrite = ringCount = 0;
    readout = {};
}

const NoteHighwayComponent::Sample& NoteHighwayComponent::ringAt (int i) const noexcept
{
    // i == 0 is the OLDEST retained sample.
    const auto n = (int) ring.size();
    const auto start = (ringWrite - ringCount + n) % n;
    return ring[(size_t) ((start + i) % n)];
}

void NoteHighwayComponent::pushSample (const Sample& s) noexcept
{
    if (ring.empty())
        return;

    ring[(size_t) ringWrite] = s;
    ringWrite = (ringWrite + 1) % (int) ring.size();

    if (ringCount < (int) ring.size())
        ++ringCount;
}

//==============================================================================
float NoteHighwayComponent::yForMidi (float midi) const noexcept
{
    const auto area = getLocalBounds().toFloat();
    const auto span = juce::jmax (1.0f, viewHighMidi - viewLowMidi);

    // MIDI increases upward, screen Y increases downward.
    return area.getBottom() - ((midi - viewLowMidi) / span) * area.getHeight();
}

float NoteHighwayComponent::xForTime (double t) const noexcept
{
    const auto& v = theme.get();
    const auto area = getLocalBounds().toFloat();

    const double history   = juce::jmax (0.05f, v.highwayHistorySeconds);
    const double lookahead = juce::jmax (0.05f, v.highwayLookaheadSeconds);
    const double span      = history + lookahead;

    const double fraction = ((t - nowTime) + history) / span;
    return area.getX() + (float) (fraction * area.getWidth());
}

double NoteHighwayComponent::timeForX (float x) const noexcept
{
    const auto& v = theme.get();
    const auto area = getLocalBounds().toFloat();

    if (area.getWidth() <= 0.0f)
        return nowTime;

    const double history   = juce::jmax (0.05f, v.highwayHistorySeconds);
    const double lookahead = juce::jmax (0.05f, v.highwayLookaheadSeconds);
    const double span      = history + lookahead;

    const double fraction = (double) (x - area.getX()) / (double) area.getWidth();
    return nowTime + fraction * span - history;
}

float NoteHighwayComponent::midiForY (float y) const noexcept
{
    const auto area = getLocalBounds().toFloat();

    if (area.getHeight() <= 0.0f)
        return viewLowMidi;

    const auto span = juce::jmax (1.0f, viewHighMidi - viewLowMidi);
    return viewLowMidi + ((area.getBottom() - y) / area.getHeight()) * span;
}

//==============================================================================
void NoteHighwayComponent::setEditMode (bool shouldEdit)
{
    if (editMode == shouldEdit)
        return;

    editMode = shouldEdit;

    // Off by default and restored to that: the highway is something you sing at,
    // and swallowing clicks during a take would be a surprise every time.
    setInterceptsMouseClicks (editMode, false);
    setMouseCursor (editMode ? juce::MouseCursor::PointingHandCursor
                             : juce::MouseCursor::NormalCursor);

    if (! editMode)
        selectedNoteId = -1;

    repaint();
}

void NoteHighwayComponent::setSelectedNoteId (int noteId) noexcept
{
    if (selectedNoteId == noteId)
        return;

    selectedNoteId = noteId;
    repaint();
}

int NoteHighwayComponent::noteIndexNear (juce::Point<float> position) const noexcept
{
    if (chart == nullptr || chart->isEmpty())
        return -1;

    const auto& v = theme.get();

    const double t0 = nowTime - v.highwayHistorySeconds;
    const double t1 = nowTime + v.highwayLookaheadSeconds;

    int first = 0, last = 0;

    if (! chart->notesOverlapping (t0, t1, first, last))
        return -1;

    const auto& notes = chart->getNotes();

    int best = -1;
    float bestDistance = std::numeric_limits<float>::max();

    for (int i = first; i < last; ++i)
    {
        const auto& n = notes[(size_t) i];

        const auto x0 = xForTime (n.startSeconds);
        const auto x1 = xForTime (n.endSeconds());
        const auto cy = yForMidi ((float) n.midi);

        // Distance to the bar as a rectangle, with a generous vertical reach.
        const auto dx = position.x < x0 ? x0 - position.x
                                        : (position.x > x1 ? position.x - x1 : 0.0f);
        const auto dy = std::abs (position.y - cy);

        const auto distance = dx * 2.0f + dy;    // horizontal misses cost more

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }

    // A click a long way from every note is a click on empty space, not a
    // near-miss on the nearest one.
    return bestDistance <= 60.0f ? best : -1;
}

void NoteHighwayComponent::mouseDown (const juce::MouseEvent& e)
{
    if (! editMode || onNoteClicked == nullptr)
        return;

    const auto index = noteIndexNear (e.position);

    onNoteClicked (index >= 0 ? chart->getNotes()[(size_t) index].id : -1);
}

//==============================================================================
juce::Colour NoteHighwayComponent::scoredColourFor (const NoteScore& score) const noexcept
{
    const auto& v = theme.get();

    if (! score.hasData)
        return v.noteNotSung;

    // Same Oklab ramp and the same two endpoint colours as the live trace, so
    // "green" means one thing in this app rather than two.
    return lerpOklab (v.noteScoredGood, v.noteScoredPoor,
                      juce::jlimit (0.0f, 1.0f, 1.0f - score.accuracy));
}

//==============================================================================
void NoteHighwayComponent::updateZoomTarget() noexcept
{
    if (chart == nullptr || chart->isEmpty())
        return;

    const auto& v = theme.get();

    // Dual horizon: fit everything in the inner window, then WIDEN (never
    // narrow) to also fit the outer window. That stops the view lurching when a
    // big interval arrives, while still keeping the near-term notes large.
    const auto collect = [this] (double t0, double t1, int& lo, int& hi)
    {
        int first = 0, last = 0;
        if (! chart->notesOverlapping (t0, t1, first, last))
            return false;

        const auto& notes = chart->getNotes();
        lo = hi = notes[(size_t) first].midi;

        for (int i = first; i < last; ++i)
        {
            lo = std::min (lo, notes[(size_t) i].midi);
            hi = std::max (hi, notes[(size_t) i].midi);
        }

        return true;
    };

    int lo = 0, hi = 0;
    const auto backFrom = nowTime - v.highwayHistorySeconds;

    if (! collect (backFrom, nowTime + v.zoomHorizonInnerSeconds, lo, hi))
    {
        // Nothing near the playhead (an intro, or past the end): fall back to
        // the whole chart so the view is still sensible.
        lo = chart->getLowestMidi();
        hi = chart->getHighestMidi();
    }

    int outerLo = lo, outerHi = hi;

    if (collect (backFrom, nowTime + v.zoomHorizonOuterSeconds, outerLo, outerHi))
    {
        lo = std::min (lo, outerLo);
        hi = std::max (hi, outerHi);
    }

    auto low  = (float) lo - v.zoomPaddingSemitones;
    auto high = (float) hi + v.zoomPaddingSemitones;

    // Never show fewer than minSemitonesVisible; grow symmetrically.
    const auto span = high - low;

    if (span < v.minSemitonesVisible)
    {
        const auto centre = 0.5f * (low + high);
        low  = centre - 0.5f * v.minSemitonesVisible;
        high = centre + 0.5f * v.minSemitonesVisible;
    }

    targetLowMidi  = low;
    targetHighMidi = high;
}

void NoteHighwayComponent::tickAnimation (double dt) noexcept
{
    updateZoomTarget();

    const auto& v = theme.get();

    // Frame-rate-independent exponential approach.
    const auto k = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi
                                        * v.zoomSmoothingHz * juce::jlimit (0.0, 0.25, dt));

    viewLowMidi  += (float) ((targetLowMidi  - viewLowMidi)  * k);
    viewHighMidi += (float) ((targetHighMidi - viewHighMidi) * k);

    updateReadout();
}

//==============================================================================
void NoteHighwayComponent::updateReadout() noexcept
{
    Readout r;

    const auto noteIdx = (chart != nullptr) ? chart->targetNoteIndex (nowTime) : -1;
    r.targetMidi = noteIdx >= 0 ? chart->getNotes()[(size_t) noteIdx].midi : -1;

    if (ringCount > 0)
    {
        const auto& newest = ringAt (ringCount - 1);

        r.clarity = newest.clarity;
        r.rmsDb   = newest.rmsDb;

        if (std::isfinite (newest.smoothedMidi))
        {
            r.voiced = true;

            if (r.targetMidi >= 0)
            {
                const auto folded = foldOntoTarget (newest.smoothedMidi, (float) r.targetMidi,
                                                    r.octaveOffset);
                r.cents = (folded - (float) r.targetMidi) * 100.0f;
            }
            else
            {
                // No active note — report against the nearest semitone instead,
                // so the readout still means something during rests.
                const auto nearest = std::round (newest.smoothedMidi);
                r.cents = (newest.smoothedMidi - nearest) * 100.0f;
            }
        }
    }

    readout = r;
}

//==============================================================================
void NoteHighwayComponent::resized()
{
    background.setBounds (getLocalBounds());
    bgBounds = {};      // force refresh
}

void NoteHighwayComponent::refreshBackgroundIfNeeded()
{
    // The grid is expensive (text layout per semitone) and only changes when the
    // zoom settles or the component resizes. A 0.02-semitone deadband stops the
    // animation repainting it on every single frame.
    const auto moved = std::abs (bgLowMidi  - viewLowMidi)  > 0.02f
                    || std::abs (bgHighMidi - viewHighMidi) > 0.02f;

    if (moved || bgBounds != getLocalBounds())
    {
        bgLowMidi  = viewLowMidi;
        bgHighMidi = viewHighMidi;
        bgBounds   = getLocalBounds();
        background.repaint();
    }
}

//==============================================================================
void NoteHighwayComponent::BackgroundLayer::paint (juce::Graphics& g)
{
    const auto& v = owner.theme.get();
    const auto area = getLocalBounds().toFloat();

    g.fillAll (v.background);

    const auto lo = (int) std::floor (owner.viewLowMidi);
    const auto hi = (int) std::ceil  (owner.viewHighMidi);

    // Guard against a pathological zoom producing thousands of lines.
    if (hi - lo > 96)
        return;

    g.setFont (juce::FontOptions ((float) v.gridLabelHeightPx));

    for (int midi = lo; midi <= hi; ++midi)
    {
        const auto y = owner.yForMidi ((float) midi);

        if (y < area.getY() - 2.0f || y > area.getBottom() + 2.0f)
            continue;

        const bool isC = (((midi % 12) + 12) % 12) == 0;

        g.setColour (isC ? v.gridLineOctave : v.gridLine);
        g.fillRect (area.getX(), y - 0.5f, area.getWidth(),
                    isC ? v.gridLineOctaveThickPx : v.gridLineThicknessPx);

        if (isC || (hi - lo) <= 24)
        {
            g.setColour (v.gridLabel);
            g.drawText (midiName (midi),
                        juce::Rectangle<float> (area.getX() + 4.0f, y - v.gridLabelHeightPx,
                                                46.0f, v.gridLabelHeightPx),
                        juce::Justification::centredLeft, false);
        }
    }
}

//==============================================================================
void NoteHighwayComponent::paintGhostContour (juce::Graphics& g, juce::Rectangle<float> area)
{
    if (chart == nullptr)
        return;

    const auto& contour = chart->getContour();

    if (contour.isEmpty())
        return;

    const auto& v = theme.get();

    if (v.ghostContourAlpha <= 0.0f)
        return;

    const double t0 = nowTime - v.highwayHistorySeconds;
    const double t1 = nowTime + v.highwayLookaheadSeconds;

    const auto i0 = juce::jmax (0, (int) std::floor ((t0 - contour.startSeconds) * contour.rateHz));
    const auto i1 = juce::jmin ((int) contour.fracMidi.size() - 1,
                                (int) std::ceil ((t1 - contour.startSeconds) * contour.rateHz));

    if (i1 <= i0)
        return;

    ghostPath.clear();
    bool penDown = false;
    float prevMidi = kNaNf;

    for (int i = i0; i <= i1; ++i)
    {
        const auto m = contour.fracMidi[(size_t) i];

        // NaN == unvoiced. Also break on an implausible jump, exactly as the
        // live trace does, so gaps read as gaps rather than vertical streaks.
        if (! std::isfinite (m)
            || (penDown && std::isfinite (prevMidi) && std::abs (m - prevMidi) > 1.0f))
        {
            penDown = false;
            prevMidi = m;
            continue;
        }

        const auto t = contour.startSeconds + (double) i / contour.rateHz;
        const auto x = xForTime (t);
        const auto y = yForMidi (m);

        if (! penDown)
        {
            ghostPath.startNewSubPath (x, y);
            penDown = true;
        }
        else
        {
            ghostPath.lineTo (x, y);
        }

        prevMidi = m;
    }

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.toNearestInt());
    g.setColour (v.ghostContour.withAlpha (v.ghostContourAlpha));
    g.strokePath (ghostPath, juce::PathStrokeType (v.ghostContourThicknessPx));
}

//==============================================================================
void NoteHighwayComponent::paintNotes (juce::Graphics& g, juce::Rectangle<float> area)
{
    if (chart == nullptr || chart->isEmpty())
        return;

    const auto& v = theme.get();

    const double t0 = nowTime - v.highwayHistorySeconds;
    const double t1 = nowTime + v.highwayLookaheadSeconds;

    int first = 0, last = 0;

    if (! chart->notesOverlapping (t0, t1, first, last))
        return;

    const auto& notes = chart->getNotes();

    // Semitones per pixel, so the bar height can express a cents window.
    const auto span = juce::jmax (1.0f, viewHighMidi - viewLowMidi);
    const auto pxPerSemitone = area.getHeight() / span;

    // THE BAR'S HEIGHT IS THE FULL-CREDIT WINDOW: +/- tFull cents.
    const auto barHeight  = juce::jmax (3.0f, 2.0f * (tFull / 100.0f) * pxPerSemitone);
    const auto haloHeight = juce::jmax (barHeight, 2.0f * (tZero / 100.0f) * pxPerSemitone);

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.toNearestInt());

    for (int i = first; i < last; ++i)
    {
        const auto& n = notes[(size_t) i];

        const auto x0 = xForTime (n.startSeconds);
        const auto x1 = xForTime (n.endSeconds());
        const auto cy = yForMidi ((float) n.midi);

        const auto w = juce::jmax (2.0f, x1 - x0);
        const auto active = n.containsTime (nowTime);

        //--------------------------------------------------------------------
        // Has this note been SUNG AND FINISHED?
        //
        // Only then is it recoloured. While the note is active the singer is
        // reading the trace against the bar — that geometry is the primary
        // signal (DESIGN.md section 7) and a bar changing colour underneath the
        // dot would both compete with it and flicker as the running average
        // moved. The instant it is over, the geometry has scrolled into history
        // and the colour is the only thing left to read.
        //--------------------------------------------------------------------
        const bool finished = n.endSeconds() <= nowTime;

        const NoteScore* score = nullptr;

        if (v.showScoredNotes && scorer != nullptr && finished && isPitchScored (n.type))
        {
            const auto& s = scorer->getNoteScore (i);

            // An unsung note is only reported as such once the take has actually
            // started. Before that every note is "unsung", and painting the
            // whole chart grey would be both useless and discouraging.
            if (s.hasData || scorer->hasAnyData())
                score = &s;
        }

        // Partial-credit halo behind the bar. Suppressed on a scored note: its
        // job is to show where partial credit WOULD be earned, and once the note
        // is scored the answer is on the bar itself.
        if (v.haloAlpha > 0.0f && haloHeight > barHeight && score == nullptr)
        {
            g.setColour ((active ? v.targetBarActive : v.targetBarUpcoming).withAlpha (v.haloAlpha));
            g.fillRoundedRectangle (x0, cy - haloHeight * 0.5f, w, haloHeight,
                                    v.noteBarCornerRadiusPx);
        }

        auto barColour = v.targetBarUpcoming;

        if (n.type == NoteType::golden)
            barColour = v.goldenNote;

        if (active)
            barColour = (n.type == NoteType::golden) ? v.goldenNote : v.targetBarActive;

        // Notes whose pitch is not scored are drawn hollow, so the singer can
        // see there is nothing to aim at.
        if (! isPitchScored (n.type))
        {
            g.setColour (barColour.withAlpha (v.upcomingBarAlpha * 0.6f));
            g.drawRoundedRectangle (x0, cy - barHeight * 0.5f, w, barHeight,
                                    v.noteBarCornerRadiusPx, v.targetBarBorderPx);

            if (n.id == selectedNoteId)
                paintSelection (g, x0, cy, w, haloHeight);

            continue;
        }

        if (score != nullptr)
        {
            const auto scored = scoredColourFor (*score);

            if (! score->hasData)
            {
                // NEVER SUNG. Hollow and grey — visibly a different outcome from
                // "sung badly", which is solid and vermilion. Conflating the two
                // is how feedback starts lying: it would report a note the singer
                // had not reached yet as a note they got wrong.
                g.setColour (scored.withAlpha (0.75f));
                g.drawRoundedRectangle (x0, cy - barHeight * 0.5f, w, barHeight,
                                        v.noteBarCornerRadiusPx,
                                        juce::jmax (1.0f, v.targetBarBorderPx));
            }
            else
            {
                // The bar is drawn at full length in the accuracy colour, and
                // the part actually SUNG is filled solid. So a note sung well
                // but only half way through reads as half-filled green rather
                // than as either a clean pass or a failure.
                g.setColour (scored.withAlpha (v.scoredBarAlpha * 0.35f));
                g.fillRoundedRectangle (x0, cy - barHeight * 0.5f, w, barHeight,
                                        v.noteBarCornerRadiusPx);

                const auto sungWidth = juce::jlimit (0.0f, w, w * score->coverage);

                if (sungWidth > 0.5f)
                {
                    g.setColour (scored.withAlpha (v.scoredBarAlpha));
                    g.fillRoundedRectangle (x0, cy - barHeight * 0.5f, sungWidth, barHeight,
                                            v.noteBarCornerRadiusPx);
                }
            }

            if (n.id == selectedNoteId)
                paintSelection (g, x0, cy, w, haloHeight);

            continue;
        }

        g.setColour (active ? barColour : barColour.withAlpha (v.upcomingBarAlpha));
        g.fillRoundedRectangle (x0, cy - barHeight * 0.5f, w, barHeight,
                                v.noteBarCornerRadiusPx);

        if (v.targetBarBorderPx > 0.0f && ! active)
        {
            g.setColour (v.targetBarActive.withAlpha (0.55f));
            g.drawRoundedRectangle (x0, cy - barHeight * 0.5f, w, barHeight,
                                    v.noteBarCornerRadiusPx, v.targetBarBorderPx);
        }

        if (n.id == selectedNoteId)
            paintSelection (g, x0, cy, w, haloHeight);
    }
}

//==============================================================================
void NoteHighwayComponent::paintSelection (juce::Graphics& g, float x0, float cy,
                                           float w, float haloHeight)
{
    const auto& v = theme.get();

    // Drawn OUTSIDE the bar rather than over it, so the note's own colour — and
    // therefore its score — stays readable while it is selected for editing.
    const auto h = juce::jmax (haloHeight, 10.0f) + 6.0f;

    g.setColour (v.editSelection);
    g.drawRoundedRectangle (x0 - 3.0f, cy - h * 0.5f, w + 6.0f, h,
                            v.noteBarCornerRadiusPx + 2.0f, 1.5f);
}

//==============================================================================
void NoteHighwayComponent::paintTrace (juce::Graphics& g, juce::Rectangle<float> area)
{
    if (ringCount <= 0)
        return;

    const auto& v = theme.get();

    const double oldest = nowTime - v.highwayHistorySeconds;
    const auto targetMidi = readout.targetMidi;

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.toNearestInt());

    //-------------------------------------------------------------- trail
    // Drawn in a small number of alpha bands so the whole trail costs a handful
    // of strokePath calls instead of one per segment.
    const auto bands = juce::jmax (1, v.trailAlphaBands);
    const double trailSeconds = v.trailMs / 1000.0;

    for (int band = 0; band < bands; ++band)
    {
        const auto bandNewer = (double) band / bands;
        const auto bandOlder = (double) (band + 1) / bands;

        // band 0 is newest and most opaque.
        const auto alpha = v.trailStartAlpha * (1.0f - (float) bandNewer);

        if (alpha <= 0.004f)
            continue;

        tracePath.clear();
        bool penDown = false;
        float prevY = 0.0f, prevMidi = kNaNf;

        for (int i = 0; i < ringCount; ++i)
        {
            const auto& s = ringAt (i);

            // Age is measured in STREAM time — the clock the samples are stamped
            // in. Measuring it against chart time is what made the whole trace
            // vanish as soon as a chart was loaded: the chart clock comes from
            // the backing track and has no relationship to the stream clock.
            const auto age = streamNow - s.timeSeconds;

            if (age < bandNewer * trailSeconds || age > bandOlder * trailSeconds)
            {
                penDown = false;
                continue;
            }

            const auto chartT = chartTimeFor (s.timeSeconds);

            if (chartT < oldest || ! std::isfinite (s.rawMidi))
            {
                penDown = false;
                prevMidi = kNaNf;
                continue;
            }

            int oct = 0;
            const auto shown = targetMidi >= 0
                                 ? foldOntoTarget (s.rawMidi, (float) targetMidi, oct)
                                 : s.rawMidi;

            // Break on an implausible jump — consonants and octave flips would
            // otherwise draw vertical streaks that read as error.
            if (penDown && std::isfinite (prevMidi)
                && std::abs (shown - prevMidi) > v.traceBreakSemitones)
                penDown = false;

            const auto x = xForTime (chartT);
            const auto y = yForMidi (shown);

            if (! penDown)
            {
                tracePath.startNewSubPath (x, y);
                penDown = true;
            }
            else
            {
                tracePath.lineTo (x, y);
            }

            prevY = y;
            prevMidi = shown;
        }

        juce::ignoreUnused (prevY);

        if (! tracePath.isEmpty())
        {
            g.setColour (v.traceInTune.withAlpha (alpha));
            g.strokePath (tracePath, juce::PathStrokeType (v.trailThicknessPx,
                                                           juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));
        }
    }

    //---------------------------------------------------------------- dot
    const auto& newest = ringAt (ringCount - 1);

    if (! std::isfinite (newest.smoothedMidi))
        return;

    int oct = 0;
    const auto shown = targetMidi >= 0
                         ? foldOntoTarget (newest.smoothedMidi, (float) targetMidi, oct)
                         : newest.smoothedMidi;

    const auto x = xForTime (chartTimeFor (newest.timeSeconds));
    const auto y = yForMidi (shown);

    // Colour by how far out of tune, interpolated in Oklab so the midpoint does
    // not go muddy. Geometry (inside the bar) is still the primary signal.
    const auto absCents = std::abs (readout.cents);
    const auto tRamp = juce::jlimit (0.0f, 1.0f, (absCents - tFull) / juce::jmax (1.0f, tZero - tFull));
    const auto dotColour = lerpOklab (v.traceInTune, v.traceOutOfTune, tRamp);

    const auto d = v.dotDiameterPx;

    g.setColour (dotColour);
    g.fillEllipse (x - d * 0.5f, y - d * 0.5f, d, d);

    g.setColour (dotColour.withAlpha (0.55f));
    g.drawEllipse (x - d * 0.5f - v.dotRingPx, y - d * 0.5f - v.dotRingPx,
                   d + 2.0f * v.dotRingPx, d + 2.0f * v.dotRingPx, v.dotRingPx);
}

//==============================================================================
void NoteHighwayComponent::paint (juce::Graphics& g)
{
    // The grid child covers the whole component and is painted AFTER this, so
    // nothing drawn here would survive. Only two things belong in paint():
    //
    //   1. a background fill, so there is never a flash of undefined pixels on
    //      the very first frame before the buffered child has rendered, and
    //   2. the staleness check that tells the grid child to redraw.
    //
    // The highway itself is in paintOverChildren(). See the header.
    g.fillAll (theme.get().background);

    refreshBackgroundIfNeeded();
}

void NoteHighwayComponent::paintOverChildren (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const auto& v = theme.get();

    // The grid child has already painted; everything below goes ON TOP of it.
    paintGhostContour (g, area);
    paintNotes (g, area);
    paintTrace (g, area);

    //----------------------------------------------------------- now-line
    const auto nowX = xForTime (nowTime);

    g.setColour (v.nowLine);
    g.fillRect (nowX - 1.0f, area.getY(), 2.0f, area.getHeight());

    //-------------------------------------------------- octave badge
    if (readout.voiced && readout.octaveOffset != 0)
    {
        const auto label = juce::String (readout.octaveOffset > 0 ? "+" : "")
                             + juce::String (readout.octaveOffset) + " 8ve";

        g.setColour (v.textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (label,
                    juce::Rectangle<float> (nowX + 6.0f, area.getY() + 4.0f, 70.0f, 14.0f),
                    juce::Justification::centredLeft, false);
    }

    //-------------------------------------------------- empty state
    if (chart == nullptr || chart->isEmpty())
    {
        g.setColour (v.textDim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("No chart loaded  -  build one with:  vocalchart build songs/<id>",
                    getLocalBounds(), juce::Justification::centred, false);
    }
}

} // namespace vt
