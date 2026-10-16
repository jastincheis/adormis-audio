#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// What WaveForge remembers between starts: the last settings of the effect
// windows, Mix, Volume and the small settings windows. Kept in settings.xml
// in the settings folder (next to waveforge.log).
namespace wf::settings
{
juce::var get (const juce::String& key);
void set (const juce::String& key, const juce::var& value);
void flush();   // writes what is waiting (at quit)

// Every slider, list and on/off button under `root` (in a fixed order),
// except those marked with dontRemember(), as one value; and back.
juce::var captureControls (juce::Component& root);
void restoreControls (juce::Component& root, const juce::var& state);

// Values the window measures from the file (a detected frequency or level)
// are not carried over to the next file.
void dontRemember (juce::Component& c);
} // namespace wf::settings
