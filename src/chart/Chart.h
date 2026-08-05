/*
    ==========================================================================

    Chart.h — the parsed, immutable song chart.

    Loaded from the chart.json emitted by the offline `vocalchart` pipeline.
    See docs/DESIGN.md section 3 for the normative schema.

    THREADING
    ---------
    A Chart is IMMUTABLE once constructed, and it lives on the MESSAGE THREAD
    only. Neither the audio thread nor the analysis worker ever touches it.

    That is a deliberate simplification. The worker needs exactly one number
    from the chart — the MIDI note currently being sung — to constrain the
    detector's tau search. The editor computes that once per frame and
    publishes it through a plain std::atomic<float>. So there is no shared
    ownership across threads, no atomic refcount traffic in the hot path, and
    no lifetime question to get wrong.

    Parsing is done on a background thread and the finished Chart handed to the
    message thread; see ChartLoader.

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

    void finalise();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Chart)
};

} // namespace vt
