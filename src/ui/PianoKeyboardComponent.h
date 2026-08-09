/*
    ==========================================================================

    PianoKeyboardComponent.h — the reference keyboard down the left edge.

    A vertical, PIANO-ROLL keyboard: one equal-height key per semitone, sharing
    the view range of the free-sing lane beside it, so a key is always at
    exactly the same height as the row it names.

    FREE SING ONLY (see MainComponent::resized). Both of the jobs below are
    already done by something else the moment a chart or an exercise is loaded:
    there is a bar to aim at, the bar carries its own note name, the gutter names
    every row, and the exercise strikes its own guide tone. A keyboard there is a
    third copy of the same answer, charging 68 px of the view being read.

    IT IS DRAWN IN THE THEME'S COLOURS, not in photographic white and black —
    see ThemeValues::pianoWhiteKey.

    WHY NOT A REAL KEYBOARD'S GEOMETRY
    ----------------------------------
    A photographic keyboard puts seven equal white keys across twelve semitones,
    which means the white keys and the pitches they name only line up at C and F.
    Beside a lane whose Y axis is chromatic-linear that is not a stylistic
    choice, it is a keyboard that points at the wrong note by up to half a
    semitone — for something whose entire job is to say "this pitch is here",
    that is the one failure mode that matters. Every piano-roll editor ever
    written reaches the same conclusion for the same reason.

    WHAT IT IS FOR
    --------------
    Two things, and they are separate:

      1. READING. The key gives a chromatic name and a black/white shape to a
         position that would otherwise just be a height. DESIGN.md §6 lists a
         keyboard reference as the first item of the beginner view.

      2. HEARING. Clicking a key strikes it. Pitch is learned by ear far more
         than by eye, and "what does that note actually sound like?" is a
         question the singer will have several times a session — every teacher
         answers it by playing the note.

    THREADING: message thread only. The click callback does nothing but hand a
    MIDI number to the caller, which forwards it to the engine's ReferenceTone.

    ==========================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <limits>

#include "Theme.h"

namespace vt
{

class PianoKeyboardComponent : public juce::Component
{
public:
    explicit PianoKeyboardComponent (Theme& themeToUse);
    ~PianoKeyboardComponent() override;

    //==========================================================================
    /** The vertical span to draw, in MIDI, taken from the view beside it. Cheap
        to call every frame: the static key layer is only rebuilt once the range
        has actually moved. */
    void setViewRange (float lowMidi, float highMidi);

    /** The note the chart wants right now, or -1 for none. */
    void setTargetMidi (int midi);

    /** Where the singer's voice actually is, in fractional MIDI, or NaN when
        unvoiced. TRUE PITCH, not the octave-folded value the highway draws —
        the whole point of the marker is to say which key you are on. */
    void setSungMidi (float fracMidi);

    /** Fires on press and on each new key a drag crosses, so running a finger
        down the keyboard plays a glissando rather than one note. */
    std::function<void (int)> onKeyPressed;

    void applyTheme();

    //==========================================================================
    /** Background fill and the staleness check only — see the highway's header
        for why nothing visible may be drawn here. */
    void paint (juce::Graphics&) override;

    /** Target, sung-pitch marker, hover and press: everything that changes. */
    void paintOverChildren (juce::Graphics&) override;

    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    //==========================================================================
    /** The keys themselves and their labels: static until the range moves, so
        buffered and repainted only then. Text layout for twenty labels is not
        something to do sixty times a second for a picture that has not changed. */
    class KeyLayer : public juce::Component
    {
    public:
        explicit KeyLayer (PianoKeyboardComponent& o) : owner (o)
        {
            setInterceptsMouseClicks (false, false);
            setBufferedToImage (true);
        }

        void paint (juce::Graphics&) override;

    private:
        PianoKeyboardComponent& owner;
    };

    //==========================================================================
    float yForMidi (float midi) const noexcept;
    int   midiForY (float y) const noexcept;

    /** Top and bottom of a key's slot. A key is CENTRED on its pitch, so it
        spans midi-0.5 to midi+0.5 and its centre line is exactly the grid line
        the lane beside it draws. */
    juce::Range<float> slotFor (int midi) const noexcept;

    void refreshKeysIfNeeded();

    /** Strikes `midi` unless it is the key already sounding under the pointer. */
    void pressKey (int midi);

    static bool isBlackKey (int midi) noexcept;

    //==========================================================================
    Theme& theme;

    KeyLayer keys { *this };

    float viewLowMidi = 54.0f, viewHighMidi = 68.0f;
    float keysLowMidi = -1.0f, keysHighMidi = -1.0f;
    juce::Rectangle<int> keysBounds;

    int   targetMidi = -1;
    float sungMidi = std::numeric_limits<float>::quiet_NaN();
    int   hoverMidi = -1;
    int   pressedMidi = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoKeyboardComponent)
};

} // namespace vt
