/*
  ==============================================================================

    SongImporter.h
    Vocal Trainer — M3.5

    Runs the offline `vocalchart add-song` pipeline as a child process and
    streams its progress back to the message thread.

    STANDALONE ONLY. The VST3 must never start this. Separation is a
    multi-minute GPU job; kicking one off from inside Live — which owns the
    audio device and expects the plugin to return from every call promptly —
    is a way to make Live look hung. Chart preparation is a before-you-play
    activity, not a during-the-song one.

    Talking to the child
    --------------------
    `--progress-json` switches the CLI's stdout to one JSON object per line
    (see python/vocalchart/progress.py). stdout and stderr are read on a single
    merged pipe, so lines arrive interleaved with tqdm noise; anything that
    does not parse as a JSON object with an "event" field is treated as log
    text rather than protocol. A dropped line costs one progress update.

    Cancelling
    ----------
    By writing a flag file the CLI polls, NOT by killing the process.
    Separation runs in a SECOND venv as a grandchild, and terminating the CLI
    from here would orphan it — a 2-minute GPU job with nothing left holding
    its handle. The CLI owns that handle, so the CLI does the killing.
    `ChildProcess::kill()` is only the backstop, after a grace period.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>   // AsyncUpdater - not in juce_core

#include <functional>

namespace vt
{

class SongImporter final : private juce::Thread,
                           private juce::AsyncUpdater
{
public:
    /** Where a run got to. Percent is 0-1 and is OVERALL, never per-stage. */
    struct Progress
    {
        double percent = 0.0;
        juce::String stage;
        juce::String message;
    };

    struct Result
    {
        bool ok = false;
        bool cancelled = false;
        juce::File chartFile;
        juce::String songId;
        juce::String message;      // one line, for the footer
        juce::String detail;       // captured output, for the log
        int notes = 0;
        int warnings = 0;
    };

    SongImporter();
    ~SongImporter() override;

    //== discovery =============================================================

    /** The venv interpreter that has `vocalchart` installed, or an invalid File.

        Looked for in this order:
          1. the path saved in importer.json (set by pickInterpreter)
          2. $VOCALCHART_PYTHON
          3. `.venv/Scripts/python.exe` in each directory from the executable
             upwards — which finds it for a build tree inside the repo. */
    static juce::File findInterpreter();

    /** Remember an interpreter chosen by hand. Empty clears it. */
    static void setInterpreter (const juce::File&);

    /** %APPDATA%/VocalTrainer/importer.json — sits beside theme.json. */
    static juce::File getSettingsFile();

    /** True when an interpreter exists AND `import vocalchart` succeeds in it.
        Runs the child synchronously; it returns in well under a second. */
    static bool checkInterpreter (const juce::File& python, juce::String& error);

    /** Extensions offered by the file chooser. Video containers are included
        because the CLI demuxes them with ffmpeg. */
    static juce::String getAudioFilePatterns();

    //== running ===============================================================

    /** Begin. Returns false and fills `error` if nothing was started. */
    bool start (const juce::File& audioFile, juce::String& error);

    /** Ask the run to stop. Returns immediately; onFinished still fires. */
    void cancel();

    bool isBusy() const noexcept { return isThreadRunning(); }

    /** The file being imported, for display. */
    juce::File getCurrentFile() const { return sourceFile; }

    /** Both are called on the MESSAGE THREAD. */
    std::function<void (Progress)> onProgress;
    std::function<void (Result)>   onFinished;

private:
    void run() override;
    void handleAsyncUpdate() override;
    void consume (const juce::String& line);

    static juce::File searchForInterpreterNear (const juce::File& start);

    juce::File sourceFile;
    juce::File cancelFile;
    juce::File interpreter;

    juce::CriticalSection lock;
    Progress pending;                  // guarded by `lock`
    Result   outcome;                  // guarded by `lock`
    bool     havePendingProgress = false;

    /** The child SAID how it went. Set from the done/error event. */
    bool outcomeRecorded = false;

    /** The result is ready to hand to onFinished. Set by run() only, as its
        last act, so a listener can start another import immediately. */
    bool haveOutcome = false;

    juce::StringArray tail;            // last few non-protocol lines, for errors

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongImporter)
};

} // namespace vt
