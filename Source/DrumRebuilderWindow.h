#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// Tools > Drum Rebuilder: the whole Drum Rebuilder (github.com/jastincheis/drum-rebuilder,
// same author, AGPLv3) in a floating window, playing through Adormis Audio's
// audio device. Its Export tab gets an "Open in Adormis Audio" button.
struct DrumRebuilderHost
{
    juce::AudioDeviceManager* devices = nullptr;
    std::function<void (const juce::String& title, juce::AudioBuffer<float>, double sampleRate)> openAsNewFile;
    std::function<void()> onClose;
};

std::unique_ptr<juce::DocumentWindow> createDrumRebuilderWindow (DrumRebuilderHost);

// Opens a beat (a WAV, AIFF or FLAC file) in a window made by createDrumRebuilderWindow;
// `name` titles what comes back ("<name> rebuilt").
void loadDrumRebuilderBeat (juce::DocumentWindow&, const juce::File& beat, const juce::String& name);

// The audio to hand over: the file itself when it is on disk, whole and
// unchanged (Drum Rebuilder then remembers its fixes with that file);
// otherwise a WAV copy in the settings folder, named after the file, the
// stretch and its content, so the same stretch finds its fixes again.
juce::File drumRebuilderBeatFile (const juce::String& title, const juce::File& original, bool wholeAndUnchanged,
                                  const juce::AudioBuffer<float>& audio, double sampleRate, double startSeconds);
} // namespace wf
