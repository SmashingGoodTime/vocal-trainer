/*
    ==========================================================================

    AppSettings.h — the small amount of state that belongs to the INSTALL
    rather than to a song, a session or a theme.

    WHY A PropertiesFile AND NOT THE APVTS
    --------------------------------------
    DESIGN.md section 3 is explicit: only song.id goes into the engine's
    AudioProcessorValueTreeState; the library root path lives in a
    juce::PropertiesFile. Absolute paths do not belong in serialised session
    state — they break permanently the moment the library moves.

    So: anything that is a PATH, or is about this machine rather than about this
    session, lives here. Everything that belongs to the session itself stays in
    the APVTS. Everything a human tunes while singing stays in theme.json (see
    Theme.h) — this file is not a second theme, and nothing in it should ever
    need editing by hand.

    THREADING: message thread only, same as Theme. Obtain it with
    juce::SharedResourcePointer<vt::AppSettings> so every holder sees one
    instance and it dies with the last holder (no process-lifetime singleton,
    which would trip the JUCE leak detector).

    ==========================================================================
*/

#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace juce { class AudioProcessor; }

namespace vt
{

class AppSettings final
{
public:
    AppSettings();
    ~AppSettings();

    //==========================================================================
    /** Where the song folders live.

        RESOLUTION ORDER, and the reason for it:
          1. a root the user has explicitly chosen (setSongLibraryRoot)
          2. `songs/` in the repo, found by walking UP from the executable
          3. %APPDATA%/VocalTrainer/songs

        Step 2 is what makes "Load chart..." land somewhere useful with nothing
        configured and nothing hardcoded. It mirrors `_default_library()` in
        python/vocalchart/cli.py, which prefers the repo's own songs/ when it
        exists — so the CLI writes a song and the plugin looks for it in the
        same place, without either end naming a path.

        Always returns a directory that exists: the fallback is created. */
    juce::File getSongLibraryRoot() const;

    /** Overrides the search. Pass an invalid File to go back to auto-detection. */
    void setSongLibraryRoot (const juce::File&);

    /** Directory the chart chooser should open in: the last one a chart was
        actually loaded from, falling back to the library root. Sticky across
        restarts, because the second thing you do in a practice session is load
        a different song from the same folder as the first. */
    juce::File getLastChartDirectory() const;
    void setLastChartDirectory (const juce::File&);

    /** Directory the "Add song..." chooser should open in — where your MUSIC
        is, which is a different place from where charts are. */
    juce::File getLastAudioDirectory() const;
    void setLastAudioDirectory (const juce::File&);

    //== Recorded takes ========================================================
    /** takes/ inside the song's own folder, so a take travels with the song it
        belongs to. Falls back to %APPDATA%/VocalTrainer/takes/<songId> when the
        song folder is not writable (a read-only library on a NAS, say).
        Creates whichever it returns. */
    juce::File getTakesDirectory (const juce::File& songDirectory,
                                  const juce::String& songId) const;

    //== Live monitoring ======================================================
    /** Monitor level in dB. Persisted because it depends on the singer's
        headphones and interface gain, not on the song. */
    float getMonitorGainDb() const;
    void  setMonitorGainDb (float db);

    bool getMonitorEnabled() const;
    void setMonitorEnabled (bool);

    //== Latency compensation =================================================
    /** OUTPUT latency: how long audio takes to get from the buffer this app
        writes to the singer's EARS. Shifts the highway and the lyric strip back
        by this much, so what is on the now-line is what is in the headphones.

        INPUT latency: how long the singer's voice takes to get from their mouth
        into the buffer this app reads. Shifts the microphone trace and the
        scorer back by this much ON TOP of the output figure — a sung note is
        late by the whole ROUND TRIP, because the singer only started singing it
        after hearing a cue that was itself already late.

        Both default to ZERO, which is the correct value for the wired
        interface this app was built against and means an existing install
        behaves exactly as before. They exist for Bluetooth headphones, where
        the delay is 150-250 ms and no driver on Windows reports it.

        Milliseconds, and always >= 0: these are physical delays, so there is no
        such thing as compensating in the other direction. */
    int  getOutputLatencyMs() const;
    void setOutputLatencyMs (int ms);

    int  getInputLatencyMs() const;
    void setInputLatencyMs (int ms);

    /** The largest offset the UI offers. 500 ms is past the worst Bluetooth
        stack and far past the point where singing in time is possible at all;
        it is a guard against a typo, not a target. */
    static constexpr int kMaxLatencyMs = 500;

    //== Exercises ============================================================
    /** Remembered so the next warm-up starts where the last one left off. */
    int  getExerciseRootMidi() const;
    void setExerciseRootMidi (int midi);

    int  getExerciseBpm() const;
    void setExerciseBpm (int bpm);

    /** Roughly how long a generated exercise should run for, in seconds. A
        DURATION rather than a repeat count, because one repeat means five
        seconds of a five-note run and the better part of a minute of the
        interval drill — see ExerciseLibrary's length presets. */
    int  getExerciseLengthSeconds() const;
    void setExerciseLengthSeconds (int seconds);

    //==========================================================================
    /** Flushes to disk. juce::PropertiesFile autosaves on a timer as well, but
        an explicit save at a natural boundary means a crash mid-song does not
        lose the setting the user just changed. */
    void save();

    juce::File getSettingsFile() const;

    /** Walks up from the executable looking for a directory that looks like the
        repo's song library. Returns an invalid File if there is nothing
        convincing — deliberately conservative, because guessing WRONG here means
        the chooser opens somewhere baffling.

        Public and static so it can be tested directly: it is the step that makes
        "the chooser opens in the song folder" true without a path anywhere in the
        source, and it depends on where the executable happens to sit, which is
        not something a unit test of getSongLibraryRoot() could pin down. */
    static juce::File detectRepoSongsDirectory();

    static juce::File defaultLibraryRoot();

private:

    std::unique_ptr<juce::PropertiesFile> props;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppSettings)
};

} // namespace vt
