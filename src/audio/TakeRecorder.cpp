#include "TakeRecorder.h"

namespace vt
{

namespace
{
    /** 24-bit rather than 16: a practice take is sung quietly as often as not,
        and the extra headroom means a take recorded with conservative gain is
        still worth listening to. Cheap at one channel. */
    constexpr int kBitsPerSample = 24;

    /** Samples buffered ahead of the writer thread. JUCE's own recording demo
        uses 32768; at one channel that is 0.7 s at 48 kHz, which is ample cover
        for a disk hiccup without making a dropout invisible. */
    constexpr int kWriterFifoSamples = 32768;
}

//==============================================================================
TakeRecorder::TakeRecorder()
{
    writerThread.startThread (juce::Thread::Priority::normal);
}

TakeRecorder::~TakeRecorder()
{
    stop();
    writerThread.stopThread (2000);
}

//==============================================================================
bool TakeRecorder::start (const juce::File& destination, double sampleRate,
                          double startChartSecondsIn, juce::String& error)
{
    error.clear();

    if (isRecording())
        stop();

    if (sampleRate <= 0.0)
    {
        error = "No audio device is running, so there is nothing to record.";
        return false;
    }

    if (! destination.getParentDirectory().createDirectory().wasOk())
    {
        error = "Could not create " + destination.getParentDirectory().getFullPathName();
        return false;
    }

    destination.deleteFile();

    // createOutputStream gives a FileOutputStream the writer takes ownership of.
    auto stream = destination.createOutputStream();

    if (stream == nullptr)
    {
        error = "Could not write to " + destination.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wav;

    // Mono: the same sum the analysis chain sees. See the header.
    //
    // The options-taking overload, not the deprecated positional one: JUCE 9
    // marks the old signature [[deprecated]], and it also takes ownership of the
    // stream through a unique_ptr& so there is no raw release() to get wrong.
    const auto options = juce::AudioFormatWriterOptions()
                            .withSampleRate (sampleRate)
                            .withNumChannels (1)
                            .withBitsPerSample (kBitsPerSample);

    std::unique_ptr<juce::OutputStream> asOutputStream (stream.release());
    auto writer = wav.createWriterFor (asOutputStream, options);

    if (writer == nullptr)
    {
        error = "Could not create a WAV writer for " + destination.getFileName();
        return false;
    }

    auto newThreadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
        writer.release(), writerThread, kWriterFifoSamples);

    destinationFile   = destination;
    startChartSeconds = juce::jmax (0.0, startChartSecondsIn);

    samplesWritten.store (0, std::memory_order_relaxed);
    sampleRateAtomic.store (sampleRate, std::memory_order_relaxed);
    dropout.store (false, std::memory_order_relaxed);

    {
        // Publish under the lock, so the audio thread cannot observe a
        // half-installed writer.
        const juce::SpinLock::ScopedLockType sl (writerLock);
        threadedWriter = std::move (newThreadedWriter);
        activeWriter.store (threadedWriter.get(), std::memory_order_release);
    }

    recording.store (true, std::memory_order_release);
    return true;
}

juce::File TakeRecorder::stop()
{
    recording.store (false, std::memory_order_release);

    {
        // Clear the pointer the audio thread reads BEFORE destroying the writer,
        // and do it under the lock so any write() already in progress has
        // finished. This ordering is the whole reason the lock exists.
        const juce::SpinLock::ScopedLockType sl (writerLock);
        activeWriter.store (nullptr, std::memory_order_release);
    }

    // Destroyed outside the lock: its destructor flushes to disk, which is file
    // I/O and must not happen while the audio thread might be spinning on the
    // lock behind it.
    threadedWriter.reset();

    if (samplesWritten.load (std::memory_order_relaxed) <= 0)
    {
        // Nothing captured — do not leave an empty WAV lying around pretending
        // to be a take.
        if (destinationFile.existsAsFile())
            destinationFile.deleteFile();

        return {};
    }

    return destinationFile;
}

//==============================================================================
void TakeRecorder::write (const float* mono, int numSamples) noexcept
{
    if (mono == nullptr || numSamples <= 0)
        return;

    if (! recording.load (std::memory_order_acquire))
        return;

    // TRY only, exactly as BackingPlayer::renderInto does. The message thread
    // holds this lock only while starting or stopping a take, so the only block
    // that can be dropped is one inside a deliberate start/stop.
    const juce::SpinLock::ScopedTryLockType sl (writerLock);

    if (! sl.isLocked())
        return;

    auto* writer = activeWriter.load (std::memory_order_acquire);

    if (writer == nullptr)
        return;

    const float* channels[1] = { mono };

    // write() returns false when its FIFO is full, i.e. the disk did not keep
    // up and these samples are LOST. Recording that fact is the difference
    // between a take the user knows has a hole in it and one they blame
    // themselves for.
    if (writer->write (channels, numSamples))
        samplesWritten.fetch_add (numSamples, std::memory_order_relaxed);
    else
        dropout.store (true, std::memory_order_relaxed);
}

//==============================================================================
juce::File TakeRecorder::sidecarFor (const juce::File& takeFile)
{
    return takeFile.withFileExtension ("json");
}

bool TakeRecorder::writeSidecar (const juce::File& takeFile,
                                 double startChartSeconds,
                                 const juce::String& songId,
                                 const juce::String& songTitle,
                                 float percentInTune,
                                 float meanCents,
                                 const juce::String& headline)
{
    if (! takeFile.existsAsFile())
        return false;

    juce::DynamicObject::Ptr obj (new juce::DynamicObject());
    obj->setProperty ("takeFile", takeFile.getFileName());
    obj->setProperty ("recordedAt", juce::Time::getCurrentTime().toISO8601 (true));
    obj->setProperty ("startChartSeconds", startChartSeconds);
    obj->setProperty ("songId", songId);
    obj->setProperty ("songTitle", songTitle);

    // Rounded on the way out: a sidecar is read by humans as often as by code,
    // and 0.8412673 tells nobody anything 84 does not.
    obj->setProperty ("percentInTune", juce::roundToInt (percentInTune * 100.0f));
    obj->setProperty ("meanCents", juce::roundToInt (meanCents));
    obj->setProperty ("summary", headline);

    return sidecarFor (takeFile).replaceWithText (juce::JSON::toString (juce::var (obj.get()), false, 4));
}

double TakeRecorder::readSidecarStartSeconds (const juce::File& takeFile)
{
    const auto sidecar = sidecarFor (takeFile);

    if (! sidecar.existsAsFile())
        return 0.0;

    const auto parsed = juce::JSON::parse (sidecar);

    if (auto* obj = parsed.getDynamicObject())
        if (obj->hasProperty ("startChartSeconds"))
            return juce::jmax (0.0, (double) obj->getProperty ("startChartSeconds"));

    return 0.0;
}

juce::String TakeRecorder::describeTake (const juce::File& takeFile)
{
    auto text = takeFile.getFileNameWithoutExtension();

    const auto sidecar = sidecarFor (takeFile);

    if (sidecar.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (sidecar);

        if (auto* obj = parsed.getDynamicObject())
            if (obj->hasProperty ("percentInTune"))
                text << "   -   " << (int) obj->getProperty ("percentInTune") << "% in tune";
    }

    return text;
}

} // namespace vt
