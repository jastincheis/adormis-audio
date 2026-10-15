#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Processes.h"
#include "WaveformView.h"

// One open audio file: its samples, format details, waveform and undo history.
// Lives inside a floating window of the workspace.
class DataWindow final : public juce::Component
{
public:
    static std::unique_ptr<DataWindow> load (const juce::File& file, juce::AudioFormatManager& formats, juce::String& error);

    // A new, unsaved document holding a copy of `source`'s selection.
    static std::unique_ptr<DataWindow> fromSelection (const DataWindow& source, const juce::String& title);

    WaveformView& getView()                          { return view; }
    juce::AudioBuffer<float>& getBuffer()            { return buffer; }
    const juce::File& getFile() const                { return file; }
    const juce::String& getTitle() const             { return title; }
    double getSampleRate() const                     { return sampleRate; }
    bool isModified() const                          { return modified; }

    // Writes the audio to `destination` (format chosen by its extension) and,
    // on success, makes it this document's file.
    bool saveTo (const juce::File& destination, juce::AudioFormatManager& formats, juce::String& error);

    // Ctrl+S can overwrite the current file only for lossless writable formats.
    bool canSaveInPlace() const;

    static bool isSaveExtension (const juce::String& extension);
    static constexpr const char* saveWildcard = "*.wav;*.flac;*.aiff;*.aif;*.ogg";

    // Selection, or the whole file when nothing is selected (Sound Forge rule).
    juce::Range<int> processRange() const;
    void applyProcess (wf::Process process);

    // Removes the selected samples; returns false when nothing is selected.
    bool deleteSelection();

    // Copy of the selected samples (empty when nothing is selected).
    juce::AudioBuffer<float> copySelection() const;

    // Mixes `source` (at `sourceRate`) into this file starting at `position`,
    // scaling the incoming audio by `sourceGain` and the existing audio it
    // overlaps by `destGain`. The file grows if the source runs past its end.
    // Selects the mixed range afterwards.
    void mixIn (const juce::AudioBuffer<float>& source, double sourceRate, juce::int64 position,
                float sourceGain, float destGain);

    // Markers are kept sorted; adding one at an existing position does nothing.
    void addMarker (juce::int64 position);
    void removeMarker (int index);

    bool canUndo() const  { return ! undoStack.empty(); }
    bool canRedo() const  { return ! redoStack.empty(); }
    void undo();
    void redo();

    juce::String describeFormat() const;

    // Called after the audio or the cursor/selection changes.
    std::function<void()> onChanged;

    void resized() override  { view.setBounds (getLocalBounds()); }

private:
    DataWindow() = default;

    void initialise (const juce::String& title);

    void pushUndo();
    void audioChanged();

    juce::File file;                 // empty for documents never saved
    juce::String title;
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    int bitDepth = 0;
    juce::String formatName;
    bool modified = false;

    std::vector<juce::int64> markers;

    // Undo keeps whole snapshots of the samples and markers.
    struct Snapshot
    {
        juce::AudioBuffer<float> audio;
        std::vector<juce::int64> markers;
    };

    void restore (Snapshot& from, std::vector<Snapshot>& saveTo);

    std::vector<Snapshot> undoStack, redoStack;
    WaveformView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DataWindow)
};
