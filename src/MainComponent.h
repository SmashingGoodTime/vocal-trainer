/*
  ==============================================================================

    MainComponent.h
    Vocal Trainer — M1

    The message-thread half of the loop.

    Driven by juce::VBlankAttachment, NOT a 60 Hz juce::Timer. A free-running
    timer beats against non-60 Hz displays and produces visible judder; the
    vblank callback fires once per actual display refresh, inline just before
    JUCE's paint cycle. (The one Timer in this project is the 4 Hz theme.json
    poll in Theme, where a Timer is the correct tool.)

    Per vblank:
      1. drain the PitchEstimate FIFO
      2. run the 1-Euro display filter over the drained values
      3. push them into the lane's ~600 ms ring
      4. feed the scorer, in chart time
      5. repaint the LANE ONLY

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

#include "AudioEngine.h"
#include "app/AppSettings.h"
#include "chart/Chart.h"
#include "dsp/OneEuroFilter.h"
#include "exercise/ExerciseLibrary.h"
#include "import/SongImporter.h"
#include "score/TakeScorer.h"
#include "ui/ChartEditPanelComponent.h"
#include "ui/ImportOverlayComponent.h"
#include "ui/LyricStripComponent.h"
#include "ui/NoteHighwayComponent.h"
#include "ui/PitchLaneComponent.h"
#include "ui/ReviewOverlayComponent.h"
#include "ui/Theme.h"

namespace vt
{

class MainComponent final : public juce::AudioProcessorEditor,
                                 private juce::ChangeListener
{
public:
    explicit MainComponent (AudioEngine&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void onVBlank();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void applyTheme();
    void updateFooter();
    void syncParametersToLane();

    //== M3: chart mode ========================================================
    void openChartChooser();
    void loadChart (const juce::File&);
    void unloadChart();
    bool inChartMode() const noexcept { return chart != nullptr; }

    /** Everything shared between loading a chart from disk and generating an
        exercise. Returns the footer summary text. */
    juce::String installChart (std::unique_ptr<Chart> newChart);

    //== M3.5: importing a song from an audio file ============================
    void openAudioChooser();
    void beginImport (const juce::File& audioFile);

    void setSongPlaying (bool shouldPlay);
    void pushGuideVocalGain();
    void pushMonitorLevel();
    void seekSong (double chartSeconds);

    //== Exercises =============================================================
    void showExerciseMenu();
    void loadExercise (const ExerciseSpec&);

    //== Controls that are not on the toolbar ==================================
    /** The tools that are not part of singing: add a song, edit the chart, open
        the theme file. All three are things you do BETWEEN takes, and each one
        was costing a permanent button on a bar the singer looks at while they
        are actually singing. */
    void showOverflowMenu();

    /** Monitor and Guide levels, in a call-out under the Levels button.

        They are sliders and sliders are wide — the two of them were most of a
        control row on their own, which is what forced the transport onto a
        second row and cost the highway 30-odd pixels of height. Behind a button
        they cost one click when you change a level, which is a thing you do
        once a session, not once a phrase. */
    void showLevelsPanel();
    void layoutLevelsPanel();

    /** True when the controls need a second row: chart mode on a window too
        narrow to hold the transport beside the library buttons. */
    bool usesTwoRows() const noexcept;

    /** Header plus however many control rows there are. Shared by paint(),
        resized() and the overlay geometry, which have to agree or the overlays
        cover the toolbar. */
    int controlsBandHeight() const noexcept;

    //== Scoring and review ===================================================
    /** Called when the take ends, by whatever means. `automatic` distinguishes
        "the song ran out" from "the user asked to see the review", which changes
        whether an empty take is worth showing a screen for. */
    void endTake (bool automatic);
    void showReview();
    void hideReview();

    //== Recording and listen-back ============================================
    void toggleRecording();
    void stopRecording();
    void setListeningBack (bool shouldListen);
    juce::File getTakesDirectory() const;

    //== Chart editing =========================================================
    void setEditMode (bool shouldEdit);
    void saveChart();
    void chartEdited();

    /** Chart-time position of the now-line, derived from the AUDIO STREAM clock
        rather than wall-clock. Estimates arrive at a steady 125 Hz even during
        silence, so the stream is a drift-free master and it is already the
        timebase the pitch samples are stamped in. */
    double currentChartTime() const noexcept;

    //==========================================================================
    AudioEngine& engine;
    juce::SharedResourcePointer<Theme> theme;
    juce::SharedResourcePointer<AppSettings> settings;

    PitchLaneComponent lane;
    NoteHighwayComponent highway { *theme };
    LyricStripComponent lyricStrip { *theme };
    ImportOverlayComponent importOverlay { *theme };
    ReviewOverlayComponent reviewOverlay { *theme };
    ChartEditPanelComponent editPanel { *theme };

    /** Its callbacks touch members declared after it, so the editor's
        destructor clears them and cancels explicitly rather than relying on
        member destruction order. */
    SongImporter importer;

    std::unique_ptr<Chart> chart;
    std::unique_ptr<juce::FileChooser> chooser;   // MUST be a member: a stack-local
                                                  // FileChooser silently never appears.

    /** Per-note results for the take in progress. Lives here rather than on the
        engine because it is fed from, and read by, the message thread only. */
    TakeScorer scorer;

    bool   songPlaying = false;
    double songAnchorStream = 0.0;    // stream seconds at chart t=0
    double pausedChartTime = 0.0;
    double lastAnimationStream = 0.0;

    /** True when the loaded chart was GENERATED rather than read from a file.
        It has no chart.json behind it, so saving an edit needs a destination
        and unloading it does not need to touch the library. */
    bool chartIsExercise = false;

    /** The spec of the generated exercise, kept so it can be regenerated at a
        new root or tempo without re-picking it from the menu. */
    ExerciseSpec exerciseSpec;

    /** True while playing a recorded take back. Scoring is suspended: the mic is
        hearing a room with a recording in it, and crediting that to the singer
        would report a performance nobody gave. */
    bool listeningBack = false;

    /** Set once a take has ended and its review has been shown, so the automatic
        end-of-song trigger cannot fire the same review twice. */
    bool reviewShownForTake = false;

    /** The finished take's analysis, kept so it survives listen-back.

        Listening back seeks the transport, and a seek resets the scorer — a
        score stitched from two attempts describes a performance nobody gave. But
        that would also mean "Listen back" silently destroyed the review it was
        launched from, so the summary is retained here and only discarded when the
        singer actually starts a NEW take. */
    TakeSummary lastTakeSummary;
    bool haveLastTakeSummary = false;

    bool editMode = false;

    juce::File loadedTakeFile;

    juce::Label  titleLabel;
    juce::Label  noteCaption;
    juce::ComboBox noteSelector;
    juce::ToggleButton freeSingToggle { "Free Sing" };
    juce::TextButton loadChartButton { "Load chart..." };
    juce::TextButton exerciseButton { "Exercises..." };
    juce::TextButton playButton { "Play" };
    juce::TextButton restartButton { "|<" };
    juce::TextButton levelsButton { "Levels" };
    juce::TextButton recordButton { "Rec" };
    juce::TextButton listenButton { "Listen" };
    juce::TextButton reviewButton { "Review" };
    juce::TextButton overflowButton { "..." };
    juce::Label  footerLabel;

    //== The levels call-out ===================================================
    /** Holds the four level controls so they can be shown together. A named
        class rather than a bare Component only so that resizing it lays its
        children out — the editor owns the layout, as it does for everything
        else on the toolbar. */
    class LevelsPanel final : public juce::Component
    {
    public:
        explicit LevelsPanel (MainComponent& o) : owner (o) {}
        void resized() override { owner.layoutLevelsPanel(); }

    private:
        MainComponent& owner;
    };

    LevelsPanel levelsPanel { *this };

    juce::ToggleButton monitorToggle { "Monitor" };
    juce::Slider monitorLevel { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::ToggleButton guideVocalToggle { "Guide" };
    juce::Slider guideVocalLevel { juce::Slider::LinearHorizontal,
                                   juce::Slider::TextBoxRight };

    /** Declared AFTER the panel it displays, so it is destroyed BEFORE it. A
        CallOutBox constructed this way does not own its content. */
    std::unique_ptr<juce::CallOutBox> levelsCallout;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> noteAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   freeSingAttachment;

    //== vblank scratch — all preallocated ====================================
    static constexpr int kDrainCapacity = 512;
    std::array<PitchEstimate, (size_t) kDrainCapacity> drainBuffer {};

    /** The 1-Euro filter lives here, not on the worker thread. See the long
        comment in MainComponent.cpp — the shared PitchEstimate contract carries a
        single fracMidi field, and it has to be the RAW value so the trail can
        show honest vibrato. Running the filter over the drained sequence is
        numerically identical to running it in the worker. */
    OneEuroFilter smoother;

    double lastEstimateTime  = 0.0;
    bool   haveLastEstimate  = false;
    juce::uint32 lastFooterUpdateMs = 0;
    bool   suppressToleranceSync = true;   // don't clobber saved state on first load

    // Declared LAST so it is destroyed FIRST: the callback captures `this` and
    // touches every member above.
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace vt
