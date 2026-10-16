#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// A new, silent file with markers on the beat grid (File > New...).
struct TemplateSettings
{
    double bpm = 120.0;
    int division = 16;          // markers every 1/division note: 4, 8, 16 or 32
    int bars = 8;               // of 4/4
    double sampleRate = 44100.0;
    int channels = 2;

    juce::int64 lengthInSamples() const;
    std::vector<juce::int64> markerPositions() const;   // one per grid step, from 0
    std::vector<juce::int64> barStartPositions() const; // the first marker of every bar
};

// Floating, non-modal window like Volume / Save As. `onCreate` gets the
// settings when Create is pressed; `onClose` means the window can go.
std::unique_ptr<juce::DocumentWindow> createTemplateWindow (const TemplateSettings& initial,
                                                            std::function<void (TemplateSettings)> onCreate,
                                                            std::function<void()> onClose);

std::optional<double> parseBpm (const juce::String& text);    // 20 ... 400, decimals with comma or dot
std::optional<int> parseBars (const juce::String& text);      // 1 ... 999
} // namespace wf
