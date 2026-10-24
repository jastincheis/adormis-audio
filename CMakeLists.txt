cmake_minimum_required(VERSION 3.22)

# Both must be set before project() to take effect.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
# Windows: the C++ runtime inside the .exe, so it starts without the
# Visual C++ Redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(AdormisAudio VERSION 0.1.26 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 9.0.4, pinned by commit and checksum.
# For local builds, -DADORMIS_JUCE_DIR=/path/to/JUCE skips the download.
set(ADORMIS_JUCE_DIR "" CACHE PATH "Local JUCE checkout (optional)")

if(ADORMIS_JUCE_DIR)
    add_subdirectory(${ADORMIS_JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        URL      https://github.com/juce-framework/JUCE/archive/3fb5cbc0340758596dac39f7c8de47d562b4896e.tar.gz
        URL_HASH SHA256=2bb7095497a7bb7269dbdf841a7c6aa0939eaeb2a2bc9724ac4758d3ae3565a7)
    FetchContent_MakeAvailable(JUCE)
endif()

# Rubber Band Library 4.0.0 (GPL-2.0-or-later) for time stretch / pitch shift,
# compiled from its single-file build so every platform gets the same engine.
# -DADORMIS_RUBBERBAND_DIR=/path/to/rubberband-4.0.0 skips the download.
set(ADORMIS_RUBBERBAND_DIR "" CACHE PATH "Local Rubber Band source tree (optional)")

if(ADORMIS_RUBBERBAND_DIR)
    set(RUBBERBAND_SOURCE_DIR ${ADORMIS_RUBBERBAND_DIR})
else()
    include(FetchContent)
    FetchContent_Declare(rubberband
        URL      https://github.com/breakfastquay/rubberband/archive/refs/tags/v4.0.0.tar.gz
        URL_HASH SHA256=24300f48a8014b7c863b573a9647e61b1b19b37875e2cdd92005e64c6424d266)
    FetchContent_MakeAvailable(rubberband)   # no CMakeLists.txt in it: sources only
    set(RUBBERBAND_SOURCE_DIR ${rubberband_SOURCE_DIR})
endif()

# Drum Rebuilder (Tools > Drum Rebuilder), same author, AGPLv3: its analysis
# core and its whole editor (drb_ui), pinned by commit.
# -DADORMIS_DRB_DIR=/path/to/drum-rebuilder skips the download.
set(ADORMIS_DRB_DIR "" CACHE PATH "Local drum-rebuilder checkout (optional)")

if(ADORMIS_DRB_DIR)
    add_subdirectory(${ADORMIS_DRB_DIR} drum-rebuilder EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(drum_rebuilder
        GIT_REPOSITORY https://github.com/jastincheis/drum-rebuilder.git
        GIT_TAG        5e407a092a0b3fa1ee8afaeadcaa00c0d695743e)
    FetchContent_MakeAvailable(drum_rebuilder)
endif()

# Get Stems: Demucs (the htdemucs model by Meta, MIT) run on the CPU by
# demucs.cpp (MIT), which needs Eigen (MPL 2.0); pinned by commit.
# -DADORMIS_DEMUCS_DIR=/path/to/demucs.cpp (with vendor/eigen checked out)
# skips the download. Only its src/ is built, not its own CMake project.
set(ADORMIS_DEMUCS_DIR "" CACHE PATH "Local demucs.cpp checkout (optional)")

if(ADORMIS_DEMUCS_DIR)
    set(DEMUCS_SOURCE_DIR ${ADORMIS_DEMUCS_DIR})
else()
    include(FetchContent)
    FetchContent_Declare(demucs_cpp
        GIT_REPOSITORY https://github.com/sevagh/demucs.cpp.git
        GIT_TAG        f1206e9adeea103aef4a636b9e62297cf1f8e34e
        GIT_SUBMODULES vendor/eigen
        SOURCE_SUBDIR  not-a-cmake-project)
    FetchContent_MakeAvailable(demucs_cpp)
    set(DEMUCS_SOURCE_DIR ${demucs_cpp_SOURCE_DIR})
endif()

file(GLOB DEMUCS_SOURCES ${DEMUCS_SOURCE_DIR}/src/*.cpp)
add_library(demucs_engine STATIC ${DEMUCS_SOURCES})
target_include_directories(demucs_engine PUBLIC ${DEMUCS_SOURCE_DIR}/src ${DEMUCS_SOURCE_DIR}/vendor/eigen)
set_target_properties(demucs_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(demucs_engine PUBLIC cxx_std_17)
# Fast even in Debug builds: the model is slow enough as it is.
if(MSVC)
    target_compile_options(demucs_engine PRIVATE /O2 /bigobj /W0)
    target_compile_definitions(demucs_engine PRIVATE NOMINMAX _USE_MATH_DEFINES)
else()
    target_compile_options(demucs_engine PRIVATE -O3 -w)
endif()

# The converted model (ggml-model-htdemucs-4s-f16.bin, 84 MB) goes next to
# the program, in "models". -DADORMIS_DEMUCS_MODEL=/path/to/it.
set(ADORMIS_DEMUCS_MODEL "" CACHE FILEPATH "Converted Demucs model for Get Stems")

add_library(rubberband_single STATIC ${RUBBERBAND_SOURCE_DIR}/single/RubberBandSingle.cpp)
target_include_directories(rubberband_single PUBLIC ${RUBBERBAND_SOURCE_DIR})
set_target_properties(rubberband_single PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    target_link_libraries(rubberband_single PUBLIC "-framework Accelerate")
endif()
if(WIN32)
    # windows.h's min/max macros break std::min/std::max in Rubber Band.
    target_compile_definitions(rubberband_single PRIVATE NOMINMAX _USE_MATH_DEFINES)
endif()

juce_add_gui_app(AdormisAudio
    PRODUCT_NAME "Adormis Audio"
    COMPANY_NAME "Adormis"
    BUNDLE_ID    "io.github.jastincheis.adormisaudio"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Adormis Audio records audio from the input you choose (Ctrl+R)."
    # curl on Linux, for the HTTPS question to GitHub (Help > Check for Updates);
    # Windows and macOS use the system's own.
    NEEDS_CURL TRUE
    VERSION      ${PROJECT_VERSION})

target_sources(AdormisAudio PRIVATE
    Source/Main.cpp
    Source/Analysis.cpp
    Source/AudioFilePreview.cpp
    Source/BackgroundPattern.cpp
    Source/BassFinder.cpp
    Source/BassGenerator.cpp
    Source/MainComponent.cpp
    Source/MixDialog.cpp
    Source/MixPreview.cpp
    Source/NoteDetector.cpp
    Source/Mp3Encoder.cpp
    Source/Commands.cpp
    Source/DataWindow.cpp
    Source/DeEsser.cpp
    Source/DrumRebuilderWindow.cpp
    Source/Effects.cpp
    Source/EffectWindow.cpp
    Source/FilePicker.cpp
    Source/Processes.cpp
    Source/SaveAsDialog.cpp
    Source/VolumeDialog.cpp
    Source/FormDialog.cpp
    Source/Mp3Restore.cpp
    Source/Loudness.cpp
    Source/MeterPanel.cpp
    Source/MoreEffects.cpp
    Source/Tempo.cpp
    Source/Restoration.cpp
    Source/StartupLog.cpp
    Source/RegionsWindow.cpp
    Source/BatchWindow.cpp
    Source/Settings.cpp
    Source/StatisticsWindow.cpp
    Source/SpectrumWindow.cpp
    Source/ProcessToolbar.cpp
    Source/Recorder.cpp
    Source/ShortcutsWindow.cpp
    Source/Sibilance.cpp
    Source/SpaceKey.cpp
    Source/StemSeparator.cpp
    Source/WaveColourPicker.cpp
    Source/Appearance.cpp
    Source/StretchDialog.cpp
    Source/TemplateDialog.cpp
    Source/TimeStretch.cpp
    Source/UndoHistoryWindow.cpp
    Source/LinkDownload.cpp
    Source/UpdateCheck.cpp
    Source/WaveformView.cpp)

# Recording what the computer plays: one backend per system.
if(WIN32)
    target_sources(AdormisAudio PRIVATE Source/SystemAudioWindows.cpp)
elseif(APPLE)
    target_sources(AdormisAudio PRIVATE Source/SystemAudioMac.mm)
    set_source_files_properties(Source/SystemAudioMac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(AdormisAudio PRIVATE "-weak_framework ScreenCaptureKit" "-framework CoreMedia" "-framework CoreGraphics")
else()
    target_sources(AdormisAudio PRIVATE Source/SystemAudioLinux.cpp)
endif()

target_compile_definitions(AdormisAudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AdormisAudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AdormisAudio,JUCE_VERSION>")

# ASIO drivers on Windows (low-latency audio interfaces). JUCE bundles the
# ASIO SDK headers, which Steinberg licenses under GPLv3 as an alternative to
# its own licence: compatible with Adormis Audio's AGPLv3.
if(WIN32)
    target_compile_definitions(AdormisAudio PRIVATE JUCE_ASIO=1)
endif()

# MP3 export through LAME (libmp3lame, LGPL). Prefer the static library so the
# app doesn't depend on it being installed (it's bundled on macOS/Windows).
find_path(LAME_INCLUDE_DIR lame/lame.h)
find_library(LAME_LIBRARY NAMES libmp3lame.a mp3lame libmp3lame)

if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
    message(STATUS "MP3 export enabled: ${LAME_LIBRARY}")
    target_include_directories(AdormisAudio PRIVATE ${LAME_INCLUDE_DIR})
    target_link_libraries(AdormisAudio PRIVATE ${LAME_LIBRARY})
    target_compile_definitions(AdormisAudio PRIVATE ADORMIS_HAS_MP3=1)
else()
    message(STATUS "MP3 export disabled: libmp3lame not found")
    target_compile_definitions(AdormisAudio PRIVATE ADORMIS_HAS_MP3=0)
endif()

# High-quality sample rate conversion (libsamplerate, BSD). Without it, Save As
# and Mix fall back to JUCE's Lagrange interpolator.
find_path(SAMPLERATE_INCLUDE_DIR samplerate.h)
find_library(SAMPLERATE_LIBRARY NAMES libsamplerate.a samplerate libsamplerate)

if(SAMPLERATE_INCLUDE_DIR AND SAMPLERATE_LIBRARY)
    message(STATUS "libsamplerate enabled: ${SAMPLERATE_LIBRARY}")
    target_include_directories(AdormisAudio PRIVATE ${SAMPLERATE_INCLUDE_DIR})
    target_link_libraries(AdormisAudio PRIVATE ${SAMPLERATE_LIBRARY})
    target_compile_definitions(AdormisAudio PRIVATE ADORMIS_HAS_SAMPLERATE=1)
else()
    message(STATUS "libsamplerate not found: sample rate conversion uses Lagrange")
    target_compile_definitions(AdormisAudio PRIVATE ADORMIS_HAS_SAMPLERATE=0)
endif()

target_link_libraries(AdormisAudio
    PRIVATE
        juce::juce_gui_extra
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_cryptography
        rubberband_single
        drb_ui
        demucs_engine
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Get Stems' model, beside the program.
if(ADORMIS_DEMUCS_MODEL)
    if(APPLE)
        set(ADORMIS_MODELS_DIR "$<TARGET_BUNDLE_CONTENT_DIR:AdormisAudio>/Resources/models")
    else()
        set(ADORMIS_MODELS_DIR "$<TARGET_FILE_DIR:AdormisAudio>/models")
    endif()
    add_custom_command(TARGET AdormisAudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ADORMIS_MODELS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ADORMIS_DEMUCS_MODEL}" "${ADORMIS_MODELS_DIR}/"
        VERBATIM)
else()
    message(WARNING "ADORMIS_DEMUCS_MODEL not set: Get Stems will say the model is missing")
endif()
