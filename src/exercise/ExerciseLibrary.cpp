#include "ExerciseLibrary.h"

#include <algorithm>
#include <cmath>

namespace vt
{

namespace
{
    /** Singable MIDI range the exercise is kept inside. Matches the target-note
        parameter's range in AudioEngine, so an exercise can never generate a
        note the rest of the app refuses to represent. */
    constexpr int kLowestMidi  = 36;
    constexpr int kHighestMidi = 84;

    /** A guard, not a design limit: a spec asking for 60 repeats of a chromatic
        scale should produce a long exercise, not run the machine out of memory. */
    constexpr int kMaxNotes = 1200;

    /** Repetition ceiling. High enough that a three-minute five-note run at 132
        bpm — 60-odd repetitions of a six-beat pattern — is generated in full
        rather than silently stopping half way. */
    constexpr int kMaxRepeats = 72;

    /** Most an exercise will transpose away from its starting note before it
        turns round. An octave is already a long climb; beyond it the exercise
        stops being "the same warm-up on a different note" and becomes a range
        test the singer did not ask for. */
    constexpr int kMaxClimbSemitones = 12;

    /** Movable-do, chromatic. Ascending and descending chromatic solfege
        traditionally use different syllables (di/ri vs ra/me); one table is used
        here on purpose — the singer is reading a pitch cue, not sitting an aural
        exam, and two tables for the same note would be noise. */
    const char* const kSolfege[12] =
        { "do", "di", "re", "me", "mi", "fa", "fi", "sol", "le", "la", "te", "ti" };

    const char* const kNoteNames[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

//==============================================================================
juce::String ExerciseLibrary::solfegeFor (int semitoneOffset)
{
    // An octave in either direction is "do" again, which is the one place the
    // wrap-around answer would be misleading if it were not special-cased.
    if (semitoneOffset == 12 || semitoneOffset == -12)
        return "do";

    const auto wrapped = ((semitoneOffset % 12) + 12) % 12;
    return kSolfege[wrapped];
}

juce::String ExerciseLibrary::noteNameFor (int midi)
{
    // Matches the highway's own grid labels (NoteHighwayComponent::midiName), so
    // the syllable in the lyric strip and the label on the line the note sits on
    // always read the same. NOTE: the Target-note dropdown uses Ableton's
    // convention instead (middle C = C3), so it will read one octave lower.
    const auto wrapped = ((midi % 12) + 12) % 12;
    return juce::String (kNoteNames[wrapped]) + juce::String (midi / 12 - 1);
}

juce::String ExerciseLibrary::intervalName (int semitones)
{
    static const char* const names[13] =
    {
        "Unison", "Minor 2nd", "Major 2nd", "Minor 3rd", "Major 3rd",
        "Perfect 4th", "Tritone", "Perfect 5th", "Minor 6th", "Major 6th",
        "Minor 7th", "Major 7th", "Octave"
    };

    const auto s = std::abs (semitones);
    return s <= 12 ? juce::String (names[s]) : juce::String (s) + " semitones";
}

//==============================================================================
std::vector<ExerciseLibrary::Definition> ExerciseLibrary::getAll()
{
    // Menu order: what you would actually do in a session. Breath first, then
    // stepwise pitch, then leaps, then the two hardest accuracy tests last.
    return {
        { ExerciseSpec::Kind::sustainedTone,   "Long tones",
          "Breath support and steadiness. Hold each note dead still — the review "
          "reports how far it wandered after the attack.", 4, 0 },

        { ExerciseSpec::Kind::fiveNoteRun,     "Five-note run (1-2-3-2-1)",
          "The standard warm-up. Walks up a semitone each time, so it finds the "
          "edge of your comfortable range.", 5, 1 },

        { ExerciseSpec::Kind::majorScale,      "Major scale",
          "Stepwise accuracy up and back down. The descent is where most singers "
          "go flat.", 2, 1 },

        { ExerciseSpec::Kind::minorScale,      "Natural minor scale",
          "Same as the major scale, but the flattened 3rd, 6th and 7th are the "
          "notes people habitually sing too high.", 2, 1 },

        { ExerciseSpec::Kind::pentatonicScale, "Major pentatonic",
          "No semitone steps at all, so it isolates interval accuracy from "
          "semitone tuning. A good first scale.", 2, 1 },

        { ExerciseSpec::Kind::majorArpeggio,   "Major arpeggio (1-3-5-8)",
          "Leaps inside one chord. Trains hearing the next note before you sing "
          "it rather than sliding up to find it.", 2, 1 },

        { ExerciseSpec::Kind::fifthLeaps,      "Fifth leaps (1-5-1)",
          "The interval most used in melodies, and the one worth being exact on. "
          "Rises a semitone each repeat.", 5, 1 },

        { ExerciseSpec::Kind::octaveLeaps,     "Octave leaps (1-8-1)",
          "Range work. Scoring is octave-agnostic, so this is one exercise where "
          "the display will tell you something the score cannot.", 3, 1 },

        { ExerciseSpec::Kind::intervalDrill,   "Interval drill (all 12)",
          "Every interval from a minor 2nd to an octave, back to the root each "
          "time. The review groups your errors by interval, so this is the "
          "exercise that tells you which one to practise.", 1, 0 },

        { ExerciseSpec::Kind::chromaticScale,  "Chromatic scale",
          "Every semitone. The hardest tuning test here — expect it to be ugly "
          "the first few times.", 1, 1 }
    };
}

ExerciseLibrary::Definition ExerciseLibrary::getDefinition (ExerciseSpec::Kind kind)
{
    for (const auto& d : getAll())
        if (d.kind == kind)
            return d;

    return getAll().front();
}

juce::String ExerciseLibrary::getName (ExerciseSpec::Kind kind)
{
    return getDefinition (kind).name;
}

juce::String ExerciseLibrary::getPurpose (ExerciseSpec::Kind kind)
{
    return getDefinition (kind).purpose;
}

ExerciseSpec ExerciseLibrary::makeDefaultSpec (ExerciseSpec::Kind kind, int rootMidi, int bpm,
                                              int targetSeconds)
{
    const auto def = getDefinition (kind);

    ExerciseSpec spec;
    spec.kind          = kind;
    spec.rootMidi      = juce::jlimit (kLowestMidi, kHighestMidi, rootMidi);
    spec.bpm           = juce::jlimit (30, 200, bpm);
    spec.transposeStep = def.defaultTransposeStep;

    // The definition's count is a FLOOR, not the answer: one pass of the interval
    // drill has to happen whatever length was asked for, and a "short" session
    // must not amputate it half way through the tritone.
    spec.repeats = juce::jmax (1, def.defaultRepeats,
                               repeatsForTargetSeconds (spec, targetSeconds));

    return spec;
}

//==============================================================================
double ExerciseLibrary::beatsPerRepetition (const ExerciseSpec& spec)
{
    ExerciseSpec one = spec;
    one.repeats       = 1;
    one.transposeStep = 0;      // also stops climbSteps() recursing back in here

    double beats = 0.0;

    for (const auto& e : buildPattern (one).events)
        beats += e.beats;

    return beats;
}

int ExerciseLibrary::repeatsForTargetSeconds (const ExerciseSpec& spec, int targetSeconds)
{
    ExerciseSpec one = spec;
    one.repeats = 1;

    const auto perRepetition = beatsPerRepetition (spec) * beatSeconds (spec);

    if (perRepetition <= 0.0)
        return 1;

    const auto want = juce::jmax (0.0, (double) targetSeconds - leadInSeconds (spec));

    // Round to NEAREST rather than up. The interval drill is fifty seconds in one
    // repetition; rounding up would turn a request for ninety seconds into a
    // hundred-second exercise, and there is no honest way to give a target
    // duration a granularity finer than one repetition.
    auto repeats = (int) std::llround (want / perRepetition);

    // The note budget is a hard limit, so it has to bound the answer rather than
    // truncating the chart later — a warm-up that stops mid-phrase because it ran
    // out of ids looks like a bug in the exercise, not a limit being hit.
    auto notesPerRepetition = 0;

    for (const auto& e : buildPattern (one).events)
        if (! e.rest)
            ++notesPerRepetition;

    if (notesPerRepetition > 0)
        repeats = juce::jmin (repeats, kMaxNotes / notesPerRepetition);

    return juce::jlimit (1, kMaxRepeats, repeats);
}

double ExerciseLibrary::estimateDurationSeconds (const ExerciseSpec& spec)
{
    const auto repeats = juce::jlimit (1, kMaxRepeats, spec.repeats);

    return leadInSeconds (spec)
             + beatsPerRepetition (spec) * beatSeconds (spec) * (double) repeats;
}

//==============================================================================
int ExerciseLibrary::climbSteps (const ExerciseSpec& spec)
{
    if (spec.transposeStep == 0)
        return 0;

    // The pattern's own span, untransposed. Measured rather than tabulated: an
    // octave-leap pattern reaches twelve semitones above its root and a
    // five-note run four, and a hardcoded headroom figure would be wrong for one
    // of them the first time either pattern changed.
    ExerciseSpec flat = spec;
    flat.repeats       = 1;
    flat.transposeStep = 0;

    int lowest = 0, highest = 0;
    getMidiRange (flat, lowest, highest);

    const auto step     = std::abs (spec.transposeStep);
    const auto headroom = spec.transposeStep > 0 ? kHighestMidi - highest
                                                 : lowest - kLowestMidi;

    return juce::jmax (0, juce::jmin (juce::jmax (0, headroom) / step,
                                      kMaxClimbSemitones / step,
                                      juce::jlimit (1, kMaxRepeats, spec.repeats) / 2));
}

//==============================================================================
ExerciseLibrary::Pattern ExerciseLibrary::buildPattern (const ExerciseSpec& spec)
{
    Pattern pattern;

    const auto repeats = juce::jlimit (1, kMaxRepeats, spec.repeats);
    const auto sign    = spec.descending ? -1 : 1;

    //-------------------------------------------------------------------------
    // Each helper appends one repetition's worth of events and one phrase
    // label. Phrases exist so the review can rank repetitions worst-first and
    // offer loop-and-retry on the one that went wrong — without them an
    // exercise would be one undifferentiated blob in the review screen.
    //-------------------------------------------------------------------------
    const auto beginPhrase = [&pattern] (const juce::String& label)
    {
        pattern.phraseLabels.add (label);
        return pattern.phraseLabels.size() - 1;
    };

    const auto addNotes = [&pattern] (const std::vector<int>& offsets, double beats,
                                      int transpose, int phrase)
    {
        for (const auto o : offsets)
        {
            Event e;
            e.semitoneOffset = o + transpose;
            e.beats          = beats;
            e.phrase         = phrase;
            pattern.events.push_back (e);
        }
    };

    const auto addRest = [&pattern] (double beats)
    {
        Event e;
        e.beats = beats;
        e.rest  = true;
        pattern.events.push_back (e);
    };

    //-------------------------------------------------------------------------
    // Transposition walks UP to the turning point and back DOWN again, as a
    // triangle wave, rather than climbing one step per repetition forever.
    //
    // Forever is what a two-repeat exercise made look reasonable. Ask for three
    // minutes of five-note runs and it becomes forty repetitions — forty
    // semitones, three and a half octaves — at which point the range clamp in
    // buildChartJson drags the whole exercise down to fit and hands back a
    // warm-up in a key nobody chose. Walking back down is also simply what a
    // warm-up does: up through the passaggio and back, not off the top.
    //-------------------------------------------------------------------------
    const auto climb = climbSteps (spec);

    const auto transposeFor = [&spec, climb] (int repeat)
    {
        if (climb <= 0)
            return 0;

        const auto period = 2 * climb;
        const auto m      = repeat % period;

        return (m <= climb ? m : period - m) * spec.transposeStep;
    };

    switch (spec.kind)
    {
        case ExerciseSpec::Kind::sustainedTone:
        {
            for (int r = 0; r < repeats; ++r)
            {
                const auto phrase = beginPhrase ("Long tone " + juce::String (r + 1));

                // EIGHT beats, not four. Four beats is under three and a half
                // seconds at 72 bpm, which is inside anyone's comfortable breath
                // and therefore tests nothing: the wobble that breath support is
                // meant to fix only shows up once the note outlasts the easy part
                // of the exhale. Four beats of rest, likewise, is a breath rather
                // than a snatched gasp.
                addNotes ({ 0 }, 8.0, transposeFor (r), phrase);
                addRest (4.0);
            }
            break;
        }

        case ExerciseSpec::Kind::majorScale:
        case ExerciseSpec::Kind::minorScale:
        case ExerciseSpec::Kind::pentatonicScale:
        case ExerciseSpec::Kind::chromaticScale:
        {
            std::vector<int> up, down;

            if (spec.kind == ExerciseSpec::Kind::majorScale)
            {
                up   = { 0, 2, 4, 5, 7, 9, 11, 12 };
                down = { 11, 9, 7, 5, 4, 2, 0 };
            }
            else if (spec.kind == ExerciseSpec::Kind::minorScale)
            {
                up   = { 0, 2, 3, 5, 7, 8, 10, 12 };
                down = { 10, 8, 7, 5, 3, 2, 0 };
            }
            else if (spec.kind == ExerciseSpec::Kind::pentatonicScale)
            {
                up   = { 0, 2, 4, 7, 9, 12 };
                down = { 9, 7, 4, 2, 0 };
            }
            else
            {
                for (int i = 0; i <= 12; ++i) up.push_back (i);
                for (int i = 11; i >= 0; --i) down.push_back (i);
            }

            if (spec.descending)
                std::swap (up, down);

            for (int r = 0; r < repeats; ++r)
            {
                const auto suffix = repeats > 1 ? " " + juce::String (r + 1) : juce::String();

                addNotes (up, 1.0, transposeFor (r),
                          beginPhrase ((spec.descending ? "Descending" : "Ascending") + suffix));

                addNotes (down, 1.0, transposeFor (r),
                          beginPhrase ((spec.descending ? "Ascending" : "Descending") + suffix));

                addRest (2.0);
            }
            break;
        }

        case ExerciseSpec::Kind::majorArpeggio:
        {
            for (int r = 0; r < repeats; ++r)
            {
                const auto phrase = beginPhrase ("Arpeggio " + juce::String (r + 1));
                addNotes ({ 0, 4, 7, 12, 7, 4, 0 }, 1.0, transposeFor (r), phrase);
                addRest (2.0);
            }
            break;
        }

        case ExerciseSpec::Kind::fiveNoteRun:
        {
            for (int r = 0; r < repeats; ++r)
            {
                const auto phrase = beginPhrase ("Run " + juce::String (r + 1));
                addNotes ({ 0, 2, 4, 2, 0 }, 1.0, transposeFor (r), phrase);
                addRest (1.0);
            }
            break;
        }

        case ExerciseSpec::Kind::fifthLeaps:
        {
            for (int r = 0; r < repeats; ++r)
            {
                const auto phrase = beginPhrase ("Fifth " + juce::String (r + 1));
                addNotes ({ 0, 7 * sign, 0 }, 1.0, transposeFor (r), phrase);
                addRest (1.0);
            }
            break;
        }

        case ExerciseSpec::Kind::octaveLeaps:
        {
            for (int r = 0; r < repeats; ++r)
            {
                const auto phrase = beginPhrase ("Octave " + juce::String (r + 1));

                // Two beats per note: an octave leap needs time to be PLACED
                // rather than scooped up to, and the note either side of it is
                // where the singer hears whether they landed.
                addNotes ({ 0, 12 * sign, 0 }, 2.0, transposeFor (r), phrase);
                addRest (2.0);
            }
            break;
        }

        case ExerciseSpec::Kind::intervalDrill:
        {
            for (int r = 0; r < repeats; ++r)
                for (int interval = 1; interval <= 12; ++interval)
                {
                    const auto phrase = beginPhrase ((spec.descending ? "Descending "
                                                                     : "Ascending ")
                                                       + intervalName (interval).toLowerCase());

                    addNotes ({ 0, interval * sign, 0 }, 1.0, transposeFor (r), phrase);
                    addRest (2.0);
                }
            break;
        }
    }

    return pattern;
}

//==============================================================================
void ExerciseLibrary::getMidiRange (const ExerciseSpec& spec, int& lowest, int& highest)
{
    const auto pattern = buildPattern (spec);

    lowest = highest = spec.rootMidi;

    for (const auto& e : pattern.events)
    {
        if (e.rest)
            continue;

        const auto midi = spec.rootMidi + e.semitoneOffset;
        lowest  = std::min (lowest,  midi);
        highest = std::max (highest, midi);
    }
}

//==============================================================================
juce::var ExerciseLibrary::buildChartJson (const ExerciseSpec& spec)
{
    const auto pattern = buildPattern (spec);
    const auto beat    = beatSeconds (spec);

    //-------------------------------------------------------------------------
    // Shift the whole exercise into the singable range rather than clipping
    // individual notes. A scale with its top three notes silently removed is a
    // different exercise, and one the singer did not ask for.
    //-------------------------------------------------------------------------
    int lowest = 0, highest = 0;
    getMidiRange (spec, lowest, highest);

    int rootMidi = spec.rootMidi;

    if (highest > kHighestMidi) rootMidi -= (highest - kHighestMidi);
    if (lowest  < kLowestMidi)  rootMidi += (kLowestMidi - lowest);

    juce::Array<juce::var> notesArray;
    juce::Array<juce::var> phrasesArray;

    // Per-phrase bookkeeping, filled as the notes are laid out.
    struct PhraseBuild
    {
        double start = -1.0;
        double end = 0.0;
        int firstMidi = -1;
        juce::Array<juce::var> noteIds;
    };

    std::vector<PhraseBuild> phraseBuilds ((size_t) juce::jmax (1, pattern.phraseLabels.size()));

    double t = leadInSeconds (spec);
    int nextId = 0;

    for (const auto& e : pattern.events)
    {
        const auto duration = e.beats * beat;

        if (e.rest)
        {
            t += duration;
            continue;
        }

        if (nextId >= kMaxNotes)
            break;

        const auto midi = rootMidi + e.semitoneOffset;

        if (midi < 0 || midi > 127)
        {
            t += duration;
            continue;
        }

        juce::DynamicObject::Ptr note (new juce::DynamicObject());
        note->setProperty ("id", nextId);
        note->setProperty ("startSeconds", t);

        // A hair of air between notes, so consecutive bars are visibly separate
        // on the highway and so the note-boundary median clear in TakeScorer has
        // an unambiguous edge to fire on.
        note->setProperty ("durationSeconds", juce::jmax (0.08, duration - 0.06));
        note->setProperty ("midi", midi);
        note->setProperty ("centsOffset", 0.0);
        note->setProperty ("type", "normal");
        note->setProperty ("syllable", spec.useSolfege ? solfegeFor (e.semitoneOffset)
                                                       : noteNameFor (midi));
        note->setProperty ("syllableRole", "wordStart");
        note->setProperty ("phraseId", e.phrase);
        note->setProperty ("confidence", 1.0);

        notesArray.add (juce::var (note.get()));

        if (e.phrase >= 0 && e.phrase < (int) phraseBuilds.size())
        {
            auto& pb = phraseBuilds[(size_t) e.phrase];

            if (pb.start < 0.0)
            {
                pb.start     = t;
                pb.firstMidi = midi;
            }

            pb.end = t + duration;
            pb.noteIds.add (nextId);
        }

        ++nextId;
        t += duration;
    }

    for (int i = 0; i < pattern.phraseLabels.size(); ++i)
    {
        const auto& pb = phraseBuilds[(size_t) i];

        if (pb.start < 0.0)
            continue;

        // A transposing exercise names the key each repetition sits in. "Run 9"
        // is a number; "Run 9 (F4)" is the thing the review is actually telling
        // you about — that the wheels come off at F, which is a fact about the
        // singer's voice rather than about the ninth time round.
        const auto label = (spec.transposeStep != 0 && pb.firstMidi >= 0)
                             ? pattern.phraseLabels[i] + " (" + noteNameFor (pb.firstMidi) + ")"
                             : pattern.phraseLabels[i];

        juce::DynamicObject::Ptr phrase (new juce::DynamicObject());
        phrase->setProperty ("id", i);
        phrase->setProperty ("startSeconds", pb.start);
        phrase->setProperty ("endSeconds", pb.end);
        phrase->setProperty ("text", label);
        phrase->setProperty ("noteIds", pb.noteIds);

        phrasesArray.add (juce::var (phrase.get()));
    }

    //-------------------------------------------------------------------------
    const auto rootName = noteNameFor (rootMidi);

    juce::DynamicObject::Ptr song (new juce::DynamicObject());
    song->setProperty ("id", "exercise-" + getName (spec.kind).toLowerCase()
                                              .replaceCharacter (' ', '-')
                                              .retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789-"));
    song->setProperty ("title", getName (spec.kind) + " on " + rootName);
    song->setProperty ("artist", juce::String (spec.bpm) + " bpm exercise");
    song->setProperty ("durationSeconds", t + 1.5);
    song->setProperty ("tuningHz", 440.0);

    juce::DynamicObject::Ptr generator (new juce::DynamicObject());

    // "manual", not "asr": these syllables are generated and exact. Marking
    // them auto-transcribed would put the italic "auto-transcribed" badge on a
    // lyric that is not a guess (see LyricStripComponent), which would teach the
    // singer to distrust the one lyric in the app that is definitely right.
    generator->setProperty ("lyricSource", "manual");
    generator->setProperty ("name", "ExerciseLibrary");

    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    root->setProperty ("schemaVersion", "1.0");
    root->setProperty ("song", juce::var (song.get()));
    root->setProperty ("generator", juce::var (generator.get()));
    root->setProperty ("notes", notesArray);
    root->setProperty ("phrases", phrasesArray);

    return juce::var (root.get());
}

//==============================================================================
std::unique_ptr<Chart> ExerciseLibrary::buildChart (const ExerciseSpec& spec, juce::String& error)
{
    return Chart::parse (buildChartJson (spec), error);
}

} // namespace vt
