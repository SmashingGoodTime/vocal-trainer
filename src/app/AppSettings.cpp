#include "AppSettings.h"

// For the length presets only. Header-only use of a constant — this file gains
// no link dependency on the exercise generator, and the alternative is the same
// number written down in two places, which is how the two would drift.
#include "../exercise/ExerciseLibrary.h"

namespace vt
{

namespace
{
    // Keys are written into a file a user may one day open, so they are spelled
    // out rather than abbreviated.
    constexpr const char* kSongLibraryRoot   = "songLibraryRoot";
    constexpr const char* kLastChartDir      = "lastChartDirectory";
    constexpr const char* kLastAudioDir      = "lastAudioDirectory";
    constexpr const char* kMonitorGainDb     = "monitorGainDb";
    constexpr const char* kMonitorEnabled    = "monitorEnabled";
    constexpr const char* kExerciseRootMidi  = "exerciseRootMidi";
    constexpr const char* kExerciseBpm       = "exerciseBpm";
    constexpr const char* kExerciseLengthSec = "exerciseLengthSeconds";

    /** How far up from the executable to look for the repo. A Release build
        lands in build/VocalTrainer_artefacts/Release/Standalone/, which is four
        levels down; six gives room for a differently-nested build tree without
        wandering off into C:\. */
    constexpr int kMaxParentHops = 6;
}

//==============================================================================
AppSettings::AppSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName     = "VocalTrainer";
    options.filenameSuffix      = ".settings";
    options.folderName          = "VocalTrainer";
    options.osxLibrarySubFolder = "Application Support";

    // Autosave on a timer as well as on demand, so a setting survives a crash
    // that never reaches our own save().
    options.millisecondsBeforeSaving = 2000;
    options.storageFormat            = juce::PropertiesFile::storeAsXML;

    props = std::make_unique<juce::PropertiesFile> (options);
}

AppSettings::~AppSettings()
{
    save();
}

void AppSettings::save()
{
    if (props != nullptr)
        props->saveIfNeeded();
}

juce::File AppSettings::getSettingsFile() const
{
    return props != nullptr ? props->getFile() : juce::File();
}

//==============================================================================
juce::File AppSettings::defaultLibraryRoot()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("VocalTrainer")
              .getChildFile ("songs");
}

juce::File AppSettings::detectRepoSongsDirectory()
{
    // Walk up from the executable. `songs` alone is too weak a signal — plenty
    // of unrelated folders are called that — so require it to sit next to the
    // things that make this THIS repo.
    auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                  .getParentDirectory();

    for (int hop = 0; hop < kMaxParentHops && dir.isDirectory(); ++hop)
    {
        const auto songs = dir.getChildFile ("songs");

        const auto looksLikeTheRepo = dir.getChildFile ("CMakeLists.txt").existsAsFile()
                                   || dir.getChildFile ("docs").getChildFile ("DESIGN.md").existsAsFile()
                                   || dir.getChildFile ("python").getChildFile ("vocalchart").isDirectory();

        if (songs.isDirectory() && looksLikeTheRepo)
            return songs;

        const auto parent = dir.getParentDirectory();

        if (parent == dir)          // reached the volume root
            break;

        dir = parent;
    }

    return {};
}

juce::File AppSettings::getSongLibraryRoot() const
{
    // 1. An explicit choice always wins — but only while it still exists. A
    //    stale saved path (unplugged drive) must fall through to something
    //    usable rather than opening a chooser at a dead location.
    if (props != nullptr)
    {
        const auto saved = props->getValue (kSongLibraryRoot);

        if (saved.isNotEmpty())
        {
            const juce::File dir (saved);

            if (dir.isDirectory())
                return dir;
        }
    }

    // 2. The repo's own songs/, which is where the CLI puts them.
    if (const auto repo = detectRepoSongsDirectory(); repo.isDirectory())
        return repo;

    // 3. The documented library root. Created, so the chooser has somewhere to
    //    open even on a first run with no songs at all.
    auto fallback = defaultLibraryRoot();
    fallback.createDirectory();
    return fallback;
}

void AppSettings::setSongLibraryRoot (const juce::File& dir)
{
    if (props == nullptr)
        return;

    if (dir.isDirectory())
        props->setValue (kSongLibraryRoot, dir.getFullPathName());
    else
        props->removeValue (kSongLibraryRoot);      // back to auto-detection
}

//==============================================================================
juce::File AppSettings::getLastChartDirectory() const
{
    if (props != nullptr)
    {
        const auto saved = props->getValue (kLastChartDir);

        if (saved.isNotEmpty())
        {
            const juce::File dir (saved);

            if (dir.isDirectory())
                return dir;
        }
    }

    return getSongLibraryRoot();
}

void AppSettings::setLastChartDirectory (const juce::File& dir)
{
    if (props != nullptr && dir.isDirectory())
        props->setValue (kLastChartDir, dir.getFullPathName());
}

juce::File AppSettings::getLastAudioDirectory() const
{
    if (props != nullptr)
    {
        const auto saved = props->getValue (kLastAudioDir);

        if (saved.isNotEmpty())
        {
            const juce::File dir (saved);

            if (dir.isDirectory())
                return dir;
        }
    }

    return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

void AppSettings::setLastAudioDirectory (const juce::File& dir)
{
    if (props != nullptr && dir.isDirectory())
        props->setValue (kLastAudioDir, dir.getFullPathName());
}

//==============================================================================
juce::File AppSettings::getTakesDirectory (const juce::File& songDirectory,
                                           const juce::String& songId) const
{
    // Prefer the song's own folder: a take is only meaningful next to the chart
    // it was sung against, and keeping them together means copying a song
    // folder copies its history too.
    if (songDirectory.isDirectory() && songDirectory.hasWriteAccess())
    {
        auto takes = songDirectory.getChildFile ("takes");

        if (takes.createDirectory().wasOk())
            return takes;
    }

    const auto safeId = songId.isNotEmpty()
                          ? juce::File::createLegalFileName (songId)
                          : juce::String ("unsorted");

    auto fallback = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("VocalTrainer")
                       .getChildFile ("takes")
                       .getChildFile (safeId);

    fallback.createDirectory();
    return fallback;
}

//==============================================================================
float AppSettings::getMonitorGainDb() const
{
    if (props == nullptr)
        return -6.0f;

    // -6 dB rather than unity: the singer is already hearing themselves through
    // bone conduction and (on this rig) the Apollo Console, so monitoring here
    // is a supplement, not the whole signal.
    return juce::jlimit (-40.0f, 6.0f, (float) props->getDoubleValue (kMonitorGainDb, -6.0));
}

void AppSettings::setMonitorGainDb (float db)
{
    if (props != nullptr)
        props->setValue (kMonitorGainDb, (double) juce::jlimit (-40.0f, 6.0f, db));
}

bool AppSettings::getMonitorEnabled() const
{
    // OFF by default, and that default is load-bearing — see the long comment
    // on setMonitorMic() in AudioEngine.h.
    return props != nullptr && props->getBoolValue (kMonitorEnabled, false);
}

void AppSettings::setMonitorEnabled (bool shouldMonitor)
{
    if (props != nullptr)
        props->setValue (kMonitorEnabled, shouldMonitor);
}

//==============================================================================
int AppSettings::getExerciseRootMidi() const
{
    if (props == nullptr)
        return 60;

    return juce::jlimit (36, 84, props->getIntValue (kExerciseRootMidi, 60));
}

void AppSettings::setExerciseRootMidi (int midi)
{
    if (props != nullptr)
        props->setValue (kExerciseRootMidi, juce::jlimit (36, 84, midi));
}

int AppSettings::getExerciseBpm() const
{
    if (props == nullptr)
        return 72;

    return juce::jlimit (30, 200, props->getIntValue (kExerciseBpm, 72));
}

void AppSettings::setExerciseBpm (int bpm)
{
    if (props != nullptr)
        props->setValue (kExerciseBpm, juce::jlimit (30, 200, bpm));
}

int AppSettings::getExerciseLengthSeconds() const
{
    // Defaults to the medium preset rather than the shortest one: a warm-up you
    // finish before your voice has warmed up is not a warm-up.
    if (props == nullptr)
        return ExerciseLibrary::kMediumSeconds;

    return juce::jlimit (20, 600, props->getIntValue (kExerciseLengthSec,
                                                      ExerciseLibrary::kMediumSeconds));
}

void AppSettings::setExerciseLengthSeconds (int seconds)
{
    if (props != nullptr)
        props->setValue (kExerciseLengthSec, juce::jlimit (20, 600, seconds));
}

} // namespace vt
