#include "ChartWriter.h"

namespace vt
{

namespace
{
    juce::var childOf (const juce::var& parent, const char* key)
    {
        if (auto* o = parent.getDynamicObject())
            if (o->hasProperty (key))
                return o->getProperty (key);

        return {};
    }
}

//==============================================================================
juce::String ChartWriter::noteTypeToString (NoteType t)
{
    switch (t)
    {
        case NoteType::golden:    return "golden";
        case NoteType::freestyle: return "freestyle";
        case NoteType::rap:       return "rap";
        case NoteType::breath:    return "breath";
        case NoteType::normal:    break;
    }

    return "normal";
}

juce::String ChartWriter::syllableRoleToString (SyllableRole r)
{
    switch (r)
    {
        case SyllableRole::wordStart:    return "wordStart";
        case SyllableRole::continuation: return "continuation";
        case SyllableRole::melisma:      return "melisma";
        case SyllableRole::none:         break;
    }

    return {};
}

//==============================================================================
void ChartWriter::mergeUnknownProperties (juce::DynamicObject& into, const juce::var& from)
{
    auto* source = from.getDynamicObject();

    if (source == nullptr)
        return;

    const auto& properties = source->getProperties();

    for (int i = 0; i < properties.size(); ++i)
    {
        const auto name = properties.getName (i);

        // Anything we have already written wins; everything else is carried
        // through untouched.
        if (! into.hasProperty (name))
            into.setProperty (name, properties.getValueAt (i));
    }
}

//==============================================================================
juce::var ChartWriter::readOriginal (const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    juce::var parsed;

    if (juce::JSON::parse (file.loadFileAsString(), parsed).wasOk())
        return parsed;

    // A chart.json that no longer parses is not a reason to refuse to save: the
    // in-memory chart is the good copy. Merging nothing loses the unread fields,
    // which is regrettable but strictly better than losing the edit.
    return {};
}

//==============================================================================
juce::var ChartWriter::toJson (const Chart& chart, const juce::var& originalJson)
{
    //-------------------------------------------------------------------- notes
    juce::Array<juce::var> notesArray;

    for (const auto& n : chart.getNotes())
    {
        juce::DynamicObject::Ptr note (new juce::DynamicObject());
        note->setProperty ("id", n.id);
        note->setProperty ("startSeconds", n.startSeconds);
        note->setProperty ("durationSeconds", n.durationSeconds);
        note->setProperty ("midi", n.midi);
        note->setProperty ("centsOffset", (double) n.centsOffset);
        note->setProperty ("type", noteTypeToString (n.type));

        // Explicit null rather than "" for a note with no text: the schema's
        // "syllable: null" means "this note has no word", and an empty string
        // could be read as "this note has an empty word".
        if (n.syllable.isNotEmpty())
            note->setProperty ("syllable", n.syllable);
        else
            note->setProperty ("syllable", juce::var());

        if (const auto role = syllableRoleToString (n.syllableRole); role.isNotEmpty())
            note->setProperty ("syllableRole", role);

        note->setProperty ("phraseId", n.phraseId);
        note->setProperty ("confidence", (double) n.confidence);

        notesArray.add (juce::var (note.get()));
    }

    //------------------------------------------------------------------ phrases
    juce::Array<juce::var> phrasesArray;

    for (const auto& p : chart.getPhrases())
    {
        juce::DynamicObject::Ptr phrase (new juce::DynamicObject());
        phrase->setProperty ("id", p.id);
        phrase->setProperty ("startSeconds", p.startSeconds);
        phrase->setProperty ("endSeconds", p.endSeconds);
        phrase->setProperty ("text", p.text);

        juce::Array<juce::var> ids;

        for (const auto id : p.noteIds)
            ids.add (id);

        phrase->setProperty ("noteIds", ids);
        phrasesArray.add (juce::var (phrase.get()));
    }

    //----------------------------------------------------------------- warnings
    juce::Array<juce::var> warningsArray;

    for (const auto& w : chart.getWarnings())
    {
        juce::DynamicObject::Ptr warning (new juce::DynamicObject());
        warning->setProperty ("code", w.code);
        warning->setProperty ("startSeconds", w.startSeconds);
        warning->setProperty ("endSeconds", w.endSeconds);
        warning->setProperty ("message", w.message);
        warningsArray.add (juce::var (warning.get()));
    }

    //--------------------------------------------------------------------- song
    juce::DynamicObject::Ptr song (new juce::DynamicObject());
    song->setProperty ("id", chart.getSongId());
    song->setProperty ("title", chart.getTitle());
    song->setProperty ("artist", chart.getArtist());
    song->setProperty ("durationSeconds", chart.getDurationSeconds());
    song->setProperty ("tuningHz", chart.getTuningHz());

    if (chart.getBackingAudioRelative().isNotEmpty())
        song->setProperty ("backingAudio", chart.getBackingAudioRelative());

    if (chart.getReferenceAudioRelative().isNotEmpty())
        song->setProperty ("referenceAudio", chart.getReferenceAudioRelative());

    // referenceAudioSha1, bpm, key, and anything else the pipeline recorded.
    mergeUnknownProperties (*song, childOf (originalJson, "song"));

    //---------------------------------------------------------------- generator
    juce::DynamicObject::Ptr generator (new juce::DynamicObject());
    generator->setProperty ("lyricSource", chart.getLyricSource());

    // Records that a human edited this, without pretending the whole chart was
    // hand-made: the pipeline's own generator fields are merged in below.
    generator->setProperty ("editedBy", "VocalTrainer chart editor");
    generator->setProperty ("editedAt", juce::Time::getCurrentTime().toISO8601 (true));

    mergeUnknownProperties (*generator, childOf (originalJson, "generator"));

    //--------------------------------------------------------------------- root
    juce::DynamicObject::Ptr root (new juce::DynamicObject());

    const auto originalSchema = [&originalJson]() -> juce::String
    {
        if (auto* o = originalJson.getDynamicObject())
            if (o->hasProperty ("schemaVersion"))
                return o->getProperty ("schemaVersion").toString();

        return {};
    }();

    root->setProperty ("schemaVersion", originalSchema.isNotEmpty() ? originalSchema
                                                                    : juce::String ("1.0"));
    root->setProperty ("song", juce::var (song.get()));
    root->setProperty ("generator", juce::var (generator.get()));
    root->setProperty ("notes", notesArray);
    root->setProperty ("phrases", phrasesArray);

    if (! warningsArray.isEmpty())
        root->setProperty ("warnings", warningsArray);

    // THE IMPORTANT LINE. Everything else in the original file — contour above
    // all, but also any block a later pipeline version introduced — survives.
    mergeUnknownProperties (*root, originalJson);

    return juce::var (root.get());
}

//==============================================================================
bool ChartWriter::save (const Chart& chart, const juce::File& target, juce::String& error)
{
    error.clear();

    const auto destination = target != juce::File() ? target : chart.getSourceFile();

    if (destination == juce::File())
    {
        error = "This chart was generated rather than loaded, so there is no file "
                "to save it back to. Use Save as... to choose one.";
        return false;
    }

    if (! destination.getParentDirectory().createDirectory().wasOk())
    {
        error = "Could not create " + destination.getParentDirectory().getFullPathName();
        return false;
    }

    // Merge against whatever is on disk NOW, not against what was loaded. If the
    // offline pipeline has re-run since, its new fields are the ones worth
    // keeping.
    const auto original = readOriginal (destination != chart.getSourceFile()
                                          ? chart.getSourceFile()
                                          : destination);

    const auto json = toJson (chart, original);

    if (destination.existsAsFile())
    {
        auto backup = destination.getSiblingFile (destination.getFileName() + ".bak");
        backup.deleteFile();

        if (! destination.copyFileTo (backup))
        {
            // Not fatal, but the user must be told: they are about to overwrite
            // a hand-tuned file with no copy behind it.
            error = "Warning: could not write a .bak backup next to "
                      + destination.getFileName();
        }
    }

    // replaceWithText writes to a temporary and renames, so an out-of-space
    // failure cannot leave a half-written chart.json where a good one was.
    if (! destination.replaceWithText (juce::JSON::toString (json, false, 6)))
    {
        error = "Could not write " + destination.getFullPathName();
        return false;
    }

    return true;
}

} // namespace vt
