#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// Tools > CDJ Export: the whole Adormis CDJ Export (github.com/jastincheis/cdj-export,
// same author, AGPLv3) in a floating window: playlist, track editor (markers,
// grid), stick list and Export for rekordbox / Pioneer CDJ and Serato.
// It plays through Adormis Audio's audio device.
struct CdjExportHost
{
    juce::AudioDeviceManager* devices = nullptr;
    std::function<void()> onClose;      // the window is closed (deleted by the owner, later)
};

std::unique_ptr<juce::DocumentWindow> createCdjExportWindow (CdjExportHost);

// Adds audio files (WAV/MP3/AIFF/FLAC) to the playlist of a window made by
// createCdjExportWindow.
void addToCdjExport (juce::DocumentWindow&, const juce::StringArray& files);

// Markers edited in CDJ Export and not yet saved into their files.
bool cdjExportHasUnsavedMarkers (juce::DocumentWindow&);
}
