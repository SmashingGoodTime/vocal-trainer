/*
    ==========================================================================

    Chart.h — the parsed, immutable song chart.

    Loaded from the chart.json emitted by the offline `vocalchart` pipeline.
    See docs/DESIGN.md section 3 for the normative schema.

    THREADING
    ---------
    A Chart lives on the MESSAGE THREAD only. Neither the audio thread nor the
    analysis worker ever touches it.

    That is a deliberate simplification. The worker needs exactly one number
    from the chart — the MIDI note currently being sung — to constrain the
    detector's tau search. The editor computes that once per frame and
    publishes it through a plain std::atomic<float>. So there is no shared
    ownership across threads, no atomic refcount traffic in the hot path, and
    no lifetime question to get wrong.

    Parsing is done on a background thread and the finished Chart handed to the
    message thread; see ChartLoader.

    MUTATION — read this before using it
    ------------------------------------
    This class was originally immutable after construction, and the edit API
    below (setNoteMidi / setNoteSyllable / ...) is a deliberate narrowing of
    that rule, not an abandonment of it. It is safe for exactly the reason the
    threading note above gives: nothing outside the message thread holds a
    pointer to a Chart or to anything inside one. The single value the worker
    needs is re-read from an atomic every frame, so an edit is picked up on the
    next frame with no synchronisation at all.

    Two invariants ARE load-bearing and every edit function preserves them
    rather than trusting the caller:

      1. notes stay sorted by startSeconds — noteIndexAt() is a binary search;
      2. notes stay NON-OVERLAPPING — the renderer, the syllable wipe and the
         scorer all assume at most one note sounds at a time. Timing edits are
         therefore CLAMPED against their neighbours instead of being rejected.

    Because an edit can re-sort, note INDICES are not stable across edits. Hold
    a note's `id` and call indexForId(); the editor does.

    ==========================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <limits>
#include <memory>
#include <vector>

namespace vt
{

//==============================================================================
enum class NoteType
{
    normal,      ///< scored on pitch
    golden,      ///< scored on pitch, 2x multiplier
    freestyle,   ///< always counts as hit — pitch is not scored
    rap,         ///< rhythm only, pitch ignored
    breath       ///< a deliberate gap; shown so the highway is not empty
};

/** True when this note's pitch should be scored at all. DESIGN.md section 6. */
inline bool isPitchScored (NoteType t) noexcept
{
    return t == NoteType::normal || t == NoteType::golden;
}

//==============================================================================
enum class SyllableRole
{
    none,
    wordStart,
    continuation,
    melisma       ///< same syllable continues onto this note — draw a tie, no text
};

//==============================================================================
struct ChartNote
{
    int    id = 0;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    int    midi = 60;
    float  centsOffset = 0.0f;      ///< reference singer's deviation from `midi`
    NoteType type = NoteType::normal;

    /** May be EMPTY, and that is legal and common — Whisper systematically
        deletes non-lexical vocables, and those are exactly the sustained held
        notes the highway most wants to show. Never assume a note has text. */
    juce::String syllable;
    SyllableRole syllableRole = SyllableRole::none;

    int    phraseId = -1;
    float  confidence = 1.0f;

    double endSeconds() const noexcept { return startSeconds + durationSeconds; }

    bool containsTime (double t) const noexcept
    {
        return t >= startSeconds && t < endSeconds();
    }
};

//==============================================================================
struct ChartPhrase
{
    int    id = 0;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    juce::String text;
    std::vector<int> noteIds;
};

//==============================================================================
/** The reference singer's raw f0, sampled on a fixed grid. Rendered as a faint
    ghost line behind the note bars.

    This is the single best hedge in the format: wherever the offline segmenter
    was unsure, the singer still sees the reference's actual scoop and vibrato.
    NaN means unvoiced. */
struct ChartContour
{
    double rateHz = 100.0;
    double startSeconds = 0.0;
    std::vector<float> fracMidi;      ///< NaN == unvoiced
    std::vector<float> confidence;

    bool isEmpty() const noexcept { return fracMidi.empty(); }

    /** Linear index for a time, or -1 when outside the sampled span. */
    int indexForTime (double t) const noexcept
    {
        if (fracMidi.empty() || rateHz <= 0.0)
            return -1;

        const auto i = (int) std::llround ((t - startSeconds) * rateHz);
        return (i >= 0 && i < (int) fracMidi.size()) ? i : -1;
    }

    /** Fractional MIDI at a time, or NaN when unvoiced / out of range. */
    float at (double t) const noexcept
    {
        const auto i = indexForTime (t);
        return i < 0 ? std::numeric_limits<float>::quiet_NaN()
                     : fracMidi[(size_t) i];
    }
};

//==============================================================================
struct ChartWarning
{
    juce::String code;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    juce::String message;
};

//==============================================================================
class Chart
{
public:
    Chart() = default;

    /** Parses a chart.json. Returns nullptr and fills `error` on failure.

        Tolerant by design: unknown fields are ignored, and a missing optional
        block (contour, phrases, warnings) is simply absent rather than fatal.
        Only genuinely unusable input fails — no notes, or a schemaVersion this
        build does not understand. */
    static std::unique_ptr<Chart> parse (const juce::var& json, juce::String& error);

    static std::unique_ptr<Chart> loadFrom (const juce::File& chartJson, juce::String& error);

    //==========================================================================
    const std::vector<ChartNote>&    getNotes()    const noexcept { return notes; }
    const std::vector<ChartPhrase>&  getPhrases()  const noexcept { return phrases; }
    const std::vector<ChartWarning>& getWarnings() const noexcept { return warnings; }
    const ChartContour&              getContour()  const noexcept { return contour; }

    const juce::String& getTitle()  const noexcept { return title; }
    const juce::String& getArtist() const noexcept { return artist; }
    const juce::String& getSongId() const noexcept { return songId; }

    double getDurationSeconds() const noexcept { return durationSeconds; }
    double getTuningHz()        const noexcept { return tuningHz; }

    /** Relative path of the backing audio, as written in the chart. May be empty. */
    const juce::String& getBackingAudioRelative() const noexcept { return backingAudio; }

    /** Relative path of the REFERENCE VOCAL — the audio this chart was analysed
        from, so it is guaranteed to be time-aligned with the notes. May be
        empty. Used for optional guide-vocal playback. */
    const juce::String& getReferenceAudioRelative() const noexcept { return referenceAudio; }

    /** "forced-alignment" | "asr" | "manual" | "none".

        The UI MUST distinguish these. ASR sits around 30% word error on sung
        vocals, so auto-transcribed words are rendered in italic with a badge —
        presenting them as authoritative would have the singer practising the
        wrong words. */
    const juce::String& getLyricSource() const noexcept { return lyricSource; }

    bool hasLyrics() const noexcept { return lyricNoteCount > 0; }
    int getLyricNoteCount() const noexcept { return lyricNoteCount; }

    /** Directory the chart.json was loaded from — used to resolve relative
        audio paths. Empty when parsed from memory. */
    const juce::File& getBaseDirectory() const noexcept { return baseDirectory; }

    juce::File resolveBackingAudio() const;

    int getLowestMidi()  const noexcept { return lowestMidi; }
    int getHighestMidi() const noexcept { return highestMidi; }

    bool isEmpty() const noexcept { return notes.empty(); }

    //==========================================================================
    /** Index of the note sounding at `t`, or -1. Notes are sorted and
        non-overlapping, so this is a binary search. */
    int noteIndexAt (double t) const noexcept;

    /** Index of the first note starting at or after `t`, or -1 if none. */
    int nextNoteIndexFrom (double t) const noexcept;

    /** The note the singer should be aiming at right now: the sounding note if
        there is one, otherwise the next note within `lookaheadSeconds` so the
        detector's range constraint is already correct when the note arrives.
        Returns -1 when neither applies. */
    int targetNoteIndex (double t, double lookaheadSeconds = 0.5) const noexcept;

    /** Half-open index range [first, last) of notes overlapping [t0, t1).
        Returns false when nothing is visible. */
    bool notesOverlapping (double t0, double t1, int& first, int& last) const noexcept;

    /** Index of the phrase containing `t`, or -1. */
    int phraseIndexAt (double t) const noexcept;

    const ChartNote* noteById (int id) const noexcept;

    /** Index of the note with this id, or -1. Use this rather than caching an
        index across an edit — see the MUTATION note at the top of this file. */
    int indexForId (int id) const noexcept;

    //==========================================================================
    // EDITING (message thread only). Every one of these keeps the sort order
    // and the no-overlap invariant intact, and bumps the modified flag.
    //==========================================================================

    /** The file this chart was read from, or an invalid File for a chart built
        in memory (a generated exercise). Saving needs it both as the default
        destination and as the source of the fields this build does not parse. */
    const juce::File& getSourceFile() const noexcept { return sourceFile; }

    /** True once anything has been edited and not yet saved. */
    bool isModified() const noexcept { return modified; }
    void markSaved() noexcept { modified = false; }

    /** Replaces a note's text. An EMPTY string is legal and meaningful — it
        removes the syllable, which is the right fix when the transcriber
        invented a word that was never sung. */
    bool setNoteSyllable (int index, const juce::String& syllable);

    /** Transposes one note. Clamped to 0-127. This is the fix for the single
        most common chart defect: a note the segmenter put an octave out. */
    bool setNoteMidi (int index, int midi);

    /** Moves and/or resizes a note, CLAMPED so it cannot cross or overlap its
        neighbours. Returns false only for an out-of-range index. Because this
        can re-sort, the caller must re-resolve indices afterwards. */
    bool setNoteTiming (int index, double startSeconds, double durationSeconds);

    bool setNoteType (int index, NoteType type);

    bool removeNote (int index);

    /** Inserts a note, giving it a fresh unique id. Returns the id, or -1 if it
        would not fit in the gap available. */
    int insertNote (double startSeconds, double durationSeconds, int midi,
                    const juce::String& syllable = {});

    /** Records that a human has taken responsibility for the words.

        This is not bookkeeping: LyricStripComponent renders "asr" lyrics in
        italic with an auto-transcribed badge, precisely so the singer never
        practises a guess as though it were the truth. Once the words HAVE been
        checked by hand, leaving that badge up is its own kind of lie. */
    void markLyricsManual();

    /** Rewrites the syllables of every note in a phrase from a line of text,
        one whitespace-separated token per note.

        This is the practical way to fix ASR output: nobody wants to click
        thirty notes to correct one line, and the alignment is already right —
        it is only the words that are wrong. Tokens run out: the remaining notes
        are cleared. Notes run out: the surplus tokens are appended to the last
        note so no typed text is silently discarded. */
    bool setPhraseText (int phraseIndex, const juce::String& text);

private:
    juce::String songId, title, artist, backingAudio, referenceAudio;
    juce::String lyricSource { "none" };
    int lyricNoteCount = 0;
    double durationSeconds = 0.0;
    double tuningHz = 440.0;

    std::vector<ChartNote>    notes;      ///< sorted by startSeconds
    std::vector<ChartPhrase>  phrases;    ///< sorted by startSeconds
    std::vector<ChartWarning> warnings;
    ChartContour              contour;

    int lowestMidi = 60, highestMidi = 60;

    juce::File baseDirectory;
    juce::File sourceFile;
    bool       modified = false;

    void finalise();

    /** Post-edit fix-up: re-sort, recount lyrics, recompute the range. Cheap
        enough at chart sizes (a few thousand notes) to run after every single
        edit, which is worth far more than the microseconds it costs — a
        half-updated chart is a rendering bug that looks like a physics one. */
    void refreshAfterEdit();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Chart)
};

} // namespace vt
