#include "PianoKeyboardComponent.h"

#include "NoteNaming.h"

#include <cmath>

namespace vt
{

namespace
{
    /** Below this many pixels per semitone a label does not fit at all; between
        this and kLabelAllPx only C is named, as on a piano roll. Above it, every
        white key is named — which is the beginner case this exists for. */
    constexpr float kLabelCPx   = 8.0f;
    constexpr float kLabelAllPx = 15.0f;

    /** A pathological zoom must not try to draw a thousand keys. */
    constexpr int kMaxKeys = 128;
}

//==============================================================================
PianoKeyboardComponent::PianoKeyboardComponent (Theme& t)
    : theme (t)
{
    addAndMakeVisible (keys);
    setOpaque (true);

    // Clicking a key plays it, so the cursor has to say so.
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (false);
}

PianoKeyboardComponent::~PianoKeyboardComponent() = default;

//==============================================================================
void PianoKeyboardComponent::applyTheme()
{
    keysLowMidi = keysHighMidi = -1.0f;    // force the buffered layer to rebuild
    repaint();
}

//==============================================================================
bool PianoKeyboardComponent::isBlackKey (int midi) noexcept
{
    switch (((midi % 12) + 12) % 12)
    {
        case 1: case 3: case 6: case 8: case 10:  return true;
        default:                                  return false;
    }
}

float PianoKeyboardComponent::yForMidi (float midi) const noexcept
{
    // Identical in form to NoteHighwayComponent::yForMidi and
    // PitchLaneComponent::yForMidi, over this component's own bounds — which
    // are laid out with the same top and height as theirs.
    const auto area = getLocalBounds().toFloat();
    const auto span = juce::jmax (1.0f, viewHighMidi - viewLowMidi);

    return area.getBottom() - ((midi - viewLowMidi) / span) * area.getHeight();
}

int PianoKeyboardComponent::midiForY (float y) const noexcept
{
    const auto area = getLocalBounds().toFloat();

    if (area.getHeight() <= 0.0f)
        return -1;

    const auto span = juce::jmax (1.0f, viewHighMidi - viewLowMidi);
    const auto midi = viewLowMidi + ((area.getBottom() - y) / area.getHeight()) * span;

    const auto rounded = juce::roundToInt (midi);

    return (rounded >= 0 && rounded <= 127) ? rounded : -1;
}

juce::Range<float> PianoKeyboardComponent::slotFor (int midi) const noexcept
{
    return { yForMidi ((float) midi + 0.5f), yForMidi ((float) midi - 0.5f) };
}

//==============================================================================
void PianoKeyboardComponent::setViewRange (float lowMidi, float highMidi)
{
    if (highMidi <= lowMidi)
        return;

    if (juce::approximatelyEqual (viewLowMidi, lowMidi)
        && juce::approximatelyEqual (viewHighMidi, highMidi))
        return;

    viewLowMidi  = lowMidi;
    viewHighMidi = highMidi;

    repaint();
}

void PianoKeyboardComponent::setTargetMidi (int midi)
{
    if (targetMidi == midi)
        return;

    targetMidi = midi;
    repaint();
}

void PianoKeyboardComponent::setSungMidi (float fracMidi)
{
    // NaN != NaN, so an explicit both-unvoiced test is needed or every silent
    // frame would count as a change and repaint.
    const bool wasVoiced = std::isfinite (sungMidi);
    const bool isVoiced  = std::isfinite (fracMidi);

    if (! wasVoiced && ! isVoiced)
        return;

    if (wasVoiced && isVoiced && std::abs (sungMidi - fracMidi) < 0.005f)
        return;

    sungMidi = fracMidi;
    repaint();
}

//==============================================================================
void PianoKeyboardComponent::resized()
{
    keys.setBounds (getLocalBounds());
    keysBounds = {};      // force a rebuild
}

void PianoKeyboardComponent::refreshKeysIfNeeded()
{
    // Same 0.02-semitone deadband as the highway's grid: the zoom animation
    // settles asymptotically and would otherwise rebuild this layer for a
    // difference no one can see, on every frame, forever.
    const auto moved = std::abs (keysLowMidi  - viewLowMidi)  > 0.02f
                    || std::abs (keysHighMidi - viewHighMidi) > 0.02f;

    if (moved || keysBounds != getLocalBounds())
    {
        keysLowMidi  = viewLowMidi;
        keysHighMidi = viewHighMidi;
        keysBounds   = getLocalBounds();
        keys.repaint();
    }
}

//==============================================================================
void PianoKeyboardComponent::KeyLayer::paint (juce::Graphics& g)
{
    const auto& v = owner.theme.get();
    const auto area = getLocalBounds().toFloat();

    g.fillAll (v.background);

    const auto lo = (int) std::floor (owner.viewLowMidi  - 0.5f);
    const auto hi = (int) std::ceil  (owner.viewHighMidi + 0.5f);

    if (hi - lo > kMaxKeys)
        return;

    const auto blackWidth = juce::jmax (4.0f, area.getWidth() * v.pianoBlackKeyWidth);
    const auto pxPerSemitone = std::abs (owner.yForMidi (1.0f) - owner.yForMidi (0.0f));

    //------------------------------------------------------------ white keys
    // Every slot, black ones included, so there is no background showing
    // through between a black key and the white key below it.
    for (int midi = lo; midi <= hi; ++midi)
    {
        const auto slot = owner.slotFor (midi);

        g.setColour (v.pianoWhiteKey);
        g.fillRect (area.getX(), slot.getStart(), area.getWidth(), slot.getLength());

        g.setColour (v.pianoKeyBorder);
        g.fillRect (area.getX(), slot.getStart(), area.getWidth(), 1.0f);
    }

    //------------------------------------------------------------ black keys
    for (int midi = lo; midi <= hi; ++midi)
    {
        if (! isBlackKey (midi))
            continue;

        const auto slot = owner.slotFor (midi);

        g.setColour (v.pianoBlackKey);
        g.fillRect (area.getX(), slot.getStart(), blackWidth, slot.getLength());
    }

    //---------------------------------------------------------------- labels
    if (pxPerSemitone < kLabelCPx)
        return;

    const auto labelAll = pxPerSemitone >= kLabelAllPx;
    const auto h = juce::jmin (pxPerSemitone - 1.0f, (float) v.gridLabelHeightPx);

    g.setFont (juce::FontOptions (juce::jmax (7.0f, h)));

    for (int midi = lo; midi <= hi; ++midi)
    {
        if (isBlackKey (midi))
            continue;

        const bool isC = (((midi % 12) + 12) % 12) == 0;

        if (! isC && ! labelAll)
            continue;

        const auto slot = owner.slotFor (midi);

        // Right-aligned, against the highway. The left of a white key is
        // covered by the black key above it on five notes out of seven, and a
        // label that is legible for C and D but clipped for E is worse than no
        // label at all.
        g.setColour (v.pianoLabel);
        g.drawText (midiNoteName (midi),
                    juce::Rectangle<float> (area.getX(), slot.getStart(),
                                            area.getWidth() - 4.0f, slot.getLength()),
                    juce::Justification::centredRight, false);
    }
}

//==============================================================================
void PianoKeyboardComponent::paint (juce::Graphics& g)
{
    // The key layer is a CHILD and JUCE paints children after the parent, so
    // anything drawn here would be covered a moment later. Only the fill that
    // stops a flash of undefined pixels on the first frame, and the staleness
    // check. See NoteHighwayComponent.h — this is the same trap.
    g.fillAll (theme.get().background);

    refreshKeysIfNeeded();
}

void PianoKeyboardComponent::paintOverChildren (juce::Graphics& g)
{
    const auto& v = theme.get();
    const auto area = getLocalBounds().toFloat();

    const auto lo = (int) std::floor (viewLowMidi  - 0.5f);
    const auto hi = (int) std::ceil  (viewHighMidi + 0.5f);

    const auto fillKey = [&] (int midi, juce::Colour colour)
    {
        if (midi < lo || midi > hi)
            return;

        const auto slot = slotFor (midi);

        g.setColour (colour);
        g.fillRect (area.getX(), slot.getStart(), area.getWidth(), slot.getLength());
    };

    //--------------------------------------------------------------- hover
    if (hoverMidi >= 0 && hoverMidi != pressedMidi)
        fillKey (hoverMidi, v.text.withAlpha (0.10f));

    //-------------------------------------------------------------- target
    // The note the chart wants. Same colour as the active bar on the highway,
    // so the two read as one statement rather than two.
    if (targetMidi >= 0)
        fillKey (targetMidi, v.targetBarActive.withAlpha (0.55f));

    //------------------------------------------------------------- pressed
    if (pressedMidi >= 0)
        fillKey (pressedMidi, v.goldenNote.withAlpha (0.75f));

    //--------------------------------------------------- the singer's pitch
    // A NEUTRAL marker, deliberately. Whether the singer is in tune is what the
    // bar and the trace on the highway say, in geometry, and saying it a second
    // time here in colour would be a second thing to learn that adds nothing.
    // This one answers a different question: which key am I actually on?
    if (std::isfinite (sungMidi) && sungMidi >= (float) lo && sungMidi <= (float) hi)
    {
        const auto y = yForMidi (sungMidi);
        const auto size = juce::jlimit (4.0f, 9.0f, area.getWidth() * 0.14f);

        juce::Path pointer;
        pointer.startNewSubPath (area.getRight(), y - size);
        pointer.lineTo (area.getRight() - size, y);
        pointer.lineTo (area.getRight(), y + size);
        pointer.closeSubPath();

        g.setColour (v.text);
        g.fillPath (pointer);
    }

    //-------------------------------------------------- edge against the lane
    g.setColour (v.gridLine);
    g.fillRect (area.getRight() - 1.0f, area.getY(), 1.0f, area.getHeight());
}

//==============================================================================
void PianoKeyboardComponent::pressKey (int midi)
{
    if (midi < 0 || midi == pressedMidi)
        return;

    pressedMidi = midi;
    repaint();

    if (onKeyPressed != nullptr)
        onKeyPressed (midi);
}

void PianoKeyboardComponent::mouseDown (const juce::MouseEvent& e)
{
    pressKey (midiForY (e.position.y));
}

void PianoKeyboardComponent::mouseDrag (const juce::MouseEvent& e)
{
    // A drag down the keyboard plays each key it crosses, which is how anyone
    // who has met a piano expects to find a note they cannot name.
    pressKey (midiForY (e.position.y));
}

void PianoKeyboardComponent::mouseUp (const juce::MouseEvent&)
{
    if (pressedMidi < 0)
        return;

    pressedMidi = -1;
    repaint();
}

void PianoKeyboardComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto midi = midiForY (e.position.y);

    if (midi == hoverMidi)
        return;

    hoverMidi = midi;
    repaint();
}

void PianoKeyboardComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoverMidi < 0)
        return;

    hoverMidi = -1;
    repaint();
}

} // namespace vt
