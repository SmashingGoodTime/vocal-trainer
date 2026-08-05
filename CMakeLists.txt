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

# Static MSVC runtime, so the Standalone and the VST3 both run on a machine with
# no VC++ redistributable installed. Must be set BEFORE add_subdirectory(JUCE)
# so JUCE's own targets (and the juce_vst3_helper tool) pick it up too.
# CMP0091 is NEW by default at cmake_minimum_required 3.22, so this is honoured.
if (WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

#------------------------------------------------------------------------------
# Options
#------------------------------------------------------------------------------

# ASIO is OFF by default so the very first build on a clean machine cannot fail
# on SDK problems. Turning it ON gives the Standalone low-latency access to the
# Apollo Twin X. The VST3 never needs it — Live owns the audio device.
#
# LICENSING: the ASIO SDK is GPLv3-or-Steinberg-proprietary. JUCE 9 bundles it
# (modules/juce_audio_devices/native/asio/iasiodrv.h), so no external download is
# needed, but it MUST stay OFF for any closed-source distribution. See LICENSING.md.
option(VT_ENABLE_ASIO "Compile ASIO support into the Standalone (see LICENSING.md)" OFF)

option(VT_BUILD_TESTS "Build the vt_tests console test runner" ON)

# Copying the built VST3 somewhere your host will find it is a personal-workflow
# convenience, so it is OFF by default — a fresh clone should not write outside
# its own build tree.
#
# Turn it on with -DVT_COPY_PLUGIN_AFTER_BUILD=ON. Point VT_VST3_COPY_DIR at a
# USER-WRITABLE folder: writing to C:/Program Files/Common Files/VST3 would
# require admin elevation on every single build. Then point Live at that folder
# via Preferences -> Plug-Ins -> "VST3 Plug-In Custom Folder" (Live requires that
# the custom folder is NOT the system VST3 folder).
option(VT_COPY_PLUGIN_AFTER_BUILD
       "Copy the built VST3 into VT_VST3_COPY_DIR after each build" OFF)

if (WIN32)
    set(VT_VST3_COPY_DIR_DEFAULT "$ENV{USERPROFILE}/VST3Dev")
else()
    set(VT_VST3_COPY_DIR_DEFAULT "$ENV{HOME}/VST3Dev")
endif()

set(VT_VST3_COPY_DIR "${VT_VST3_COPY_DIR_DEFAULT}" CACHE PATH
    "Destination for the post-build VST3 copy (used only when VT_COPY_PLUGIN_AFTER_BUILD=ON)")

#------------------------------------------------------------------------------
# JUCE — plain directory at ./JUCE, NOT a git submodule
#------------------------------------------------------------------------------

add_subdirectory(JUCE)

#------------------------------------------------------------------------------
# The plugin / Standalone
#------------------------------------------------------------------------------

juce_add_plugin(VocalTrainer
    COMPANY_NAME                "SmashingGoodTime"

    # PIN THESE. PLUGIN_CODE otherwise defaults to a random value that changes on
    # EVERY configure, which orphans the plugin in every saved Live set.
    PLUGIN_MANUFACTURER_CODE    Sgtm
    PLUGIN_CODE                 Vtr1

    FORMATS                     VST3 Standalone
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             "Fx" "Analyzer"

    # Both configured above. Off by default; see the VT_COPY_PLUGIN_AFTER_BUILD
    # option for why, and for the folder Live needs to be pointed at.
    COPY_PLUGIN_AFTER_BUILD     ${VT_COPY_PLUGIN_AFTER_BUILD}
    VST3_COPY_DIR               "${VT_VST3_COPY_DIR}"

    PRODUCT_NAME                "Vocal Trainer")

target_sources(VocalTrainer PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/dsp/AnalysisChain.cpp
    src/audio/BackingPlayer.cpp
    src/chart/Chart.cpp
    src/import/SongImporter.cpp
    src/ui/Theme.cpp
    src/ui/PitchLaneComponent.cpp
    src/ui/NoteHighwayComponent.cpp
    src/ui/ImportOverlayComponent.cpp
    src/ui/LyricStripComponent.cpp)

target_include_directories(VocalTrainer PRIVATE
    src
    # JUCE's module root, so the shared code can reach
    # <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>.
    #
    # It is needed for exactly one thing: switching OFF the standalone wrapper's
    # "Mute audio input" checkbox. That control swaps an EMPTY BUFFER for the
    # microphone before the plugin runs, which for an analyser kills the pitch
    # display along with the sound. juce_add_plugin puts this module on the
    # FORMAT targets only, not on the shared-code target that compiles
    # PluginEditor.cpp, so without this line the __has_include guard there
    # silently evaluates false and the fix is compiled out.
    ${CMAKE_CURRENT_SOURCE_DIR}/JUCE/modules)

target_compile_definitions(VocalTrainer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
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
        src/chart/Chart.cpp)

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

    # BackingPlayer lifecycle. Drives the exact sequence that crashed the app
    # with 0xC0000094 on 2026-08-04: play, releaseResources(), then one more
    # audio callback. WITHOUT the fix this target does not fail, it CRASHES —
    # which is the correct signal for a divide-by-zero regression.
    juce_add_console_app(vt_backing_tests
        PRODUCT_NAME "vt_backing_tests")

    target_sources(vt_backing_tests PRIVATE
        tests/test_backing.cpp
        src/audio/BackingPlayer.cpp)

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
        src/chart/Chart.cpp
        src/import/SongImporter.cpp
        src/ui/Theme.cpp
        src/ui/ImportOverlayComponent.cpp
        src/ui/NoteHighwayComponent.cpp)

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
endif()
