#pragma once

#include <juce_core/juce_core.h>

// A small log of each start (what was done, how long the audio device took,
// any crash with its stack), so a start that shows nothing can be explained:
// %APPDATA%\Adormis Audio\adormis-audio.log on Windows, ~/Library/Application Support/
// Adormis Audio on macOS, ~/.config/Adormis Audio on Linux. The previous start's log
// is kept as adormis-audio-previous.log.
namespace wf::startup
{
juce::File settingsFolder();

void beginLog();                        // first thing at start
void log (const juce::String& line);    // timestamped line
void endLog();

// True when the last start didn't get as far as opening the audio device
// (it hung or crashed there): this start then skips ASIO and the saved device.
bool lastStartFailedAtAudio();
void markAudioStarting();
void markAudioStarted();
} // namespace wf::startup
