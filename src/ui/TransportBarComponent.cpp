#include "TransportBarComponent.h"

#include <algorithm>
#include <cmath>

namespace vt
{

namespace
{
    /** Height of the loop lane along the top, in px. Big enough to hit without
        aiming, small enough that it never looks like the transport itself. */
    constexpr float kLoopLaneHeight = 10.0f;

    /** How close to a loop edge counts as grabbing that edge rather than
        starting a new region. Generous: the edges are 2 px wide and nobody is
        going to hit that on purpose. */
    constexpr float kEdgeGrabPx = 7.0f;

    /** A drag shorter than this is a click. Below it, a press in the loop lane
        clears the loop instead of creating a zero-length one. */
    constexpr float kClickSlopPx = 3.0f;

    /** Shortest loop the bar will create. A two-frame loop is not a section,
        it is a mistake, and it would seek every single block. */
    constexpr double kMinLoopSeconds = 0.25;

    juce::String clock (double seconds)
    {
        const auto total = juce::jmax (0, juce::roundToInt (seconds));
        return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
    }
}

//==============================================================================
TransportBarComponent::TransportBarComponent (Theme& t)
    : theme (t)
{
    addAndMakeVisible (track);
    setOpaque (true);

    setTooltip ("Click or drag to move through the song.  Drag the thin lane along "
                "the top to loop a section; click it to clear the loop.");
}

TransportBarComponent::~TransportBarComponent() = default;

//==============================================================================
void TransportBarComponent::applyTheme()
{
    densityWidth = -1;      // force the buffered layer to rebuild
    repaint();
}

void TransportBarComponent::setChart (const Chart* c)
{
    chart = c;

    // A new song has a new shape and its own length; a loop measured against
    // the previous one would point at the wrong music.
    loopStart = loopEnd = 0.0;
    loopEnabled = false;
    position = 0.0;

    durationSeconds = (chart != nullptr) ? chart->getDurationSeconds() : 0.0;

    densityWidth = -1;
    track.repaint();
    repaint();
}

void TransportBarComponent::setDurationSeconds (double seconds)
{
    if (! (seconds > 0.0) || juce::approximatelyEqual (seconds, durationSeconds))
        return;

    durationSeconds = seconds;
    densityWidth = -1;
    track.repaint();
    repaint();
}

void TransportBarComponent::setPosition (double seconds)
{
    // A pixel is worth several hundred milliseconds on a four-minute song, so
    // repainting on every sub-pixel move is repainting for nothing.
    if (durationSeconds > 0.0 && getWidth() > 0)
    {
        const auto perPixel = durationSeconds / (double) getWidth();

        if (std::abs (seconds - position) < perPixel * 0.5)
            return;
    }

    position = seconds;
    repaint();
}

void TransportBarComponent::setLoop (double startSeconds, double endSeconds, bool enabled)
{
    loopStart = juce::jmax (0.0, startSeconds);
    loopEnd   = juce::jmax (0.0, endSeconds);
    loopEnabled = enabled && loopEnd > loopStart;

    repaint();
}

//==============================================================================
float TransportBarComponent::xForTime (double seconds) const noexcept
{
    const auto area = getLocalBounds().toFloat();

    if (durationSeconds <= 0.0)
        return area.getX();

    const auto fraction = juce::jlimit (0.0, 1.0, seconds / durationSeconds);
    return area.getX() + (float) (fraction * area.getWidth());
}

double TransportBarComponent::timeForX (float x) const noexcept
{
    const auto area = getLocalBounds().toFloat();

    if (durationSeconds <= 0.0 || area.getWidth() <= 0.0f)
        return 0.0;

    const auto fraction = juce::jlimit (0.0f, 1.0f,
                                        (x - area.getX()) / area.getWidth());

    return (double) fraction * durationSeconds;
}

juce::Rectangle<float> TransportBarComponent::getLoopLane() const noexcept
{
    return getLocalBounds().toFloat().removeFromTop (kLoopLaneHeight);
}

juce::Rectangle<float> TransportBarComponent::getScrubArea() const noexcept
{
    return getLocalBounds().toFloat().withTrimmedTop (kLoopLaneHeight);
}

//==============================================================================
void TransportBarComponent::resized()
{
    track.setBounds (getLocalBounds());

    if (densityWidth != getWidth())
        rebuildDensity();
}

void TransportBarComponent::rebuildDensity()
{
    densityWidth = getWidth();
    densityChart = chart;
    density.assign ((size_t) juce::jmax (0, densityWidth), 0.0f);

    if (chart == nullptr || chart->isEmpty() || durationSeconds <= 0.0 || densityWidth <= 0)
        return;

    const auto perColumn = durationSeconds / (double) densityWidth;

    for (int x = 0; x < densityWidth; ++x)
    {
        const auto t0 = (double) x * perColumn;
        const auto t1 = t0 + perColumn;

        int first = 0, last = 0;

        if (! chart->notesOverlapping (t0, t1, first, last))
            continue;

        double covered = 0.0;

        for (int i = first; i < last; ++i)
        {
            const auto& n = chart->getNotes()[(size_t) i];
            covered += juce::jmax (0.0, std::min (n.endSeconds(), t1) - std::max (n.startSeconds, t0));
        }

        density[(size_t) x] = (float) juce::jlimit (0.0, 1.0, covered / perColumn);
    }
}

//==============================================================================
void TransportBarComponent::TrackLayer::paint (juce::Graphics& g)
{
    const auto& v = owner.theme.get();
    const auto area = getLocalBounds().toFloat();

    g.fillAll (v.transportTrack);

    const auto scrub = owner.getScrubArea();

    //--------------------------------------------------------------- density
    // Drawn from the middle outwards, so a dense passage reads as a thick band
    // and a rest as a thin one. Growing from the bottom would make the top edge
    // a second, meaningless outline of the same data.
    if (owner.densityWidth == owner.getWidth() && ! owner.density.empty())
    {
        const auto midY = scrub.getCentreY();
        const auto maxHalf = scrub.getHeight() * 0.42f;

        g.setColour (v.transportDensity);

        for (int x = 0; x < (int) owner.density.size(); ++x)
        {
            const auto d = owner.density[(size_t) x];

            if (d <= 0.0f)
                continue;

            // A floor, so a single short note in a bar is still visible rather
            // than a sub-pixel smudge that reads as silence.
            const auto half = juce::jmax (1.0f, maxHalf * juce::jmax (0.18f, d));

            g.fillRect ((float) x + area.getX(), midY - half, 1.0f, half * 2.0f);
        }
    }

    //------------------------------------------------------------ loop lane
    g.setColour (v.gridLine);
    g.fillRect (area.getX(), kLoopLaneHeight - 1.0f, area.getWidth(), 1.0f);

    //---------------------------------------------------------------- times
    if (owner.durationSeconds <= 0.0)
        return;

    g.setFont (juce::FontOptions (juce::jmin (11.0f, scrub.getHeight() - 2.0f)));
    g.setColour (v.textDim);

    g.drawText (clock (owner.durationSeconds),
                scrub.reduced (5.0f, 0.0f), juce::Justification::centredRight, false);
}

//==============================================================================
void TransportBarComponent::paint (juce::Graphics& g)
{
    // The track layer is a CHILD and is painted after this, so nothing drawn
    // here survives. Fill only — see NoteHighwayComponent.h for the full story.
    g.fillAll (theme.get().transportTrack);

    if (densityWidth != getWidth() || densityChart != chart)
    {
        rebuildDensity();
        track.repaint();
    }
}

void TransportBarComponent::paintOverChildren (juce::Graphics& g)
{
    const auto& v = theme.get();
    const auto area = getLocalBounds().toFloat();

    if (durationSeconds <= 0.0)
    {
        g.setColour (v.textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("No song loaded", area, juce::Justification::centred, false);
        return;
    }

    const auto lane = getLoopLane();

    //---------------------------------------------------------- loop region
    if (hasLoopRegion())
    {
        const auto x0 = xForTime (loopStart);
        const auto x1 = xForTime (loopEnd);

        // A disabled region is drawn much fainter but is still drawn: it is
        // what the Loop button re-arms, and a control whose target is invisible
        // is a control that appears to do nothing.
        const auto alpha = loopEnabled ? v.loopRegionAlpha : v.loopRegionAlpha * 0.4f;

        g.setColour (v.loopRegion.withAlpha (alpha));
        g.fillRect (x0, area.getY(), juce::jmax (1.0f, x1 - x0), area.getHeight());

        g.setColour (v.loopRegion.withAlpha (loopEnabled ? 0.95f : 0.45f));
        g.fillRect (x0, lane.getY(), juce::jmax (1.0f, x1 - x0), lane.getHeight() - 1.0f);

        // Edge grips, so the two draggable pixels look draggable.
        g.fillRect (x0 - 1.0f, area.getY(), 2.0f, area.getHeight());
        g.fillRect (x1 - 1.0f, area.getY(), 2.0f, area.getHeight());
    }

    //-------------------------------------------------------------- playhead
    const auto px = xForTime (position);

    g.setColour (v.transportPlayhead);
    g.fillRect (px - 1.0f, area.getY(), 2.0f, area.getHeight());

    //------------------------------------------------------------- position
    const auto scrub = getScrubArea();

    g.setFont (juce::FontOptions (juce::jmin (11.0f, scrub.getHeight() - 2.0f)));
    g.setColour (v.text);
    g.drawText (clock (position), scrub.reduced (5.0f, 0.0f),
                juce::Justification::centredLeft, false);
}

//==============================================================================
TransportBarComponent::Grab TransportBarComponent::grabAt (juce::Point<float> p) const noexcept
{
    if (durationSeconds <= 0.0)
        return Grab::none;

    if (! getLoopLane().contains (p))
        return Grab::seek;

    if (hasLoopRegion())
    {
        if (std::abs (p.x - xForTime (loopStart)) <= kEdgeGrabPx) return Grab::loopStart;
        if (std::abs (p.x - xForTime (loopEnd))   <= kEdgeGrabPx) return Grab::loopEnd;
    }

    return Grab::loopNew;
}

void TransportBarComponent::publishLoop()
{
    repaint();

    if (onLoopChanged != nullptr)
        onLoopChanged (loopStart, loopEnd);
}

//==============================================================================
void TransportBarComponent::mouseDown (const juce::MouseEvent& e)
{
    dragging = grabAt (e.position);
    dragMoved = false;

    const auto t = timeForX (e.position.x);

    switch (dragging)
    {
        case Grab::seek:
            if (onSeek != nullptr)
                onSeek (t);
            break;

        case Grab::loopStart:
            loopAnchor = loopEnd;       // drag the start, hold the end
            break;

        case Grab::loopEnd:
            loopAnchor = loopStart;
            break;

        case Grab::loopNew:
            loopAnchor = t;
            break;

        case Grab::none:
        default:
            break;
    }
}

void TransportBarComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > kClickSlopPx)
        dragMoved = true;

    const auto t = timeForX (e.position.x);

    if (dragging == Grab::seek)
    {
        if (onSeek != nullptr)
            onSeek (t);

        return;
    }

    if (dragging == Grab::none || ! dragMoved)
        return;

    // Every loop gesture is "one end is fixed at the anchor, the other follows
    // the pointer", so dragging back past the anchor swaps the two rather than
    // producing an inverted region the transport would never satisfy.
    loopStart = std::min (loopAnchor, t);
    loopEnd   = std::max (loopAnchor, t);

    if (loopEnd - loopStart < kMinLoopSeconds)
        loopEnd = loopStart + kMinLoopSeconds;

    loopEnabled = true;
    publishLoop();
}

void TransportBarComponent::mouseUp (const juce::MouseEvent&)
{
    const auto wasDragging = dragging;
    const auto moved = dragMoved;

    dragging = Grab::none;
    dragMoved = false;

    // A click in the loop lane with no drag clears the loop. It is the only
    // gesture in the bar that destroys anything, which is why it is the one
    // that cannot happen by accident while seeking.
    if (wasDragging == Grab::loopNew && ! moved && hasLoopRegion())
    {
        loopStart = loopEnd = 0.0;
        loopEnabled = false;
        publishLoop();
    }
}

void TransportBarComponent::mouseMove (const juce::MouseEvent& e)
{
    switch (grabAt (e.position))
    {
        case Grab::loopStart:
        case Grab::loopEnd:
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            break;

        case Grab::none:
            setMouseCursor (juce::MouseCursor::NormalCursor);
            break;

        default:
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            break;
    }
}

} // namespace vt
