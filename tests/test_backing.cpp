/*
    ==========================================================================

    test_backing.cpp — BackingPlayer lifecycle, i.e. the crash of 2026-08-04.

    WHY THIS EXISTS
    ---------------
    The app died with 0xC0000094 — an INTEGER DIVIDE BY ZERO — right after a
    song import finished. Symbolised from the minidump against a linker map of
    the identical binary, the fault was:

        WASAPIAudioIODevice::run
          -> AudioEngine::processBlock
          -> BackingPlayer::renderInto
          -> AudioTransportSource::getNextAudioBlock
          -> ResamplingAudioSource::getNextAudioBlock + 0x14d   <-- idiv r12d

    That instruction is `bufferPos %= bufferSize` in JUCE's resampler.
    `ResamplingAudioSource::releaseResources()` sets its buffer to ZERO
    samples, and `AudioTransportSource::getNextAudioBlock` guards only on
    `masterSource != nullptr && ! stopped` — nothing stops the audio thread
    pulling a released source. `loaded` and `playing` do not help: both stay
    true across a device restart.

    So a single audio callback arriving between releaseResources() and the next
    prepareToPlay() kills the process. That window opens whenever the audio
    device restarts, and a multi-minute CPU/GPU-saturating song import is a
    very good way to make one restart.

    These tests drive that sequence directly. Without the `prepared` guard in
    BackingPlayer, the first one does not fail — it CRASHES THE RUNNER, which
    is exactly the signal wanted.

    ==========================================================================
*/

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <iostream>

#include "audio/BackingPlayer.h"
#include "exercise/ExerciseAudio.h"

namespace
{
    int failures = 0, checks = 0;

    void check (bool ok, const juce::String& name, const juce::String& detail = {})
    {
        ++checks;
        if (! ok) ++failures;

        std::cout << (ok ? "[PASS] " : "[FAIL] ")
                  << name.paddedRight (' ', 56).toStdString();

        if (detail.isNotEmpty())
            std::cout << detail.toStdString();

        std::cout << std::endl;
    }

    /** A short WAV at a rate that does NOT match the device rate used below, so
        AudioTransportSource really does build a ResamplingAudioSource. With
        matching rates the resampler still exists but the bug is easier to miss
        by accident. 44.1 kHz file against a 48 kHz device is also the exact
        combination the separator produces. */
    juce::File writeTestWav (double fileRate = 44100.0, double seconds = 2.0)
    {
        auto file = juce::File::createTempFile ("vt_backing_test.wav");
        const int numSamples = (int) (fileRate * seconds);

        juce::AudioBuffer<float> tone (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                tone.setSample (ch, i, 0.2f * std::sin (juce::MathConstants<float>::twoPi
                                                          * 220.0f * (float) i / (float) fileRate));

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

        if (stream == nullptr)
            return {};

        auto writer = wav.createWriterFor (stream,
                                           juce::AudioFormatWriterOptions{}
                                               .withSampleRate (fileRate)
                                               .withNumChannels (2)
                                               .withBitsPerSample (16));

        if (writer == nullptr)
            return {};

        writer->writeFromAudioSampleBuffer (tone, 0, numSamples);
        writer.reset();
        return file;
    }

    constexpr double kDeviceRate = 48000.0;
    constexpr int    kBlockSize  = 512;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::cout << "vocal-trainer BackingPlayer lifecycle tests\n"
              << "-------------------------------------------" << std::endl;

    const auto wav = writeTestWav();

    if (wav == juce::File() || ! wav.existsAsFile())
    {
        std::cout << "[SKIP] could not write a temporary WAV" << std::endl;
        return 0;
    }

    juce::AudioBuffer<float> block (2, kBlockSize);

    //==========================================================================
    // THE CRASH. Play, then release the device, then take one more callback.
    //==========================================================================
    {
        vt::BackingPlayer player;
        juce::String error;

        player.prepare (kDeviceRate, kBlockSize);
        check (player.load (wav, error), "backing: a 44.1 kHz file loads", error);
        player.start();

        block.clear();
        player.renderInto (block);
        check (block.getMagnitude (0, kBlockSize) > 0.0f,
               "backing: renders audio while prepared and playing",
               "magnitude " + juce::String (block.getMagnitude (0, kBlockSize), 4));

        // The device goes away. JUCE zeroes the resampler's internal buffer
        // here; `loaded` and `playing` both stay true.
        player.releaseResources();

        // ...and one more callback arrives before prepareToPlay() comes back.
        // THIS LINE IS THE BUG. Unguarded it is an integer divide by zero and
        // the process dies here rather than failing a check.
        block.clear();
        player.renderInto (block);

        check (block.getMagnitude (0, kBlockSize) == 0.0f,
               "backing: a callback after releaseResources() is SILENT, not fatal",
               "this is the 0xC0000094 crash of 2026-08-04");

        // And it must come back to life afterwards, not stay mute forever.
        player.prepare (kDeviceRate, kBlockSize);
        block.clear();
        player.renderInto (block);

        check (block.getMagnitude (0, kBlockSize) > 0.0f,
               "backing: re-preparing after a release resumes audio",
               "magnitude " + juce::String (block.getMagnitude (0, kBlockSize), 4));
    }

    //==========================================================================
    // Rendering before prepare() has ever run — the same hole at startup.
    //==========================================================================
    {
        vt::BackingPlayer player;
        juce::String error;

        player.load (wav, error);
        player.start();

        block.clear();
        player.renderInto (block);

        check (block.getMagnitude (0, kBlockSize) == 0.0f,
               "backing: rendering before prepare() is silent, not fatal");
    }

    //==========================================================================
    // The import hand-off: unload and reload while the device is live. This is
    // what "Add song..." does the instant a chart finishes building.
    //==========================================================================
    {
        vt::BackingPlayer player;
        juce::String error;

        player.prepare (kDeviceRate, kBlockSize);
        player.load (wav, error);
        player.start();
        block.clear();
        player.renderInto (block);

        player.unload();
        block.clear();
        player.renderInto (block);
        check (block.getMagnitude (0, kBlockSize) == 0.0f,
               "backing: unload mid-playback renders silence");

        check (player.load (wav, error), "backing: reload after unload succeeds", error);
        player.start();
        block.clear();
        player.renderInto (block);
        check (block.getMagnitude (0, kBlockSize) > 0.0f,
               "backing: audio returns after the import hand-off",
               "magnitude " + juce::String (block.getMagnitude (0, kBlockSize), 4));
    }

    //==========================================================================
    // The guide vocal rides a second transport with the same lifecycle, so it
    // has the same hole and needs the same proof.
    //==========================================================================
    {
        vt::BackingPlayer player;
        juce::String error;

        player.prepare (kDeviceRate, kBlockSize);
        player.load (wav, error);
        check (player.loadReference (wav, error), "backing: reference vocal loads", error);
        player.setReferenceGain (1.0f);
        player.start();

        block.clear();
        player.renderInto (block);
        check (block.getMagnitude (0, kBlockSize) > 0.0f,
               "backing: backing + reference both render");

        player.releaseResources();
        block.clear();
        player.renderInto (block);
        check (block.getMagnitude (0, kBlockSize) == 0.0f,
               "backing: released reference transport is silent, not fatal");
    }

    //==========================================================================
    // The two buffers a generated exercise plays. Rendering them is pure
    // arithmetic, but WHICH DECK each sound lands on is a pedagogical decision
    // that is invisible from the code that plays them — and getting it wrong is
    // silent: the app still works, the singer just loses their starting note the
    // first time they turn the guide off to test their ear.
    //==========================================================================
    {
        const auto spec = vt::ExerciseLibrary::makeDefaultSpec (vt::ExerciseSpec::Kind::majorScale,
                                                                60, 72);
        juce::String error;
        auto chart = vt::ExerciseLibrary::buildChart (spec, error);

        check (chart != nullptr, "exercise audio: the chart builds", error);

        if (chart != nullptr)
        {
            const auto click = vt::ExerciseAudio::renderClickTrack (*chart, spec, kDeviceRate);
            const auto guide = vt::ExerciseAudio::renderGuideTone (*chart, spec, kDeviceRate);

            const auto rangeMagnitude = [kDeviceRate = kDeviceRate] (const juce::AudioBuffer<float>& b,
                                                                     double fromSeconds, double toSeconds)
            {
                const auto from = juce::jlimit (0, b.getNumSamples(),
                                                (int) (fromSeconds * kDeviceRate));
                const auto to   = juce::jlimit (from, b.getNumSamples(),
                                                (int) (toSeconds * kDeviceRate));

                return to > from ? b.getMagnitude (from, to - from) : 0.0f;
            };

            const auto reference = vt::ExerciseLibrary::referencePitchSeconds (spec);
            const auto leadIn    = vt::ExerciseLibrary::leadInSeconds (spec);

            check (click.getNumSamples() > 0 && guide.getNumSamples() > 0,
                   "exercise audio: both decks rendered",
                   juce::String (click.getNumSamples() / kDeviceRate, 1) + " s");

            // The starting pitch is on the CLICK deck, so silencing the guide
            // does not silence it.
            check (rangeMagnitude (click, 0.0, reference) > 0.05f,
                   "exercise audio: the reference pitch is on the click deck",
                   juce::String (rangeMagnitude (click, 0.0, reference), 3));

            check (rangeMagnitude (guide, 0.0, leadIn) == 0.0f,
                   "exercise audio: the guide deck is silent through the lead-in",
                   juce::String (rangeMagnitude (guide, 0.0, leadIn), 3));

            // And it rings ALONE: a tick over the strike is the one moment the
            // singer is listening hardest to the pitch.
            check (rangeMagnitude (click, 0.001, reference - 0.05) > 0.0f
                     && rangeMagnitude (click, reference, reference + 0.05) > 0.05f,
                   "exercise audio: count-in clicks start after the reference pitch");

            check (rangeMagnitude (guide, leadIn, leadIn + 0.5) > 0.05f,
                   "exercise audio: the guide strikes the first note on time",
                   juce::String (rangeMagnitude (guide, leadIn, leadIn + 0.5), 3));

            // A piano note DECAYS. If this ever came back equal, the tone has
            // silently gone back to being a steady drone.
            const auto atStrike = rangeMagnitude (guide, leadIn, leadIn + 0.10);
            const auto later    = rangeMagnitude (guide, leadIn + 0.45, leadIn + 0.55);

            check (later < atStrike * 0.9f && later > 0.0f,
                   "exercise audio: the guide tone decays like a struck string",
                   juce::String (atStrike, 3) + " -> " + juce::String (later, 3));

            check (click.getMagnitude (0, click.getNumSamples()) <= 1.0f
                     && guide.getMagnitude (0, guide.getNumSamples()) <= 1.0f,
                   "exercise audio: neither deck clips",
                   juce::String (juce::jmax (click.getMagnitude (0, click.getNumSamples()),
                                             guide.getMagnitude (0, guide.getNumSamples())), 3));

            //------------------------------------------------------------------
            // Rendering happens on the MESSAGE THREAD, in the handler for a menu
            // item, so it has to be fast enough not to read as the app hanging.
            // The bound is deliberately generous — it is wall clock on unknown
            // hardware — because what it guards is the ORDER OF MAGNITUDE:
            // twelve partials over three and a half minutes is ~10^8 samples,
            // and going back to a std::sin per partial per sample lands a long
            // way the wrong side of this.
            //------------------------------------------------------------------
            {
                const auto longest = vt::ExerciseLibrary::makeDefaultSpec (
                                         vt::ExerciseSpec::Kind::majorScale, 60, 72,
                                         vt::ExerciseLibrary::kLongSeconds);

                if (auto longest_chart = vt::ExerciseLibrary::buildChart (longest, error))
                {
                    const auto startMs = juce::Time::getMillisecondCounterHiRes();

                    const auto clickTrack = vt::ExerciseAudio::renderClickTrack (*longest_chart, longest, kDeviceRate);
                    const auto guideTrack = vt::ExerciseAudio::renderGuideTone (*longest_chart, longest, kDeviceRate);

                    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;

                    check (elapsedMs < 2000.0 && clickTrack.getNumSamples() > 0
                             && guideTrack.getNumSamples() > 0,
                           "exercise audio: the longest exercise renders without a visible freeze",
                           juce::String (elapsedMs, 0) + " ms for "
                             + juce::String (guideTrack.getNumSamples() / kDeviceRate, 0) + " s");
                }
            }

            // Long tones are re-struck, or the reference is gone by the time the
            // note gets hard to hold.
            auto longSpec = vt::ExerciseLibrary::makeDefaultSpec (vt::ExerciseSpec::Kind::sustainedTone,
                                                                  60, 72);
            auto longChart = vt::ExerciseLibrary::buildChart (longSpec, error);

            if (longChart != nullptr && ! longChart->getNotes().empty())
            {
                const auto tones = vt::ExerciseAudio::renderGuideTone (*longChart, longSpec, kDeviceRate);
                const auto& note = longChart->getNotes().front();
                const auto lastQuarter = note.startSeconds + note.durationSeconds * 0.75;

                check (rangeMagnitude (tones, lastQuarter, note.endSeconds()) > 0.02f,
                       "exercise audio: a long tone is re-struck before it dies away",
                       juce::String (rangeMagnitude (tones, lastQuarter, note.endSeconds()), 3));
            }
            else
            {
                check (false, "exercise audio: the long-tone chart builds", error);
            }
        }
    }

    wav.deleteFile();

    std::cout << "-------------------------------------------" << std::endl;

    if (failures == 0)
        std::cout << "ALL " << checks << " BACKING CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " of " << checks << " BACKING CHECKS FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
