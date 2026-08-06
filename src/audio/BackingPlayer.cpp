#include "BackingPlayer.h"

namespace vt
{

namespace
{
    /** Read-ahead buffer for the file-backed transports. */
    constexpr int kReadAheadSamples = 32768;
}

BackingPlayer::BackingPlayer()
{
    formatManager.registerBasicFormats();
    readAheadThread.startThread (juce::Thread::Priority::normal);
}

BackingPlayer::~BackingPlayer()
{
    prepared.store (false, std::memory_order_release);

    // Order matters: detach the source before the reader dies underneath it.
    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        transport.setSource (nullptr);
        referenceDeck.clear();
        takeDeck.clear();
        resampler.reset();
        readerSource.reset();
        memorySource.reset();
    }

    readAheadThread.stopThread (2000);
}

//==============================================================================
std::unique_ptr<juce::MemoryAudioSource> BackingPlayer::makeMemorySource (const juce::AudioBuffer<float>& buffer)
{
    // copyMemory = true. The caller's buffer is a local in the exercise builder
    // and will be gone before the first audio callback; a MemoryAudioSource that
    // merely referenced it would read freed memory.
    auto& mutableBuffer = const_cast<juce::AudioBuffer<float>&> (buffer);
    return std::make_unique<juce::MemoryAudioSource> (mutableBuffer, true, false);
}

//==============================================================================
bool BackingPlayer::load (const juce::File& audioFile, juce::String& error)
{
    error.clear();

    if (! audioFile.existsAsFile())
    {
        unload();
        error = "No backing audio at " + audioFile.getFullPathName();
        return false;
    }

    // createReaderFor returns nullptr for an unknown extension or a corrupt
    // file; both are user error, not a crash.
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));

    if (reader == nullptr)
    {
        unload();
        error = "Unsupported or unreadable audio file: " + audioFile.getFileName()
              + " (formats: " + formatManager.getWildcardForAllFormats() + ")";
        return false;
    }

    const auto fileRate = reader->sampleRate;
    const auto lengthSecs = fileRate > 0.0 ? (double) reader->lengthInSamples / fileRate : 0.0;

    // Built OUTSIDE the lock — this allocates and may touch the disk.
    auto newSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);

        transport.setSource (nullptr);
        resampler.reset();
        memorySource.reset();
        readerSource = std::move (newSource);

        // AudioTransportSource resamples for us when told the file's native
        // rate, so a 44.1 kHz backing track plays correctly on a 48 kHz device.
        transport.setSource (readerSource.get(),
                             kReadAheadSamples,
                             &readAheadThread,
                             fileRate);

        if (preparedRate > 0.0)
            transport.prepareToPlay (preparedBlock, preparedRate);

        transport.setPosition (0.0);
    }

    loadedName = audioFile.getFileName();
    lengthSeconds.store (lengthSecs, std::memory_order_relaxed);
    positionSeconds.store (0.0, std::memory_order_relaxed);
    finished.store (false, std::memory_order_relaxed);
    playing.store (false, std::memory_order_relaxed);
    loaded.store (true, std::memory_order_relaxed);

    return true;
}

bool BackingPlayer::loadFromBuffer (const juce::AudioBuffer<float>& buffer,
                                    double sourceSampleRate,
                                    const juce::String& name)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0 || sourceSampleRate <= 0.0)
        return false;

    auto newSource = makeMemorySource (buffer);
    const auto lengthSecs = (double) buffer.getNumSamples() / sourceSampleRate;

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);

        transport.setSource (nullptr);
        resampler.reset();
        readerSource.reset();
        memorySource = std::move (newSource);

        // No read-ahead thread: it is already in memory, so a background reader
        // would only add a thread hop and a buffer copy.
        transport.setSource (memorySource.get(), 0, nullptr, sourceSampleRate);

        if (preparedRate > 0.0)
            transport.prepareToPlay (preparedBlock, preparedRate);

        transport.setPosition (0.0);
    }

    loadedName = name;
    lengthSeconds.store (lengthSecs, std::memory_order_relaxed);
    positionSeconds.store (0.0, std::memory_order_relaxed);
    finished.store (false, std::memory_order_relaxed);
    playing.store (false, std::memory_order_relaxed);
    loaded.store (true, std::memory_order_relaxed);

    return true;
}

//==============================================================================
bool BackingPlayer::installDeck (Deck& deck,
                                 std::unique_ptr<juce::PositionableAudioSource> newSource,
                                 double sourceSampleRate,
                                 const juce::String& name,
                                 double lengthSecs,
                                 double startPositionSeconds)
{
    if (newSource == nullptr)
    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        deck.clear();
        return false;
    }

    const juce::SpinLock::ScopedLockType sl (sourceLock);

    deck.transport.setSource (nullptr);
    deck.source = std::move (newSource);

    // A file-backed deck gets the read-ahead thread; an in-memory one does not
    // need it (see loadFromBuffer).
    const auto isFileBacked = dynamic_cast<juce::AudioFormatReaderSource*> (deck.source.get()) != nullptr;

    deck.transport.setSource (deck.source.get(),
                              isFileBacked ? kReadAheadSamples : 0,
                              isFileBacked ? &readAheadThread : nullptr,
                              sourceSampleRate);

    if (preparedRate > 0.0)
        deck.transport.prepareToPlay (preparedBlock, preparedRate);

    deck.transport.setPosition (juce::jmax (0.0, startPositionSeconds));
    deck.name = name;
    deck.lengthSeconds = lengthSecs;
    deck.loaded.store (true, std::memory_order_relaxed);

    // Match whatever is already running, so enabling a deck mid-song does not
    // start it from the top.
    if (playing.load (std::memory_order_relaxed))
        deck.transport.start();

    return true;
}

bool BackingPlayer::loadReference (const juce::File& audioFile, juce::String& error)
{
    error.clear();

    if (! audioFile.existsAsFile())
    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        referenceDeck.clear();
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));

    if (reader == nullptr)
    {
        error = "Unreadable reference vocal: " + audioFile.getFileName();

        const juce::SpinLock::ScopedLockType sl (sourceLock);
        referenceDeck.clear();
        return false;
    }

    const auto fileRate   = reader->sampleRate;
    const auto lengthSecs = fileRate > 0.0 ? (double) reader->lengthInSamples / fileRate : 0.0;

    auto newSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

    // Line the guide up with wherever the backing track already is.
    const auto startAt = transport.getCurrentPosition();

    return installDeck (referenceDeck, std::move (newSource), fileRate,
                        audioFile.getFileName(), lengthSecs, startAt);
}

bool BackingPlayer::loadReferenceFromBuffer (const juce::AudioBuffer<float>& buffer,
                                             double sourceSampleRate,
                                             const juce::String& name)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0 || sourceSampleRate <= 0.0)
        return false;

    const auto lengthSecs = (double) buffer.getNumSamples() / sourceSampleRate;

    return installDeck (referenceDeck, makeMemorySource (buffer), sourceSampleRate,
                        name, lengthSecs, transport.getCurrentPosition());
}

bool BackingPlayer::loadTake (const juce::File& audioFile, double takeStartChartSeconds,
                              juce::String& error)
{
    error.clear();

    if (! audioFile.existsAsFile())
    {
        error = "No take at " + audioFile.getFullPathName();
        unloadTake();
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));

    if (reader == nullptr)
    {
        error = "Unreadable take: " + audioFile.getFileName();
        unloadTake();
        return false;
    }

    const auto fileRate   = reader->sampleRate;
    const auto lengthSecs = fileRate > 0.0 ? (double) reader->lengthInSamples / fileRate : 0.0;

    auto newSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

    takeOffsetSeconds = juce::jmax (0.0, takeStartChartSeconds);

    const auto startAt = juce::jmax (0.0, transport.getCurrentPosition() - takeOffsetSeconds);

    return installDeck (takeDeck, std::move (newSource), fileRate,
                        audioFile.getFileName(), lengthSecs, startAt);
}

void BackingPlayer::unloadTake()
{
    const juce::SpinLock::ScopedLockType sl (sourceLock);
    takeDeck.clear();
    takeOffsetSeconds = 0.0;
}

void BackingPlayer::unload()
{
    playing.store (false, std::memory_order_relaxed);
    loaded.store (false, std::memory_order_relaxed);

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        transport.stop();
        transport.setSource (nullptr);
        referenceDeck.clear();
        takeDeck.clear();
        resampler.reset();
        readerSource.reset();
        memorySource.reset();
    }

    takeOffsetSeconds = 0.0;
    loadedName.clear();
    positionSeconds.store (0.0, std::memory_order_relaxed);
    lengthSeconds.store (0.0, std::memory_order_relaxed);
    finished.store (false, std::memory_order_relaxed);
}

//==============================================================================
void BackingPlayer::prepare (double sampleRate, int maxBlockSize)
{
    // Clear the flag BEFORE taking the lock. renderInto() checks it before it
    // even tries the lock, so clearing it first and then acquiring the lock
    // guarantees no render is in flight and none can start — which is what
    // makes it safe to REALLOCATE mixScratch below. Resizing those buffers
    // outside the lock, as this used to, could pull the memory out from under
    // a render that was already running.
    prepared.store (false, std::memory_order_release);

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);

        preparedRate  = sampleRate;
        preparedBlock = maxBlockSize;

        // Preallocated so renderInto() never allocates.
        const auto scratchSize = juce::jmax (16, maxBlockSize);
        mixScratch.setSize (2, scratchSize, false, true, true);
        referenceDeck.scratch.setSize (2, scratchSize, false, true, true);
        takeDeck.scratch.setSize (2, scratchSize, false, true, true);

        transport.prepareToPlay (maxBlockSize, sampleRate);
        referenceDeck.transport.prepareToPlay (maxBlockSize, sampleRate);
        takeDeck.transport.prepareToPlay (maxBlockSize, sampleRate);
    }

    preparedRateAtomic.store (sampleRate, std::memory_order_relaxed);
    prepared.store (true, std::memory_order_release);
}

void BackingPlayer::releaseResources()
{
    // See the comment on `prepared` in the header. This store is the thing that
    // prevents an integer divide by zero inside JUCE's resampler: after
    // releaseResources() its buffer is zero samples long, and nothing in
    // AudioTransportSource stops the audio thread pulling it anyway.
    prepared.store (false, std::memory_order_release);

    const juce::SpinLock::ScopedLockType sl (sourceLock);
    transport.releaseResources();
    referenceDeck.transport.releaseResources();
    takeDeck.transport.releaseResources();
}

//==============================================================================
void BackingPlayer::start()
{
    // Any deck is enough: an a cappella chart with a recorded take has audio to
    // play even though there is no backing track.
    if (! hasAnyAudio())
        return;

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);

        // Restart from the top if we ran off the end.
        if (transport.hasStreamFinished())
        {
            transport.setPosition (0.0);
            referenceDeck.transport.setPosition (0.0);
            takeDeck.transport.setPosition (0.0);
        }

        transport.start();
        referenceDeck.transport.start();
        takeDeck.transport.start();
    }

    finished.store (false, std::memory_order_relaxed);
    playing.store (true, std::memory_order_relaxed);
}

void BackingPlayer::stop()
{
    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        transport.stop();
        referenceDeck.transport.stop();
        takeDeck.transport.stop();
    }

    playing.store (false, std::memory_order_relaxed);
}

void BackingPlayer::setPosition (double seconds)
{
    const auto clamped = juce::jmax (0.0, seconds);

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        transport.setPosition (clamped);
        referenceDeck.transport.setPosition (clamped);

        // The take is offset by the chart time it started recording at.
        takeDeck.transport.setPosition (juce::jmax (0.0, clamped - takeOffsetSeconds));
    }

    positionSeconds.store (clamped, std::memory_order_relaxed);
    finished.store (false, std::memory_order_relaxed);
}

//==============================================================================
void BackingPlayer::renderDeck (Deck& deck, int numSamples) noexcept
{
    if (! deck.loaded.load (std::memory_order_relaxed)
        || numSamples > deck.scratch.getNumSamples())
        return;

    // IMPORTANT: getNextAudioBlock is pulled EVERY block whenever a deck is
    // loaded, even at zero gain. Skipping the pull when muted would leave that
    // transport behind, and it would drift out of sync with the backing track
    // the moment it was unmuted.  Pull always; scale after.
    deck.scratch.clear (0, numSamples);

    juce::AudioSourceChannelInfo info (&deck.scratch, 0, numSamples);
    deck.transport.getNextAudioBlock (info);

    const auto gain = deck.getGain();

    if (gain <= 0.0f)
        return;

    for (int ch = 0; ch < mixScratch.getNumChannels(); ++ch)
    {
        const auto srcCh = juce::jmin (ch, deck.scratch.getNumChannels() - 1);
        mixScratch.addFrom (ch, 0, deck.scratch, srcCh, 0, numSamples, gain);
    }
}

void BackingPlayer::renderInto (juce::AudioBuffer<float>& buffer) noexcept
{
    // `prepared` FIRST. It is the only one of the flags that is false while the
    // transports hold released, zero-length resampler buffers; pulling one in
    // that state is an instant integer divide by zero deep inside JUCE.
    if (! prepared.load (std::memory_order_acquire)
        || ! playing.load (std::memory_order_relaxed))
        return;

    const auto backingLoaded = loaded.load (std::memory_order_relaxed);

    if (! backingLoaded
        && ! referenceDeck.loaded.load (std::memory_order_relaxed)
        && ! takeDeck.loaded.load (std::memory_order_relaxed))
        return;

    // TRY only. A load is in flight; render silence this block rather than
    // block the audio thread. One dropped block during a song load is inaudible.
    const juce::SpinLock::ScopedTryLockType sl (sourceLock);

    if (! sl.isLocked())
        return;

    const auto numSamples = buffer.getNumSamples();
    const auto numChans   = buffer.getNumChannels();

    if (numSamples <= 0 || numChans <= 0 || numSamples > mixScratch.getNumSamples())
        return;

    mixScratch.clear (0, numSamples);

    if (backingLoaded)
    {
        juce::AudioSourceChannelInfo info (&mixScratch, 0, numSamples);
        transport.getNextAudioBlock (info);
    }

    renderDeck (referenceDeck, numSamples);
    renderDeck (takeDeck, numSamples);

    // Mix on top of the mic pass-through. The analysis chain has already read
    // the input, so this cannot leak into the pitch detector.
    for (int ch = 0; ch < numChans; ++ch)
    {
        const auto srcCh = juce::jmin (ch, mixScratch.getNumChannels() - 1);
        buffer.addFrom (ch, 0, mixScratch, srcCh, 0, numSamples);
    }

    //-------------------------------------------------------------------------
    // WHICH DECK OWNS THE CLOCK.
    //
    // The backing track whenever there is one — the singer syncs to what they
    // hear, and that is the track. Otherwise the take, so that listening back
    // to an a cappella practice still moves the highway. Never the guide: it is
    // an optional overlay that can be loaded and unloaded mid-song, and a clock
    // that stops existing when you untick a checkbox is not a clock.
    //-------------------------------------------------------------------------
    if (backingLoaded)
    {
        positionSeconds.store (transport.getCurrentPosition(), std::memory_order_relaxed);

        if (transport.hasStreamFinished())
        {
            finished.store (true, std::memory_order_relaxed);
            playing.store (false, std::memory_order_relaxed);
        }
    }
    else if (takeDeck.loaded.load (std::memory_order_relaxed))
    {
        positionSeconds.store (takeDeck.transport.getCurrentPosition() + takeOffsetSeconds,
                               std::memory_order_relaxed);

        if (takeDeck.transport.hasStreamFinished())
        {
            finished.store (true, std::memory_order_relaxed);
            playing.store (false, std::memory_order_relaxed);
        }
    }
}

} // namespace vt
