#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// MP3 export through LAME (libmp3lame, LGPL). Only available when the build
// found the library (WAVEFORGE_HAS_MP3); JUCE itself can read MP3 but not write it.
namespace wf
{
bool mp3EncodingAvailable();

// Encodes `audio` at a constant `kbps` bitrate. Mono stays mono, anything
// with more channels is written as stereo (first two channels).
bool writeMp3 (const juce::File& destination, const juce::AudioBuffer<float>& audio, double sampleRate,
               int kbps, juce::String& error);
} // namespace wf
