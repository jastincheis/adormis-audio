#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Processes.h"
#include "SaveAsDialog.h"
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
    // on success, makes it this document's file. Without `format`, the current
    // bit depth is kept (Ctrl+S). A different sample rate in `format` converts
    // the document too (undoable), so it matches what was saved.
    bool saveTo (const juce::File& destination, juce::AudioFormatManager& formats, juce::String& error,
                 const wf::SaveFormat* format = nullptr);

    // Ctrl+S can overwrite the current file only for lossless writable formats.
    bool canSaveInPlace() const;

    static bool isSaveExtension (const juce::String& extension);

    // Selection, or the whole file when nothing is selected (Sound Forge rule).
    juce::Range<int> processRange() const;
    void applyProcess (wf::Process process);

    // Removes the selected samples; returns false when nothing is selected.
    bool deleteSelection();

    // Removes the markers picked in the view (undoable); false when none are.
    bool deleteSelectedMarkers();

    // Copy of the selected samples (empty when nothing is selected).
    juce::AudioBuffer<float> copySelection() const;

    // `source` converted to `rate` and `channels` (mono fans out, extra
    // channels are averaged down), ready to be added sample by sample.
    static juce::AudioBuffer<float> prepareForMix (const juce::AudioBuffer<float>& source, double sourceRate,
                                                   double rate, int channels);

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

    // "44,100 Hz, 16-bit, Stereo": the top-left line of the window.
    juce::String describeResolution() const;

    // Called after the audio or the cursor/selection changes.
    std::function<void()> onChanged;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    DataWindow() = default;

    void initialise (const juce::String& title);

    void pushUndo();
    void audioChanged();

    // Refreshes the resolution line and the Length / Cursor boxes.
    void updateInfo();

    juce::File file;                 // empty for documents never saved
    juce::String title;
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    int bitDepth = 0;
    bool floatingPoint = false;   // 32-bit float samples rather than integers
    juce::String formatName;
    bool modified = false;

    std::vector<juce::int64> markers;

    // Undo keeps whole snapshots of the samples, markers and sample rate
    // (Save As can convert the rate).
    struct Snapshot
    {
        juce::AudioBuffer<float> audio;
        std::vector<juce::int64> markers;
        double rate = 44100.0;
    };

    void restore (Snapshot& from, std::vector<Snapshot>& saveTo);

    std::vector<Snapshot> undoStack, redoStack;
    WaveformView view;

    // Top-left resolution; bottom-right file length and cursor position.
    juce::Label resolutionLabel, lengthBox, cursorBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DataWindow)
};
