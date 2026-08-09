/*
    ==========================================================================

    NoteNaming.h — what this app calls MIDI note 60, in one place.

    WHY THIS FILE EXISTS
    --------------------
    Because it did not, and three components drifted apart.

    There are two live conventions for octave numbers. Scientific pitch notation
    calls MIDI 60 **C4**; Yamaha's, which Ableton Live, Logic and most DAWs use,
    calls it **C3**. Neither is wrong, and code that picks one silently is
    perfectly correct on its own.

    PitchLaneComponent picked C3, with a comment explaining that anything else
    makes the note selector disagree with the DAW the user is staring at. The
    note selector, the exercise menu and the footer all followed it. The note
    HIGHWAY had its own private table and picked C4. Both were internally
    consistent and nothing looked wrong, because the two were never on screen at
    the same time.

    Then the piano keyboard arrived, which is by definition a column of note
    names beside the lane — and the two conventions ended up an inch apart,
    naming the same pixel row two different things. That is not a cosmetic
    problem for a tool whose whole job is to tell you which note you are
    singing.

    So: ONE constant, ONE function, and no component names a note any other way.

    ==========================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace vt
{

/** Ableton Live shows middle C (MIDI 60) as C3, so this app does too. Anything
    else makes every note name in the UI disagree with the DAW the user is
    staring at while they use it. */
constexpr int kOctaveNumberForMiddleC = 3;

/** "C3", "F#4", or just "C" / "F#" without the octave.

    Deliberately not juce::MidiMessage::getMidiNoteName: this header is included
    by the highway and the keyboard, and neither has any other reason to pull in
    juce_audio_basics. The mapping is twelve names and a division. */
inline juce::String midiNoteName (int midi, bool withOctave = true)
{
    static const char* const names[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    juce::String name (names[((midi % 12) + 12) % 12]);

    if (! withOctave)
        return name;

    // std::floor rather than integer division so a negative note number (which
    // cannot reach here, but costs nothing to be right about) does not round
    // toward zero and skip an octave at MIDI 0.
    const auto octave = (int) std::floor ((double) midi / 12.0) - 5 + kOctaveNumberForMiddleC;

    return name + juce::String (octave);
}

} // namespace vt
