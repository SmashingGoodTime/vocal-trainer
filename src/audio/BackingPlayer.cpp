#include "BackingPlayer.h"

namespace vt
{

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
        referenceTransport.setSource (nullptr);
        resampler.reset();
        readerSource.reset();
        referenceSource.reset();
    }

    readAheadThread.stopThread (2000);
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
        readerSource = std::move (newSource);

        // AudioTransportSource resamples for us when told the file's native
        // rate, so a 44.1 kHz backing track plays correctly on a 48 kHz device.
        transport.setSource (readerSource.get(),
                             32768,                 // read-ahead buffer
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

bool BackingPlayer::loadReference (const juce::File& audioFile, juce::String& error)
{
    error.clear();

    if (! audioFile.existsAsFile())
    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        referenceTransport.setSource (nullptr);
        referenceSource.reset();
        referenceLoaded.store (false, std::memory_order_relaxed);
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));

    if (reader == nullptr)
    {
        error = "Unreadable reference vocal: " + audioFile.getFileName();
        referenceLoaded.store (false, std::memory_order_relaxed);
        return false;
    }

    const auto fileRate = reader->sampleRate;
    auto newSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);

        referenceTransport.setSource (nullptr);
        referenceSource = std::move (newSource);
        referenceTransport.setSource (referenceSource.get(), 32768, &readAheadThread, fileRate);

        if (preparedRate > 0.0)
            referenceTransport.prepareToPlay (preparedBlock, preparedRate);

        // Match wherever the backing track currently is, so enabling the guide
        // vocal mid-song does not start it from the top.
        referenceTransport.setPosition (transport.getCurrentPosition());

        if (playing.load (std::memory_order_relaxed))
            referenceTransport.start();
    }

    referenceLoaded.store (true, std::memory_order_relaxed);
    return true;
}

void BackingPlayer::unload()
{
    playing.store (false, std::memory_order_relaxed);
    loaded.store (false, std::memory_order_relaxed);

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        transport.stop();
        transport.setSource (nullptr);
        referenceTransport.stop();
        referenceTransport.setSource (nullptr);
        resampler.reset();
        readerSource.reset();
        referenceSource.reset();
    }

    referenceLoaded.store (false, std::memory_order_relaxed);

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
        mixScratch.setSize (2, juce::jmax (16, maxBlockSize), false, true, true);
        referenceScratch.setSize (2, juce::jmax (16, maxBlockSize), false, true, true);

        transport.prepareToPlay (maxBlockSize, sampleRate);
        referenceTransport.prepareToPlay (maxBlockSize, sampleRate);
    }

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
    referenceTransport.releaseResources();
}

//==============================================================================
void BackingPlayer::start()
{
    if (! loaded.load (std::memory_order_relaxed))
        return;

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);

        // Restart from the top if we ran off the end.
        if (transport.hasStreamFinished())
        {
            transport.setPosition (0.0);
            referenceTransport.setPosition (0.0);
        }

        transport.start();
        referenceTransport.start();
    }

    finished.store (false, std::memory_order_relaxed);
    playing.store (true, std::memory_order_relaxed);
}

void BackingPlayer::stop()
{
    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        transport.stop();
        referenceTransport.stop();
    }

    playing.store (false, std::memory_order_relaxed);
}

void BackingPlayer::setPosition (double seconds)
{
    const auto clamped = juce::jmax (0.0, seconds);

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        transport.setPosition (clamped);
        referenceTransport.setPosition (clamped);
    }

    positionSeconds.store (clamped, std::memory_order_relaxed);
    finished.store (false, std::memory_order_relaxed);
}

//==============================================================================
void BackingPlayer::renderInto (juce::AudioBuffer<float>& buffer) noexcept
{
    // `prepared` FIRST. It is the only one of the three that is false while the
    // transports hold released, zero-length resampler buffers; pulling one in
    // that state is an instant integer divide by zero deep inside JUCE.
    if (! prepared.load (std::memory_order_acquire)
        || ! loaded.load (std::memory_order_relaxed)
        || ! playing.load (std::memory_order_relaxed))
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

    juce::AudioSourceChannelInfo info (&mixScratch, 0, numSamples);
    transport.getNextAudioBlock (info);

    // Reference vocal, mixed in at its own gain.
    //
    // IMPORTANT: getNextAudioBlock is pulled EVERY block whenever a reference is
    // loaded, even at zero gain. Skipping the pull when muted would leave this
    // transport behind and it would drift out of sync with the backing track the
    // moment it was unmuted. Pull always; scale after.
    const auto refGain = referenceGain.load (std::memory_order_relaxed);

    if (referenceLoaded.load (std::memory_order_relaxed)
        && numSamples <= referenceScratch.getNumSamples())
    {
        referenceScratch.clear (0, numSamples);

        juce::AudioSourceChannelInfo refInfo (&referenceScratch, 0, numSamples);
        referenceTransport.getNextAudioBlock (refInfo);

        if (refGain > 0.0f)
            for (int ch = 0; ch < mixScratch.getNumChannels(); ++ch)
            {
                const auto srcCh = juce::jmin (ch, referenceScratch.getNumChannels() - 1);
                mixScratch.addFrom (ch, 0, referenceScratch, srcCh, 0, numSamples, refGain);
            }
    }

    // Mix on top of the mic pass-through. The analysis chain has already read
    // the input, so this cannot leak into the pitch detector.
    for (int ch = 0; ch < numChans; ++ch)
    {
        const auto srcCh = juce::jmin (ch, mixScratch.getNumChannels() - 1);
        buffer.addFrom (ch, 0, mixScratch, srcCh, 0, numSamples);
    }

    positionSeconds.store (transport.getCurrentPosition(), std::memory_order_relaxed);

    if (transport.hasStreamFinished())
    {
        finished.store (true, std::memory_order_relaxed);
        playing.store (false, std::memory_order_relaxed);
    }
}

} // namespace vt
