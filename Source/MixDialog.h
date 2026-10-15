#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "MixPreview.h"

// Asks for the volume of the dropped audio (source) and of the audio it lands
// on (destination) before mixing, like Sound Forge's Mix dialog, with a live
// preview.
namespace wf
{
struct MixDialogCallbacks
{
    std::function<void()> onOk;                    // mix using the values in `gains`
    std::function<void (bool play)> onPreview;     // start or stop the preview
    std::function<bool()> isPreviewing;           // drives the Preview button label
    std::function<void()> onClose;                 // the window should now be deleted
};

// A floating, non-modal window (the rest of the app stays usable while it is
// open). The sliders write straight into `gains`, which the preview reads
// while playing. The caller owns the window and deletes it from `onClose`.
std::unique_ptr<juce::DocumentWindow> createMixWindow (const juce::String& description,
                                                       std::shared_ptr<MixGains> gains,
                                                       MixDialogCallbacks callbacks);
} // namespace wf
