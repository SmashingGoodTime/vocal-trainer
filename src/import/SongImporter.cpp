/*
  ==============================================================================

    SongImporter.cpp
    Vocal Trainer — M3.5

  ==============================================================================
*/

#include "SongImporter.h"

namespace vt
{

namespace
{
    /** How long to wait for the CLI to notice the cancel file before killing it
        outright. It polls at stage boundaries and every 250 ms during
        separation, so this is generous. */
    constexpr int kCancelGraceMs = 8000;

    /** Non-protocol output kept for the error report. Enough to hold a Python
        traceback, not enough to hold a whole separation's worth of tqdm. */
    constexpr int kTailLines = 60;

    juce::String describeStage (const juce::String& stage)
    {
        // The two slow stages say so. A bar that sits still for two minutes is
        // only alarming if nothing told you it was going to.
        if (stage == "ingest")   return "Reading the file";
        if (stage == "separate") return "Separating the vocal from the backing"
                                        " - the slow one, a couple of minutes";
        if (stage == "f0")       return "Tracking the pitch of the vocal";
        if (stage == "notes")    return "Finding the notes";
        if (stage == "lyrics")   return "Transcribing the lyrics - this one takes a while too";
        if (stage == "chart")    return "Writing the chart";
        return stage;
    }
}

//==============================================================================
SongImporter::SongImporter()
    : juce::Thread ("vt-song-import")
{
}

SongImporter::~SongImporter()
{
    cancel();
    stopThread (kCancelGraceMs + 2000);
    cancelPendingUpdate();

    if (cancelFile.existsAsFile())
        cancelFile.deleteFile();
}

//==============================================================================
juce::File SongImporter::getSettingsFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("VocalTrainer")
              .getChildFile ("importer.json");
}

void SongImporter::setInterpreter (const juce::File& python)
{
    auto file = getSettingsFile();
    file.getParentDirectory().createDirectory();

    juce::DynamicObject::Ptr obj (new juce::DynamicObject());
    obj->setProperty ("pythonPath", python == juce::File() ? juce::String()
                                                           : python.getFullPathName());

    file.replaceWithText (juce::JSON::toString (juce::var (obj.get()), true));
}

juce::File SongImporter::searchForInterpreterNear (const juce::File& start)
{
    // Walk up from the executable. A Release build lives at
    // build/VocalTrainer_artefacts/Release/Standalone/, which is four levels
    // below the repo root, so eight is comfortable headroom without wandering
    // off into C:\.
    auto dir = start;

    for (int i = 0; i < 8 && dir != juce::File(); ++i)
    {
        const auto candidate = dir.getChildFile (".venv")
                                  .getChildFile ("Scripts")
                                  .getChildFile ("python.exe");

        if (candidate.existsAsFile())
            return candidate;

        const auto posix = dir.getChildFile (".venv").getChildFile ("bin").getChildFile ("python");

        if (posix.existsAsFile())
            return posix;

        const auto parent = dir.getParentDirectory();

        if (parent == dir)
            break;

        dir = parent;
    }

    return {};
}

juce::File SongImporter::findInterpreter()
{
    // 1. Explicitly chosen.
    if (auto settings = getSettingsFile(); settings.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (settings.loadFileAsString());

        if (auto* obj = parsed.getDynamicObject())
        {
            const auto saved = obj->getProperty ("pythonPath").toString();

            if (saved.isNotEmpty() && juce::File (saved).existsAsFile())
                return juce::File (saved);
        }
    }

    // 2. Environment.
    const auto fromEnv = juce::SystemStats::getEnvironmentVariable ("VOCALCHART_PYTHON", {});

    if (fromEnv.isNotEmpty() && juce::File (fromEnv).existsAsFile())
        return juce::File (fromEnv);

    // 3. The repo's own venv, found by walking up from the binary.
    return searchForInterpreterNear (
        juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
}

bool SongImporter::checkInterpreter (const juce::File& python, juce::String& error)
{
    if (python == juce::File() || ! python.existsAsFile())
    {
        error = "No Python interpreter found. Point it at the venv's python.exe.";
        return false;
    }

    juce::ChildProcess probe;
    juce::StringArray cmd { python.getFullPathName(), "-c", "import vocalchart" };

    if (! probe.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = "Could not run " + python.getFileName();
        return false;
    }

    const auto output = probe.readAllProcessOutput();

    if (! probe.waitForProcessToFinish (20000))
    {
        probe.kill();
        error = "Timed out checking " + python.getFileName();
        return false;
    }

    if (probe.getExitCode() != 0)
    {
        error = "'vocalchart' is not installed in " + python.getFullPathName()
                  + " - run: pip install -e python"
                  + (output.isNotEmpty() ? "\n" + output.trim() : juce::String());
        return false;
    }

    return true;
}

juce::String SongImporter::getAudioFilePatterns()
{
    return "*.wav;*.flac;*.mp3;*.m4a;*.aac;*.ogg;*.opus;*.wma;"
           "*.mp4;*.m4v;*.mkv;*.webm;*.mov;*.avi";
}

//==============================================================================
bool SongImporter::start (const juce::File& audioFile, juce::String& error)
{
    if (isThreadRunning())
    {
        error = "An import is already running.";
        return false;
    }

    if (! audioFile.existsAsFile())
    {
        error = "No such file: " + audioFile.getFullPathName();
        return false;
    }

    interpreter = findInterpreter();

    if (! checkInterpreter (interpreter, error))
        return false;

    sourceFile = audioFile;

    // In the temp directory rather than the song folder: the song folder does
    // not exist yet at this point, and a stray cancel file left in it by a
    // crash would silently abort every later run.
    cancelFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("vt-import-cancel-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30))
                                    + ".flag");
    cancelFile.deleteFile();

    {
        const juce::ScopedLock sl (lock);
        pending = {};
        outcome = {};
        havePendingProgress = false;
        haveOutcome = false;
        outcomeRecorded = false;
    }

    tail.clearQuick();

    startThread (juce::Thread::Priority::background);
    return true;
}

void SongImporter::cancel()
{
    if (! isThreadRunning())
        return;

    // Creating the file IS the cancel. See the header: the separator is a
    // grandchild in another venv and only the CLI can stop it cleanly.
    if (cancelFile != juce::File())
        cancelFile.replaceWithText ("cancel");

    signalThreadShouldExit();
}

//==============================================================================
void SongImporter::run()
{
    juce::StringArray cmd;
    cmd.add (interpreter.getFullPathName());
    cmd.add ("-m");
    cmd.add ("vocalchart.cli");
    cmd.add ("add-song");
    cmd.add (sourceFile.getFullPathName());
    cmd.add ("--progress-json");
    cmd.add ("--cancel-file");
    cmd.add (cancelFile.getFullPathName());

    juce::ChildProcess proc;

    if (! proc.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        const juce::ScopedLock sl (lock);
        outcome = {};
        outcome.message = "Could not start the chart pipeline.";
        haveOutcome = true;
        triggerAsyncUpdate();
        return;
    }

    juce::String buffer;
    juce::uint32 cancelledAtMs = 0;

    for (;;)
    {
        // Small buffer on purpose. JUCE's Windows read() keeps peeking until it
        // has filled the buffer or the child exits, so the size IS the update
        // latency: 4 KB would hold a whole stage's worth of progress lines back.
        char raw[256];
        const auto got = proc.readProcessOutput (raw, (int) sizeof (raw));

        if (got <= 0)
            break;   // read() only returns 0 once the child has exited AND the
                     // pipe is drained, so this is the end, not a lull.

        buffer += juce::String::fromUTF8 (raw, got);

        // Split on \n only. tqdm's \r redraws stay glued to the following line,
        // which is fine: they are log text, never protocol.
        for (;;)
        {
            const auto nl = buffer.indexOfChar ('\n');

            if (nl < 0)
                break;

            consume (buffer.substring (0, nl).trim());
            buffer = buffer.substring (nl + 1);
        }

        // The cancel file has been written; give the CLI a chance to act on it
        // itself, and only then take the hammer out.
        if (threadShouldExit())
        {
            const auto now = juce::Time::getMillisecondCounter();

            if (cancelledAtMs == 0)
                cancelledAtMs = now;
            else if (now - cancelledAtMs > (juce::uint32) kCancelGraceMs)
            {
                proc.kill();
                break;
            }
        }
    }

    if (buffer.trim().isNotEmpty())
        consume (buffer.trim());

    proc.waitForProcessToFinish (2000);

    const auto exitCode = proc.getExitCode();
    cancelFile.deleteFile();

    const juce::ScopedLock sl (lock);

    if (! outcomeRecorded)
    {
        // No done and no error event. Either the child died before it could say
        // anything, or it was killed. Report the captured text rather than a
        // bare exit code - that text is usually a Python traceback.
        outcome = {};
        outcome.message = "The chart pipeline stopped unexpectedly (exit "
                            + juce::String (exitCode) + ").";
        outcome.detail = tail.joinIntoString ("\n");
    }

    // Exit code 130 is the CLI's own "cancelled", and threadShouldExit() covers
    // the case where we killed it before it could say so. Deciding this here
    // rather than by pattern-matching the message keeps it out of the
    // translation surface.
    if (exitCode == 130 || threadShouldExit())
    {
        outcome.ok = false;
        outcome.cancelled = true;

        if (outcome.message.isEmpty() || ! outcomeRecorded)
            outcome.message = "Import cancelled.";
    }

    // Only NOW is the result published: run() is one statement from returning,
    // so onFinished can safely start another import.
    haveOutcome = true;
    triggerAsyncUpdate();
}

void SongImporter::consume (const juce::String& line)
{
    if (line.isEmpty())
        return;

    // Protocol lines are JSON objects. Everything else is log noise from the
    // child's stderr, kept only so a failure can show what happened.
    if (! line.startsWithChar ('{'))
    {
        tail.add (line);

        while (tail.size() > kTailLines)
            tail.remove (0);

        return;
    }

    const auto parsed = juce::JSON::parse (line);
    auto* obj = parsed.getDynamicObject();

    if (obj == nullptr || ! obj->hasProperty ("event"))
    {
        tail.add (line);
        return;
    }

    const auto event = obj->getProperty ("event").toString();

    // Each event type owns exactly one field. `progress` carries a message too,
    // but it is the raw stage name ("separate"), and `log` carries absolute
    // cache paths — letting either near the overlay replaces the one line the
    // user needs ("Separating vocals from the backing") with noise, several
    // times a second.
    if (event == "progress")
    {
        const juce::ScopedLock sl (lock);
        pending.percent = (double) obj->getProperty ("percent") / 100.0;
        havePendingProgress = true;
        triggerAsyncUpdate();
    }
    else if (event == "stage")
    {
        const juce::ScopedLock sl (lock);
        pending.stage = obj->getProperty ("name").toString();
        pending.message = describeStage (pending.stage);
        havePendingProgress = true;
        triggerAsyncUpdate();
    }
    else if (event == "log")
    {
        tail.add (obj->getProperty ("message").toString());

        while (tail.size() > kTailLines)
            tail.remove (0);
    }
    else if (event == "done")
    {
        // Recorded, NOT published. run() publishes once it is about to return,
        // so that onFinished cannot fire while this thread is still alive.
        const juce::ScopedLock sl (lock);
        outcome = {};
        outcome.ok = true;
        outcome.chartFile = juce::File (obj->getProperty ("chart").toString());
        outcome.songId = obj->getProperty ("songId").toString();
        outcome.notes = (int) obj->getProperty ("notes");
        outcome.warnings = (int) obj->getProperty ("warnings");
        outcome.message = "Imported " + outcome.songId + " - "
                            + juce::String (outcome.notes) + " notes";
        outcomeRecorded = true;
    }
    else if (event == "error")
    {
        const juce::ScopedLock sl (lock);
        outcome = {};
        outcome.ok = false;
        outcome.message = obj->getProperty ("message").toString();
        outcome.detail = obj->getProperty ("detail").toString();

        if (outcome.detail.isEmpty())
            outcome.detail = tail.joinIntoString ("\n");

        outcomeRecorded = true;
    }
}

//==============================================================================
void SongImporter::handleAsyncUpdate()
{
    Progress progressCopy;
    Result resultCopy;
    bool deliverProgress = false;
    bool deliverResult = false;

    {
        const juce::ScopedLock sl (lock);

        if (havePendingProgress)
        {
            progressCopy = pending;
            havePendingProgress = false;
            deliverProgress = true;
        }

        if (haveOutcome)
        {
            resultCopy = outcome;
            haveOutcome = false;
            deliverResult = true;
        }
    }

    // run() sets haveOutcome as its last act, so this returns immediately. It
    // exists so that onFinished can start another import without tripping the
    // "already running" check.
    if (deliverResult)
        waitForThreadToExit (2000);

    if (deliverProgress && onProgress != nullptr)
        onProgress (progressCopy);

    if (deliverResult && onFinished != nullptr)
        onFinished (resultCopy);
}

} // namespace vt
