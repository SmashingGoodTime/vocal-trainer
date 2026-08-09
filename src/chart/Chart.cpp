#include "Chart.h"

#include <algorithm>
#include <cmath>

namespace vt
{

//==============================================================================
namespace
{
    double getDouble (const juce::var& obj, const char* key, double fallback = 0.0)
    {
        if (auto* o = obj.getDynamicObject())
            if (o->hasProperty (key))
            {
                const auto v = o->getProperty (key);
                if (! v.isVoid() && ! v.isUndefined() && v.toString().isNotEmpty())
                    return (double) v;
            }

        return fallback;
    }

    int getInt (const juce::var& obj, const char* key, int fallback = 0)
    {
        if (auto* o = obj.getDynamicObject())
            if (o->hasProperty (key))
            {
                const auto v = o->getProperty (key);
                if (! v.isVoid() && ! v.isUndefined())
                    return (int) v;
            }

        return fallback;
    }

    juce::String getString (const juce::var& obj, const char* key)
    {
        if (auto* o = obj.getDynamicObject())
            if (o->hasProperty (key))
            {
                const auto v = o->getProperty (key);

                // JSON null must read as "absent", not as the text "null".
                if (v.isVoid() || v.isUndefined() || v.isVoid())
                    return {};

                if (v.isString())
                    return v.toString();
            }

        return {};
    }

    juce::var getChild (const juce::var& obj, const char* key)
    {
        if (auto* o = obj.getDynamicObject())
            if (o->hasProperty (key))
                return o->getProperty (key);

        return {};
    }

    NoteType parseNoteType (const juce::String& s)
    {
        if (s == "golden")    return NoteType::golden;
        if (s == "freestyle") return NoteType::freestyle;
        if (s == "rap")       return NoteType::rap;
        if (s == "breath")    return NoteType::breath;
        return NoteType::normal;
    }

    SyllableRole parseSyllableRole (const juce::String& s)
    {
        if (s == "wordStart")    return SyllableRole::wordStart;
        if (s == "continuation") return SyllableRole::continuation;
        if (s == "melisma")      return SyllableRole::melisma;
        return SyllableRole::none;
    }
} // namespace

//==============================================================================
std::unique_ptr<Chart> Chart::parse (const juce::var& json, juce::String& error)
{
    error.clear();

    if (! json.isObject())
    {
        error = "chart.json did not contain a JSON object.";
        return {};
    }

    // Accept 1.x. A future 2.0 would be a real format break, so refuse it
    // loudly rather than silently misreading fields.
    const auto schema = getString (json, "schemaVersion");

    if (schema.isNotEmpty() && ! schema.startsWith ("1."))
    {
        error = "chart.json is schemaVersion " + schema
              + ", but this build only understands 1.x.";
        return {};
    }

    auto chart = std::make_unique<Chart>();

    //----------------------------------------------------------- generator
    if (const auto gen = getChild (json, "generator"); gen.isObject())
    {
        const auto src = getString (gen, "lyricSource");

        if (src.isNotEmpty())
            chart->lyricSource = src;
    }

    //---------------------------------------------------------------- song
    const auto song = getChild (json, "song");

    chart->songId          = getString (song, "id");
    chart->title           = getString (song, "title");
    chart->artist          = getString (song, "artist");
    chart->backingAudio    = getString (song, "backingAudio");
    chart->referenceAudio  = getString (song, "referenceAudio");
    chart->durationSeconds = getDouble (song, "durationSeconds", 0.0);
    chart->tuningHz        = getDouble (song, "tuningHz", 440.0);

    if (! (chart->tuningHz > 0.0))
        chart->tuningHz = 440.0;

    if (chart->title.isEmpty())
        chart->title = chart->songId.isNotEmpty() ? chart->songId : juce::String ("Untitled");

    //--------------------------------------------------------------- notes
    if (const auto notesVar = getChild (json, "notes"); auto* arr = notesVar.getArray())
    {
        chart->notes.reserve ((size_t) arr->size());

        for (const auto& nv : *arr)
        {
            ChartNote n;
            n.id              = getInt    (nv, "id", (int) chart->notes.size());
            n.startSeconds    = getDouble (nv, "startSeconds", 0.0);
            n.durationSeconds = getDouble (nv, "durationSeconds", 0.0);
            n.midi            = getInt    (nv, "midi", 60);
            n.centsOffset     = (float) getDouble (nv, "centsOffset", 0.0);
            n.type            = parseNoteType (getString (nv, "type"));
            n.syllable        = getString (nv, "syllable");     // "" when JSON null
            n.syllableRole    = parseSyllableRole (getString (nv, "syllableRole"));
            n.phraseId        = getInt    (nv, "phraseId", -1);
            n.confidence      = (float) getDouble (nv, "confidence", 1.0);

            // Drop only genuinely unusable notes. A zero-length note would make
            // the highway draw a zero-width bar and could divide by zero in the
            // syllable wipe.
            if (n.durationSeconds > 0.0 && n.midi >= 0 && n.midi <= 127)
                chart->notes.push_back (std::move (n));
        }
    }

    if (chart->notes.empty())
    {
        error = "chart.json contains no usable notes.";
        return {};
    }

    //------------------------------------------------------------- phrases
    if (const auto phrasesVar = getChild (json, "phrases"); auto* arr = phrasesVar.getArray())
    {
        chart->phrases.reserve ((size_t) arr->size());

        for (const auto& pv : *arr)
        {
            ChartPhrase p;
            p.id           = getInt    (pv, "id", (int) chart->phrases.size());
            p.startSeconds = getDouble (pv, "startSeconds", 0.0);
            p.endSeconds   = getDouble (pv, "endSeconds", 0.0);
            p.text         = getString (pv, "text");

            if (const auto idsVar = getChild (pv, "noteIds"); auto* ids = idsVar.getArray())
                for (const auto& idv : *ids)
                    p.noteIds.push_back ((int) idv);

            if (p.endSeconds > p.startSeconds)
                chart->phrases.push_back (std::move (p));
        }
    }

    //------------------------------------------------------------- contour
    if (const auto cv = getChild (json, "contour"); cv.isObject())
    {
        chart->contour.rateHz       = getDouble (cv, "rateHz", 100.0);
        chart->contour.startSeconds = getDouble (cv, "startSeconds", 0.0);

        if (! (chart->contour.rateHz > 0.0))
            chart->contour.rateHz = 100.0;

        if (const auto fv = getChild (cv, "fracMidi"); auto* arr = fv.getArray())
        {
            chart->contour.fracMidi.reserve ((size_t) arr->size());

            for (const auto& v : *arr)
            {
                // null == unvoiced. Anything non-numeric is treated the same way.
                if (v.isVoid() || v.isUndefined() || v.isString())
                    chart->contour.fracMidi.push_back (std::numeric_limits<float>::quiet_NaN());
                else
                    chart->contour.fracMidi.push_back ((float) (double) v);
            }
        }

        if (const auto cfv = getChild (cv, "confidence"); auto* arr = cfv.getArray())
        {
            chart->contour.confidence.reserve ((size_t) arr->size());

            for (const auto& v : *arr)
                chart->contour.confidence.push_back ((v.isVoid() || v.isUndefined() || v.isString())
                                                        ? 0.0f : (float) (double) v);
        }
    }

    //------------------------------------------------------------ warnings
    if (const auto wv = getChild (json, "warnings"); auto* arr = wv.getArray())
    {
        for (const auto& v : *arr)
        {
            ChartWarning w;
            w.code         = getString (v, "code");
            w.startSeconds = getDouble (v, "startSeconds", 0.0);
            w.endSeconds   = getDouble (v, "endSeconds", 0.0);
            w.message      = getString (v, "message");

            if (w.message.isNotEmpty() || w.code.isNotEmpty())
                chart->warnings.push_back (std::move (w));
        }
    }

    chart->finalise();
    return chart;
}

//==============================================================================
std::unique_ptr<Chart> Chart::loadFrom (const juce::File& chartJson, juce::String& error)
{
    error.clear();

    if (! chartJson.existsAsFile())
    {
        error = "No such file: " + chartJson.getFullPathName();
        return {};
    }

    juce::var parsed;
    const auto result = juce::JSON::parse (chartJson.loadFileAsString(), parsed);

    if (result.failed())
    {
        error = "Could not parse " + chartJson.getFileName() + ": " + result.getErrorMessage();
        return {};
    }

    auto chart = parse (parsed, error);

    if (chart != nullptr)
    {
        chart->baseDirectory = chartJson.getParentDirectory();
        chart->sourceFile    = chartJson;
    }

    return chart;
}

//==============================================================================
void Chart::finalise()
{
    std::sort (notes.begin(), notes.end(),
               [] (const ChartNote& a, const ChartNote& b)
               { return a.startSeconds < b.startSeconds; });

    // Normalise MICROSCOPIC overlaps left by writers that round start and
    // duration separately (the pipeline serialises both to 6 decimals): two
    // touching notes can arrive with prev.end a few 1e-16 above next.start.
    // The editor's clamps and the scorer's note lookup assume strict
    // non-overlap, so snap the boundary rather than trusting the dust.
    // Anything past a millisecond is real data and is left alone.
    for (size_t i = 1; i < notes.size(); ++i)
    {
        const auto prevEnd = notes[i - 1].endSeconds();
        const auto overlap = prevEnd - notes[i].startSeconds;

        if (overlap > 0.0 && overlap <= 0.001)
        {
            notes[i].durationSeconds = juce::jmax (0.001, notes[i].durationSeconds - overlap);
            notes[i].startSeconds    = prevEnd;
        }
    }

    std::sort (phrases.begin(), phrases.end(),
               [] (const ChartPhrase& a, const ChartPhrase& b)
               { return a.startSeconds < b.startSeconds; });

    lyricNoteCount = 0;

    for (const auto& n : notes)
        if (n.syllable.isNotEmpty())
            ++lyricNoteCount;

    lowestMidi  = notes.front().midi;
    highestMidi = notes.front().midi;

    for (const auto& n : notes)
    {
        lowestMidi  = std::min (lowestMidi,  n.midi);
        highestMidi = std::max (highestMidi, n.midi);
    }

    // A chart with no stated duration still has to scroll to the end, so fall
    // back to the last note plus a little air.
    if (durationSeconds <= 0.0)
        durationSeconds = notes.back().endSeconds() + 2.0;
}

//==============================================================================
juce::File Chart::resolveBackingAudio() const
{
    if (backingAudio.isEmpty() || baseDirectory == juce::File())
        return {};

    return baseDirectory.getChildFile (backingAudio);
}

//==============================================================================
int Chart::noteIndexAt (double t) const noexcept
{
    if (notes.empty())
        return -1;

    // First note whose start is > t, then step back one.
    const auto it = std::upper_bound (notes.begin(), notes.end(), t,
                                      [] (double time, const ChartNote& n)
                                      { return time < n.startSeconds; });

    if (it == notes.begin())
        return -1;

    const auto idx = (int) std::distance (notes.begin(), it) - 1;
    return notes[(size_t) idx].containsTime (t) ? idx : -1;
}

int Chart::nextNoteIndexFrom (double t) const noexcept
{
    const auto it = std::lower_bound (notes.begin(), notes.end(), t,
                                      [] (const ChartNote& n, double time)
                                      { return n.startSeconds < time; });

    return it == notes.end() ? -1 : (int) std::distance (notes.begin(), it);
}

int Chart::targetNoteIndex (double t, double lookaheadSeconds) const noexcept
{
    if (const auto sounding = noteIndexAt (t); sounding >= 0)
        return sounding;

    const auto next = nextNoteIndexFrom (t);

    if (next < 0)
        return -1;

    return (notes[(size_t) next].startSeconds - t) <= lookaheadSeconds ? next : -1;
}

bool Chart::notesOverlapping (double t0, double t1, int& first, int& last) const noexcept
{
    first = last = 0;

    if (notes.empty() || t1 <= t0)
        return false;

    // Notes are non-overlapping and sorted, so the first candidate is the one
    // sounding at t0 (if any), otherwise the first starting after it.
    auto begin = nextNoteIndexFrom (t0);

    if (const auto sounding = noteIndexAt (t0); sounding >= 0)
        begin = sounding;

    if (begin < 0)
        return false;

    const auto endIt = std::lower_bound (notes.begin(), notes.end(), t1,
                                         [] (const ChartNote& n, double time)
                                         { return n.startSeconds < time; });

    first = begin;
    last  = (int) std::distance (notes.begin(), endIt);

    return last > first;
}

int Chart::phraseIndexAt (double t) const noexcept
{
    for (size_t i = 0; i < phrases.size(); ++i)
        if (t >= phrases[i].startSeconds && t < phrases[i].endSeconds)
            return (int) i;

    return -1;
}

const ChartNote* Chart::noteById (int id) const noexcept
{
    for (const auto& n : notes)
        if (n.id == id)
            return &n;

    return nullptr;
}

int Chart::indexForId (int id) const noexcept
{
    for (size_t i = 0; i < notes.size(); ++i)
        if (notes[i].id == id)
            return (int) i;

    return -1;
}

//==============================================================================
// Editing — message thread only. See the MUTATION note in Chart.h.
//==============================================================================
namespace
{
    /** Smallest note this build will produce. Below this the highway draws a
        bar narrower than its own border and the syllable wipe divides by
        something indistinguishable from zero. */
    constexpr double kMinNoteSeconds = 0.05;

    /** Kept clear between adjacent notes so they stay visibly separate and so
        TakeScorer's note-boundary median clear has an unambiguous edge. */
    constexpr double kMinNoteGapSeconds = 0.01;
}

void Chart::refreshAfterEdit()
{
    std::sort (notes.begin(), notes.end(),
               [] (const ChartNote& a, const ChartNote& b)
               { return a.startSeconds < b.startSeconds; });

    lyricNoteCount = 0;

    for (const auto& n : notes)
        if (n.syllable.isNotEmpty())
            ++lyricNoteCount;

    if (! notes.empty())
    {
        lowestMidi = highestMidi = notes.front().midi;

        for (const auto& n : notes)
        {
            lowestMidi  = std::min (lowestMidi,  n.midi);
            highestMidi = std::max (highestMidi, n.midi);
        }

        // Never SHRINK the duration on an edit: a chart's duration also decides
        // how far the backing track is expected to run, and quietly cropping it
        // because the last note was shortened would cut the song's outro off.
        durationSeconds = std::max (durationSeconds, notes.back().endSeconds() + 2.0);
    }

    modified = true;
}

bool Chart::setNoteSyllable (int index, const juce::String& syllable)
{
    if (index < 0 || index >= (int) notes.size())
        return false;

    notes[(size_t) index].syllable = syllable.trim();

    // A note that now has text but no role would render as a bare syllable with
    // no word grouping; treat a hand-typed syllable as the start of a word,
    // which is what it almost always is.
    if (notes[(size_t) index].syllable.isNotEmpty()
        && notes[(size_t) index].syllableRole == SyllableRole::none)
        notes[(size_t) index].syllableRole = SyllableRole::wordStart;

    refreshAfterEdit();
    return true;
}

bool Chart::setNoteMidi (int index, int midi)
{
    if (index < 0 || index >= (int) notes.size())
        return false;

    notes[(size_t) index].midi = juce::jlimit (0, 127, midi);

    // The reference singer's deviation belonged to the OLD pitch. Carrying it
    // over would leave a hand-corrected note deliberately 40 cents off the note
    // the user just chose, and the target bar would not sit where they put it.
    notes[(size_t) index].centsOffset = 0.0f;

    refreshAfterEdit();
    return true;
}

bool Chart::setNoteTiming (int index, double startSeconds, double durationSeconds_)
{
    if (index < 0 || index >= (int) notes.size())
        return false;

    // Clamp against the neighbours rather than rejecting the edit: dragging a
    // note into its neighbour is a perfectly reasonable gesture, and the useful
    // response is to stop it at the boundary, not to do nothing.
    const auto lowerBound = index > 0
                              ? notes[(size_t) (index - 1)].endSeconds() + kMinNoteGapSeconds
                              : 0.0;

    const auto upperBound = (index + 1) < (int) notes.size()
                              ? notes[(size_t) (index + 1)].startSeconds - kMinNoteGapSeconds
                              : std::numeric_limits<double>::max();

    auto start = std::max (lowerBound, startSeconds);
    auto duration = std::max (kMinNoteSeconds, durationSeconds_);

    // If the note no longer fits before the next one, shorten it rather than
    // pushing the next one along — a timing fix must never cascade through the
    // rest of the song.
    if (start + duration > upperBound)
    {
        if (upperBound - start >= kMinNoteSeconds)
        {
            duration = upperBound - start;
        }
        else
        {
            start = std::max (lowerBound, upperBound - kMinNoteSeconds);
            duration = std::max (kMinNoteSeconds, upperBound - start);
        }
    }

    notes[(size_t) index].startSeconds    = start;
    notes[(size_t) index].durationSeconds = duration;

    refreshAfterEdit();
    return true;
}

bool Chart::setNoteType (int index, NoteType type)
{
    if (index < 0 || index >= (int) notes.size())
        return false;

    notes[(size_t) index].type = type;
    refreshAfterEdit();
    return true;
}

bool Chart::removeNote (int index)
{
    if (index < 0 || index >= (int) notes.size())
        return false;

    // Never delete the last note. Chart::parse() refuses a chart with no notes,
    // so a chart edited down to nothing could be saved but never loaded again —
    // an edit that destroys the file it is editing.
    if (notes.size() <= 1)
        return false;

    notes.erase (notes.begin() + index);
    refreshAfterEdit();
    return true;
}

int Chart::insertNote (double startSeconds, double durationSeconds_, int midi,
                       const juce::String& syllable)
{
    auto start = std::max (0.0, startSeconds);
    auto duration = std::max (kMinNoteSeconds, durationSeconds_);

    // Find the gap this lands in and fit inside it.
    for (const auto& n : notes)
    {
        if (start < n.endSeconds() && (start + duration) > n.startSeconds)
        {
            if (start < n.startSeconds)
            {
                duration = n.startSeconds - kMinNoteGapSeconds - start;
            }
            else
            {
                start = n.endSeconds() + kMinNoteGapSeconds;
                duration = std::max (kMinNoteSeconds, duration);
            }
        }
    }

    if (duration < kMinNoteSeconds)
        return -1;

    // Re-check: moving the start may have walked into the NEXT note.
    for (const auto& n : notes)
        if (start < n.endSeconds() && (start + duration) > n.startSeconds)
            return -1;

    int nextId = 0;

    for (const auto& n : notes)
        nextId = std::max (nextId, n.id + 1);

    ChartNote note;
    note.id              = nextId;
    note.startSeconds    = start;
    note.durationSeconds = duration;
    note.midi            = juce::jlimit (0, 127, midi);
    note.type            = NoteType::normal;
    note.syllable        = syllable.trim();
    note.syllableRole    = note.syllable.isNotEmpty() ? SyllableRole::wordStart
                                                      : SyllableRole::none;
    note.confidence      = 1.0f;

    // Inherit the phrase of whatever it was inserted into, so a hand-added note
    // is included in that phrase's score rather than floating outside every one.
    note.phraseId = -1;

    for (size_t i = 0; i < phrases.size(); ++i)
        if (start >= phrases[i].startSeconds && start < phrases[i].endSeconds)
        {
            note.phraseId = phrases[i].id;
            phrases[i].noteIds.push_back (note.id);
            break;
        }

    notes.push_back (std::move (note));
    refreshAfterEdit();
    return nextId;
}

void Chart::markLyricsManual()
{
    lyricSource = "manual";
    modified = true;
}

bool Chart::setPhraseText (int phraseIndex, const juce::String& text)
{
    if (phraseIndex < 0 || phraseIndex >= (int) phrases.size())
        return false;

    auto& phrase = phrases[(size_t) phraseIndex];

    // Collect the phrase's notes IN TIME ORDER. noteIds is written by the
    // pipeline and is not guaranteed sorted, and re-flowing words onto notes in
    // the wrong order would scramble the line being fixed.
    std::vector<int> indices;

    if (! phrase.noteIds.empty())
    {
        for (const auto id : phrase.noteIds)
            if (const auto idx = indexForId (id); idx >= 0)
                indices.push_back (idx);
    }
    else
    {
        int first = 0, last = 0;

        if (notesOverlapping (phrase.startSeconds, phrase.endSeconds, first, last))
            for (int i = first; i < last; ++i)
                indices.push_back (i);
    }

    std::sort (indices.begin(), indices.end(),
               [this] (int a, int b)
               { return notes[(size_t) a].startSeconds < notes[(size_t) b].startSeconds; });

    if (indices.empty())
        return false;

    auto tokens = juce::StringArray::fromTokens (text.trim(), " \t\r\n", "");
    tokens.removeEmptyStrings();

    for (size_t i = 0; i < indices.size(); ++i)
    {
        auto& note = notes[(size_t) indices[i]];

        if (i < (size_t) tokens.size())
        {
            // Last note takes any surplus, so nothing the user typed is thrown
            // away without them seeing where it went.
            const bool isLast = (i + 1) == indices.size();

            if (isLast && tokens.size() > (int) indices.size())
            {
                juce::StringArray rest;

                for (int t = (int) i; t < tokens.size(); ++t)
                    rest.add (tokens[t]);

                note.syllable = rest.joinIntoString (" ");
            }
            else
            {
                note.syllable = tokens[(int) i];
            }

            note.syllableRole = SyllableRole::wordStart;
        }
        else
        {
            note.syllable = {};
        }
    }

    phrase.text = text.trim();

    markLyricsManual();
    refreshAfterEdit();
    return true;
}

} // namespace vt
