/*
    ==========================================================================

    ChartEditPanelComponent.h — fixing what the offline pipeline got wrong.

    WHY THIS EXISTS AT ALL, GIVEN THE ORIGINAL PLAN SAID IT WOULD NOT
    ----------------------------------------------------------------
    The architecture was settled on "lyrics auto-only with zero manual
    correction per song", and that was the right call for the BULK of the work:
    hand-charting 21 songs is what kills projects like this. But the numbers in
    DESIGN.md section 4 are also clear that automatic output is not always
    right — ASR sits around 30% word error on sung vocals, and the note
    segmenter's F-measure target is 0.70, not 1.0.

    So this is not a chart editor in the UltraStar sense, and it deliberately
    does not try to be one. There is no piano roll, no drag-and-drop, no
    multi-select. It fixes the three specific defects that automatic charting
    actually produces:

      1. A WORD IS WRONG. One text field, and a phrase-level re-flow for when a
         whole line came out as nonsense — because nobody is going to click
         thirty notes to correct one line, and the TIMING is already right.
      2. A NOTE IS AN OCTAVE OUT. This is the dominant segmenter failure, so it
         gets its own two buttons (-12 / +12) rather than being buried in a
         spinner.
      3. A NOTE IS A BIT EARLY OR A BIT LONG. Nudges, in tens of milliseconds,
         which is the resolution the ear actually notices.

    Anything more elaborate belongs in the UltraStar sidecar and a real editor.
    The point here is that a defect you can hear while singing can be fixed in
    the five seconds after you hear it, instead of becoming a note in a file
    somewhere that never gets acted on.

    THREADING: message thread. It mutates the Chart directly — see the MUTATION
    note in Chart.h for why that is safe.

    ==========================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "Theme.h"
#include "../chart/Chart.h"

namespace vt
{

class ChartEditPanelComponent final : public juce::Component
{
public:
    explicit ChartEditPanelComponent (Theme&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** MUTABLE, unlike everywhere else in the UI. Pass nullptr to detach. */
    void setChart (Chart* chart);

    /** Selection is held by note ID, not index: an edit can re-sort the notes
        and an index would then point at a different note. See Chart.h. */
    void setSelectedNoteId (int noteId);
    int getSelectedNoteId() const noexcept { return selectedNoteId; }

    /** Selects whichever note is sounding at (or next after) this chart time.
        Used so the playhead and the selection follow each other. */
    void selectNoteAtTime (double chartSeconds);

    /** Re-reads the selected note into the controls. Call after anything else
        changes the chart. */
    void refresh();

    void applyTheme();

    /** Something about the chart changed and the highway needs repainting. */
    std::function<void()> onChartEdited;

    /** The selection moved; the highway should highlight this note id. */
    std::function<void (int)> onSelectionChanged;

    /** Move the playhead to this chart time — used when stepping between notes,
        so you hear what you are editing. */
    std::function<void (double)> onSeekTo;

    /** The user asked to write the chart back to disk. The editor owns saving
        because it owns the file and the footer that reports the result. */
    std::function<void()> onSave;

    /** Shown in the panel's own status line. */
    void setStatus (const juce::String&);

private:
    int  selectedIndex() const noexcept;
    const ChartNote* selectedNote() const noexcept;

    void nudgePitch (int semitones);
    void nudgeStart (double seconds);
    void nudgeLength (double seconds);
    void step (int delta);
    void commitSyllable();
    void commitPhraseText();
    void deleteSelected();
    void insertAfterSelected();
    void applyType();

    /** Every edit funnels through here: it re-resolves the selection, tells the
        highway, and updates the controls. Doing it in one place is what stops a
        new edit action forgetting one of the three. */
    void afterEdit();

    Theme& theme;
    Chart* chart = nullptr;

    int selectedNoteId = -1;

    /** True while refresh() is writing into the controls, so their own change
        callbacks do not write straight back into the chart. */
    bool updating = false;

    juce::Label  positionLabel;
    juce::Label  pitchLabel;

    juce::TextButton prevButton   { "<" };
    juce::TextButton nextButton   { ">" };

    juce::TextButton octaveDown   { "-12" };
    juce::TextButton semiDown     { "-1" };
    juce::TextButton semiUp       { "+1" };
    juce::TextButton octaveUp     { "+12" };

    juce::TextButton earlier      { "<10ms" };
    juce::TextButton later        { "10ms>" };
    juce::TextButton shorter      { "-" };
    juce::TextButton longer       { "+" };

    juce::ComboBox   typeBox;
    juce::TextButton deleteButton { "Delete" };
    juce::TextButton insertButton { "Insert" };
    juce::TextButton saveButton   { "Save chart" };

    juce::Label      syllableCaption;
    juce::TextEditor syllableEditor;
    juce::Label      phraseCaption;
    juce::TextEditor phraseEditor;

    juce::Label      statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChartEditPanelComponent)
};

} // namespace vt
