#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>

namespace wf
{
// The audio a tool window (Bass Finder, De-esser) works on: a copy of the active file's selection
// (or the whole file), and how to put a corrected version back.
struct BassFinderSource
{
    juce::String title;                // "Song.wav (selection)"
    bool isSelection = false;          // a selection, not the whole file
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;

    // Replaces the analysed audio in the file (undoable); false when the file is gone.
    std::function<bool (const juce::AudioBuffer<float>&)> applyToFile;

    // Like applyToFile, but `audio` may be longer: what is past the end (an
    // effect's tail) is added onto the audio that follows, growing the file
    // if needed.
    std::function<bool (const juce::AudioBuffer<float>&)> applyWithTail;

    // Where the file's cursor / playhead is, in seconds from the start of the audio.
    std::function<double()> cursorSeconds;

    // Selects and shows this stretch (seconds from the start of the audio) in the file's window.
    std::function<void (double start, double end)> showRange;

    // Moves the file window's playhead to what the tool is playing (seconds
    // from the start of the audio; negative hides it).
    std::function<void (double seconds)> showPlayhead;

    // Where the analysed audio starts in its file, and how long the file is (seconds).
    double startInFile = 0.0, fileDuration = 0.0;

    // The loudest sample of each of `columns` equal pieces of the whole file.
    std::function<std::vector<float> (int columns)> filePeaks;

    // Another stretch of the same file (seconds from its start; end <= start =
    // the whole file), also selected in the file's window; empty when the file is gone.
    std::function<std::optional<BassFinderSource> (double start, double end)> otherRange;
};

// What the Bass Finder needs from the main window.
struct BassFinderHost
{
    std::function<std::optional<BassFinderSource>()> currentSource;   // the active file, for "Analyse again"

    // Plays a stretch of audio (looping or not); the position is in samples, -1 when stopped.
    std::function<void (std::unique_ptr<juce::PositionableAudioSource>, double sampleRate, int channels)> play;
    std::function<void()> stop;
    std::function<juce::int64()> playPosition;

    std::function<void (const juce::String& title, juce::AudioBuffer<float>, double sampleRate,
                        std::vector<juce::int64> markers)> openAsNewFile;

    // Asks where to save (the suggested file gives the folder and name).
    std::function<void (const juce::String& title, const juce::File& suggested, const juce::String& pattern,
                        std::function<void (juce::File)> onChosen)> chooseSaveFile;

    // Runs `work` (which reports 0..1 and returns false when cancelled) behind a
    // progress bar, then calls `done` on the message thread (true = finished).
    std::function<void (const juce::String& title, std::function<bool (std::function<bool (float)>)> work,
                        std::function<void (bool finished)> done)> runJob;
};

// Tools > Bass Finder: key, tempo and bass tuning; a note detector with pitch
// correction; and a MIDI bass line generator. A floating, resizable window.
std::unique_ptr<juce::DocumentWindow> createBassFinderWindow (BassFinderSource source, BassFinderHost host,
                                                              std::function<void()> onClose);

// A buffer played from memory, optionally looped, with an optional second
// layer (the generated bass) whose gains can change while it plays.
class LayeredPlayer final : public juce::PositionableAudioSource
{
public:
    LayeredPlayer (juce::AudioBuffer<float> main, juce::AudioBuffer<float> layer, bool loop);

    std::atomic<float> mainGain { 1.0f }, layerGain { 1.0f };

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;
    void setNextReadPosition (juce::int64 p) override   { position = p; }
    juce::int64 getNextReadPosition() const override    { return position; }
    juce::int64 getTotalLength() const override         { return looping ? std::numeric_limits<juce::int64>::max() : length; }
    bool isLooping() const override                     { return looping; }

    juce::int64 getLength() const                       { return length; }

private:
    juce::AudioBuffer<float> mainAudio, layerAudio;
    bool looping;
    juce::int64 length;
    std::atomic<juce::int64> position { 0 };
};
} // namespace wf
