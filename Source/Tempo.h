#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace wf
{
// The tempo shown on the toolbar: beats per minute of `range`, to 0.1 BPM
// (60..200), or empty when there's no steady beat (speech, silence, pads).
// More precise than detectTempo (which the Bass Finder keeps as it is, to
// match its web original): finer onset grid, sub-beat and 3-beat checks
// against 2/3 and 3/2 mistakes, and a phase-locked refinement over the
// whole range.
std::optional<double> measureTempo (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate);
} // namespace wf
