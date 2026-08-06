/*
  ==============================================================================

    MainComponent.cpp
    Vocal Trainer — M1

  ==============================================================================
*/

#include "MainComponent.h"

#include "chart/ChartWriter.h"
#include "exercise/ExerciseAudio.h"

#include <juce_audio_basics/juce_audio_basics.h>   // juce::MidiMessage::getMidiNoteName

// Used ONLY to defuse JUCE's "Mute audio input" checkbox — see the constructor.
//
// A HARD include, deliberately. This was once behind __has_include, and the
// guard evaluated false SILENTLY because juce_add_plugin puts
// juce_audio_plugin_client on the format targets but not on the shared-code
// target that compiles this file — the build stayed green and the fix simply
// was not there. If the JUCE/modules entry ever leaves this target's
// target_include_directories, the correct outcome is a compile error, not a
// working build with a checkbox that kills the pitch display.
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#include <cmath>

namespace vt
{

namespace
{
    constexpr int kLowestSelectableNote  = 36;   // must match the APVTS range
    constexpr int kHighestSelectableNote = 84;

    /** The footer is prose, and prose flickering at 60 Hz is unreadable. It also
        builds juce::Strings, which allocate; keeping that off the paint path is
        the point of the throttle. */
    constexpr juce::uint32 kFooterIntervalMs = 80;   // ~12 Hz

    /** Run-up given when jumping to a phrase to retry it. Dropping the playhead
        exactly on the first note gives the singer no chance to hear where they
        are, and they miss the entry every time. */
    constexpr double kRetryLeadInSeconds = 1.25;

    //== Exercise menu item ids ================================================
    // Disjoint ranges rather than one counter, so adding an exercise cannot
    // silently renumber the tempo items.
    constexpr int kExerciseIdBase = 1;
    constexpr int kRootIdBase     = 1000;
    constexpr int kTempoIdBase    = 2000;
    constexpr int kLengthIdBase   = 3000;

    const int kTempos[] = { 50, 60, 72, 84, 96, 112, 132 };

    struct LengthPreset { const char* name; int seconds; };

    const LengthPreset kLengths[] =
    {
        { "Short",  ExerciseLibrary::kShortSeconds  },
        { "Medium", ExerciseLibrary::kMediumSeconds },
        { "Long",   ExerciseLibrary::kLongSeconds   }
    };

    /** Below this the transport does not fit beside the library buttons, so the
        controls split onto two rows. Sized from what is actually placed on the
        row — 618 px of controls plus margins — with enough slack that a button
        never lands hard against the window edge. */
    constexpr int kSingleRowMinWidth = 700;
}

//==============================================================================
MainComponent::MainComponent (AudioEngine& p)
    : juce::AudioProcessorEditor (&p),
      engine (p)
{
    addAndMakeVisible (lane);

    // Both views exist at all times; only one is visible. Swapping visibility is
    // cheaper and less error-prone than building/tearing down a component, and it
    // keeps the highway's ring buffer alive across a chart reload.
    addChildComponent (highway);
    addChildComponent (lyricStrip);
    addChildComponent (editPanel);

    highway.setScorer (&scorer);

    highway.onNoteClicked = [this] (int noteId)
    {
        // A click on empty space keeps the current selection rather than clearing
        // it. Clearing would mean a slightly-off click silently disarmed every
        // button in the edit panel, and the next nudge would appear to do nothing.
        if (noteId < 0)
            return;

        editPanel.setSelectedNoteId (noteId);
        highway.setSelectedNoteId (noteId);
    };

    loadChartButton.onClick = [this] { openChartChooser(); };
    addAndMakeVisible (loadChartButton);

    exerciseButton.setTooltip ("Scales, arpeggios and interval drills, generated on the "
                              "spot. They score exactly like a song, so the review can "
                              "tell you which interval you are weakest on.");
    exerciseButton.onClick = [this] { showExerciseMenu(); };
    addAndMakeVisible (exerciseButton);

    // The tools you use between takes rather than during one. "Add song..." is
    // minutes of GPU work, which is why it lives behind the overflow menu and
    // runs out-of-process: preparing songs is a before-you-play activity.
    overflowButton.setTooltip ("Add a song, edit the chart, open the theme file");
    overflowButton.onClick = [this] { showOverflowMenu(); };
    addAndMakeVisible (overflowButton);

    levelsButton.setTooltip ("Monitor and guide levels");
    levelsButton.onClick = [this] { showLevelsPanel(); };
    // Always meaningful: the monitor control exists unconditionally, so unlike
    // the transport buttons this one never has nothing behind it.
    addAndMakeVisible (levelsButton);

    // levelsPanel is deliberately NOT a child of the editor: the CallOutBox
    // adopts it while it is open and hands nothing back when it closes, and a
    // panel that lived here as well would be a second, invisible copy of the
    // same four controls sitting behind the highway.

    importOverlay.onCancel   = [this] { importer.cancel(); };
    importOverlay.onDismiss  = [this] { importOverlay.setVisible (false); };
    addChildComponent (importOverlay);

    //-------------------------------------------------------------- review
    reviewOverlay.onClose = [this] { hideReview(); };

    reviewOverlay.onListenBack = [this]
    {
        hideReview();
        setListeningBack (true);
    };

    reviewOverlay.onRetryPhrase = [this] (double startSeconds)
    {
        // One click from "this phrase went worst" to singing it again, with
        // enough run-up to hear where the entry is.
        hideReview();
        seekSong (juce::jmax (0.0, startSeconds - kRetryLeadInSeconds));
        setSongPlaying (true);
    };

    addChildComponent (reviewOverlay);

    //---------------------------------------------------------- chart editing
    editPanel.onChartEdited     = [this] { chartEdited(); };
    editPanel.onSelectionChanged = [this] (int noteId) { highway.setSelectedNoteId (noteId); };
    editPanel.onSeekTo          = [this] (double t) { seekSong (t); };
    editPanel.onSave            = [this] { saveChart(); };

    importer.onProgress = [this] (const SongImporter::Progress& p)
    {
        importOverlay.setProgress (p.percent, p.message);
    };

    importer.onFinished = [this] (const SongImporter::Result& r)
    {
        if (r.ok)
        {
            importOverlay.setVisible (false);

            // Load it immediately. The user asked for a song, not for a file on
            // disk — making them hunt for it in a chooser afterwards would be
            // a strange way to end a five-minute wait.
            if (inChartMode())
                unloadChart();

            loadChart (r.chartFile);

            if (r.warnings > 0)
                footerLabel.setText (footerLabel.getText()
                                       + "   |   " + juce::String (r.warnings)
                                       + " import warning(s)",
                                     juce::dontSendNotification);
        }
        else if (r.cancelled)
        {
            importOverlay.setVisible (false);
            footerLabel.setText ("Import cancelled.", juce::dontSendNotification);
        }
        else
        {
            importOverlay.showFailure (r.message, r.detail);
        }
    };

    playButton.onClick = [this] { setSongPlaying (! songPlaying); };
    playButton.setEnabled (false);
    addChildComponent (playButton);

    monitorToggle.setTooltip ("Hear your own microphone through this app, at the level "
                              "beside it. Off by default: the Apollo Console already "
                              "monitors at near-zero latency, so this would only add a "
                              "delayed duplicate of your voice. Turning it off never "
                              "affects the pitch display.");
    monitorToggle.setToggleState (settings->getMonitorEnabled(), juce::dontSendNotification);
    monitorToggle.onClick = [this] { pushMonitorLevel(); };

    // A LEVEL, not just on/off — how much of your own voice you want against the
    // backing track depends on your headphones and your interface gain.
    //
    // The top of the range is +6 dB, not +24: this is a live path from a
    // microphone to a speaker, and a monitor control with a lot of makeup gain
    // is a feedback machine with a friendly label.
    monitorLevel.setRange (AudioEngine::kMonitorMinDb,
                           AudioEngine::kMonitorMaxDb, 0.5);
    monitorLevel.setSkewFactor (1.6);
    monitorLevel.setTextValueSuffix (" dB");
    monitorLevel.setNumDecimalPlacesToDisplay (1);
    monitorLevel.setValue (settings->getMonitorGainDb(), juce::dontSendNotification);
    monitorLevel.setTooltip ("How loud your own voice is in the output. Remembered "
                             "between sessions. Watch for feedback if you are on "
                             "speakers rather than headphones.");
    monitorLevel.onValueChange = [this] { pushMonitorLevel(); };

    levelsPanel.addAndMakeVisible (monitorToggle);
    levelsPanel.addAndMakeVisible (monitorLevel);

    // Push the remembered level in before anything can be heard.
    pushMonitorLevel();

    // DEFUSE JUCE'S "Mute audio input".
    //
    // That checkbox does not mute the output — it substitutes an EMPTY BUFFER
    // for the microphone before the engine runs, to break feedback loops in
    // ordinary effect processors. For an ANALYSER that is fatal: the pitch
    // display goes dead along with the sound, which is exactly what the user hit.
    //
    // We do not need it, because monitoring is off by default and is an
    // output-side decision here, so there is no feedback loop to break.
    // Clearing processorHasPotentialFeedbackLoop removes the checkbox from
    // Audio Settings entirely, so it cannot silently kill the display again.
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        holder->getMuteInputValue().setValue (false);
        holder->processorHasPotentialFeedbackLoop = false;
    }

    guideVocalToggle.setTooltip ("Hear the reference singer. Off by default: a guide vocal "
                                 "you cannot silence teaches you to follow it rather than to "
                                 "pitch it.");
    guideVocalToggle.onClick = [this] { pushGuideVocalGain(); };
    levelsPanel.addChildComponent (guideVocalToggle);

    // A LEVEL, not just on/off. How loud the reference should sit under your own
    // voice is a per-song, per-singer judgement — you cannot pick it for someone
    // else, and unity is far too loud in practice.
    guideVocalLevel.setRange (-40.0, 0.0, 0.5);
    guideVocalLevel.setSkewFactor (1.6);          // finer control near the top
    guideVocalLevel.setTextValueSuffix (" dB");
    guideVocalLevel.setNumDecimalPlacesToDisplay (1);
    guideVocalLevel.setTooltip ("Guide vocal level relative to the backing track. "
                                "Default comes from guideVocalDb in theme.json.");
    guideVocalLevel.onValueChange = [this] { pushGuideVocalGain(); };
    levelsPanel.addChildComponent (guideVocalLevel);

    //--------------------------------------------------- recording / review
    recordButton.setTooltip ("Record what you sing to a WAV in the song's takes/ folder, "
                             "and start the song. Listening back is the one form of "
                             "feedback that cannot be wrong.");
    recordButton.onClick = [this] { toggleRecording(); };
    addChildComponent (recordButton);

    listenButton.setTooltip ("Play the last take back against the backing track.");
    listenButton.setEnabled (false);
    listenButton.onClick = [this] { setListeningBack (! listeningBack); };
    addChildComponent (listenButton);

    reviewButton.setTooltip ("How the take went: where you were flat or sharp, which "
                             "phrases went worst, and which interval to practise.");
    reviewButton.setEnabled (false);
    reviewButton.onClick = [this] { showReview(); };
    addChildComponent (reviewButton);

    restartButton.setTooltip ("Back to the start of the chart");
    restartButton.onClick = [this] { seekSong (0.0); };
    addChildComponent (restartButton);

    titleLabel.setText ("Vocal Trainer", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    noteCaption.setText ("Target", juce::dontSendNotification);
    noteCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (noteCaption);

    // Populate BEFORE attaching: ComboBoxParameterAttachment maps the parameter
    // range linearly onto the item indices, so the item count must be final.
    for (int m = kLowestSelectableNote; m <= kHighestSelectableNote; ++m)
        noteSelector.addItem (juce::MidiMessage::getMidiNoteName (m, true, true, 3),
                              m - kLowestSelectableNote + 1);

    addAndMakeVisible (noteSelector);
    addAndMakeVisible (freeSingToggle);

    footerLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (footerLabel);

    noteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        engine.apvts, AudioEngine::pidTargetNote, noteSelector);

    freeSingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        engine.apvts, AudioEngine::pidFreeSing, freeSingToggle);

    theme->addChangeListener (this);
    applyTheme();
    suppressToleranceSync = false;   // from here on, a theme.json save is a deliberate act

    syncParametersToLane();

    engine.setGuiActive (true);

    setResizable (true, true);

    if (auto* c = getConstrainer())
        c->setMinimumSize (640, 360);

    setSize (960, 540);

    // Constructed last, on purpose: everything it touches must already exist.
    vblank = juce::VBlankAttachment (this, [this] { onVBlank(); });
}

MainComponent::~MainComponent()
{
    vblank = {};                       // stop callbacks before anything is torn down

    // The call-out does not own the panel it is showing, and the panel is about
    // to be destroyed. Closing it here rather than leaving it to member order
    // also cancels its modal state, which would otherwise outlive the window.
    levelsCallout.reset();

    // Stop the import worker HERE rather than leaving it to member destruction
    // order. Its callbacks touch importOverlay and footerLabel, which are
    // declared after it and would therefore be destroyed first.
    importer.onProgress = nullptr;
    importer.onFinished = nullptr;
    importer.cancel();

    // A recording is owned by the PROCESSOR and would happily outlive this
    // window, which for a file being written is the wrong answer: closing the
    // editor is the user's way of saying they have finished.
    stopRecording();

    // The highway holds a pointer to the scorer, and both die with this object;
    // clear it anyway so a future reordering of members cannot leave the
    // renderer reading a destroyed scorer for one frame.
    highway.setScorer (nullptr);

    engine.setGuiActive (false);
    theme->removeChangeListener (this);

    settings->save();
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    applyTheme();
}

void MainComponent::applyTheme()
{
    const auto& t = theme->get();

    lane.applyTheme();

    // Estimates arrive exactly one analysis hop apart (16000 / 128 = 125 Hz).
    // That rate is a property of the DSP, NOT of when the GUI happens to drain
    // the FIFO — deriving it from wall-clock deltas would make the filter's
    // cutoff jitter with frame timing.
    smoother.prepare (kAnalysisRate / (double) kChunkSize,
                      t.oneEuroMinCutoffHz,
                      t.oneEuroBeta,
                      t.oneEuroDerivCutoffHz);

    // The worker thread must not touch the Theme, so bridge the one value it
    // needs through an atomic on the engine.
    engine.setClarityThreshold (t.clarityThreshold);

    // The scoring median is a theme value too, and the same rule applies: the
    // scorer must never see an instantaneous pitch. See TakeScorer.h.
    scorer.setMedianMs (t.colourMedianMs);

    // theme.json is the live tuning surface for the tolerances, but the APVTS is
    // what gets saved with the session. Pushing on RELOAD only means: opening the
    // editor respects the saved session; saving theme.json deliberately retunes.
    if (! suppressToleranceSync)
    {
        if (auto* pf = engine.apvts.getParameter (AudioEngine::pidTFullCents))
            pf->setValueNotifyingHost (pf->convertTo0to1 (t.tFullCents));

        if (auto* pz = engine.apvts.getParameter (AudioEngine::pidTZeroCents))
            pz->setValueNotifyingHost (pz->convertTo0to1 (t.tZeroCents));
    }

    const auto textColour = t.text;
    const auto dimColour  = t.textDim;

    titleLabel.setColour  (juce::Label::textColourId, textColour);
    noteCaption.setColour (juce::Label::textColourId, dimColour);
    footerLabel.setColour (juce::Label::textColourId, dimColour);
    freeSingToggle.setColour (juce::ToggleButton::textColourId, textColour);
    freeSingToggle.setColour (juce::ToggleButton::tickColourId, t.targetBarActive);

    for (auto* toggle : { &monitorToggle, &guideVocalToggle })
    {
        toggle->setColour (juce::ToggleButton::textColourId, textColour);
        toggle->setColour (juce::ToggleButton::tickColourId, t.targetBarActive);
    }

    noteSelector.setColour (juce::ComboBox::backgroundColourId, t.panel);
    noteSelector.setColour (juce::ComboBox::textColourId, textColour);
    noteSelector.setColour (juce::ComboBox::outlineColourId, t.gridLine);

    // The two menu buttons are chrome, not controls, so they recede.
    for (auto* button : { &levelsButton, &overflowButton })
    {
        button->setColour (juce::TextButton::buttonColourId, t.panel);
        button->setColour (juce::TextButton::textColourOffId, dimColour);
    }

    // Record is the only control here that writes a file while you are not
    // looking, so it is the only one that gets a colour of its own.
    recordButton.setColour (juce::TextButton::textColourOffId, t.traceOutOfTune);
    recordButton.setColour (juce::TextButton::buttonColourId, t.panel);

    importOverlay.applyTheme();
    reviewOverlay.applyTheme();
    editPanel.applyTheme();

    titleLabel.setFont  (juce::Font (juce::FontOptions ((float) juce::jmax (12, t.headerHeight - 14))));
    footerLabel.setFont (juce::Font (juce::FontOptions ((float) juce::jmax (10, t.footerHeight - 12))));

    resized();
    repaint();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    const auto& t = theme->get();

    g.fillAll (t.background);

    auto bounds = getLocalBounds();

    const auto controlsBand = controlsBandHeight();

    g.setColour (t.panel);
    g.fillRect (bounds.removeFromTop (controlsBand));
    g.fillRect (bounds.removeFromBottom (t.footerHeight));

    g.setColour (t.gridLine);
    g.fillRect (0, controlsBand, getWidth(), 1);
    g.fillRect (0, getHeight() - t.footerHeight - 1, getWidth(), 1);
}

bool MainComponent::usesTwoRows() const noexcept
{
    // One row is the normal case. The split only happens on a window too narrow
    // to hold the transport beside the library buttons — a clipped button is
    // unusable, and the singer would not find out until they reached for it.
    return inChartMode() && getWidth() < kSingleRowMinWidth;
}

int MainComponent::controlsBandHeight() const noexcept
{
    const auto& t = theme->get();

    return t.headerHeight + t.controlsHeight + (usesTwoRows() ? t.controlsRow2Height : 0);
}

void MainComponent::resized()
{
    const auto& t = theme->get();

    auto bounds = getLocalBounds();

    auto header = bounds.removeFromTop (t.headerHeight).reduced (10, 2);
    titleLabel.setBounds (header);

    //-------------------------------------------------------------- row 1
    // Library at the left, overflow at the right, and — on a wide enough window
    // — everything else in between. What is NOT here is as deliberate as what
    // is: the free-sing controls are hidden in chart mode rather than greyed,
    // because a permanently disabled control is a permanent question ("what am I
    // doing wrong?") about something that is not available at all.
    auto row1 = bounds.removeFromTop (t.controlsHeight).reduced (10, 3);

    auto row2 = usesTwoRows() ? bounds.removeFromTop (t.controlsRow2Height).reduced (10, 3)
                              : juce::Rectangle<int>();

    loadChartButton.setBounds (row1.removeFromLeft (98));
    row1.removeFromLeft (6);
    exerciseButton.setBounds (row1.removeFromLeft (98));

    overflowButton.setBounds (row1.removeFromRight (34));
    row1.removeFromRight (8);

    if (inChartMode())
    {
        // Transport, levels, recording and review. The row they land on is the
        // only difference between the wide and narrow layouts.
        auto& row = usesTwoRows() ? row2 : row1;

        if (! usesTwoRows())
            row.removeFromLeft (12);

        restartButton.setBounds (row.removeFromLeft (30));
        row.removeFromLeft (4);
        playButton.setBounds (row.removeFromLeft (62));

        if (levelsButton.isVisible())
        {
            row.removeFromLeft (10);
            levelsButton.setBounds (row.removeFromLeft (64));
        }

        // Take controls hug the right, so they stay in the same place whether or
        // not the Levels button is there.
        reviewButton.setBounds (row.removeFromRight (62));
        row.removeFromRight (4);
        listenButton.setBounds (row.removeFromRight (58));
        row.removeFromRight (4);
        recordButton.setBounds (row.removeFromRight (44));
    }
    else
    {
        freeSingToggle.setBounds (row1.removeFromRight (100));
        row1.removeFromRight (8);
        noteSelector.setBounds (row1.removeFromRight (86));
        noteCaption.setBounds (row1.removeFromRight (50));

        if (levelsButton.isVisible())
        {
            row1.removeFromLeft (12);
            levelsButton.setBounds (row1.removeFromLeft (64));
        }
    }

    footerLabel.setBounds (bounds.removeFromBottom (t.footerHeight).reduced (10, 2));

    lane.setBounds (bounds);

    // The chart editor sits at the very bottom, below the lyrics: it is a tool
    // panel, and putting it between the highway and the words would separate the
    // two things being sung from each other.
    if (inChartMode() && editMode)
    {
        const auto panelHeight = juce::jlimit (56, 76, bounds.getHeight() / 4);
        editPanel.setBounds (bounds.removeFromBottom (panelHeight));
    }
    else
    {
        editPanel.setBounds ({});
    }

    // The lyric strip sits BELOW the highway, in its own fixed band. It only
    // takes space when there is text to show.
    if (inChartMode() && lyricStrip.hasAnyLyrics())
    {
        const auto stripHeight = juce::jlimit (28, 56, bounds.getHeight() / 9);
        lyricStrip.setBounds (bounds.removeFromBottom (stripHeight));
    }
    else
    {
        lyricStrip.setBounds ({});
    }

    highway.setBounds (bounds);

    // Over the whole playing area, including the lyric strip: nothing under it
    // is usable while a chart is being built.
    const auto overlayArea = getLocalBounds()
                               .withTrimmedTop (controlsBandHeight())
                               .withTrimmedBottom (t.footerHeight);

    importOverlay.setBounds (overlayArea);
    reviewOverlay.setBounds (overlayArea);
}

//==============================================================================
void MainComponent::syncParametersToLane()
{
    const auto readParam = [this] (const char* id, float fallback)
    {
        if (auto* raw = engine.apvts.getRawParameterValue (id))
            return raw->load (std::memory_order_relaxed);

        return fallback;
    };

    const auto tFull = readParam (AudioEngine::pidTFullCents, 25.0f);
    const auto tZero = readParam (AudioEngine::pidTZeroCents, 100.0f);

    lane.setTargetNote (juce::roundToInt (readParam (AudioEngine::pidTargetNote, 60.0f)));
    lane.setFreeSing   (readParam (AudioEngine::pidFreeSing, 1.0f) > 0.5f);
    lane.setTolerances (tFull, tZero);

    // The same two numbers the target bar's drawn height comes from. If these
    // ever diverged, "inside the bar" would stop meaning "full credit" — which
    // DESIGN.md section 7 calls the highest-value visual decision in the design.
    highway.setTolerances (tFull, tZero);
    scorer.setTolerances (tFull, tZero);
}

//==============================================================================
void MainComponent::onVBlank()
{
    // Cheap atomic loads; the lane setters early-out when nothing changed, so
    // polling here avoids an APVTS listener and its thread-affinity questions.
    syncParametersToLane();

    const double hostRate = engine.getHostSampleRate() > 0.0 ? engine.getHostSampleRate()
                                                                : 48000.0;

    const int n = engine.drainPitchEstimates (drainBuffer.data(), kDrainCapacity);

    for (int i = 0; i < n; ++i)
    {
        const auto& e = drainBuffer[(size_t) i];

        PitchLaneComponent::Sample s;
        s.timeSeconds = (double) e.hostSampleAtWindowCentre / hostRate;
        s.rawMidi     = e.fracMidi;
        s.clarity     = e.clarity;
        s.rmsDb       = e.rmsDb;

        //------------------------------------------------------------------
        // WHY THE 1-EURO FILTER RUNS HERE AND NOT ON THE WORKER THREAD.
        //
        // The shared PitchEstimate contract carries exactly one fracMidi field,
        // and DESIGN.md §6 requires BOTH signals downstream: the raw 125 Hz value
        // for the honest wobbling trail, and the smoothed value for the dot. The
        // published field therefore has to be the raw (post-median) value.
        //
        // Because we drain every estimate in order, and consecutive estimates are
        // always exactly one hop apart, the filter sees exactly the sequence it
        // would have seen on the worker thread. The output is identical; only the
        // thread it is computed on differs.
        //
        // Smoothing is done in the LOG domain (fractional MIDI), never in Hz —
        // smoothing in Hz makes high notes look jumpier than low notes for the
        // same cents error.
        //------------------------------------------------------------------
        if (std::isnan (e.fracMidi))
        {
            smoother.reset();
            s.smoothedMidi = e.fracMidi;
        }
        else
        {
            s.smoothedMidi = smoother.process (e.fracMidi);
        }

        lane.pushSample (s);

        if (inChartMode())
        {
            NoteHighwayComponent::Sample hs;
            // STREAM time, deliberately NOT converted to chart time here. The
            // highway maps it per frame via setClock(), so the mic trace stays
            // correct whether the chart clock is running, paused, seeked, or
            // driven by the backing track.
            hs.timeSeconds  = s.timeSeconds;
            hs.rawMidi      = s.rawMidi;
            hs.smoothedMidi = s.smoothedMidi;
            hs.clarity      = s.clarity;
            hs.rmsDb        = s.rmsDb;
            highway.pushSample (hs);
        }

        lastEstimateTime = s.timeSeconds;
        haveLastEstimate = true;
    }

    if (inChartMode())
    {
        // Animation is stepped off the STREAM clock too, so a stalled audio
        // device freezes the highway rather than letting it slide away from the
        // pitch data.
        const auto dt = juce::jlimit (0.0, 0.25, lastEstimateTime - lastAnimationStream);
        lastAnimationStream = lastEstimateTime;

        // Tie the two clocks together for this frame.
        //
        // The newest estimate describes the CENTRE of its analysis window, so
        // the audio it summarises is already ~32 ms old (1024/2 samples at
        // 16 kHz). Treating "now" as that centre would pin the trace's leading
        // edge exactly on the now-line and erase the deliberate trailing tail
        // that makes the analysis lag read as natural rather than as lag.
        constexpr double kWindowCentreLag = (double) (kAnalysisWindow / 2) / kAnalysisRate;

        highway.setClock (lastEstimateTime + kWindowCentreLag, currentChartTime());
        highway.tickAnimation (dt);

        //------------------------------------------------------------------
        // Scoring. Fed AFTER setClock, because the stream -> chart mapping for
        // this frame is what turns a microphone timestamp into a position in the
        // song. Only while actually playing, and never during listen-back: the
        // mic is then hearing a room with a recording in it, and crediting that
        // to the singer would report a performance nobody gave.
        //------------------------------------------------------------------
        if (songPlaying && ! listeningBack)
        {
            for (int i = 0; i < n; ++i)
            {
                const auto streamTime = (double) drainBuffer[(size_t) i].hostSampleAtWindowCentre
                                          / hostRate;

                scorer.feed (highway.chartTimeFor (streamTime),
                             drainBuffer[(size_t) i].fracMidi);
            }

            if (n > 0)
                reviewButton.setEnabled (scorer.hasAnyData());
        }

        if (lyricStrip.hasAnyLyrics())
        {
            lyricStrip.setNowTime (currentChartTime());
            lyricStrip.repaint();
        }

        // Hand the detector the note to constrain its octave search around.
        const auto target = highway.getTargetMidi();
        engine.setChartTargetMidi (target >= 0 ? (float) target : -1.0f);

        if (n > 0 || songPlaying)
            highway.repaint();

        //------------------------------------------------------------------
        // END OF TAKE. Two ways it can happen, and both have to be caught here
        // rather than by a callback from the audio thread.
        //------------------------------------------------------------------
        if (songPlaying)
        {
            const auto& backing = engine.getBackingPlayer();

            if (backing.hasFinished())
            {
                endTake (true);
            }
            else if (! backing.ownsClock()
                     && currentChartTime() > chart->getDurationSeconds())
            {
                // An a cappella chart has no transport to run out, so the chart's
                // own duration is the end. Without this the song never finishes
                // and the review never appears.
                endTake (true);
            }
        }
    }
    else if (n > 0)
    {
        lane.setNowTime (lastEstimateTime);

        // Repaint the LANE ONLY. The header, controls and footer are separate
        // components and are left alone.
        lane.repaint();
    }

    const auto now = juce::Time::getMillisecondCounter();

    if (now - lastFooterUpdateMs >= kFooterIntervalMs)
    {
        lastFooterUpdateMs = now;
        updateFooter();
    }
}

//==============================================================================
void MainComponent::updateFooter()
{
    const auto r = lane.getReadout();

    juce::String text;

    if (r.voiced)
    {
        const int cents = juce::roundToInt (r.cents);
        text << (cents > 0 ? "+" : "") << cents << " cents";
    }
    else
    {
        text << "--- cents";
    }

    text << "   |   clarity " << juce::String (r.clarity, 2)
         << "   |   level "   << juce::String (juce::jmax (-99.0f, r.rmsDb), 1) << " dB";

    if (r.voiced && r.octaveOffset != 0)
        text << "   |   " << (r.octaveOffset > 0 ? "+" : "") << r.octaveOffset << " octave";

    // The backing track IS the transport — the same clock getBackingPlayer() and
    // currentChartTime() run off. Free sing has no transport at all, so say
    // nothing rather than invent one: this line once read "stopped @ 120.0 bpm"
    // permanently, right through actual playback, because it printed a snapshot
    // that was never filled in.
    if (const auto& backing = engine.getBackingPlayer(); backing.ownsClock())
    {
        const auto clock = [] (double seconds)
        {
            const auto total = juce::jmax (0, juce::roundToInt (seconds));
            return juce::String (total / 60) + ":"
                     + juce::String (total % 60).paddedLeft ('0', 2);
        };

        text << "   |   " << (backing.isPlaying() ? "playing" : "stopped")
             << " " << clock (backing.getPositionSeconds())
             << " / " << clock (juce::jmax (backing.getLengthSeconds(),
                                            inChartMode() ? chart->getDurationSeconds()
                                                          : 0.0));
    }

    //---------------------------------------------------------- take state
    // Recording and listen-back both change what the numbers on screen MEAN, so
    // neither may be silent.
    if (const auto& recorder = engine.getTakeRecorder(); recorder.isRecording())
    {
        text << "   |   RECORDING " << juce::String (recorder.getRecordedSeconds(), 1) << " s";

        if (recorder.hadDropout())
            text << " (dropped samples - disk too slow)";
    }
    else if (listeningBack)
    {
        text << "   |   listening back: " << engine.getBackingPlayer().getTakeName()
             << " (not scoring)";
    }

    if (inChartMode() && chart->isModified())
        text << "   |   chart edited, not saved";

    if (theme->hasParseError())
        text << "   |   theme.json: parse error, using last good values";

    if (text != footerLabel.getText())
        footerLabel.setText (text, juce::dontSendNotification);
}

//==============================================================================
// M3 — chart mode
//==============================================================================

void MainComponent::pushGuideVocalGain()
{
    const auto db = (float) guideVocalLevel.getValue();
    const auto gain = guideVocalToggle.getToggleState()
                        ? juce::Decibels::decibelsToGain (db, -40.0f)
                        : 0.0f;

    engine.getBackingPlayer().setReferenceGain (gain);
}

void MainComponent::pushMonitorLevel()
{
    const auto on = monitorToggle.getToggleState();

    engine.setMonitorMic (on);
    engine.setMonitorGainDb ((float) monitorLevel.getValue());

    // Persisted because it depends on the singer's headphones and interface
    // gain, not on the song — so it is the same next time they open the app.
    settings->setMonitorEnabled (on);
    settings->setMonitorGainDb ((float) monitorLevel.getValue());

    // The slider stays live whether or not monitoring is on, so the level can be
    // set to something safe BEFORE unmuting a microphone into headphones.
}

double MainComponent::currentChartTime() const noexcept
{
    // When a transport is loaded it IS the clock, because the singer syncs to
    // what they HEAR. Running the highway off a different clock than the audio
    // would let the two drift apart, which is exactly the failure the whole
    // timestamping design exists to prevent.
    auto& backing = engine.getBackingPlayer();

    if (backing.ownsClock())
        return backing.getPositionSeconds();

    // No transport: fall back to the audio-stream clock.
    return songPlaying ? (lastEstimateTime - songAnchorStream) : pausedChartTime;
}

void MainComponent::setSongPlaying (bool shouldPlay)
{
    if (shouldPlay == songPlaying || ! inChartMode())
        return;

    auto& backing = engine.getBackingPlayer();

    if (shouldPlay)
    {
        // Starting again after the song ran out is a NEW TAKE, not a resumption
        // of the old one. BackingPlayer::start() rewinds the audio in that case,
        // so without this the scores of the previous attempt would be carried
        // into a run that begins at zero.
        if (backing.hasFinished() || reviewShownForTake)
            seekSong (0.0);

        // Singing again is what discards the previous take's review — not
        // listening back to it, and not seeking around inside it.
        if (! listeningBack)
        {
            haveLastTakeSummary = false;
            reviewButton.setEnabled (false);
        }

        // Re-anchor so the chart resumes exactly where it paused.
        songAnchorStream = lastEstimateTime - pausedChartTime;
        songPlaying = true;

        if (backing.hasAnyAudio())
            backing.start();
    }
    else
    {
        pausedChartTime = currentChartTime();
        songPlaying = false;

        if (backing.hasAnyAudio())
            backing.stop();
    }

    playButton.setButtonText (songPlaying ? "Pause" : "Play");
}

void MainComponent::seekSong (double chartSeconds)
{
    if (! inChartMode())
        return;

    pausedChartTime  = juce::jmax (0.0, chartSeconds);
    songAnchorStream = lastEstimateTime - pausedChartTime;

    engine.getBackingPlayer().setPosition (pausedChartTime);

    // A seek starts a new take. A score stitched together from two different
    // attempts at the same song describes a performance that never happened, and
    // there is no honest way to merge them.
    //
    // The RETAINED summary of the finished take deliberately survives this: the
    // review of what you just sang must not disappear because you rewound to try
    // it again. It is discarded when the new take actually starts.
    scorer.reset();
    reviewShownForTake = false;
    reviewButton.setEnabled (haveLastTakeSummary);

    highway.clearHistory();
    highway.setNowTime (pausedChartTime);
    highway.repaint();
}

//==============================================================================
// Controls that are not on the toolbar
//==============================================================================
void MainComponent::showOverflowMenu()
{
    juce::PopupMenu menu;

    menu.addItem (1, "Add song...", ! importer.isBusy());

    if (inChartMode())
        menu.addItem (2, "Edit chart", true, editMode);

    menu.addSeparator();
    menu.addItem (3, "Edit theme...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (overflowButton),
                        [this] (int result)
                        {
                            if (result == 1)
                            {
                                openAudioChooser();
                            }
                            else if (result == 2)
                            {
                                setEditMode (! editMode);
                            }
                            else if (result == 3)
                            {
                                const auto f = Theme::getThemeFile();

                                if (f.existsAsFile())
                                    f.revealToUser();
                                else
                                    f.getParentDirectory().revealToUser();
                            }
                        });
}

void MainComponent::layoutLevelsPanel()
{
    constexpr int kRowHeight = 26;
    constexpr int kLabelWidth = 74;
    constexpr int kGap = 6;

    auto area = levelsPanel.getLocalBounds().reduced (8);

    if (monitorToggle.isVisible())
    {
        auto row = area.removeFromTop (kRowHeight);
        monitorToggle.setBounds (row.removeFromLeft (kLabelWidth));
        monitorLevel.setBounds (row);
        area.removeFromTop (kGap);
    }

    if (guideVocalToggle.isVisible())
    {
        auto row = area.removeFromTop (kRowHeight);
        guideVocalToggle.setBounds (row.removeFromLeft (kLabelWidth));
        guideVocalLevel.setBounds (row);
    }
}

void MainComponent::showLevelsPanel()
{
    // A second click on the button closes it, which is what a button that opens
    // a panel is expected to do.
    if (levelsCallout != nullptr)
    {
        levelsCallout->dismiss();
        return;
    }

    const auto rows = (monitorToggle.isVisible() ? 1 : 0)
                        + (guideVocalToggle.isVisible() ? 1 : 0);

    if (rows == 0)
        return;

    levelsPanel.setSize (356, 16 + rows * 26 + (rows > 1 ? 6 : 0));
    layoutLevelsPanel();

    levelsCallout = std::make_unique<juce::CallOutBox> (levelsPanel,
                                                        levelsButton.getBounds(),
                                                        this);

    // Modal so that clicking anywhere else dismisses it — and NOT deleting
    // itself on dismissal, because the panel it displays is a member of this
    // editor and outlives any one call-out.
    juce::Component::SafePointer<MainComponent> safe (this);

    levelsCallout->enterModalState (false,
                                    juce::ModalCallbackFunction::create (
                                        [safe] (int) mutable
                                        {
                                            if (safe != nullptr)
                                                safe->levelsCallout.reset();
                                        }),
                                    false);
}

//==============================================================================
// Exercises
//==============================================================================
void MainComponent::showExerciseMenu()
{
    const auto rootMidi = settings->getExerciseRootMidi();
    const auto bpm      = settings->getExerciseBpm();
    const auto length   = settings->getExerciseLengthSeconds();

    juce::PopupMenu menu;

    menu.addSectionHeader ("Exercise");

    const auto all = ExerciseLibrary::getAll();

    for (size_t i = 0; i < all.size(); ++i)
        menu.addItem (kExerciseIdBase + (int) i, all[i].name);

    //------------------------------------------------------------ starting note
    juce::PopupMenu roots;

    // Two octaves either side of middle C covers every voice type without
    // turning the menu into a piano.
    for (int m = 48; m <= 72; ++m)
        roots.addItem (kRootIdBase + m,
                       juce::MidiMessage::getMidiNoteName (m, true, true, 3),
                       true, m == rootMidi);

    menu.addSubMenu ("Starting note", roots);

    juce::PopupMenu tempos;

    for (const auto t : kTempos)
        tempos.addItem (kTempoIdBase + t, juce::String (t) + " bpm", true, t == bpm);

    menu.addSubMenu ("Tempo", tempos);

    //------------------------------------------------------------------ length
    juce::PopupMenu lengths;

    for (const auto& preset : kLengths)
        lengths.addItem (kLengthIdBase + preset.seconds,
                         juce::String (preset.name) + "  ("
                           + juce::String (preset.seconds / 60) + ":"
                           + juce::String (preset.seconds % 60).paddedLeft ('0', 2) + ")",
                         true, preset.seconds == length);

    menu.addSubMenu ("Length", lengths);

    menu.addSeparator();
    menu.addSectionHeader (juce::MidiMessage::getMidiNoteName (rootMidi, true, true, 3)
                             + ", " + juce::String (bpm) + " bpm, "
                             + juce::String (length / 60) + ":"
                             + juce::String (length % 60).paddedLeft ('0', 2));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (exerciseButton),
                        [this] (int result)
                        {
                            if (result <= 0)
                                return;

                            // Every branch rebuilds from the SETTINGS rather than
                            // patching the current spec, because the repeat count
                            // is derived from all three: at 132 bpm a warm-up needs
                            // nearly twice the repetitions it needs at 72 to last
                            // the length that was asked for. Patching bpm alone
                            // would quietly halve the exercise.
                            const auto regenerate = [this] (ExerciseSpec::Kind kind)
                            {
                                loadExercise (ExerciseLibrary::makeDefaultSpec (
                                                  kind,
                                                  settings->getExerciseRootMidi(),
                                                  settings->getExerciseBpm(),
                                                  settings->getExerciseLengthSeconds()));
                            };

                            if (result >= kLengthIdBase)
                            {
                                settings->setExerciseLengthSeconds (result - kLengthIdBase);

                                // Regenerate in place, so changing the length of the
                                // exercise you are doing does not mean finding it in
                                // the menu again.
                                if (chartIsExercise)
                                    regenerate (exerciseSpec.kind);

                                return;
                            }

                            if (result >= kTempoIdBase)
                            {
                                settings->setExerciseBpm (result - kTempoIdBase);

                                if (chartIsExercise)
                                    regenerate (exerciseSpec.kind);

                                return;
                            }

                            if (result >= kRootIdBase)
                            {
                                settings->setExerciseRootMidi (result - kRootIdBase);

                                if (chartIsExercise)
                                    regenerate (exerciseSpec.kind);

                                return;
                            }

                            const auto index = result - kExerciseIdBase;
                            const auto all = ExerciseLibrary::getAll();

                            if (index >= 0 && index < (int) all.size())
                                regenerate (all[(size_t) index].kind);
                        });
}

void MainComponent::loadExercise (const ExerciseSpec& spec)
{
    juce::String error;
    auto generated = ExerciseLibrary::buildChart (spec, error);

    if (generated == nullptr)
    {
        footerLabel.setText ("Could not build that exercise: " + error,
                            juce::dontSendNotification);
        return;
    }

    if (songPlaying)
        setSongPlaying (false);

    exerciseSpec = spec;
    chartIsExercise = true;

    auto summary = installChart (std::move (generated));

    //-------------------------------------------------------------------------
    // The two generated tracks. See ExerciseAudio.h for why they are separate:
    // the click owns the clock and ends the take, the tone is the pitch
    // reference and must be silenceable through the existing Guide control.
    //-------------------------------------------------------------------------
    auto& backing = engine.getBackingPlayer();

    const auto rate = backing.getPreparedSampleRate() > 0.0 ? backing.getPreparedSampleRate()
                                                            : 48000.0;

    backing.loadFromBuffer (ExerciseAudio::renderClickTrack (*chart, spec, rate),
                            rate, "metronome");

    backing.loadReferenceFromBuffer (ExerciseAudio::renderGuideTone (*chart, spec, rate),
                                     rate, "pitch guide");

    // ON by default for an exercise, and only for an exercise. A scale with
    // no reference pitch is not an exercise, it is a guessing game — but the
    // singer can still silence it the moment they want to test their own ear,
    // which is the whole reason it is on the reference deck.
    guideVocalToggle.setVisible (true);
    guideVocalToggle.setEnabled (true);
    guideVocalToggle.setToggleState (true, juce::dontSendNotification);
    guideVocalLevel.setVisible (true);
    guideVocalLevel.setEnabled (true);
    guideVocalLevel.setValue (theme->get().guideVocalDb, juce::dontSendNotification);
    pushGuideVocalGain();

    summary << "   |   " << juce::String (spec.bpm) << " bpm, "
            << ExerciseLibrary::getName (spec.kind).toLowerCase() << " x "
            << juce::String (spec.repeats);

    if (spec.referenceBeats > 0 && ! chart->getNotes().empty())
        summary << "   |   starts on "
                << juce::MidiMessage::getMidiNoteName (chart->getNotes().front().midi,
                                                       true, true, 3)
                << ", then " << juce::String (juce::jmax (0, spec.countInBeats))
                << " counts";

    summary << "   |   " << ExerciseLibrary::getPurpose (spec.kind);

    footerLabel.setText (summary, juce::dontSendNotification);
    resized();
    repaint();
}

//==============================================================================
// Loading
//==============================================================================
void MainComponent::openAudioChooser()
{
    if (importer.isBusy())
        return;

    chooser = std::make_unique<juce::FileChooser> (
        "Pick a song to turn into a chart",
        settings->getLastAudioDirectory(),
        SongImporter::getAudioFilePatterns());

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();

                              if (f.existsAsFile())
                              {
                                  settings->setLastAudioDirectory (f.getParentDirectory());
                                  beginImport (f);
                              }
                          });
}

void MainComponent::beginImport (const juce::File& audioFile)
{
    juce::String error;

    // Playing while a separation runs would fight it for the GPU and for the
    // user's attention, and the chart is about to be replaced anyway.
    if (songPlaying)
        setSongPlaying (false);

    importOverlay.beginImport (audioFile.getFileName());
    resized();

    if (! importer.start (audioFile, error))
    {
        importOverlay.showFailure (error, {});
        return;
    }

    // Nothing to disable: "Add song..." lives in the overflow menu now, and the
    // menu greys it while importer.isBusy() — one source of truth rather than a
    // button whose enabled state has to be restored down every exit path.
    footerLabel.setText ("Preparing " + audioFile.getFileName() + "...",
                         juce::dontSendNotification);
}

void MainComponent::openChartChooser()
{
    if (inChartMode())
    {
        unloadChart();
        return;
    }

    //-------------------------------------------------------------------------
    // WHERE THIS OPENS, and why there is no path in this file.
    //
    // AppSettings resolves it: the folder a chart was last loaded from, else the
    // song library root, which is itself found by looking for the repo's songs/
    // directory next to the executable before falling back to
    // %APPDATA%/VocalTrainer/songs. That mirrors _default_library() in the
    // Python CLI, so the tool that WRITES a song and the app that READS it agree
    // on where songs live without either of them naming a path.
    //
    // DESIGN.md section 3 is explicit that this belongs in a PropertiesFile and
    // not in plugin state: an absolute path serialised into an .als breaks
    // permanently the day the library moves.
    //-------------------------------------------------------------------------
    chooser = std::make_unique<juce::FileChooser> ("Load a chart.json",
                                                   settings->getLastChartDirectory(),
                                                   "*.json");

    // launchAsync ONLY. JUCE_MODAL_LOOPS_PERMITTED is 0 in plugin builds, so the
    // blocking browseForFileToOpen() is not even declared. And the chooser must
    // outlive this call, which is why it is a member.
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();

                              if (f.existsAsFile())
                                  loadChart (f);
                          });
}

void MainComponent::loadChart (const juce::File& file)
{
    juce::String error;

    // Parsed synchronously: a chart.json is tens of KB and this is a deliberate
    // user action on the message thread, not a hot path. If charts ever grow to
    // where this is visible, move it to a background thread and swap the result
    // in — do NOT parse inside setStateInformation, which can run on any thread.
    auto loaded = Chart::loadFrom (file, error);

    if (loaded == nullptr)
    {
        chart.reset();
        highway.setChart (nullptr);

        footerLabel.setText ("Could not load chart: " + error, juce::dontSendNotification);
        return;
    }

    // Remembered so the next Load chart... opens here rather than at the
    // library root.
    settings->setLastChartDirectory (file.getParentDirectory());

    chartIsExercise = false;

    auto summary = installChart (std::move (loaded));

    //-------------------------------------------------------- backing track
    auto& backing = engine.getBackingPlayer();

    const auto audioFile = chart->resolveBackingAudio();

    if (audioFile != juce::File() && audioFile.existsAsFile())
    {
        juce::String audioError;

        if (backing.load (audioFile, audioError))
            summary << "   |   backing: " << backing.getLoadedName();
        else
            summary << "   |   " << audioError;
    }
    else
    {
        // Not an error — plenty of charts are practised a cappella. Say so
        // plainly rather than leaving the user wondering why it is silent.
        summary << "   |   no backing audio (silent practice)";
    }

    // The reference vocal is the chart's own analysis source, so it is
    // guaranteed to be time-aligned with the notes on screen.
    const auto refRelative = chart->getReferenceAudioRelative();
    const auto refFile = (refRelative.isNotEmpty() && chart->getBaseDirectory() != juce::File())
                           ? chart->getBaseDirectory().getChildFile (refRelative)
                           : juce::File();

    juce::String refError;
    const auto haveReference = refFile != juce::File()
                                 && backing.loadReference (refFile, refError);

    guideVocalToggle.setVisible (haveReference);
    guideVocalToggle.setEnabled (haveReference);
    guideVocalToggle.setToggleState (false, juce::dontSendNotification);
    guideVocalLevel.setVisible (haveReference);
    guideVocalLevel.setEnabled (haveReference);
    guideVocalLevel.setValue (theme->get().guideVocalDb, juce::dontSendNotification);
    backing.setReferenceGain (0.0f);

    if (haveReference)
        summary << "   |   guide vocal available";
    else if (refError.isNotEmpty())
        summary << "   |   " << refError;

    if (! chart->getWarnings().empty())
        summary << "   |   " << chart->getWarnings().size() << " chart warning(s): "
                << chart->getWarnings().front().code;

    footerLabel.setText (summary, juce::dontSendNotification);

    // Repaint, not just relayout: entering or leaving chart mode changes how
    // many control rows there are, and the panel band behind them is painted by
    // THIS component. Moving the children only repaints where the children were.
    resized();
    repaint();
}

juce::String MainComponent::installChart (std::unique_ptr<Chart> newChart)
{
    // Anything left over from the previous chart has to go before the new one is
    // installed: a take, a review and a selection all refer to note indices that
    // are about to mean something else.
    stopRecording();
    setListeningBack (false);
    hideReview();

    engine.getBackingPlayer().unload();

    chart = std::move (newChart);

    scorer.setChart (chart.get());
    scorer.setMedianMs (theme->get().colourMedianMs);
    reviewShownForTake = false;
    haveLastTakeSummary = false;
    lastTakeSummary = {};
    reviewButton.setEnabled (false);
    listenButton.setEnabled (false);
    loadedTakeFile = juce::File();

    highway.setChart (chart.get());
    highway.setSelectedNoteId (-1);
    highway.setVisible (true);
    lane.setVisible (false);

    lyricStrip.setChart (chart.get());
    lyricStrip.setAutoTranscribed (chart->getLyricSource() == "asr");
    lyricStrip.setVisible (lyricStrip.hasAnyLyrics());

    editPanel.setChart (chart.get());
    editPanel.setStatus ({});

    // Hidden here and shown by whichever loader has something for them, so a
    // chart with no guide can never inherit a live Guide toggle from the chart
    // before it.
    guideVocalToggle.setVisible (false);
    guideVocalLevel.setVisible (false);
    engine.getBackingPlayer().setReferenceGain (0.0f);

    playButton.setVisible (true);
    playButton.setEnabled (true);
    restartButton.setVisible (true);
    recordButton.setVisible (true);
    recordButton.setButtonText ("Rec");
    listenButton.setVisible (true);
    reviewButton.setVisible (true);
    loadChartButton.setButtonText ("Unload");

    // The chart supersedes the manual target note and Free Sing entirely, so
    // they go away rather than sitting there greyed out. A disabled control is a
    // standing invitation to work out what you did wrong; a control that is not
    // there says the mode has moved on.
    noteCaption.setVisible (false);
    noteSelector.setVisible (false);
    freeSingToggle.setVisible (false);

    songPlaying = false;
    playButton.setButtonText ("Play");
    pausedChartTime = 0.0;
    songAnchorStream = lastEstimateTime;
    lastAnimationStream = lastEstimateTime;

    titleLabel.setText (chart->getTitle()
                          + (chart->getArtist().isNotEmpty() ? "  -  " + chart->getArtist()
                                                             : juce::String()),
                        juce::dontSendNotification);

    return juce::String (chart->getNotes().size()) + " notes, "
             + juce::String (chart->getDurationSeconds(), 1) + " s";
}

void MainComponent::unloadChart()
{
    stopRecording();
    setListeningBack (false);
    hideReview();
    setEditMode (false);

    chart.reset();
    chartIsExercise = false;

    scorer.setChart (nullptr);

    highway.setChart (nullptr);
    highway.setVisible (false);
    lane.setVisible (true);

    lyricStrip.setChart (nullptr);
    lyricStrip.setVisible (false);

    editPanel.setChart (nullptr);

    playButton.setVisible (false);
    playButton.setEnabled (false);
    restartButton.setVisible (false);
    recordButton.setVisible (false);
    listenButton.setVisible (false);
    reviewButton.setVisible (false);
    guideVocalToggle.setVisible (false);
    guideVocalLevel.setVisible (false);
    loadChartButton.setButtonText ("Load chart...");

    noteCaption.setVisible (true);
    noteSelector.setVisible (true);
    freeSingToggle.setVisible (true);

    songPlaying = false;
    pausedChartTime = 0.0;

    engine.getBackingPlayer().unload();
    engine.setChartTargetMidi (-1.0f);

    titleLabel.setText ("Vocal Trainer", juce::dontSendNotification);
    resized();
    repaint();
}

//==============================================================================
// Take review
//==============================================================================
void MainComponent::endTake (bool automatic)
{
    if (! inChartMode())
        return;

    if (songPlaying)
    {
        songPlaying = false;
        pausedChartTime = currentChartTime();
        playButton.setButtonText ("Play");
        engine.getBackingPlayer().stop();
    }

    stopRecording();

    // Reaching the end of a listen-back is not the end of a take: leave the
    // review that was already there alone, and drop out of listen-back so the
    // button does not sit reading "Stop" with nothing playing.
    if (listeningBack)
    {
        setListeningBack (false);
        return;
    }

    reviewButton.setEnabled (scorer.hasAnyData() || haveLastTakeSummary);

    // Nothing was sung, so there is nothing to review. Popping an empty review
    // screen up at the end of a silent run would be pure noise.
    if (automatic && ! scorer.hasAnyData())
        return;

    if (automatic && reviewShownForTake)
        return;

    showReview();
}

void MainComponent::showReview()
{
    if (! inChartMode())
        return;

    // Prefer the live scorer, but fall back to the retained summary: after a
    // listen-back the scorer has been reset and the take it described is the
    // thing the user still wants to look at.
    if (scorer.hasAnyData())
    {
        lastTakeSummary = scorer.buildSummary();
        haveLastTakeSummary = true;
    }
    else if (! haveLastTakeSummary)
    {
        lastTakeSummary = scorer.buildSummary();     // empty, with its own message
    }

    const auto& summary = lastTakeSummary;

    reviewOverlay.show (summary, chart->getTitle());
    reviewOverlay.setTake (loadedTakeFile, engine.getTakeRecorder().hadDropout());

    reviewShownForTake = true;

    // The sidecar is written HERE rather than at stop(), because this is the
    // first moment the summary exists — and a take on disk whose sidecar says
    // how it went is what makes a takes folder browsable later.
    if (loadedTakeFile.existsAsFile())
        TakeRecorder::writeSidecar (loadedTakeFile,
                                    engine.getTakeRecorder().getStartChartSeconds(),
                                    chart->getSongId(), chart->getTitle(),
                                    summary.percentInTune, summary.meanCents,
                                    summary.headline());

    resized();
}

void MainComponent::hideReview()
{
    if (reviewOverlay.isVisible())
        reviewOverlay.setVisible (false);
}

//==============================================================================
// Recording and listen-back
//==============================================================================
juce::File MainComponent::getTakesDirectory() const
{
    const auto songDirectory = inChartMode() ? chart->getBaseDirectory() : juce::File();
    const auto songId = inChartMode() ? chart->getSongId() : juce::String();

    return settings->getTakesDirectory (songDirectory, songId);
}

void MainComponent::toggleRecording()
{
    auto& recorder = engine.getTakeRecorder();

    if (recorder.isRecording())
    {
        // Stopping the recording stops the take: they are the same act, and
        // leaving the song running after the recording ended would give a take
        // that covers only part of what the review describes.
        endTake (false);
        return;
    }

    if (! inChartMode())
        return;

    // Recording while listening back would capture the previous take coming out
    // of the speakers.
    setListeningBack (false);

    const auto stamp = juce::Time::getCurrentTime().formatted ("take-%Y%m%d-%H%M%S");
    const auto destination = getTakesDirectory().getChildFile (stamp + ".wav");

    juce::String error;

    if (! recorder.start (destination, engine.getHostSampleRate(),
                          currentChartTime(), error))
    {
        footerLabel.setText ("Could not start recording: " + error,
                            juce::dontSendNotification);
        return;
    }

    recordButton.setButtonText ("Stop");
    loadedTakeFile = juce::File();
    listenButton.setEnabled (false);

    // Recording without the song running would produce a take with nothing to
    // compare it to, so pressing Rec starts the song as well.
    if (! songPlaying)
        setSongPlaying (true);
}

void MainComponent::stopRecording()
{
    auto& recorder = engine.getTakeRecorder();

    if (! recorder.isRecording())
        return;

    const auto finished = recorder.stop();

    recordButton.setButtonText ("Rec");

    if (finished == juce::File())
    {
        footerLabel.setText ("Nothing was recorded.", juce::dontSendNotification);
        return;
    }

    loadedTakeFile = finished;

    // Load it into the take deck straight away, so Listen is one click rather
    // than one click and a wait.
    juce::String error;

    if (engine.getBackingPlayer().loadTake (finished, recorder.getStartChartSeconds(), error))
        listenButton.setEnabled (true);
    else
        footerLabel.setText ("Recorded, but could not load it back: " + error,
                            juce::dontSendNotification);
}

void MainComponent::setListeningBack (bool shouldListen)
{
    auto& backing = engine.getBackingPlayer();

    if (shouldListen && ! backing.hasTake())
        return;

    if (shouldListen == listeningBack)
        return;

    listeningBack = shouldListen;

    // Unity for the take: it is the thing being listened to, not a reference
    // under something else.
    backing.setTakeGain (listeningBack ? 1.0f : 0.0f);

    listenButton.setButtonText (listeningBack ? "Stop" : "Listen");

    if (listeningBack)
    {
        if (songPlaying)
            setSongPlaying (false);

        // Start from where the recording began, so the take and the chart line
        // up. seekSong also clears the scores, which is correct: the run about to
        // happen is playback, not a performance.
        seekSong (backing.getTakeStartChartSeconds());
        setSongPlaying (true);
    }
    else if (songPlaying)
    {
        setSongPlaying (false);
    }
}

//==============================================================================
// Chart editing
//==============================================================================
void MainComponent::setEditMode (bool shouldEdit)
{
    if (editMode == shouldEdit)
        return;

    editMode = shouldEdit && inChartMode();

    editPanel.setVisible (editMode);
    highway.setEditMode (editMode);

    if (editMode)
    {
        // Start on whatever note is under the playhead: that is almost always
        // the one whose defect was just noticed.
        editPanel.selectNoteAtTime (currentChartTime());
        highway.setSelectedNoteId (editPanel.getSelectedNoteId());
        editPanel.setStatus ("Click a note on the highway to select it.");
    }
    else
    {
        highway.setSelectedNoteId (-1);
    }

    resized();
    repaint();
}

void MainComponent::chartEdited()
{
    if (! inChartMode())
        return;

    // The lyric strip caches which notes carry text, and the highway's zoom is
    // derived from the chart's range; both have to be told.
    lyricStrip.setChart (chart.get());
    lyricStrip.setAutoTranscribed (chart->getLyricSource() == "asr");
    lyricStrip.setVisible (lyricStrip.hasAnyLyrics());

    // The scores belong to the chart as it WAS. After an edit the note indices
    // can refer to different notes, and a score attached to the wrong note is
    // worse than no score, so the take AND its review are both discarded.
    scorer.setChart (chart.get());
    reviewShownForTake = false;
    haveLastTakeSummary = false;
    lastTakeSummary = {};
    reviewButton.setEnabled (false);

    highway.repaint();
    resized();
}

void MainComponent::saveChart()
{
    if (! inChartMode())
        return;

    //-------------------------------------------------------------------------
    // A generated exercise has no file behind it, so saving one means choosing
    // where it goes. Offering that rather than refusing is worth it: a warm-up
    // you liked at a particular tempo and root becomes an ordinary song in the
    // library, and the offline tools can then treat it as one.
    //-------------------------------------------------------------------------
    if (chart->getSourceFile() == juce::File())
    {
        const auto suggested = settings->getSongLibraryRoot()
                                  .getChildFile (chart->getSongId().isNotEmpty()
                                                   ? chart->getSongId()
                                                   : juce::String ("exercise"))
                                  .getChildFile ("chart.json");

        chooser = std::make_unique<juce::FileChooser> ("Save this chart as...",
                                                       suggested, "*.json");

        chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto target = fc.getResult();

                                  if (target == juce::File())
                                      return;

                                  juce::String error;

                                  if (ChartWriter::save (*chart, target, error))
                                  {
                                      chart->markSaved();
                                      editPanel.setStatus ("Saved to " + target.getFileName());
                                  }
                                  else
                                  {
                                      editPanel.setStatus (error);
                                  }
                              });

        return;
    }

    juce::String error;

    if (ChartWriter::save (*chart, {}, error))
    {
        chart->markSaved();

        editPanel.setStatus (error.isNotEmpty()
                               ? error
                               : "Saved. Previous version kept as chart.json.bak.");
    }
    else
    {
        editPanel.setStatus (error);
    }
}

} // namespace vt
