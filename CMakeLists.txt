cmake_minimum_required(VERSION 3.22)

# The deployment target must be set before the first target is created, otherwise
# CMake bakes the default (current macOS) into every object file.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum supported macOS version")
# Universal binaries double the build time, so they are opt-in: on for releases and CI,
# off for day-to-day development where you only need your own architecture.
option(PPM_UNIVERSAL_BINARY "Build a macOS universal (arm64 + x86_64) binary" OFF)
if(APPLE AND PPM_UNIVERSAL_BINARY)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

project(PhonePostMix VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------------------
# JUCE
#
# Fetched at configure time so a fresh clone needs nothing but CMake and a compiler.
# Set -DPPM_JUCE_PATH=/path/to/JUCE to reuse a local checkout and skip the download.
# --------------------------------------------------------------------------------------
set(PPM_JUCE_PATH "" CACHE PATH "Path to an existing JUCE checkout (skips the download)")

if(PPM_JUCE_PATH)
    message(STATUS "PhonePostMix: using local JUCE at ${PPM_JUCE_PATH}")
    add_subdirectory("${PPM_JUCE_PATH}" juce EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(PhonePostMix
    PLUGIN_MANUFACTURER_CODE  Ppmx
    PLUGIN_CODE               Ppm1
    COMPANY_NAME              "PhonePostMix"
    BUNDLE_ID                 dev.phonepostmix.plugin
    FORMATS                   AU VST3 Standalone
    PRODUCT_NAME              "PhonePostMix"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    COPY_PLUGIN_AFTER_BUILD   TRUE)

set(PPM_CORE_SOURCES
    src/core/HttpMessage.cpp
    src/core/StreamServer.cpp
    src/core/WebSocketProtocol.cpp)

target_sources(PhonePostMix PRIVATE
    ${PPM_CORE_SOURCES}
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp)

target_compile_definitions(PhonePostMix PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_include_directories(PhonePostMix PRIVATE src)

target_link_libraries(PhonePostMix
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# --------------------------------------------------------------------------------------
# Tests
#
# A console app rather than a plugin, so `ctest` can run the DSP and protocol code with no
# host, no audio device and no network.
# --------------------------------------------------------------------------------------
option(PPM_BUILD_TESTS "Build the PhonePostMix test suite" ON)

if(PPM_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(PhonePostMixTests PRODUCT_NAME "PhonePostMixTests")

    target_sources(PhonePostMixTests PRIVATE
        ${PPM_CORE_SOURCES}
        tests/TestMain.cpp
        tests/AudioRingBufferTests.cpp
        tests/WebSocketProtocolTests.cpp
        tests/HttpMessageTests.cpp
        tests/StreamServerTests.cpp)

    target_include_directories(PhonePostMixTests PRIVATE src tests)

    # Tests compare floats that were copied, not computed, so exact comparison is the
    # correct assertion here and -Wfloat-equal is pure noise. Set per source file, because
    # JUCE's recommended warning flags arrive through the link interface and would
    # otherwise be applied after any target-wide option and win.
    if(NOT MSVC)
        set_source_files_properties(tests/AudioRingBufferTests.cpp
            PROPERTIES COMPILE_OPTIONS "-Wno-float-equal")
    endif()

    target_compile_definitions(PhonePostMixTests PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

    target_link_libraries(PhonePostMixTests
        PRIVATE
            juce::juce_audio_basics
            juce::juce_events
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    add_test(NAME PhonePostMixTests COMMAND PhonePostMixTests)
endif()
