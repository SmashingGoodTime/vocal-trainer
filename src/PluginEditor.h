/*
  ==============================================================================

    PluginEditor.h
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
      4. repaint the LANE ONLY

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

#include "PluginProcessor.h"
#include "chart/Chart.h"
#include "dsp/OneEuroFilter.h"
#include "import/SongImporter.h"
#include "ui/ImportOverlayComponent.h"
#include "ui/LyricStripComponent.h"
#include "ui/NoteHighwayComponent.h"
#include "ui/PitchLaneComponent.h"
#include "ui/Theme.h"

namespace vt
{

class VocalTrainerEditor final : public juce::AudioProcessorEditor,
                                 private juce::ChangeListener
{
public:
    explicit VocalTrainerEditor (VocalTrainerProcessor&);
    ~VocalTrainerEditor() override;

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

    //== M3.5: importing a song from an audio file ============================
    void openAudioChooser();
    void beginImport (const juce::File& audioFile);
    bool isStandalone() const noexcept
    {
        return processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
    }

    void setSongPlaying (bool shouldPlay);
    void pushGuideVocalGain();
    void seekSong (double chartSeconds);

    /** Chart-time position of the now-line, derived from the AUDIO STREAM clock
        rather than wall-clock. Estimates arrive at a steady 125 Hz even during
        silence, so the stream is a drift-free master and it is already the
        timebase the pitch samples are stamped in. */
    double currentChartTime() const noexcept;

    //==========================================================================
    VocalTrainerProcessor& processor;
    juce::SharedResourcePointer<Theme> theme;

    PitchLaneComponent lane;
    NoteHighwayComponent highway { *theme };
    LyricStripComponent lyricStrip { *theme };
    ImportOverlayComponent importOverlay { *theme };

    /** Its callbacks touch members declared after it, so the editor's
        destructor clears them and cancels explicitly rather than relying on
        member destruction order. */
    SongImporter importer;

    std::unique_ptr<Chart> chart;
    std::unique_ptr<juce::FileChooser> chooser;   // MUST be a member: a stack-local
                                                  // FileChooser silently never appears.

    bool   songPlaying = false;
    double songAnchorStream = 0.0;    // stream seconds at chart t=0
    double pausedChartTime = 0.0;
    double lastAnimationStream = 0.0;

    juce::Label  titleLabel;
    juce::Label  noteCaption;
    juce::ComboBox noteSelector;
    juce::ToggleButton freeSingToggle { "Free Sing" };
    juce::TextButton themeButton { "Edit theme..." };
    juce::TextButton loadChartButton { "Load chart..." };
    juce::TextButton addSongButton { "Add song..." };
    juce::TextButton playButton { "Play" };
    juce::TextButton restartButton { "|<" };
    juce::ToggleButton monitorToggle { "Monitor" };
    juce::ToggleButton guideVocalToggle { "Guide" };
    juce::Slider guideVocalLevel { juce::Slider::LinearHorizontal,
                                   juce::Slider::TextBoxRight };
    juce::Label  footerLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> noteAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   freeSingAttachment;

    //== vblank scratch — all preallocated ====================================
    static constexpr int kDrainCapacity = 512;
    std::array<PitchEstimate, (size_t) kDrainCapacity> drainBuffer {};

    /** The 1-Euro filter lives here, not on the worker thread. See the long
        comment in PluginEditor.cpp — the shared PitchEstimate contract carries a
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalTrainerEditor)
};

} // namespace vt
