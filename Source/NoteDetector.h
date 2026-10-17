#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace wf
{
// Note detector of the Bass Finder: how far off each played note is, and
// a correction that moves single notes by some cents without changing the
// timing or the length. Ported from the Bass Key Finder page (YIN pitch
// tracking, a long spectrum per note to refine the cents, and a two-tap
// delay-line shifter).

enum class NoteSource
{
    bass,     // 28 ... 300 Hz; correcting moves only the band below ~450 Hz
    melody    // 55 ... 1100 Hz; correcting moves the whole signal during the note
};

// Notes and cents are measured against a reference A: 440 Hz, or the
// recording's own tuning (referenceCents away from 440 Hz), so a song tuned to
// A = 432 Hz isn't "out of tune" everywhere.
struct PitchFrame
{
    double time = 0.0;          // seconds from the start of the analysed range
    std::optional<double> midi; // fractional MIDI note against the reference, empty when unclear
    float clarity = 0.0f;       // 0..1, how periodic the frame is
};

struct DetectedNote
{
    double start = 0.0, end = 0.0;   // seconds from the start of the range
    int note = 0;                    // MIDI note number
    int cents = 0;                   // measured, against the reference A
    double hz = 0.0;                 // measured frequency (absolute)
    int shift = 0;                   // correction in cents (0 = none)

    // The note and cents it will have after the correction.
    std::pair<int, int> corrected() const;
};

struct NoteTrack
{
    std::vector<PitchFrame> frames;
    double hopSeconds = 0.02;
    std::vector<DetectedNote> notes;
};

// `progress` gets 0..1 and returns false to cancel (then it returns empty).
std::optional<NoteTrack> detectNotes (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate,
                                      NoteSource source, const std::function<bool (float)>& progress, int referenceCents = 0);

// The audio of `range` with every note that has a shift moved by it.
juce::AudioBuffer<float> renderCorrection (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate,
                                           const std::vector<DetectedNote>& notes, NoteSource source);

// "ok" within the tolerance, "warn" up to 25 cents, "bad" beyond.
enum class TuneStatus { ok, warn, bad };
TuneStatus tuneStatus (int cents, int tolerance);

struct AutoCorrect
{
    int strength = 100;      // percent of the error that is removed: 50, 75, 100
    bool allNotes = false;   // false = only the notes outside the tolerance
    bool snapToKey = false;  // also move notes outside the key to the nearest scale note
};

// Sets the shifts; returns how many notes changed.
int autoCorrect (std::vector<DetectedNote>& notes, const AutoCorrect& settings, int tolerance, int keyTonic, bool minor);

bool isInKey (int midiNote, int keyTonic, bool minor);
juce::String midiNoteName (int midiNote, bool flats);   // "F2"
juce::String formatCents (int cents);                   // "+12¢", "−3¢", "±0¢"
} // namespace wf
