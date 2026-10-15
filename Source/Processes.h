#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Destructive processes applied in place to a range of samples on all channels.
namespace wf
{
enum class Process
{
    fadeIn,
    fadeOut,
    mute,
    normalize,   // peak to -0.1 dBFS
    reverse,
    invert,
    dcOffset     // remove the average offset
};

void applyProcess (Process process, juce::AudioBuffer<float>& buffer, juce::Range<int> range);

// Process > Volume: changes the level by `decibels` (negative = quieter).
void applyVolume (juce::AudioBuffer<float>& buffer, juce::Range<int> range, float decibels);
} // namespace wf
