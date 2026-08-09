#==============================================================================
# Vocal Trainer — build configuration
#
# See docs/DESIGN.md section 5 "Build configuration" for the rationale behind
# every pinned value in this file. Nothing here is incidental.
#
# Verified against the actual JUCE 9.0.0 clone in ./JUCE
# (docs/CMake API.md + extras/Build/CMake/JUCEUtils.cmake).
#==============================================================================

cmake_minimum_required(VERSION 3.22)

project(VocalTrainer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)          # JUCE 9 baseline is C++17; 20 costs nothing on MSVC 2022
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime, so the app runs on a machine with no VC++ redistributable
# installed. Must be set BEFORE add_subdirectory(JUCE) so JUCE's own targets pick
# it up too. CMP0091 is NEW by default at cmake_minimum_required 3.22, so this is
# honoured.
if (WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

#------------------------------------------------------------------------------
# Options
#------------------------------------------------------------------------------

# ASIO is OFF by default so the very first build on a clean machine cannot fail
# on SDK problems. Turning it ON gives low-latency access to the Apollo Twin X.
#
# LICENSING: the ASIO SDK is GPLv3-or-Steinberg-proprietary. JUCE 9 bundles it
# (modules/juce_audio_devices/native/asio/iasiodrv.h), so no external download is
# needed, but it MUST stay OFF for any closed-source distribution. See LICENSING.md.
option(VT_ENABLE_ASIO "Compile ASIO support into the app (see LICENSING.md)" OFF)

option(VT_BUILD_TESTS "Build the vt_tests console test runner" ON)

#------------------------------------------------------------------------------
# JUCE — plain directory at ./JUCE, NOT a git submodule
#------------------------------------------------------------------------------

add_subdirectory(JUCE)

#------------------------------------------------------------------------------
# The app
#
# STANDALONE ONLY. juce_add_plugin with FORMATS Standalone is how JUCE builds a
# standalone audio application: the wrapper supplies the Audio/MIDI Settings
# dialog, remembers the chosen device between runs, and drives the engine's
# processBlock from the device callback. There is no plugin build — nothing here
# is ever loaded by a host.
#------------------------------------------------------------------------------

juce_add_plugin(VocalTrainer
    COMPANY_NAME                "SmashingGoodTime"

    # PIN THESE. PLUGIN_CODE otherwise defaults to a random value that changes on
    # EVERY configure, and the standalone wrapper derives the name of its saved
    # audio-device settings file from it — so a drifting code silently resets the
    # user's device choice on every reconfigure.
    PLUGIN_MANUFACTURER_CODE    Sgtm
    PLUGIN_CODE                 Vtr1

    FORMATS                     Standalone
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE

    PRODUCT_NAME                "Vocal Trainer")

target_sources(VocalTrainer PRIVATE
    src/AudioEngine.cpp
    src/MainComponent.cpp
    src/app/AppSettings.cpp
    src/dsp/AnalysisChain.cpp
    src/audio/BackingPlayer.cpp
    src/audio/ReferenceTone.cpp
    src/audio/TakeRecorder.cpp
    src/chart/Chart.cpp
    src/chart/ChartWriter.cpp
    src/exercise/ExerciseLibrary.cpp
    src/exercise/ExerciseAudio.cpp
    src/score/TakeScorer.cpp
    src/import/SongImporter.cpp
    src/ui/Theme.cpp
    src/ui/PitchLaneComponent.cpp
    src/ui/NoteHighwayComponent.cpp
    src/ui/PianoKeyboardComponent.cpp
    src/ui/TransportBarComponent.cpp
    src/ui/ImportOverlayComponent.cpp
    src/ui/LyricStripComponent.cpp
    src/ui/ReviewOverlayComponent.cpp
    src/ui/ChartEditPanelComponent.cpp)

target_include_directories(VocalTrainer PRIVATE
    src
    # JUCE's module root, so the shared code can reach
    # <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>.
    #
    # It is needed for exactly one thing: switching OFF the standalone wrapper's
    # "Mute audio input" checkbox. That control swaps an EMPTY BUFFER for the
    # microphone before the engine runs, which for an analyser kills the pitch
    # display along with the sound. juce_add_plugin puts this module on the
    # FORMAT target only, not on the shared-code target that compiles
    # MainComponent.cpp — so REMOVING THIS LINE BREAKS THE BUILD, which is
    # deliberate: the include there is hard rather than guarded precisely so this
    # cannot regress silently again.
    ${CMAKE_CURRENT_SOURCE_DIR}/JUCE/modules)

target_compile_definitions(VocalTrainer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_ASIO=$<BOOL:${VT_ENABLE_ASIO}>)

# NOTE (deviation from the brief, deliberate):
#   JUCE_DISPLAY_SPLASH_SCREEN and JUCE_REPORT_APP_USAGE are NOT set.
#   JUCE 8 removed the splash screen entirely; JUCE 9's juce_gui_basics.cpp now
#   emits a compiler warning if JUCE_DISPLAY_SPLASH_SCREEN is defined at all
#   ("This version of JUCE does not use the splash screen, the flag ... is ignored").
#   JUCE_REPORT_APP_USAGE no longer exists anywhere in the JUCE 9 source tree.
#   Defining either would add build noise for zero effect. There is no splash
#   screen and no usage reporting in this build.

target_link_libraries(VocalTrainer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

#------------------------------------------------------------------------------
# Tests — a plain console app, NOT a JUCE plugin target
#------------------------------------------------------------------------------

if (VT_BUILD_TESTS)
    # Register every runner with CTest, so `ctest` aggregates all five instead
    # of reporting "No tests were found!!!" while 190 passing checks sit unrun.
    # Each runner already exits non-zero on its first failing check, which is
    # all CTest needs. The vt_backing_tests crash-not-fail behaviour (see below)
    # surfaces as a failed test with exit code 0xC0000094.
    enable_testing()

    juce_add_console_app(vt_tests
        PRODUCT_NAME "vt_tests")

    # AnalysisChain.cpp is compiled into the test runner too — test_pitch.cpp
    # exercises the real front end (HPF -> anti-alias -> 16 kHz resample -> chunking),
    # not a reimplementation of it.
    target_sources(vt_tests PRIVATE
        tests/test_pitch.cpp
        src/dsp/AnalysisChain.cpp)

    target_include_directories(vt_tests PRIVATE src)

    # Chart tests are a SECOND executable rather than more files in vt_tests,
    # because each console app owns a main(). Chart.cpp needs only juce_core,
    # so this stays fast to build.
    juce_add_console_app(vt_chart_tests
        PRODUCT_NAME "vt_chart_tests")

    target_sources(vt_chart_tests PRIVATE
        tests/test_chart.cpp
        src/chart/Chart.cpp
        src/chart/ChartWriter.cpp)

    target_include_directories(vt_chart_tests PRIVATE src)

    target_compile_definitions(vt_chart_tests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

    target_link_libraries(vt_chart_tests
        PRIVATE
            juce::juce_core
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # No JuceHeader.h is generated. Test code must include module headers
    # directly, e.g. #include <juce_dsp/juce_dsp.h>.
    target_compile_definitions(vt_tests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

    target_link_libraries(vt_tests
        PRIVATE
            juce::juce_dsp
            juce::juce_core
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Scoring and exercise generation. Both are pure juce_core code, so this is
    # the cheapest target in the project to build and the one that catches the
    # two most expensive-to-diagnose classes of bug: a scoring change that
    # punishes vibrato (which reads as "I sang badly", not as a bug), and a
    # generated exercise with a wrong interval in it (which looks like a
    # perfectly plausible scale on the highway).
    juce_add_console_app(vt_score_tests
        PRODUCT_NAME "vt_score_tests")

    target_sources(vt_score_tests PRIVATE
        tests/test_score.cpp
        src/chart/Chart.cpp
        src/exercise/ExerciseLibrary.cpp
        src/score/TakeScorer.cpp)

    target_include_directories(vt_score_tests PRIVATE src)

    target_compile_definitions(vt_score_tests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

    target_link_libraries(vt_score_tests
        PRIVATE
            juce::juce_core
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # BackingPlayer lifecycle. Drives the exact sequence that crashed the app
    # with 0xC0000094 on 2026-08-04: play, releaseResources(), then one more
    # audio callback. WITHOUT the fix this target does not fail, it CRASHES —
    # which is the correct signal for a divide-by-zero regression.
    juce_add_console_app(vt_backing_tests
        PRODUCT_NAME "vt_backing_tests")

    # The exercise renderers ride along here rather than in vt_score_tests
    # because they produce AUDIO, and juce_core alone cannot hold a buffer of it.
    # What is being tested is not the arithmetic but which DECK each sound lands
    # on — a decision that is silent when wrong.
    target_sources(vt_backing_tests PRIVATE
        tests/test_backing.cpp
        src/audio/BackingPlayer.cpp
        src/chart/Chart.cpp
        src/exercise/ExerciseAudio.cpp
        src/exercise/ExerciseLibrary.cpp)

    target_include_directories(vt_backing_tests PRIVATE src)

    target_compile_definitions(vt_backing_tests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

    target_link_libraries(vt_backing_tests
        PRIVATE
            juce::juce_audio_devices
            juce::juce_audio_formats
            juce::juce_graphics      # juce_audio_devices pulls in juce::Colours
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Offscreen render test. Renders the highway into a juce::Image and counts
    # pixels — the only automated check that catches "it computes correctly but
    # you cannot SEE it", which is exactly how the highway shipped blank: every
    # layer was tested and correct, and the child grid painted over all of it.
    juce_add_console_app(vt_render_tests
        PRODUCT_NAME "vt_render_tests")

    target_sources(vt_render_tests PRIVATE
        tests/test_render.cpp
        src/app/AppSettings.cpp         # library-root detection is tested here too
        src/chart/Chart.cpp
        src/import/SongImporter.cpp
        src/score/TakeScorer.cpp        # the highway reads NoteScores to recolour notes
        src/ui/Theme.cpp
        src/ui/ImportOverlayComponent.cpp
        src/ui/NoteHighwayComponent.cpp
        src/ui/PianoKeyboardComponent.cpp
        src/ui/TransportBarComponent.cpp)

    target_include_directories(vt_render_tests PRIVATE src)

    target_compile_definitions(vt_render_tests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

    target_link_libraries(vt_render_tests
        PRIVATE
            juce::juce_gui_basics
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # WORKING_DIRECTORY is the repo root: vt_chart_tests parses the real
    # songs/synth-demo/chart.json and vt_render_tests resolves the library root
    # by walking up from the executable, and both are happier started from here.
    foreach(runner vt_tests vt_chart_tests vt_score_tests vt_backing_tests vt_render_tests)
        add_test(NAME ${runner}
                 COMMAND ${runner}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
endif()
