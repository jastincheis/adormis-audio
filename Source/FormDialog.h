#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// One row of a small settings window: a text box, or a list when `choices`
// isn't empty.
struct FormField
{
    juce::String label;
    juce::String value;                  // the text, or the chosen item of `choices`
    juce::StringArray choices;
    juce::String unit;                   // shown after a text box ("s", "dB"...)
    std::function<bool (const juce::String&)> isValid;   // text boxes; OK stays off while false
};

// A floating, non-modal settings window in the style of Volume: a line saying
// what it applies to, the fields, an optional hint, OK / Cancel. `onOk` gets
// the values in field order (texts, or the chosen items).
std::unique_ptr<juce::DocumentWindow> createFormWindow (const juce::String& title, const juce::String& info,
                                                        std::vector<FormField> fields, const juce::String& hint,
                                                        std::function<void (const juce::StringArray& values)> onOk,
                                                        std::function<void()> onClose);

// "1.5", "1,5", "90", "1:30", "00:01:30.250" -> seconds; empty when not a time.
std::optional<double> parseTime (const juce::String& text);

// A plain number ("-60", "0,5"); empty when it isn't one.
std::optional<double> parseNumber (const juce::String& text);
} // namespace wf
