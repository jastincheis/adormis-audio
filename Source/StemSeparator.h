#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <optional>

// Get Stems: splits a song into drums, bass, other and vocals with Demucs
// (the htdemucs model by Meta, MIT licence), run on the CPU by demucs.cpp
// (Sevag Hanssian, MIT licence). The model ships with the program.
namespace wf::stems
{
// The four parts, in Demucs' order.
enum Part { drums, bass, other, vocals, numParts };

// Where the bundled model is: next to the program (Windows, Linux), in the
// app's Resources (macOS), or in the settings folder. Missing = empty file.
juce::File modelFile();

// How many pieces of the song are worked on at once: each needs about
// 0.7 GB on top of ~2.7 GB, so it follows the computer's memory and cores.
int piecesAtOnce();

// The model's sample rate; the input must be stereo at this rate.
constexpr double modelRate = 44100.0;

// Blocking: run it off the message thread. `progress` gets 0..1 and returns
// false to cancel. The parts come back stereo at modelRate, as long as the
// input. Nothing when cancelled or on an error (then `error` says why).
std::optional<std::array<juce::AudioBuffer<float>, numParts>> separate (const juce::AudioBuffer<float>& stereoAt44k,
                                                                         const std::function<bool (float)>& progress,
                                                                         juce::String& error);
} // namespace wf::stems
