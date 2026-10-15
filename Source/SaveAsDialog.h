#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// What Save As should write.
struct SaveFormat
{
    juce::String extension;   // ".wav", ".flac", ".aiff", ".mp3", ".ogg"
    int bitsPerSample = 16;   // WAV / FLAC / AIFF
    bool floatingPoint = false;
    int kbps = 0;             // MP3 / OGG
    double sampleRate = 0.0;  // 0 keeps the file's rate
};

// Asks which format, quality and sample rate to save in. `suggested` (an
// extension) picks the initial format; `fileRate` is the file's current rate. `onChosen` runs when the user presses "Save..." and is
// followed by the usual file chooser; `onClose` means the window can go.
std::unique_ptr<juce::DocumentWindow> createSaveAsWindow (const juce::String& fileTitle,
                                                          const juce::String& suggested,
                                                          double fileRate,
                                                          std::function<void (SaveFormat)> onChosen,
                                                          std::function<void()> onClose);
} // namespace wf
