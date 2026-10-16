#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace wf
{
// Time stretch and pitch shift of a range of audio, offline and at the best
// quality (Rubber Band Library, R3 "finer" engine, built in from source).
struct StretchSettings
{
    double timeRatio = 1.0;     // output length / input length: old BPM / new BPM
    double semitones = 0.0;     // pitch change; 0 keeps the pitch
    bool preserveFormants = false;

    bool changesSomething() const  { return std::abs (timeRatio - 1.0) > 1.0e-9 || std::abs (semitones) > 1.0e-9; }
};

constexpr double minStretchRatio = 0.25, maxStretchRatio = 4.0;
constexpr double maxPitchSemitones = 24.0;

// `progress` gets 0..1 and returns false to cancel (then the result is empty).
// The result has round(length * timeRatio) samples and the same channels.
std::optional<juce::AudioBuffer<float>> stretchAudio (const juce::AudioBuffer<float>& audio, juce::Range<int> range,
                                                      double sampleRate, const StretchSettings& settings,
                                                      const std::function<bool (float)>& progress);
} // namespace wf
