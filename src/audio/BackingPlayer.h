/*
    ==========================================================================

    BackingPlayer.h — plays the song's backing track.

    WHY THIS IS THE CLOCK
    ---------------------
    When a backing track is loaded it becomes the master clock for chart time,
    replacing the audio-stream clock. The reason is simple: the singer syncs to
    what they HEAR. If the highway ran off a different clock than the audio,
    any drift between them would show up as the chart sliding against the
    music, which is exactly the failure the whole timestamping design exists to
    avoid.

    THREE DECKS, ONE TRANSPORT MODEL
    --------------------------------
    Backing, guide vocal (the reference the chart was analysed from), and the
    singer's own recorded take. They are three separate decks rather than one
    pre-mixed stream because each has to be silenceable on its own:

      - the GUIDE must be silenceable or it teaches the singer to follow rather
        than to pitch (DESIGN.md section 6);
      - the TAKE must be silenceable because listening back to yourself over
        the backing track is a different activity from singing along to it;
      - and for a generated exercise the two extra decks carry the metronome
        and the pitch reference, which is what lets an exercise reuse this
        class unchanged instead of needing its own player.

    All decks are started, stopped and seeked by the same calls and pull the
    same number of samples per block, so they stay sample-locked. The backing
    deck remains the clock whenever it is loaded.

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

    /** Loads the backing deck from a buffer already in memory, for audio this
        app generated rather than read — the metronome of a generated exercise.

        Writing it to a temporary WAV and reading it back would work too, and
        would be worse: it puts disk I/O and a file lifetime problem in the path
        of a button that should feel instant.

        `sourceSampleRate` is the rate the buffer was rendered at. Pass
        getPreparedSampleRate() to avoid any resampling at all. */
    bool loadFromBuffer (const juce::AudioBuffer<float>& buffer,
                         double sourceSampleRate,
                         const juce::String& name);

    /** Loads the REFERENCE VOCAL — the recorded singer, for listening to.

        Muted by default, deliberately: a guide vocal you cannot silence teaches
        you to follow rather than to pitch, and the pedagogy in DESIGN.md
        section 6 depends on being able to take the crutch away. */
    bool loadReference (const juce::File& audioFile, juce::String& error);

    /** The reference deck from memory — the pitch guide tone of a generated
        exercise, which is exactly a reference: silenceable, and off the moment
        the singer wants to test themselves. */
    bool loadReferenceFromBuffer (const juce::AudioBuffer<float>& buffer,
                                  double sourceSampleRate,
                                  const juce::String& name);

    /** Loads a recorded take for listen-back.

        `takeStartChartSeconds` is the chart time the recording began at, so the
        take can be lined up against the chart it was sung to. Seeking to a chart
        position BEFORE that offset plays the take from its own beginning
        (juce::AudioTransportSource clamps at zero) — harmless, and the normal
        flow of "record from the top" has an offset of zero anyway. */
    bool loadTake (const juce::File& audioFile, double takeStartChartSeconds,
                   juce::String& error);

    void unloadTake();

    void unload();

    bool hasTrack() const noexcept     { return loaded.load (std::memory_order_relaxed); }
    bool hasReference() const noexcept { return referenceDeck.loaded.load (std::memory_order_relaxed); }
    bool hasTake() const noexcept      { return takeDeck.loaded.load (std::memory_order_relaxed); }

    /** True when there is anything at all to play — the condition start() needs,
        since an a cappella chart with only a recorded take still has audio. */
    bool hasAnyAudio() const noexcept
    {
        return hasTrack() || hasReference() || hasTake();
    }

    /** True when getPositionSeconds() is a real transport position that chart
        time should be driven from, rather than a parked zero.

        The caller cannot just ask hasTrack(): listening back to an a cappella
        take has no backing track but does have an authoritative clock, and
        running the highway off the audio-stream clock in that case would let
        the take drift against the notes it is being compared to. */
    bool ownsClock() const noexcept { return hasTrack() || hasTake(); }

    /** Linear gain for the reference vocal. 0 == off (the default). */
    void setReferenceGain (float g) noexcept { referenceDeck.setGain (g); }
    float getReferenceGain() const noexcept  { return referenceDeck.getGain(); }

    /** Linear gain for the recorded take. 0 == off (the default), so loading a
        take never starts playing it at you. */
    void setTakeGain (float g) noexcept { takeDeck.setGain (g); }
    float getTakeGain() const noexcept  { return takeDeck.getGain(); }

    const juce::String& getTakeName() const noexcept { return takeDeck.name; }
    double getTakeLengthSeconds() const noexcept     { return takeDeck.lengthSeconds; }
    double getTakeStartChartSeconds() const noexcept { return takeOffsetSeconds; }

    /** Message thread. Safe to call before prepare(). */
    void prepare (double sampleRate, int maxBlockSize);
    void releaseResources();

    /** The rate the decks are prepared at, or 0 before the first prepare().
        Generated audio should be rendered at this rate so nothing resamples. */
    double getPreparedSampleRate() const noexcept
    {
        return preparedRateAtomic.load (std::memory_order_relaxed);
    }

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
    /** AUDIO THREAD. Mixes every loaded deck into `buffer`.

        Call this AFTER the analysis chain has consumed the mic input — the
        buffer is shared between input and output, so mixing first would feed
        the backing track straight into the pitch detector. */
    void renderInto (juce::AudioBuffer<float>& buffer) noexcept;

    const juce::String& getLoadedName() const noexcept { return loadedName; }

private:
    //==========================================================================
    /** One auxiliary deck: a positionable source, its transport, its own gain,
        and a preallocated scratch buffer to pull into.

        The reference and the take are structurally identical, so they share this
        rather than existing as two near-copies of the same six members — which
        is how the second one would drift out of sync with the first. */
    struct Deck
    {
        std::unique_ptr<juce::PositionableAudioSource> source;
        juce::AudioTransportSource transport;
        juce::AudioBuffer<float>   scratch;

        std::atomic<bool>  loaded { false };
        std::atomic<float> gain   { 0.0f };     // OFF by default, deliberately

        juce::String name;
        double lengthSeconds = 0.0;

        void setGain (float g) noexcept
        {
            gain.store (juce::jlimit (0.0f, 2.0f, g), std::memory_order_relaxed);
        }

        float getGain() const noexcept { return gain.load (std::memory_order_relaxed); }

        /** Caller must hold the source lock. */
        void clear()
        {
            transport.stop();
            transport.setSource (nullptr);
            source.reset();
            loaded.store (false, std::memory_order_relaxed);
            name.clear();
            lengthSeconds = 0.0;
        }
    };

    /** Both take the source lock; called on the message thread only. */
    bool installDeck (Deck&, std::unique_ptr<juce::PositionableAudioSource>,
                      double sourceSampleRate, const juce::String& name,
                      double lengthSeconds, double startPositionSeconds);

    /** AUDIO THREAD. Pulls a deck and mixes it into mixScratch at its gain.
        Pulled every block even when silent — see the comment in the .cpp. */
    void renderDeck (Deck&, int numSamples) noexcept;

    static std::unique_ptr<juce::MemoryAudioSource> makeMemorySource (const juce::AudioBuffer<float>&);

    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread readAheadThread { "vt-backing-read" };

    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::MemoryAudioSource>       memorySource;   // generated backing
    std::unique_ptr<juce::ResamplingAudioSource>   resampler;
    juce::AudioTransportSource transport;

    Deck referenceDeck;
    Deck takeDeck;

    /** Chart time the loaded take began at. See loadTake(). */
    double takeOffsetSeconds = 0.0;

    /** Guards every access to transport/readerSource and both decks. The audio
        thread only ever TRIES to take it. */
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
    std::atomic<bool>   playing  { false };
    std::atomic<bool>   finished { false };
    std::atomic<double> positionSeconds { 0.0 };
    std::atomic<double> lengthSeconds   { 0.0 };

    /** Published so generated audio can be rendered at the device rate. The
        non-atomic preparedRate below is only touched under the lock. */
    std::atomic<double> preparedRateAtomic { 0.0 };

    double preparedRate = 0.0;
    int    preparedBlock = 0;
    juce::String loadedName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BackingPlayer)
};

} // namespace vt
