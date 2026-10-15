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
    Source/Commands.cpp
    Source/DataWindow.cpp
    Source/Processes.cpp
    Source/ProcessToolbar.cpp
    Source/ShortcutsWindow.cpp
    Source/WaveformView.cpp)

target_compile_definitions(WaveForge PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:WaveForge,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:WaveForge,JUCE_VERSION>")

target_link_libraries(WaveForge
    PRIVATE
        juce::juce_gui_extra
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
