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

    // A new, unsaved, silent 16-bit document with the given markers (File > New).
    // A new, unsaved 24-bit document holding `audio` (the Bass Finder's renders).
    // `barStarts` are the markers that begin a bar (drawn in another colour).
    static std::unique_ptr<DataWindow> fromAudio (const juce::String& title, double sampleRate, juce::AudioBuffer<float> audio,
                                                  std::vector<juce::int64> markers, std::vector<juce::int64> barStarts = {});

    // A new, empty (0 samples) 24-bit document to record into (Ctrl+R).
    static std::unique_ptr<DataWindow> createEmpty (const juce::String& title, double sampleRate, int channels);

    bool isEmpty() const   { return buffer.getNumSamples() == 0; }

    // While still empty, the format the recording will have.
    void setEmptyFormat (double sampleRate, int channels);

    // The resolution of a new file: 8, 12, 14, 16, 24 or 32 (float) bits.
    // 12 and 14 bits (the sound of old samplers) are stored in 16-bit files;
    // recorded audio is rounded to that many steps.
    void setResolution (int bits);
    int getResolution() const   { return resolutionBits > 0 ? resolutionBits : bitDepth; }
    static const std::vector<int>& resolutionChoices();

    // Rounds `numSamples` from `start` of every channel to `bits` (no dither;
    // 32 = float, unchanged).
    static void quantize (juce::AudioBuffer<float>& audio, int start, int numSamples, int bits);

    // Recording: one take = one undo step. Samples are added at the end.
    void beginTake();
    void appendAudio (const juce::AudioBuffer<float>& audio, int numSamples);
    void endTake();
    juce::int64 getTakeLength() const   { return (juce::int64) buffer.getNumSamples() - takeStart; }

    static std::unique_ptr<DataWindow> createSilent (const juce::String& title, double sampleRate, int channels,
                                                     juce::int64 numSamples, std::vector<juce::int64> markers,
                                                     std::vector<juce::int64> barStarts = {}, int bits = 16);

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
    void applyVolume (float decibels);

    // Puts `audio` (same channels, any length: a time stretch of `range`) in
    // place of `range`, undoably. Markers inside it move with the stretch
    // (`timeRatio` = new length / old length), later ones shift. Selects the
    // new audio if `range` was a selection.
    void replaceRange (juce::Range<int> range, const juce::AudioBuffer<float>& audio, double timeRatio);

    // An effect's result for `range`: its first range-length samples replace
    // the range, the rest (the tail) is added onto what follows; the file
    // grows when the tail runs past the end. Undoable; keeps the selection.
    void applyEffect (juce::Range<int> range, const juce::AudioBuffer<float>& processed);

    // "Selection, left channel", "whole file"...: what a process would change.
    juce::String describeProcessTarget() const;

    // Removes the selected samples; returns false when nothing is selected.
    bool deleteSelection();

    // Paste: `source` (at `sourceRate`, converted to this file's rate and
    // channels) replaces the selection, or goes in at the cursor when nothing
    // is selected. Later markers move along. Undoable; selects what came in.
    void pasteAudio (const juce::AudioBuffer<float>& source, double sourceRate);

    // Trim/Crop: keeps only the selection (markers inside it move along).
    // False when nothing, or only one channel, is selected.
    bool trimToSelection();

    // Insert > Silence: `seconds` of silence at `position` (undoable).
    void insertSilence (juce::int64 position, double seconds);

    // Process > Resample: converts the whole file to `newRate` (undoable).
    void resample (double newRate);

    // Process > Bit-Depth Converter: 8, 16, 24 or 32 (float). Rounds the
    // samples to the new depth, with TPDF dither when `dither` (undoable).
    void convertBitDepth (int bits, bool dither);
    int getBitDepth() const      { return bitDepth; }
    bool isFloatingPoint() const { return floatingPoint; }

    // Process > Channel Converter.
    enum class ChannelConversion { stereoToMono, monoToStereo, swap, leftOnly, rightOnly };
    bool convertChannels (ChannelConversion);

    // Process > Auto Trim/Crop: removes the start and end quieter than
    // `thresholdDb`; false when there's nothing to trim (or it's all quiet).
    bool autoTrim (float thresholdDb);

    // Tools > Detect Clipping: a marker at the start of every stretch of at
    // least `minRun` samples at or above `levelDb`; returns how many.
    int markClipping (float levelDb, int minRun);

    // Tools > Copy Other Channel: the selected channel's selection gets the
    // other channel's audio (stereo only; undoable).
    bool copyOtherChannel();

    // Removes the markers picked in the view (undoable); false when none are.
    bool deleteSelectedMarkers();

    // Copy of the selected samples (empty when nothing is selected); a
    // selection on one channel gives a mono buffer.
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

    // Runs `fn` (undoably) on the process range of the picked channel, or of
    // every channel when none is picked.
    void processChannels (const std::function<void (juce::AudioBuffer<float>&, juce::Range<int>)>& fn);

    // Refreshes the resolution line and the Length / Cursor boxes.
    void updateInfo();

    juce::File file;                 // empty for documents never saved
    juce::String title;
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    int bitDepth = 0;
    int resolutionBits = 0;       // 12 / 14 inside a 16-bit file; 0 = same as bitDepth
    bool floatingPoint = false;   // 32-bit float samples rather than integers
    juce::String formatName;
    bool modified = false;

    juce::int64 takeStart = 0;   // where the current recording take began
    size_t capacityBytes = 0;    // memory reserved for the take being recorded

    std::vector<juce::int64> markers;
    std::vector<juce::int64> barStarts;   // sorted; the markers that begin a bar

    // Moves (or, for an empty result, removes) every marker, bar starts included.
    void remapMarkers (const std::function<std::optional<juce::int64> (juce::int64)>& fn);

    // Bar starts whose marker has gone are forgotten.
    void pruneBarStarts();

    // Undo keeps whole snapshots of the samples, markers and sample rate
    // (Save As can convert the rate).
    struct Snapshot
    {
        juce::AudioBuffer<float> audio;
        std::vector<juce::int64> markers, barStarts;
        double rate = 44100.0;
        int bitDepth = 0, resolutionBits = 0;
        bool floatingPoint = false;
    };


    void restore (Snapshot& from, std::vector<Snapshot>& saveTo);

    std::vector<Snapshot> undoStack, redoStack;
    WaveformView view;

    // Top-left resolution; bottom-right file length and cursor position.
    juce::Label resolutionLabel, lengthBox, cursorBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DataWindow)
};
