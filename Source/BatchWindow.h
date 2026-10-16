#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "FilePicker.h"

namespace wf
{
// Tools > Batch Converter: the same steps (channels, trim, normalize or
// loudness, fades, sample rate) and output format for many files at once.
// The originals are never changed; the results go into the chosen folder.
std::unique_ptr<juce::DocumentWindow> createBatchWindow (juce::AudioFormatManager& formats, FilePicker& picker,
                                                         juce::Array<juce::File> initialFiles,
                                                         std::function<void()> onClose);
} // namespace wf
