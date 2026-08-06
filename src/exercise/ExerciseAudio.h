/*
    ==========================================================================

    ExerciseAudio.h — the two sounds a generated exercise needs.

    A metronome CLICK and a pitch GUIDE TONE, rendered into plain buffers and
    handed to BackingPlayer's backing and reference decks respectively.

    BOTH PITCHED SOUNDS ARE A SYNTHESISED PIANO
    -------------------------------------------
    Not a sample — no sample library ships with this, and a 25 MB one would be a
    licence question in a project that has to stay clear of GPL — but a struck
    string modelled directly: twelve slightly inharmonic partials, each decaying
    faster than the one below it, over a four-millisecond attack.

    The point is not realism, it is that a piano is what singers have pitched
    against for two centuries and their ear already knows how to hear one. A
    steady electronic tone has no attack to place the note against and no decay,
    so it sits UNDER the voice as a drone the singer slowly stops hearing; a
    struck note arrives, tells you where it is, and gets out of the way while you
    sing. The decay is doing pedagogical work, not decoration.

    WHERE THE STARTING REFERENCE PITCH LIVES, AND WHY IT IS NOT ON THE GUIDE DECK
    ----------------------------------------------------------------------------
    The first note of the exercise, struck alone before the count-in, is part of
    the CLICK track. So it survives turning the guide off.

    That is deliberate and it is not a contradiction of the paragraph below.
    Silencing the running guide is how the singer tests their own ear through the
    exercise; being told where to START is not a crutch, it is the premise —
    every teacher plays the note before counting you in. Putting it on the
    reference deck would mean "test my ear on this scale" and "don't tell me what
    key it is in" were the same request, and the singer would spend the count-in
    guessing instead of preparing.

    WHY TWO SEPARATE BUFFERS AND NOT ONE MIX
    ----------------------------------------
    Because the whole pedagogical point is being able to turn the guide tone
    OFF while keeping the click. DESIGN.md section 6: a reference you cannot
    silence teaches you to follow it rather than to pitch it, and the
    feedback-fading evidence says the singer must spend part of their practice
    relying on their own ear. Mixing the two would make "silence the guide"
    also mean "lose the tempo".

    It also means the exercise reuses the existing UI unchanged: the Guide
    toggle and its level slider already control the reference deck, so they
    control the exercise's pitch reference with no new controls and no new
    plumbing.

    WHY IT IS RENDERED UP FRONT RATHER THAN SYNTHESISED LIVE
    -------------------------------------------------------
    A live synth would need the chart on the audio thread. The chart is
    message-thread-only by design (see Chart.h), and the alternative —
    publishing a lock-free copy of the note schedule to the audio thread — is
    real machinery to maintain for no benefit.

    The cost is memory and a short pause, and both are worth knowing: stereo
    float at 48 kHz is 384 kB per second per deck, so the longest exercise the
    length presets offer (3½ minutes) is about 80 MB per deck, and BackingPlayer
    copies what it is given — call it 160 MB resident for the pair. Rendering
    both takes ~175 ms on the machine this was written on, which is inside what
    reads as a menu item doing something rather than the app hanging. Both
    numbers scale linearly with the length preset, which is the thing to check
    first if either ever becomes a problem.

    ==========================================================================
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "ExerciseLibrary.h"
#include "../chart/Chart.h"

namespace vt
{

class ExerciseAudio
{
public:
    /** The starting pitch, then one tick per beat for the whole exercise, with
        the count-in ticks accented.

        The count-in accent is not decoration: without it there is no way to
        tell the four preparation beats from the first four beats of the
        exercise, and the singer comes in a bar late. */
    static juce::AudioBuffer<float> renderClickTrack (const Chart&,
                                                      const ExerciseSpec&,
                                                      double sampleRate);

    /** One struck piano note per chart note, at the note's pitch.

        Notes long enough to outlast the decay are re-struck rather than left to
        fade into nothing — which is exactly what an accompanist does, and what
        the long-tone exercise needs if the reference is to still be there at the
        end of an eight-beat note. */
    static juce::AudioBuffer<float> renderGuideTone (const Chart&,
                                                     const ExerciseSpec&,
                                                     double sampleRate);

private:
    /** Adds a decaying tick at `startSeconds`. */
    static void addClick (juce::AudioBuffer<float>&, double startSeconds,
                          double sampleRate, float frequency, float amplitude);

    /** Adds one struck piano note. `sustainSeconds` is how long the note is
        wanted for; the tone rings a little past it and is faded out, because a
        piano note that stops dead at a bar line is the one thing a real one
        never does. */
    static void addPianoNote (juce::AudioBuffer<float>&, double startSeconds,
                              double sustainSeconds, double frequency,
                              float amplitude, double sampleRate);
};

} // namespace vt
