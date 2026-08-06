/*
    ==========================================================================

    ExerciseLibrary.h — scales, arpeggios and interval drills, generated.

    WHY THESE ARE CHARTS AND NOT A SEPARATE MODE
    -------------------------------------------
    An exercise is emitted as chart JSON and handed to the ordinary
    Chart::parse(). Nothing downstream knows the difference: the highway, the
    auto-zoom, the octave-folded trace, the lyric strip, the chart-informed tau
    constraint and the whole of TakeScorer work on an exercise exactly as they
    work on Mr Brightside.

    That is the entire design decision here. A parallel "exercise mode" with its
    own renderer and its own scoring would double the surface area of every
    future change and would drift — the exercise view would slowly stop
    matching the song view, and the numbers would stop being comparable.
    Because a warm-up produces the same NoteScores as a song, "you are flat on
    ascending 6ths" in the review means the same thing in both.

    WHAT IS DELIBERATELY NOT HERE
    -----------------------------
    Glides and sirens. A continuous glide has no target pitch at any instant,
    so it cannot be scored on the two-threshold ramp, and showing an unscoreable
    bar teaches the singer that green means nothing. Octave LEAPS give the same
    range work and are scoreable, so that is what is offered instead.

    THREADING: pure functions, no state. Message thread in practice, but there
    is nothing here that would mind otherwise. Depends only on juce_core and
    Chart, so it is covered by the console test runner.

    ==========================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

#include "../chart/Chart.h"

namespace vt
{

//==============================================================================
struct ExerciseSpec
{
    enum class Kind
    {
        sustainedTone,      ///< long tones on one pitch — breath support
        majorScale,         ///< up and back down
        minorScale,         ///< natural minor
        pentatonicScale,    ///< major pentatonic, no semitones to fight
        chromaticScale,     ///< every semitone, the hardest tuning test
        majorArpeggio,      ///< 1-3-5-8-5-3-1
        fiveNoteRun,        ///< 1-2-3-2-1, transposing up — the classic warm-up
        fifthLeaps,         ///< 1-5-1, transposing up
        octaveLeaps,        ///< 1-8-1, range work
        intervalDrill       ///< every interval in turn, root-interval-root
    };

    Kind kind = Kind::majorScale;

    /** Starting pitch. Everything is generated relative to this, so an exercise
        is transposed by changing one number — which is the point: singing a
        warm-up outside your range teaches nothing but strain. */
    int rootMidi = 60;

    int bpm = 72;

    /** How many times the pattern repeats.

        Each repetition sits `transposeStep` semitones from the last, walking UP
        and then BACK DOWN again (see climbSteps()) rather than climbing forever.
        A warm-up that only ever ascends runs off the top of the voice after a
        dozen repeats, and the range clamp in buildChartJson would then quietly
        drop the whole exercise to a lower key than the one asked for. */
    int repeats = 1;
    int transposeStep = 1;

    /** Intervals descend from the root instead of ascending. */
    bool descending = false;

    /** Solfege ("do re mi") versus note names ("C D E") in the lyric strip.
        Solfege is the default because it is what a warm-up is actually sung on,
        and because it is transposition-invariant — the syllables do not change
        when you move the exercise up a tone. */
    bool useSolfege = true;

    /** Beats of RINGING REFERENCE PITCH before the count-in clicks start.

        The first thing an exercise plays is the note you are about to sing,
        struck alone with nothing over it — which is what a teacher does at the
        piano before counting you in. Without it the count-in tells you WHEN to
        come in and nothing at all about WHERE, so the first note of every warm-up
        is a guess, and a guessed entry is exactly the habit these drills exist to
        break. Zero disables it. */
    int referenceBeats = 2;

    /** Beats of count-in clicks between the reference pitch and the first note. */
    int countInBeats = 4;
};

//==============================================================================
class ExerciseLibrary
{
public:
    struct Definition
    {
        ExerciseSpec::Kind kind;
        const char* name;
        const char* purpose;        ///< what it is FOR, shown as a tooltip

        /** The FEWEST repetitions that still makes it this exercise — one full
            pass of all twelve intervals, say. A length preset can only add to
            it, never cut below it. */
        int defaultRepeats;

        /** Semitones added per repeat. Zero for the exercises where repeating on
            the SAME pitch is the point — a long tone is about breath support,
            and walking it up the keyboard turns it into a different exercise. */
        int defaultTransposeStep;
    };

    //== Length presets ========================================================
    /** Roughly how long the generated exercise should last, in seconds.

        A DURATION rather than a repeat count because the repeat count means
        something different for every exercise — one "repeat" of the interval
        drill is twelve intervals and the better part of a minute, one repeat of
        a five-note run is five seconds — so a single number of repeats would
        make some exercises trivial and others interminable. Tempo changes the
        answer too, and asking for a length keeps the choice honest across both. */
    static constexpr int kShortSeconds  = 45;
    static constexpr int kMediumSeconds = 105;
    static constexpr int kLongSeconds   = 210;

    /** The definition for one kind. */
    static Definition getDefinition (ExerciseSpec::Kind);

    /** A spec filled from the definition's defaults, ready to be adjusted.
        `targetSeconds` sets the repeat count; the definition's own minimum wins
        when it is larger. */
    static ExerciseSpec makeDefaultSpec (ExerciseSpec::Kind, int rootMidi, int bpm,
                                         int targetSeconds = kMediumSeconds);

    /** Repeats needed for a spec to last about `targetSeconds`, counting the
        lead-in and clamped to what the note budget allows. */
    static int repeatsForTargetSeconds (const ExerciseSpec&, int targetSeconds);

    /** How long the spec will actually last, lead-in included. Cheap enough to
        call while building a menu. */
    static double estimateDurationSeconds (const ExerciseSpec&);

    /** Every exercise, in the order they belong in a menu: warm-up first,
        hardest last. */
    static std::vector<Definition> getAll();

    static juce::String getName (ExerciseSpec::Kind);
    static juce::String getPurpose (ExerciseSpec::Kind);

    /** The chart JSON for a spec — schema 1.0, identical in shape to what the
        offline pipeline emits. Exposed separately from buildChart() so a
        generated exercise can also be SAVED to a songs/ folder and treated as
        an ordinary song. */
    static juce::var buildChartJson (const ExerciseSpec&);

    /** Parsed and ready to hand to the highway. Returns nullptr with `error`
        filled on the (only) failure mode: a spec whose notes all fall outside
        the singable MIDI range. */
    static std::unique_ptr<Chart> buildChart (const ExerciseSpec&, juce::String& error);

    /** Lowest and highest MIDI the spec will produce, so the caller can warn
        before generating something unsingable. */
    static void getMidiRange (const ExerciseSpec&, int& lowest, int& highest);

    /** Beat length in seconds. Shared with the click renderer so the notes and
        the clicks cannot disagree. */
    static double beatSeconds (const ExerciseSpec& spec) noexcept
    {
        return 60.0 / (double) juce::jlimit (20, 300, spec.bpm);
    }

    /** Chart time the count-in clicks start at — i.e. the length of the
        reference pitch that rings alone before them. */
    static double referencePitchSeconds (const ExerciseSpec& spec) noexcept
    {
        return beatSeconds (spec) * (double) juce::jmax (0, spec.referenceBeats);
    }

    /** Chart time the first note starts at: reference pitch, then count-in. */
    static double leadInSeconds (const ExerciseSpec& spec) noexcept
    {
        return referencePitchSeconds (spec)
                 + beatSeconds (spec) * (double) juce::jmax (0, spec.countInBeats);
    }

    /** How many transposition steps the exercise climbs before turning round.

        Bounded by three things at once: the singable range above the pattern's
        own top note, an octave of total climb (past that it stops being the same
        exercise), and half the repeat count — because the walk back down has to
        fit in the repeats too. Zero when the spec does not transpose. */
    static int climbSteps (const ExerciseSpec&);

private:
    /** One generated event, before it becomes JSON. A rest carries a duration
        and nothing else — exercises need real silence between repetitions,
        both to breathe and so the review can tell repetitions apart. */
    struct Event
    {
        int    semitoneOffset = 0;
        double beats = 1.0;
        int    phrase = 0;
        bool   rest = false;
    };

    struct Pattern
    {
        std::vector<Event> events;
        juce::StringArray  phraseLabels;
    };

    static Pattern buildPattern (const ExerciseSpec&);

    /** Beats in ONE repetition, rests included. Measured off the pattern rather
        than tabulated, so it cannot drift when a pattern changes. */
    static double beatsPerRepetition (const ExerciseSpec&);

    static juce::String solfegeFor (int semitoneOffset);
    static juce::String noteNameFor (int midi);
    static juce::String intervalName (int semitones);
};

} // namespace vt
