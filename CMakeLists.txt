cmake_minimum_required(VERSION 3.22)

project(WaveForge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# JUCE 9.0.4, pinned by commit and checksum.
# For local builds, -DWAVEFORGE_JUCE_DIR=/path/to/JUCE skips the download.
set(WAVEFORGE_JUCE_DIR "" CACHE PATH "Local JUCE checkout (optional)")

if(WAVEFORGE_JUCE_DIR)
    add_subdirectory(${WAVEFORGE_JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        URL      https://github.com/juce-framework/JUCE/archive/3fb5cbc0340758596dac39f7c8de47d562b4896e.tar.gz
        URL_HASH SHA256=2bb7095497a7bb7269dbdf841a7c6aa0939eaeb2a2bc9724ac4758d3ae3565a7)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(WaveForge
    PRODUCT_NAME "WaveForge"
    COMPANY_NAME "WaveForge"
    BUNDLE_ID    "io.github.jastincheis.waveforge"
    VERSION      ${PROJECT_VERSION})

target_sources(WaveForge PRIVATE
    Source/Main.cpp
    Source/BackgroundPattern.cpp
    Source/MainComponent.cpp
    Source/MixDialog.cpp
    Source/MixPreview.cpp
    Source/Mp3Encoder.cpp
    Source/Commands.cpp
    Source/DataWindow.cpp
    Source/FilePicker.cpp
    Source/Processes.cpp
    Source/SaveAsDialog.cpp
    Source/VolumeDialog.cpp
    Source/ProcessToolbar.cpp
    Source/ShortcutsWindow.cpp
    Source/WaveformView.cpp)

target_compile_definitions(WaveForge PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:WaveForge,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:WaveForge,JUCE_VERSION>")

# MP3 export through LAME (libmp3lame, LGPL). Prefer the static library so the
# app doesn't depend on it being installed (it's bundled on macOS/Windows).
find_path(LAME_INCLUDE_DIR lame/lame.h)
find_library(LAME_LIBRARY NAMES libmp3lame.a mp3lame libmp3lame)

if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
    message(STATUS "MP3 export enabled: ${LAME_LIBRARY}")
    target_include_directories(WaveForge PRIVATE ${LAME_INCLUDE_DIR})
    target_link_libraries(WaveForge PRIVATE ${LAME_LIBRARY})
    target_compile_definitions(WaveForge PRIVATE WAVEFORGE_HAS_MP3=1)
else()
    message(STATUS "MP3 export disabled: libmp3lame not found")
    target_compile_definitions(WaveForge PRIVATE WAVEFORGE_HAS_MP3=0)
endif()

# High-quality sample rate conversion (libsamplerate, BSD). Without it, Save As
# and Mix fall back to JUCE's Lagrange interpolator.
find_path(SAMPLERATE_INCLUDE_DIR samplerate.h)
find_library(SAMPLERATE_LIBRARY NAMES libsamplerate.a samplerate libsamplerate)

if(SAMPLERATE_INCLUDE_DIR AND SAMPLERATE_LIBRARY)
    message(STATUS "libsamplerate enabled: ${SAMPLERATE_LIBRARY}")
    target_include_directories(WaveForge PRIVATE ${SAMPLERATE_INCLUDE_DIR})
    target_link_libraries(WaveForge PRIVATE ${SAMPLERATE_LIBRARY})
    target_compile_definitions(WaveForge PRIVATE WAVEFORGE_HAS_SAMPLERATE=1)
else()
    message(STATUS "libsamplerate not found: sample rate conversion uses Lagrange")
    target_compile_definitions(WaveForge PRIVATE WAVEFORGE_HAS_SAMPLERATE=0)
endif()

target_link_libraries(WaveForge
    PRIVATE
        juce::juce_gui_extra
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
