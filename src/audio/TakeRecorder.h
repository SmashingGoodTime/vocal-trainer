/*
    ==========================================================================

    TakeRecorder.h — writes the microphone to a WAV while you sing.

    DESIGN.md milestone 6: "take recording to disk so review survives a
    restart". Hearing yourself back is also the one form of feedback that
    cannot mislead — the numbers in the review screen are an interpretation,
    the recording is what actually came out of you.

    WHAT IS RECORDED, AND WHY IT IS MONO
    -----------------------------------
    The same mono sum the pitch detector analysed, tapped after the channel sum
    in processBlock. Recording the raw stereo input instead would give a take
    that is subtly not the signal the score was computed from, which makes
    "the review says I was flat here but it sounds fine" impossible to settle.
    One channel also halves the file, and a practice take is not a master.

    THREADING — the audio thread never blocks and never allocates
    ------------------------------------------------------------
    juce::AudioFormatWriter::ThreadedWriter exists for exactly this: write()
    hands samples to a preallocated lock-free FIFO, and a background
    TimeSliceThread does the file I/O.

    The one lock is taken with tryEnter, following BackingPlayer: the message
    thread holds it only while starting or stopping, so the only block that can
    ever be dropped is one inside a deliberate start/stop — a few milliseconds
    at the very edge of a take, which is silence anyway.

    A FIFO overrun (a disk that cannot keep up) is RECORDED AS A FLAG rather
    than ignored. A take with a silent hole in it that the app claims is fine
    would have the singer debugging their own performance.

    ==========================================================================
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

namespace vt
{

class TakeRecorder
{
public:
    TakeRecorder();
    ~TakeRecorder();

    //==========================================================================
    /** Message thread. Creates `destination` and starts recording.

        `startChartSeconds` is remembered only so it can be written into the
        sidecar on stop() — this class does not interpret it. */
    bool start (const juce::File& destination, double sampleRate,
                double startChartSeconds, juce::String& error);

    /** Message thread. Flushes and closes the file. Safe to call when not
        recording. Returns the finished file, or an invalid File if nothing was
        written. */
    juce::File stop();

    bool isRecording() const noexcept { return recording.load (std::memory_order_relaxed); }

    /** The file currently being written, or the last one finished. */
    juce::File getFile() const { return destinationFile; }

    double getRecordedSeconds() const noexcept
    {
        const auto rate = sampleRateAtomic.load (std::memory_order_relaxed);
        return rate > 0.0 ? (double) samplesWritten.load (std::memory_order_relaxed) / rate : 0.0;
    }

    double getStartChartSeconds() const noexcept { return startChartSeconds; }

    /** True if the writer FIFO overran at any point during the take, i.e. the
        file has a gap in it. */
    bool hadDropout() const noexcept { return dropout.load (std::memory_order_relaxed); }

    //==========================================================================
    /** AUDIO THREAD. `mono` is one channel of `numSamples` samples. */
    void write (const float* mono, int numSamples) noexcept;

    //==========================================================================
    /** Writes a small JSON sidecar next to a take describing when it started and
        how it scored, so a takes list can show something useful without
        re-analysing the audio. Called on the message thread after stop().

        A sidecar rather than WAV metadata chunks: it is readable, it is
        editable, and a take whose sidecar is missing still plays. */
    static bool writeSidecar (const juce::File& takeFile,
                              double startChartSeconds,
                              const juce::String& songId,
                              const juce::String& songTitle,
                              float percentInTune,
                              float meanCents,
                              const juce::String& headline);

    /** Reads the startChartSeconds back out of a take's sidecar, or 0 when
        there is not one. */
    static double readSidecarStartSeconds (const juce::File& takeFile);

    /** A one-line human description of a take for a menu, built from the
        sidecar when there is one: "take-20260805-142530  -  84% in tune". */
    static juce::String describeTake (const juce::File& takeFile);

private:
    static juce::File sidecarFor (const juce::File& takeFile);

    juce::TimeSliceThread writerThread { "vt-take-writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;

    /** Read on the audio thread, written on the message thread under the lock.
        Cleared BEFORE the writer is destroyed, which is what makes the
        destruction safe. */
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };

    juce::SpinLock writerLock;

    juce::File destinationFile;
    double startChartSeconds = 0.0;

    std::atomic<bool>    recording      { false };
    std::atomic<bool>    dropout        { false };
    std::atomic<int64_t> samplesWritten { 0 };
    std::atomic<double>  sampleRateAtomic { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeRecorder)
};

} // namespace vt
