/*
    ==========================================================================

    BackingPlayer.h — plays the song's backing track in the Standalone.

    STANDALONE ONLY. In the VST3, Ableton owns playback and the transport;
    a plugin that also played the backing track would double it.

    WHY THIS IS THE CLOCK
    ---------------------
    When a backing track is loaded it becomes the master clock for chart time,
    replacing the audio-stream clock. The reason is simple: the singer syncs to
    what they HEAR. If the highway ran off a different clock than the audio,
    any drift between them would show up as the chart sliding against the
    music, which is exactly the failure the whole timestamping design exists to
    avoid.

    THREADING
    ---------
    Loading allocates and does file I/O, so it happens on the message thread.
    The audio thread only ever calls renderInto(), which takes a SPIN LOCK with
    tryEnter — if a load is in flight it renders silence for that one block
    rather than blocking. A dropped block during a deliberate song load is
    inaudible; a blocked audio thread is a dropout.

    ==========================================================================
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>

namespace vt
{

class BackingPlayer
{
public:
    BackingPlayer();
    ~BackingPlayer();

    //==========================================================================
    /** Message thread. Returns false and fills `error` if the file cannot be
        read. Passing a non-existent file clears any current track. */
    bool load (const juce::File& audioFile, juce::String& error);

    /** Loads the REFERENCE VOCAL — the recorded singer, for listening to.

        Kept as a second transport rather than pre-mixed into the backing track,
        because the whole point is being able to turn it off: a guide vocal you
        cannot silence teaches you to follow rather than to pitch, and the
        pedagogy in DESIGN.md section 6 depends on being able to take the crutch
        away. Muted by default for the same reason.

        Both transports are started, stopped and seeked by the same calls and
        pull the same number of samples per block, so they stay sample-locked.
        The backing track remains the clock. */
    bool loadReference (const juce::File& audioFile, juce::String& error);

    void unload();

    bool hasTrack() const noexcept { return loaded.load (std::memory_order_relaxed); }
    bool hasReference() const noexcept { return referenceLoaded.load (std::memory_order_relaxed); }

    /** Linear gain for the reference vocal. 0 == off (the default). */
    void setReferenceGain (float g) noexcept
    {
        referenceGain.store (juce::jlimit (0.0f, 2.0f, g), std::memory_order_relaxed);
    }

    float getReferenceGain() const noexcept
    {
        return referenceGain.load (std::memory_order_relaxed);
    }

    /** Message thread. Safe to call before prepare(). */
    void prepare (double sampleRate, int maxBlockSize);
    void releaseResources();

    //==========================================================================
    void start();
    void stop();
    void setPosition (double seconds);

    bool isPlaying() const noexcept { return playing.load (std::memory_order_relaxed); }

    /** Current playback position in seconds. Published from the audio thread
        after every render, so this is what the singer is actually hearing. */
    double getPositionSeconds() const noexcept
    {
        return positionSeconds.load (std::memory_order_relaxed);
    }

    double getLengthSeconds() const noexcept
    {
        return lengthSeconds.load (std::memory_order_relaxed);
    }

    /** True once playback has run past the end of the file. */
    bool hasFinished() const noexcept { return finished.load (std::memory_order_relaxed); }

    //==========================================================================
    /** AUDIO THREAD. Mixes the backing track into `buffer`.

        Call this AFTER the analysis chain has consumed the mic input — the
        buffer is shared between input and output, so mixing first would feed
        the backing track straight into the pitch detector. */
    void renderInto (juce::AudioBuffer<float>& buffer) noexcept;

    const juce::String& getLoadedName() const noexcept { return loadedName; }

private:
    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread readAheadThread { "vt-backing-read" };

    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::ResamplingAudioSource>   resampler;
    juce::AudioTransportSource transport;

    // The reference vocal rides alongside on its own transport, driven by
    // exactly the same start/stop/setPosition calls.
    std::unique_ptr<juce::AudioFormatReaderSource> referenceSource;
    juce::AudioTransportSource referenceTransport;
    juce::AudioBuffer<float> referenceScratch;

    /** Guards every access to transport/readerSource. The audio thread only
        ever TRIES to take it. */
    juce::SpinLock sourceLock;

    juce::AudioBuffer<float> mixScratch;   // preallocated in prepare()

    /** THE ONE THAT STOPS THE CRASH. False between releaseResources() and the
        next prepare(), and renderInto() bails on it before anything else.

        juce::ResamplingAudioSource::releaseResources() sets its internal buffer
        to ZERO samples, and AudioTransportSource::getNextAudioBlock has NO
        guard for being pulled in that state — it runs straight into
        `bufferPos %= bufferSize` and takes an integer divide by zero, which on
        Windows is 0xC0000094 and kills the process instantly. `loaded` and
        `playing` do not cover this: both stay true across a device restart.
        One audio callback arriving in that window is all it takes, and a long
        CPU/GPU-saturating song import is exactly what provokes one.

        It also makes it safe for prepare() to reallocate the scratch buffers
        that renderInto() reads. */
    std::atomic<bool>   prepared        { false };

    std::atomic<bool>   loaded          { false };
    std::atomic<bool>   referenceLoaded { false };
    std::atomic<float>  referenceGain   { 0.0f };   // OFF by default, deliberately
    std::atomic<bool>   playing  { false };
    std::atomic<bool>   finished { false };
    std::atomic<double> positionSeconds { 0.0 };
    std::atomic<double> lengthSeconds   { 0.0 };

    double preparedRate = 0.0;
    int    preparedBlock = 0;
    juce::String loadedName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BackingPlayer)
};

} // namespace vt
