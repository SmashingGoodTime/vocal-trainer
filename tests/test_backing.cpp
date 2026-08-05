/*
    ==========================================================================

    test_backing.cpp — BackingPlayer lifecycle, i.e. the crash of 2026-08-04.

    WHY THIS EXISTS
    ---------------
    The app died with 0xC0000094 — an INTEGER DIVIDE BY ZERO — right after a
    song import finished. Symbolised from the minidump against a linker map of
    the identical binary, the fault was:

        WASAPIAudioIODevice::run
          -> VocalTrainerProcessor::processBlock
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

    wav.deleteFile();

    std::cout << "-------------------------------------------" << std::endl;

    if (failures == 0)
        std::cout << "ALL " << checks << " BACKING CHECKS PASSED" << std::endl;
    else
        std::cout << failures << " of " << checks << " BACKING CHECKS FAILED" << std::endl;

    return failures == 0 ? 0 : 1;
}
