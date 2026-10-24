#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// Asks how many dB to change the volume by (e.g. 0.1, -0.1, 3, -6; comma or
// dot), like Sound Forge's Process > Volume. `target` says what it applies to
// ("Selection, left channel"...). `onApply` gets the value when OK is pressed;
// `onClose` means the window can go. Floating and non-modal like Mix/Save As.
// The Mute button calls `onMute` instead (silence, like Process > Mute).
std::unique_ptr<juce::DocumentWindow> createVolumeWindow (const juce::String& target, float initialDecibels,
                                                          std::function<void (float decibels)> onApply,
                                                          std::function<void()> onMute,
                                                          std::function<void()> onClose);

// Parses "0,1", "-0.1", "+3 dB"... Empty when it isn't a number in the allowed range.
std::optional<float> parseDecibels (const juce::String& text);

constexpr float minVolumeDecibels = -96.0f;
constexpr float maxVolumeDecibels = 24.0f;
} // namespace wf
