/*
  ==============================================================================

    PluginEditor.cpp
    Vocal Trainer — M1

  ==============================================================================
*/

#include "PluginEditor.h"

#include <juce_audio_basics/juce_audio_basics.h>   // juce::MidiMessage::getMidiNoteName

// Used ONLY to defuse JUCE's "Mute audio input" checkbox — see the constructor.
// __has_include so a build configuration without the standalone client simply
// skips it rather than failing.
#if __has_include(<juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>)
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
 #define VT_HAVE_STANDALONE_HOLDER 1
#else
 #define VT_HAVE_STANDALONE_HOLDER 0
 // SAY SO OUT LOUD. This guard once evaluated false silently because
 // juce_add_plugin puts juce_audio_plugin_client on the FORMAT targets but not
 // on the shared-code target — the build stayed green and the fix simply was
 // not there. A skipped fix must never again look identical to an applied one.
 #pragma message("VocalTrainer: juce_StandaloneFilterWindow.h not on the include path - " \
                 "the standalone 'Mute audio input' checkbox will NOT be disabled, and " \
                 "ticking it will kill the pitch display. Add JUCE/modules to " \
                 "target_include_directories for the shared-code target.")
#endif

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
}

//==============================================================================
VocalTrainerEditor::VocalTrainerEditor (VocalTrainerProcessor& p)
    : juce::AudioProcessorEditor (&p),
      processor (p)
{
    addAndMakeVisible (lane);

    // Both views exist at all times; only one is visible. Swapping visibility is
    // cheaper and less error-prone than building/tearing down a component, and it
    // keeps the highway's ring buffer alive across a chart reload.
    addChildComponent (highway);
    addChildComponent (lyricStrip);

    loadChartButton.onClick = [this] { openChartChooser(); };
    addAndMakeVisible (loadChartButton);

    // STANDALONE ONLY. Chart prep is minutes of GPU work; starting one from
    // inside Live — which owns the audio device and expects prompt returns —
    // would make Live look hung. Preparing songs is a before-you-play activity.
    if (isStandalone())
    {
        addSongButton.setTooltip ("Turn any audio file into a chart: splits the vocal "
                                  "from the backing, tracks pitch, transcribes lyrics. "
                                  "Takes a few minutes per song.");
        addSongButton.onClick = [this] { openAudioChooser(); };
        addAndMakeVisible (addSongButton);
    }

    importOverlay.onCancel   = [this] { importer.cancel(); };
    importOverlay.onDismiss  = [this] { importOverlay.setVisible (false); };
    addChildComponent (importOverlay);

    importer.onProgress = [this] (const SongImporter::Progress& p)
    {
        importOverlay.setProgress (p.percent, p.message);
    };

    importer.onFinished = [this] (const SongImporter::Result& r)
    {
        addSongButton.setEnabled (true);

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

    monitorToggle.setTooltip ("Hear your own microphone through this app. Off by default: "
                              "the Apollo Console already monitors at near-zero latency, so "
                              "this would only add a delayed duplicate of your voice. "
                              "Turning it off never affects the pitch display.");
    monitorToggle.setToggleState (processor.getMonitorMic(), juce::dontSendNotification);
    monitorToggle.onClick = [this]
    {
        processor.setMonitorMic (monitorToggle.getToggleState());
    };

    if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
    {
        addAndMakeVisible (monitorToggle);

        // DEFUSE JUCE'S "Mute audio input".
        //
        // That checkbox does not mute the output — it substitutes an EMPTY
        // BUFFER for the microphone before the plugin runs, to break feedback
        // loops in ordinary effect plugins. For an ANALYSER that is fatal: the
        // pitch display goes dead along with the sound, which is exactly what
        // the user hit.
        //
        // We do not need it, because monitoring is off by default and is an
        // output-side decision here, so there is no feedback loop to break.
        // Clearing processorHasPotentialFeedbackLoop removes the checkbox from
        // Audio Settings entirely, so it cannot silently kill the display again.
       #if VT_HAVE_STANDALONE_HOLDER
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            holder->getMuteInputValue().setValue (false);
            holder->processorHasPotentialFeedbackLoop = false;
        }
       #endif
    }

    guideVocalToggle.setTooltip ("Hear the reference singer. Off by default: a guide vocal "
                                 "you cannot silence teaches you to follow it rather than to "
                                 "pitch it.");
    guideVocalToggle.onClick = [this] { pushGuideVocalGain(); };
    addChildComponent (guideVocalToggle);

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
    addChildComponent (guideVocalLevel);

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

    themeButton.setTooltip (Theme::getThemeFile().getFullPathName());
    themeButton.onClick = []
    {
        const auto f = Theme::getThemeFile();

        if (f.existsAsFile())
            f.revealToUser();
        else
            f.getParentDirectory().revealToUser();
    };
    addAndMakeVisible (themeButton);

    footerLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (footerLabel);

    noteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.apvts, VocalTrainerProcessor::pidTargetNote, noteSelector);

    freeSingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.apvts, VocalTrainerProcessor::pidFreeSing, freeSingToggle);

    theme->addChangeListener (this);
    applyTheme();
    suppressToleranceSync = false;   // from here on, a theme.json save is a deliberate act

    syncParametersToLane();

    processor.setGuiActive (true);

    setResizable (true, true);

    if (auto* c = getConstrainer())
        c->setMinimumSize (640, 360);

    setSize (960, 540);

    // Constructed last, on purpose: everything it touches must already exist.
    vblank = juce::VBlankAttachment (this, [this] { onVBlank(); });
}

VocalTrainerEditor::~VocalTrainerEditor()
{
    vblank = {};                       // stop callbacks before anything is torn down

    // Stop the import worker HERE rather than leaving it to member destruction
    // order. Its callbacks touch importOverlay and footerLabel, which are
    // declared after it and would therefore be destroyed first.
    importer.onProgress = nullptr;
    importer.onFinished = nullptr;
    importer.cancel();

    processor.setGuiActive (false);
    theme->removeChangeListener (this);
}

//==============================================================================
void VocalTrainerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    applyTheme();
}

void VocalTrainerEditor::applyTheme()
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
    // needs through an atomic on the processor.
    processor.setClarityThreshold (t.clarityThreshold);

    // theme.json is the live tuning surface for the tolerances, but the APVTS is
    // what gets saved with the session. Pushing on RELOAD only means: opening the
    // editor respects the saved session; saving theme.json deliberately retunes.
    if (! suppressToleranceSync)
    {
        if (auto* pf = processor.apvts.getParameter (VocalTrainerProcessor::pidTFullCents))
            pf->setValueNotifyingHost (pf->convertTo0to1 (t.tFullCents));

        if (auto* pz = processor.apvts.getParameter (VocalTrainerProcessor::pidTZeroCents))
            pz->setValueNotifyingHost (pz->convertTo0to1 (t.tZeroCents));
    }

    const auto textColour = t.text;
    const auto dimColour  = t.textDim;

    titleLabel.setColour  (juce::Label::textColourId, textColour);
    noteCaption.setColour (juce::Label::textColourId, dimColour);
    footerLabel.setColour (juce::Label::textColourId, dimColour);
    freeSingToggle.setColour (juce::ToggleButton::textColourId, textColour);
    freeSingToggle.setColour (juce::ToggleButton::tickColourId, t.targetBarActive);

    noteSelector.setColour (juce::ComboBox::backgroundColourId, t.panel);
    noteSelector.setColour (juce::ComboBox::textColourId, textColour);
    noteSelector.setColour (juce::ComboBox::outlineColourId, t.gridLine);

    themeButton.setColour (juce::TextButton::buttonColourId, t.panel);
    themeButton.setColour (juce::TextButton::textColourOffId, dimColour);

    importOverlay.applyTheme();

    titleLabel.setFont  (juce::Font (juce::FontOptions ((float) juce::jmax (12, t.headerHeight - 14))));
    footerLabel.setFont (juce::Font (juce::FontOptions ((float) juce::jmax (10, t.footerHeight - 12))));

    resized();
    repaint();
}

//==============================================================================
void VocalTrainerEditor::paint (juce::Graphics& g)
{
    const auto& t = theme->get();

    g.fillAll (t.background);

    auto bounds = getLocalBounds();

    g.setColour (t.panel);
    g.fillRect (bounds.removeFromTop (t.headerHeight + t.controlsHeight));
    g.fillRect (bounds.removeFromBottom (t.footerHeight));

    g.setColour (t.gridLine);
    g.fillRect (0, t.headerHeight + t.controlsHeight, getWidth(), 1);
    g.fillRect (0, getHeight() - t.footerHeight - 1, getWidth(), 1);
}

void VocalTrainerEditor::resized()
{
    const auto& t = theme->get();

    auto bounds = getLocalBounds();

    auto header = bounds.removeFromTop (t.headerHeight).reduced (10, 2);
    titleLabel.setBounds (header);

    auto controls = bounds.removeFromTop (t.controlsHeight).reduced (10, 3);
    themeButton.setBounds (controls.removeFromRight (100));
    controls.removeFromRight (12);
    freeSingToggle.setBounds (controls.removeFromRight (110));
    controls.removeFromRight (12);
    noteSelector.setBounds (controls.removeFromRight (90));
    noteCaption.setBounds (controls.removeFromRight (54));

    // Transport lives on the LEFT so it does not shuffle the M1 controls around
    // when a chart is loaded.
    loadChartButton.setBounds (controls.removeFromLeft (100));
    controls.removeFromLeft (8);

    if (addSongButton.isVisible())
    {
        addSongButton.setBounds (controls.removeFromLeft (96));
        controls.removeFromLeft (8);
    }

    restartButton.setBounds (controls.removeFromLeft (32));
    controls.removeFromLeft (4);
    playButton.setBounds (controls.removeFromLeft (66));
    controls.removeFromLeft (12);
    monitorToggle.setBounds (controls.removeFromLeft (86));
    controls.removeFromLeft (8);
    guideVocalToggle.setBounds (controls.removeFromLeft (66));
    guideVocalLevel.setBounds (controls.removeFromLeft (176));

    footerLabel.setBounds (bounds.removeFromBottom (t.footerHeight).reduced (10, 2));

    lane.setBounds (bounds);

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
    importOverlay.setBounds (getLocalBounds().withTrimmedTop (t.headerHeight + t.controlsHeight)
                                             .withTrimmedBottom (t.footerHeight));
}

//==============================================================================
void VocalTrainerEditor::syncParametersToLane()
{
    const auto readParam = [this] (const char* id, float fallback)
    {
        if (auto* raw = processor.apvts.getRawParameterValue (id))
            return raw->load (std::memory_order_relaxed);

        return fallback;
    };

    lane.setTargetNote (juce::roundToInt (readParam (VocalTrainerProcessor::pidTargetNote, 60.0f)));
    lane.setFreeSing   (readParam (VocalTrainerProcessor::pidFreeSing, 1.0f) > 0.5f);
    lane.setTolerances (readParam (VocalTrainerProcessor::pidTFullCents, 25.0f),
                        readParam (VocalTrainerProcessor::pidTZeroCents, 100.0f));
}

//==============================================================================
void VocalTrainerEditor::onVBlank()
{
    // Cheap atomic loads; the lane setters early-out when nothing changed, so
    // polling here avoids an APVTS listener and its thread-affinity questions.
    syncParametersToLane();

    const double hostRate = processor.getHostSampleRate() > 0.0 ? processor.getHostSampleRate()
                                                                : 48000.0;

    const int n = processor.drainPitchEstimates (drainBuffer.data(), kDrainCapacity);

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
        // The backing track running off the end stops the song, so the button
        // has to follow. Polling here beats a callback from the audio thread.
        if (songPlaying && processor.getBackingPlayer().hasFinished())
        {
            songPlaying = false;
            pausedChartTime = currentChartTime();
            playButton.setButtonText ("Play");
        }

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

        if (lyricStrip.hasAnyLyrics())
        {
            lyricStrip.setNowTime (currentChartTime());
            lyricStrip.repaint();
        }

        // Hand the detector the note to constrain its octave search around.
        const auto target = highway.getTargetMidi();
        processor.setChartTargetMidi (target >= 0 ? (float) target : -1.0f);

        if (n > 0 || songPlaying)
            highway.repaint();
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
void VocalTrainerEditor::updateFooter()
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

    TransportSnapshot snap;

    if (processor.readTransport (snap))
    {
        text << "   |   " << (snap.isPlaying ? "playing" : "stopped")
             << " @ " << juce::String (snap.bpm, 1) << " bpm";
    }

    if (theme->hasParseError())
        text << "   |   theme.json: parse error, using last good values";

    if (text != footerLabel.getText())
        footerLabel.setText (text, juce::dontSendNotification);
}

//==============================================================================
// M3 — chart mode
//==============================================================================

void VocalTrainerEditor::pushGuideVocalGain()
{
    const auto db = (float) guideVocalLevel.getValue();
    const auto gain = guideVocalToggle.getToggleState()
                        ? juce::Decibels::decibelsToGain (db, -40.0f)
                        : 0.0f;

    processor.getBackingPlayer().setReferenceGain (gain);
}

double VocalTrainerEditor::currentChartTime() const noexcept
{
    // When a backing track is loaded it IS the clock, because the singer syncs
    // to what they HEAR. Running the highway off a different clock than the
    // audio would let the two drift apart, which is exactly the failure the
    // whole timestamping design exists to prevent.
    auto& backing = processor.getBackingPlayer();

    if (backing.hasTrack())
        return backing.getPositionSeconds();

    // No backing track: fall back to the audio-stream clock.
    return songPlaying ? (lastEstimateTime - songAnchorStream) : pausedChartTime;
}

void VocalTrainerEditor::setSongPlaying (bool shouldPlay)
{
    if (shouldPlay == songPlaying || ! inChartMode())
        return;

    auto& backing = processor.getBackingPlayer();

    if (shouldPlay)
    {
        // Re-anchor so the chart resumes exactly where it paused.
        songAnchorStream = lastEstimateTime - pausedChartTime;
        songPlaying = true;

        if (backing.hasTrack())
            backing.start();
    }
    else
    {
        pausedChartTime = currentChartTime();
        songPlaying = false;

        if (backing.hasTrack())
            backing.stop();
    }

    playButton.setButtonText (songPlaying ? "Pause" : "Play");
}

void VocalTrainerEditor::seekSong (double chartSeconds)
{
    if (! inChartMode())
        return;

    pausedChartTime  = juce::jmax (0.0, chartSeconds);
    songAnchorStream = lastEstimateTime - pausedChartTime;

    processor.getBackingPlayer().setPosition (pausedChartTime);

    highway.clearHistory();
    highway.setNowTime (pausedChartTime);
    highway.repaint();
}

void VocalTrainerEditor::openAudioChooser()
{
    if (importer.isBusy())
        return;

    chooser = std::make_unique<juce::FileChooser> (
        "Pick a song to turn into a chart",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        SongImporter::getAudioFilePatterns());

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();

                              if (f.existsAsFile())
                                  beginImport (f);
                          });
}

void VocalTrainerEditor::beginImport (const juce::File& audioFile)
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

    addSongButton.setEnabled (false);
    footerLabel.setText ("Preparing " + audioFile.getFileName() + "...",
                         juce::dontSendNotification);
}

void VocalTrainerEditor::openChartChooser()
{
    if (inChartMode())
    {
        unloadChart();
        return;
    }

    chooser = std::make_unique<juce::FileChooser> ("Load a chart.json",
                                                   juce::File::getSpecialLocation (
                                                       juce::File::userHomeDirectory),
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

void VocalTrainerEditor::loadChart (const juce::File& file)
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

    chart = std::move (loaded);

    highway.setChart (chart.get());
    highway.setVisible (true);
    lane.setVisible (false);

    lyricStrip.setChart (chart.get());
    lyricStrip.setAutoTranscribed (chart->getLyricSource() == "asr");
    lyricStrip.setVisible (lyricStrip.hasAnyLyrics());

    playButton.setVisible (true);
    playButton.setEnabled (true);
    restartButton.setVisible (true);
    loadChartButton.setButtonText ("Unload");

    // The chart's target note supersedes the manual selector.
    noteSelector.setEnabled (false);
    noteCaption.setEnabled (false);

    songPlaying = false;
    playButton.setButtonText ("Play");
    pausedChartTime = 0.0;
    songAnchorStream = lastEstimateTime;
    lastAnimationStream = lastEstimateTime;

    titleLabel.setText (chart->getTitle()
                          + (chart->getArtist().isNotEmpty() ? "  -  " + chart->getArtist()
                                                             : juce::String()),
                        juce::dontSendNotification);

    auto summary = juce::String (chart->getNotes().size()) + " notes, "
                     + juce::String (chart->getDurationSeconds(), 1) + " s";

    //-------------------------------------------------------- backing track
    auto& backing = processor.getBackingPlayer();
    backing.unload();

    if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
    {
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
    }

    if (! chart->getWarnings().empty())
        summary << "   |   " << chart->getWarnings().size() << " chart warning(s): "
                << chart->getWarnings().front().code;

    footerLabel.setText (summary, juce::dontSendNotification);

    resized();
}

void VocalTrainerEditor::unloadChart()
{
    chart.reset();

    highway.setChart (nullptr);
    highway.setVisible (false);
    lane.setVisible (true);

    lyricStrip.setChart (nullptr);
    lyricStrip.setVisible (false);

    playButton.setVisible (false);
    playButton.setEnabled (false);
    restartButton.setVisible (false);
    guideVocalToggle.setVisible (false);
    guideVocalLevel.setVisible (false);
    loadChartButton.setButtonText ("Load chart...");

    noteSelector.setEnabled (true);
    noteCaption.setEnabled (true);

    songPlaying = false;
    pausedChartTime = 0.0;

    processor.getBackingPlayer().unload();
    processor.setChartTargetMidi (-1.0f);

    titleLabel.setText ("Vocal Trainer", juce::dontSendNotification);
    resized();
}

} // namespace vt
