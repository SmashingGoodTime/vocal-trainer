#include "ChartEditPanelComponent.h"

namespace vt
{

namespace
{
    /** The resolution the ear notices on an onset. DESIGN.md section 6 puts
        "tight" at 30 ms, so 10 ms nudges are fine enough to fix a note without
        being so fine that correcting a 60 ms error takes six clicks. */
    constexpr double kNudgeSeconds = 0.010;
    constexpr double kLengthNudgeSeconds = 0.025;

    const char* const kNoteNames[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    juce::String midiName (int midi)
    {
        return juce::String (kNoteNames[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }

    /** typeBox item ids. 1-based because 0 means "nothing selected" in
        juce::ComboBox. */
    enum TypeId { typeNormal = 1, typeGolden, typeFreestyle, typeRap, typeBreath };

    NoteType typeForId (int id)
    {
        switch (id)
        {
            case typeGolden:    return NoteType::golden;
            case typeFreestyle: return NoteType::freestyle;
            case typeRap:       return NoteType::rap;
            case typeBreath:    return NoteType::breath;
            default:            return NoteType::normal;
        }
    }

    int idForType (NoteType t)
    {
        switch (t)
        {
            case NoteType::golden:    return typeGolden;
            case NoteType::freestyle: return typeFreestyle;
            case NoteType::rap:       return typeRap;
            case NoteType::breath:    return typeBreath;
            case NoteType::normal:    break;
        }

        return typeNormal;
    }
}

//==============================================================================
ChartEditPanelComponent::ChartEditPanelComponent (Theme& t)
    : theme (t)
{
    positionLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (positionLabel);

    pitchLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (pitchLabel);

    prevButton.setTooltip ("Select the previous note (also moves the playhead to it)");
    prevButton.onClick = [this] { step (-1); };
    addAndMakeVisible (prevButton);

    nextButton.setTooltip ("Select the next note");
    nextButton.onClick = [this] { step (1); };
    addAndMakeVisible (nextButton);

    // The octave buttons are first and biggest because an octave error is the
    // segmenter's dominant failure mode, not because octaves are special.
    octaveDown.setTooltip ("Down an octave - the usual fix when the tracker "
                           "halved or doubled the frequency");
    octaveDown.onClick = [this] { nudgePitch (-12); };
    addAndMakeVisible (octaveDown);

    semiDown.onClick = [this] { nudgePitch (-1); };
    addAndMakeVisible (semiDown);

    semiUp.onClick = [this] { nudgePitch (1); };
    addAndMakeVisible (semiUp);

    octaveUp.setTooltip ("Up an octave");
    octaveUp.onClick = [this] { nudgePitch (12); };
    addAndMakeVisible (octaveUp);

    earlier.setTooltip ("Move this note 10 ms earlier");
    earlier.onClick = [this] { nudgeStart (-kNudgeSeconds); };
    addAndMakeVisible (earlier);

    later.setTooltip ("Move this note 10 ms later");
    later.onClick = [this] { nudgeStart (kNudgeSeconds); };
    addAndMakeVisible (later);

    shorter.setTooltip ("Shorten this note by 25 ms");
    shorter.onClick = [this] { nudgeLength (-kLengthNudgeSeconds); };
    addAndMakeVisible (shorter);

    longer.setTooltip ("Lengthen this note by 25 ms");
    longer.onClick = [this] { nudgeLength (kLengthNudgeSeconds); };
    addAndMakeVisible (longer);

    typeBox.addItem ("Normal",    typeNormal);
    typeBox.addItem ("Golden",    typeGolden);
    typeBox.addItem ("Freestyle", typeFreestyle);
    typeBox.addItem ("Rap",       typeRap);
    typeBox.addItem ("Breath",    typeBreath);
    typeBox.setTooltip ("Freestyle, rap and breath notes are not scored on pitch - "
                        "use them where the chart found a note that is not really "
                        "a sung pitch.");
    typeBox.onChange = [this] { applyType(); };
    addAndMakeVisible (typeBox);

    deleteButton.setTooltip ("Remove this note. Use it where the tracker heard a "
                             "note in a breath or a consonant.");
    deleteButton.onClick = [this] { deleteSelected(); };
    addAndMakeVisible (deleteButton);

    insertButton.setTooltip ("Add a note in the gap after this one, at the same pitch");
    insertButton.onClick = [this] { insertAfterSelected(); };
    addAndMakeVisible (insertButton);

    saveButton.setTooltip ("Write these changes back to chart.json. The previous "
                           "file is kept as chart.json.bak.");
    saveButton.onClick = [this] { if (onSave != nullptr) onSave(); };
    addAndMakeVisible (saveButton);

    syllableCaption.setText ("Word", juce::dontSendNotification);
    syllableCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (syllableCaption);

    syllableEditor.setMultiLine (false);
    syllableEditor.setReturnKeyStartsNewLine (false);
    syllableEditor.setTooltip ("The word or syllable sung on this note. Leave it "
                               "empty for a note with no lyric - that is normal and "
                               "common on held notes.");
    syllableEditor.onReturnKey = [this] { commitSyllable(); };
    syllableEditor.onFocusLost = [this] { commitSyllable(); };
    addAndMakeVisible (syllableEditor);

    phraseCaption.setText ("Line", juce::dontSendNotification);
    phraseCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (phraseCaption);

    phraseEditor.setMultiLine (false);
    phraseEditor.setReturnKeyStartsNewLine (false);
    phraseEditor.setTooltip ("Retype the whole line and press Return: the words are "
                             "spread one per note across this phrase. This is the fast "
                             "way to fix a line the transcriber got wrong.");
    phraseEditor.onReturnKey = [this] { commitPhraseText(); };
    addAndMakeVisible (phraseEditor);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);
}

//==============================================================================
void ChartEditPanelComponent::setChart (Chart* newChart)
{
    chart = newChart;
    selectedNoteId = -1;

    if (chart != nullptr && ! chart->isEmpty())
        selectedNoteId = chart->getNotes().front().id;

    refresh();
}

void ChartEditPanelComponent::setSelectedNoteId (int noteId)
{
    if (noteId == selectedNoteId)
        return;

    selectedNoteId = noteId;
    refresh();
}

void ChartEditPanelComponent::selectNoteAtTime (double chartSeconds)
{
    if (chart == nullptr || chart->isEmpty())
        return;

    auto index = chart->noteIndexAt (chartSeconds);

    if (index < 0)
        index = chart->nextNoteIndexFrom (chartSeconds);

    if (index < 0)
        index = (int) chart->getNotes().size() - 1;

    setSelectedNoteId (chart->getNotes()[(size_t) index].id);
}

int ChartEditPanelComponent::selectedIndex() const noexcept
{
    return chart != nullptr ? chart->indexForId (selectedNoteId) : -1;
}

const ChartNote* ChartEditPanelComponent::selectedNote() const noexcept
{
    const auto index = selectedIndex();
    return index >= 0 ? &chart->getNotes()[(size_t) index] : nullptr;
}

//==============================================================================
void ChartEditPanelComponent::refresh()
{
    const juce::ScopedValueSetter<bool> guard (updating, true);

    const auto* note = selectedNote();
    const auto haveNote = note != nullptr;

    for (auto* c : { &prevButton, &nextButton, &octaveDown, &semiDown, &semiUp, &octaveUp,
                     &earlier, &later, &shorter, &longer, &deleteButton, &insertButton })
        c->setEnabled (haveNote);

    typeBox.setEnabled (haveNote);
    syllableEditor.setEnabled (haveNote);
    phraseEditor.setEnabled (haveNote);
    saveButton.setEnabled (chart != nullptr);

    if (! haveNote)
    {
        positionLabel.setText ("No note selected", juce::dontSendNotification);
        pitchLabel.setText ({}, juce::dontSendNotification);
        syllableEditor.setText ({}, false);
        phraseEditor.setText ({}, false);
        return;
    }

    const auto index = selectedIndex();

    positionLabel.setText ("Note " + juce::String (index + 1) + " / "
                             + juce::String ((int) chart->getNotes().size())
                             + "    " + juce::String (note->startSeconds, 2) + " s"
                             + "    " + juce::String (note->durationSeconds * 1000.0, 0) + " ms",
                           juce::dontSendNotification);

    pitchLabel.setText (midiName (note->midi) + "  (" + juce::String (note->midi) + ")",
                        juce::dontSendNotification);

    syllableEditor.setText (note->syllable, false);
    typeBox.setSelectedId (idForType (note->type), juce::dontSendNotification);

    // The phrase field shows the whole line the note belongs to, so retyping it
    // is a matter of editing what you can already see.
    const auto phraseIndex = chart->phraseIndexAt (note->startSeconds);

    if (phraseIndex >= 0)
    {
        const auto& phrase = chart->getPhrases()[(size_t) phraseIndex];

        // Prefer the words actually on the notes over the phrase's stored text:
        // after an edit the stored text is the one that is stale.
        juce::StringArray words;

        int first = 0, last = 0;

        if (chart->notesOverlapping (phrase.startSeconds, phrase.endSeconds, first, last))
            for (int i = first; i < last; ++i)
                if (chart->getNotes()[(size_t) i].syllable.isNotEmpty())
                    words.add (chart->getNotes()[(size_t) i].syllable);

        phraseEditor.setText (words.isEmpty() ? phrase.text : words.joinIntoString (" "), false);
        phraseEditor.setEnabled (true);
    }
    else
    {
        phraseEditor.setText ({}, false);
        phraseEditor.setEnabled (false);
    }
}

void ChartEditPanelComponent::setStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}

//==============================================================================
void ChartEditPanelComponent::afterEdit()
{
    refresh();

    if (onSelectionChanged != nullptr)
        onSelectionChanged (selectedNoteId);

    if (onChartEdited != nullptr)
        onChartEdited();
}

void ChartEditPanelComponent::nudgePitch (int semitones)
{
    const auto index = selectedIndex();
    const auto* note = selectedNote();

    if (index < 0 || note == nullptr)
        return;

    chart->setNoteMidi (index, note->midi + semitones);
    setStatus ("Pitch changed. Save to keep it.");
    afterEdit();
}

void ChartEditPanelComponent::nudgeStart (double seconds)
{
    const auto index = selectedIndex();
    const auto* note = selectedNote();

    if (index < 0 || note == nullptr)
        return;

    // Start moves, length is held: nudging the onset of a note should not also
    // change how long it is.
    chart->setNoteTiming (index, note->startSeconds + seconds, note->durationSeconds);
    setStatus ("Timing changed. Save to keep it.");
    afterEdit();
}

void ChartEditPanelComponent::nudgeLength (double seconds)
{
    const auto index = selectedIndex();
    const auto* note = selectedNote();

    if (index < 0 || note == nullptr)
        return;

    chart->setNoteTiming (index, note->startSeconds, note->durationSeconds + seconds);
    setStatus ("Length changed. Save to keep it.");
    afterEdit();
}

void ChartEditPanelComponent::step (int delta)
{
    if (chart == nullptr || chart->isEmpty())
        return;

    const auto index = selectedIndex();
    const auto next = juce::jlimit (0, (int) chart->getNotes().size() - 1,
                                    (index < 0 ? 0 : index) + delta);

    selectedNoteId = chart->getNotes()[(size_t) next].id;

    // Move the playhead with the selection. Editing a note you cannot hear is
    // guesswork; this makes stepping through notes also a way of auditioning
    // them.
    if (onSeekTo != nullptr)
        onSeekTo (chart->getNotes()[(size_t) next].startSeconds);

    refresh();

    if (onSelectionChanged != nullptr)
        onSelectionChanged (selectedNoteId);
}

void ChartEditPanelComponent::commitSyllable()
{
    if (updating)
        return;

    const auto index = selectedIndex();

    if (index < 0)
        return;

    const auto typed = syllableEditor.getText().trim();

    if (typed == chart->getNotes()[(size_t) index].syllable)
        return;

    chart->setNoteSyllable (index, typed);

    // A hand-typed word is not an ASR guess any more, so the auto-transcribed
    // badge must come off — see Chart::markLyricsManual.
    chart->markLyricsManual();

    setStatus ("Word changed. Save to keep it.");
    afterEdit();
}

void ChartEditPanelComponent::commitPhraseText()
{
    if (updating || chart == nullptr)
        return;

    const auto* note = selectedNote();

    if (note == nullptr)
        return;

    const auto phraseIndex = chart->phraseIndexAt (note->startSeconds);

    if (phraseIndex < 0)
    {
        setStatus ("This note is not inside a phrase, so there is no line to re-flow.");
        return;
    }

    if (! chart->setPhraseText (phraseIndex, phraseEditor.getText()))
    {
        setStatus ("Could not re-flow that line.");
        return;
    }

    setStatus ("Line re-flowed across the phrase. Save to keep it.");
    afterEdit();
}

void ChartEditPanelComponent::deleteSelected()
{
    const auto index = selectedIndex();

    if (index < 0)
        return;

    // Choose the replacement selection BEFORE deleting, so the panel does not
    // land on nothing.
    const auto notes = (int) chart->getNotes().size();
    const auto nextIndex = juce::jlimit (0, notes - 2, index);

    if (! chart->removeNote (index))
    {
        setStatus ("A chart must keep at least one note.");
        return;
    }

    selectedNoteId = chart->getNotes()[(size_t) juce::jlimit (0, (int) chart->getNotes().size() - 1,
                                                              nextIndex)].id;

    setStatus ("Note deleted. Save to keep it.");
    afterEdit();
}

void ChartEditPanelComponent::insertAfterSelected()
{
    const auto index = selectedIndex();
    const auto* note = selectedNote();

    if (index < 0 || note == nullptr)
        return;

    // Insert into the gap after this note, at the same pitch. Same pitch because
    // the common case is a held note the segmenter split, or a note it dropped
    // in the middle of a run.
    const auto start = note->endSeconds() + 0.02;

    const auto available = (index + 1) < (int) chart->getNotes().size()
                             ? chart->getNotes()[(size_t) (index + 1)].startSeconds - start - 0.01
                             : note->durationSeconds;

    if (available < 0.05)
    {
        setStatus ("No room after this note - lengthen the gap first.");
        return;
    }

    const auto newId = chart->insertNote (start, juce::jmin (available, note->durationSeconds),
                                          note->midi);

    if (newId < 0)
    {
        setStatus ("Could not insert a note there.");
        return;
    }

    selectedNoteId = newId;
    setStatus ("Note inserted. Save to keep it.");
    afterEdit();
}

void ChartEditPanelComponent::applyType()
{
    if (updating)
        return;

    const auto index = selectedIndex();

    if (index < 0)
        return;

    chart->setNoteType (index, typeForId (typeBox.getSelectedId()));
    setStatus ("Note type changed. Save to keep it.");
    afterEdit();
}

//==============================================================================
void ChartEditPanelComponent::applyTheme()
{
    const auto& v = theme.get();

    for (auto* l : { &positionLabel, &pitchLabel })
        l->setColour (juce::Label::textColourId, v.text);

    for (auto* l : { &syllableCaption, &phraseCaption, &statusLabel })
        l->setColour (juce::Label::textColourId, v.textDim);

    for (auto* b : { &prevButton, &nextButton, &octaveDown, &semiDown, &semiUp, &octaveUp,
                     &earlier, &later, &shorter, &longer, &deleteButton, &insertButton,
                     &saveButton })
    {
        b->setColour (juce::TextButton::buttonColourId, v.panel);
        b->setColour (juce::TextButton::textColourOffId, v.text);
    }

    // Save is the only button here with a consequence outside the app, so it is
    // the only one that is coloured.
    saveButton.setColour (juce::TextButton::buttonColourId, v.targetBarUpcoming);

    deleteButton.setColour (juce::TextButton::textColourOffId, v.traceOutOfTune);

    for (auto* e : { &syllableEditor, &phraseEditor })
    {
        e->setColour (juce::TextEditor::backgroundColourId, v.background);
        e->setColour (juce::TextEditor::textColourId, v.text);
        e->setColour (juce::TextEditor::outlineColourId, v.gridLine);
        e->setColour (juce::TextEditor::focusedOutlineColourId, v.targetBarActive);
    }

    typeBox.setColour (juce::ComboBox::backgroundColourId, v.background);
    typeBox.setColour (juce::ComboBox::textColourId, v.text);
    typeBox.setColour (juce::ComboBox::outlineColourId, v.gridLine);

    positionLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    pitchLabel.setFont    (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    statusLabel.setFont   (juce::Font (juce::FontOptions (11.0f)));
    syllableCaption.setFont (juce::Font (juce::FontOptions (11.0f)));
    phraseCaption.setFont   (juce::Font (juce::FontOptions (11.0f)));

    repaint();
}

//==============================================================================
void ChartEditPanelComponent::paint (juce::Graphics& g)
{
    const auto& v = theme.get();

    g.fillAll (v.panel);

    g.setColour (v.gridLine);
    g.fillRect (0, 0, getWidth(), 1);
}

void ChartEditPanelComponent::resized()
{
    auto bounds = getLocalBounds().reduced (8, 4);

    const auto rowHeight = juce::jmax (20, (bounds.getHeight() - 4) / 2);

    //--------------------------------------------------------------- top row
    auto top = bounds.removeFromTop (rowHeight);

    positionLabel.setBounds (top.removeFromLeft (190));
    prevButton.setBounds (top.removeFromLeft (26));
    top.removeFromLeft (2);
    nextButton.setBounds (top.removeFromLeft (26));
    top.removeFromLeft (10);

    pitchLabel.setBounds (top.removeFromLeft (70));

    octaveDown.setBounds (top.removeFromLeft (38));
    top.removeFromLeft (2);
    semiDown.setBounds (top.removeFromLeft (30));
    top.removeFromLeft (2);
    semiUp.setBounds (top.removeFromLeft (30));
    top.removeFromLeft (2);
    octaveUp.setBounds (top.removeFromLeft (38));
    top.removeFromLeft (10);

    earlier.setBounds (top.removeFromLeft (48));
    top.removeFromLeft (2);
    later.setBounds (top.removeFromLeft (48));
    top.removeFromLeft (8);
    shorter.setBounds (top.removeFromLeft (24));
    top.removeFromLeft (2);
    longer.setBounds (top.removeFromLeft (24));

    // Right-aligned, so the destructive and the file-writing buttons are away
    // from the ones being clicked repeatedly.
    saveButton.setBounds (top.removeFromRight (86));
    top.removeFromRight (6);
    deleteButton.setBounds (top.removeFromRight (58));
    top.removeFromRight (4);
    insertButton.setBounds (top.removeFromRight (54));
    top.removeFromRight (6);
    typeBox.setBounds (top.removeFromRight (86));

    bounds.removeFromTop (4);

    //------------------------------------------------------------ bottom row
    auto row = bounds.removeFromTop (rowHeight);

    syllableCaption.setBounds (row.removeFromLeft (34));
    row.removeFromLeft (4);
    syllableEditor.setBounds (row.removeFromLeft (juce::jmin (140, row.getWidth() / 4)));
    row.removeFromLeft (12);

    phraseCaption.setBounds (row.removeFromLeft (30));
    row.removeFromLeft (4);

    // The status line takes a fixed slice and the phrase field takes the rest:
    // the line being retyped is the widest thing in this panel and wants the
    // room.
    auto status = row.removeFromRight (juce::jmin (240, row.getWidth() / 3));
    statusLabel.setBounds (status);
    row.removeFromRight (8);

    phraseEditor.setBounds (row);
}

} // namespace vt
