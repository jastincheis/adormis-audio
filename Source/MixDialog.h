#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Asks for the volume of the dropped audio (source) and of the audio it lands
// on (destination) before mixing, like Sound Forge's Mix dialog.
namespace wf
{
struct MixSettings
{
    float sourceGain = 1.0f;
    float destGain = 1.0f;
};

// `description` explains what is mixed where; `onOk` runs only when confirmed.
void showMixDialog (const juce::String& description, std::function<void (MixSettings)> onOk);
} // namespace wf
