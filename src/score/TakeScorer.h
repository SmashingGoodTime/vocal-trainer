/*
    ==========================================================================

    TakeScorer.h — per-note scoring of a take, accumulated live.

    Implements DESIGN.md section 6: octave-agnostic, per-frame,
    time-integrated, two-threshold linear ramp. Score is proportional to
    SECONDS SPENT IN TUNE, not to notes hit, which rewards *staying* in tune
    rather than *arriving* in tune and makes onset timing implicitly scored
    without a separate mechanism.

    THE ONE RULE THAT MATTERS MOST HERE
    -----------------------------------
    **Nothing is ever scored from an instantaneous pitch reading.** Everything
    goes through a trailing median first (see MedianWindow below).

    Classical vibrato is 4.5-6.5 Hz at 50-150 cents peak-to-peak, so a singer
    with textbook technique shows +/-25 to +/-60 cents of *instantaneous*
    deviation while being perfectly in tune. Scoring that raw would tell them
    they are constantly flat and sharp, and would teach them to suppress
    vibrato — i.e. it would make them worse at singing. DESIGN.md calls this
    "the single most likely way to build a technically-correct trainer that
    makes the user worse", and it is the reason this class exists as its own
    tested unit instead of a few lines inside the renderer.

    The rule in one line: DISPLAY THE WIGGLE, SCORE THE CENTRE.

    THREADING
    ---------
    Message thread only, like Chart. It is fed from the editor's vblank
    callback, one call per drained pitch estimate, in chart-time order.

    It deliberately depends on nothing but Chart, PitchMath and juce_core, so
    the whole thing is exercised by the console test runner with no audio
    device, no GUI and no timing dependency.

    ==========================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <limits>
#include <vector>

#include "../chart/Chart.h"

namespace vt
{

//==============================================================================
/** A trailing median over the most recent samples.

    Median rather than mean because the failure mode being suppressed is the
    OUTLIER — a single octave-flipped or consonant-corrupted frame — and a mean
    is dragged by exactly those while a median ignores them.

    Cleared on a voicing gap AND at every note boundary. The note-boundary
    clear is the important one: this window exists to average out vibrato
    *within* a sustained note, and letting it straddle two notes would score
    the start of each note partly against the pitch of the one before it. On a
    stepwise scale that alone would read as "you are late and flat on every
    note". */
class MedianWindow
{
public:
    /** Window length in samples. 25 samples at the 125 Hz estimate rate is the
        200 ms live window from DESIGN.md section 6. */
    static constexpr int kMaxLength = 64;

    void setLength (int numSamples) noexcept
    {
        length = juce::jlimit (1, kMaxLength, numSamples);
        clear();
    }

    void clear() noexcept { count = 0; write = 0; }

    /** Adds a sample and returns the median of everything currently held, or
        NaN while fewer than `minimumSamples` have arrived.

        Returning a median of a PARTIAL window rather than waiting for a full
        one is deliberate: a 0.25 s note contains only ~30 estimates, so
        refusing to score until 25 had arrived would throw away most of the
        credit on every short note in the chart. */
    float push (float value, int minimumSamples = 3) noexcept;

    int getCount() const noexcept { return count; }

private:
    std::array<float, (size_t) kMaxLength> samples {};
    std::array<float, (size_t) kMaxLength> scratch {};
    int length = 25;
    int count = 0;
    int write = 0;
};

//==============================================================================
/** What one note of the take came out as. */
struct NoteScore
{
    /** True once enough pitch has been heard on this note to say anything at
        all. Below this the note is "not attempted", which the highway must show
        differently from "attempted and missed" — claiming a note was missed
        when the singer had not started yet is the fastest way to lose their
        trust in the feedback. */
    bool hasData = false;

    /** Of the time the singer WAS producing usable pitch on this note, how much
        credit did it earn: 1.0 = entirely inside the full-credit window.

        The denominator is voiced time, not note duration, on purpose.
        Unvoiced consonants, plosives and breaths produce genuine gaps where no
        pitch exists; dividing by note duration would score a perfectly sung
        "stop" the same as a badly sung sustained vowel. Coverage carries that
        information separately. */
    float accuracy = 0.0f;

    /** Fraction of the note's duration that had usable pitch. Low coverage with
        high accuracy means "sung well but short"; low both means "not sung". */
    float coverage = 0.0f;

    /** Signed mean cents error, octave-folded. NEGATIVE IS FLAT. This is the
        number a teacher would actually say out loud. */
    float meanCents = 0.0f;

    /** Standard deviation of the cents error after the attack, so a scooped
        entry does not read as an unsteady note. NaN when too short to mean
        anything. */
    float steadinessCents = std::numeric_limits<float>::quiet_NaN();

    /** Milliseconds between the note's written start and the singer's first
        voiced frame on it. POSITIVE IS LATE. NaN when never voiced.
        Singers are usually late, and saying so per phrase is one of the
        highest-value non-pitch metrics available. */
    float onsetOffsetMs = std::numeric_limits<float>::quiet_NaN();

    /** Which octave the singer actually used relative to the written note.
        Scoring is octave-agnostic, so this is reported rather than penalised. */
    int octaveOffset = 0;

    double scoredSeconds = 0.0;    ///< voiced time on this note
    double creditSeconds = 0.0;    ///< integral of the credit ramp
    double noteSeconds   = 0.0;    ///< clock time the note was the active target

    /** Voiced time that earned ANY credit at all, i.e. inside tZeroCents.
        Kept separately from creditSeconds because the phrase pass threshold in
        DESIGN.md section 6 was calibrated against "power > 0" ticks, not
        against the credit integral — scoring it with the integral instead would
        silently make the documented 0.45 mean something stricter. */
    double hitSeconds = 0.0;

    /** False for freestyle / rap / breath notes, whose pitch is not scored at
        all (DESIGN.md section 6). Excluded from every summary statistic. */
    bool pitchScored = true;
};

//==============================================================================
/** Per-phrase verdict. DESIGN.md section 6: pass at 0.45 of voiced ticks in
    tune, which looks absurdly low until you read YARG's own source comment
    explaining that unvoiced consonants and plosives make anything higher read
    competent singing as failure. */
struct PhraseScore
{
    int    phraseIndex = -1;
    int    notesAttempted = 0;
    int    notesTotal = 0;
    float  percentHit = 0.0f;      ///< 0-1
    float  meanCents = 0.0f;       ///< signed
    float  meanOnsetMs = 0.0f;     ///< signed, + late
    bool   pass = false;
    double startSeconds = 0.0;     ///< for one-click loop-and-retry
    juce::String text;
};

//==============================================================================
/** Errors grouped by the interval leapt INTO the note.

    "You are consistently flat on ascending 6ths" is actionable in a way that
    "82%" is not — it names a thing to practise. This is the single most useful
    grouping in the review screen. */
struct IntervalError
{
    int   semitones = 0;           ///< absolute size of the leap
    bool  ascending = true;
    int   count = 0;
    float meanCents = 0.0f;        ///< signed error on the note landed on
    float meanAbsCents = 0.0f;

    /** "ascending 6th", "descending 2nd", "same note". */
    juce::String describe() const;
};

//==============================================================================
/** Everything the review screen needs, computed in one pass. */
struct TakeSummary
{
    bool   hasData = false;

    int    notesTotal = 0;         ///< pitch-scored notes in the chart
    int    notesAttempted = 0;
    int    notesGood = 0;          ///< accuracy >= 0.8
    int    notesFair = 0;          ///< 0.45 - 0.8
    int    notesPoor = 0;          ///< attempted, below 0.45
    int    notesMissed = 0;        ///< never voiced

    double scoredSeconds = 0.0;
    double creditSeconds = 0.0;
    double chartNoteSeconds = 0.0;

    /** Fraction of voiced singing time that was in tune. The headline number. */
    float  percentInTune = 0.0f;

    /** Fraction of the chart's scored note time that was sung at all. */
    float  coverage = 0.0f;

    /** Signed mean across attempted notes. The single most useful diagnostic in
        the whole screen: a consistent -20 says "you are singing flat", which is
        a fixable habit, where a mean absolute error says only "you missed". */
    float  meanCents = 0.0f;
    float  meanAbsCents = 0.0f;

    /** Share of attempted notes that were flat / sharp beyond the full-credit
        window. A 70/10 split is a systematic problem; 40/40 is imprecision. */
    float  fractionFlat = 0.0f;
    float  fractionSharp = 0.0f;

    float  meanOnsetMs = 0.0f;     ///< signed, + late
    float  meanAbsOnsetMs = 0.0f;
    int    onsetSamples = 0;

    float  meanSteadinessCents = std::numeric_limits<float>::quiet_NaN();

    /** How much of the take was sung in a different octave than written.
        Not an error — but worth saying, because it changes what the numbers
        mean if the singer transposed the whole song. */
    float  fractionOctaveShifted = 0.0f;
    int    dominantOctaveOffset = 0;

    /** Signed cents-error histogram, 25 cents per bin over +/- 150 cents plus
        an out-of-range bin at each end. Ableton's Tuner histogram is the model:
        the shape of the distribution shows systematic flatness at a glance in a
        way no single number does. Weighted by voiced seconds. */
    static constexpr int kHistogramBins = 14;
    std::array<float, (size_t) kHistogramBins> centsHistogram {};
    float histogramPeak = 0.0f;

    /** Bin centres in cents, for axis labelling. */
    static float histogramBinCentreCents (int bin) noexcept;

    std::vector<PhraseScore>   phrases;        ///< worst-first
    std::vector<IntervalError> intervalErrors; ///< worst-first
    std::vector<int>           worstNoteIndices;

    /** One or two sentences a voice teacher would actually say. Built from the
        numbers above, ordered so the most actionable thing comes first. */
    juce::String headline() const;

    /** The supporting observations, one per line. Empty when there is nothing
        honest to add — padding this out with filler is worse than saying less. */
    juce::StringArray observations() const;
};

//==============================================================================
class TakeScorer
{
public:
    TakeScorer() = default;

    /** Non-owning; the chart must outlive the scorer. Resets everything. */
    void setChart (const Chart* newChart);

    /** Live values from theme.json, same two numbers the target bar's drawn
        height comes from — so "inside the bar" and "full credit" cannot drift
        apart. */
    void setTolerances (float tFullCents, float tZeroCents) noexcept;

    /** Length of the trailing median in milliseconds (theme: colourMedianMs).
        Clamped to something that can still score a short note. */
    void setMedianMs (int ms) noexcept;

    /** Forgets the take. Called on load, on seek and on restart: a take is a
        continuous run at the chart, and stitching together the halves of two
        different attempts would report a performance that never happened. */
    void reset();

    /** One pitch estimate.

        `chartSeconds` is chart time, `rawFracMidi` the RAW (post-median-of-3,
        pre-1-Euro) fractional MIDI, NaN when unvoiced. Must be called in
        non-decreasing chart-time order; a backwards jump is treated as a seek
        and ignored rather than un-scoring anything.

        dt is measured from the chart clock rather than assumed, so this stays
        correct if the clock is ever driven at a different rate (the eventual
        rateFactor slow-down practice mode). */
    void feed (double chartSeconds, float rawFracMidi) noexcept;

    /** Per-note result. Returns a default-constructed score for an out-of-range
        index, so callers never need to bounds-check before painting. */
    const NoteScore& getNoteScore (int noteIndex) const noexcept;

    int getNumNotes() const noexcept { return (int) scores.size(); }

    /** True once any note has usable data — i.e. it is worth showing a review. */
    bool hasAnyData() const noexcept { return notesWithData > 0; }

    /** Chart time of the last estimate fed in. */
    double getLastChartTime() const noexcept { return lastChartTime; }

    /** Full analysis. Cheap enough to call on demand (a few thousand notes at
        worst), so it is NOT cached — a cached summary that went stale mid-take
        would be a silent lie. */
    TakeSummary buildSummary() const;

private:
    void finaliseNote (int noteIndex) noexcept;

    /** Live accumulation for the note currently under the playhead. */
    struct Accumulator
    {
        int    noteIndex = -1;
        double scoredSeconds = 0.0;
        double creditSeconds = 0.0;
        double hitSeconds = 0.0;
        double noteSeconds = 0.0;
        double centsSum = 0.0;
        double absCentsSum = 0.0;
        double postAttackSum = 0.0;
        double postAttackSumSq = 0.0;
        int    postAttackCount = 0;
        int    frames = 0;
        double firstVoicedChartTime = std::numeric_limits<double>::quiet_NaN();
        std::array<int, 5> octaveVotes { 0, 0, 0, 0, 0 };   // offsets -2..+2

        void clear() noexcept { *this = Accumulator {}; }
    };

    const Chart* chart = nullptr;

    std::vector<NoteScore> scores;
    NoteScore              emptyScore;

    Accumulator  live;
    MedianWindow median;

    float tFull = 25.0f;
    float tZero = 100.0f;
    int   medianMs = 200;

    double lastChartTime = std::numeric_limits<double>::quiet_NaN();
    int    notesWithData = 0;

    /** Weighted signed-cents histogram, accumulated live so the review does not
        have to keep every estimate of a four-minute take in memory. */
    std::array<float, (size_t) TakeSummary::kHistogramBins> histogram {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeScorer)
};

} // namespace vt
