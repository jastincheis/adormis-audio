#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Loudness.h"

namespace wf
{
// Tools > Statistics: the measures of the selection (or file) in a table,
// with buttons to put the cursor on a channel's highest peak and to copy
// the table as text.
std::unique_ptr<juce::DocumentWindow> createStatisticsWindow (const juce::String& what, const AudioStatistics& stats,
                                                              double sampleRate,
                                                              std::function<void (juce::int64 position)> goTo,
                                                              std::function<void()> onClose);

// The table as plain text (also what Copy puts on the clipboard).
juce::String statisticsText (const juce::String& what, const AudioStatistics& stats, double sampleRate);
} // namespace wf
