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

# --------------------------------------------------------------------------------------
# The receiver page, compiled into the binary.
#
# The plugin must be able to serve the page with no runtime file dependency: a VST3 that
# needed a sibling directory would break the moment someone moved the bundle.
# --------------------------------------------------------------------------------------
juce_add_binary_data(PhonePostMixAssets
    HEADER_NAME BinaryData.h
    NAMESPACE   BinaryData
    SOURCES
        web/index.html
        web/app.js)

# Binary data is linked into shared modules (VST3, AU), which requires position-independent
# code or the link fails on Linux.
set_target_properties(PhonePostMixAssets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

set(PPM_CORE_SOURCES
    src/core/HttpMessage.cpp
    src/core/StreamEngine.cpp
    src/core/StreamServer.cpp
    src/core/WebSocketProtocol.cpp
    src/core/WirePacket.cpp)

target_sources(PhonePostMix PRIVATE
    ${PPM_CORE_SOURCES}
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/QrCodeComponent.cpp
    third_party/qrcodegen/qrcodegen.cpp)

# Vendored third-party code is not ours to warn about, and nayuki's QR generator is
# unmodified upstream.
set_source_files_properties(third_party/qrcodegen/qrcodegen.cpp PROPERTIES COMPILE_OPTIONS "-w")

target_compile_definitions(PhonePostMix PUBLIC
    PPM_VERSION="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0)

target_include_directories(PhonePostMix PRIVATE src third_party/qrcodegen)

target_link_libraries(PhonePostMix
    PRIVATE
        PhonePostMixAssets
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
        tests/WirePacketTests.cpp
        tests/StreamEngineTests.cpp
        tests/StreamServerTests.cpp)

    target_include_directories(PhonePostMixTests PRIVATE src tests)

    # Tests compare floats that were copied, not computed, so exact comparison is the
    # correct assertion here and -Wfloat-equal is pure noise. Set per source file, because
    # JUCE's recommended warning flags arrive through the link interface and would
    # otherwise be applied after any target-wide option and win.
    if(NOT MSVC)
        set_source_files_properties(tests/AudioRingBufferTests.cpp tests/WirePacketTests.cpp
            PROPERTIES COMPILE_OPTIONS "-Wno-float-equal")
    endif()

    target_compile_definitions(PhonePostMixTests PUBLIC
        PPM_VERSION="${PROJECT_VERSION}"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

    target_link_libraries(PhonePostMixTests
        PRIVATE
            PhonePostMixAssets
            juce::juce_audio_basics
            juce::juce_events
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    add_test(NAME PhonePostMixTests COMMAND PhonePostMixTests)

    # The receiver's decoding and playback arithmetic is tested under Node, because the
    # bugs that live there — a resampler that drifts, concealment that clicks — sound like
    # a bad mix rather than like a bug. Skipped rather than failed when Node is absent, so
    # a contributor without it can still build and run the C++ suite.
    find_program(PPM_NODE_EXECUTABLE NAMES node nodejs)

    if(PPM_NODE_EXECUTABLE)
        add_test(NAME PhonePostMixReceiverTests
                 COMMAND "${PPM_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/web/receiver.test.js")
    else()
        message(STATUS "PhonePostMix: node not found, skipping the receiver test")
    endif()
endif()
