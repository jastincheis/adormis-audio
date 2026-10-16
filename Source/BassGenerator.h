#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Analysis.h"

// MIDI bass line generator of the Bass Finder, ported from the Bass Key
// Finder page. Each style has idiomatic one-bar rhythm templates and melodic
// tendencies; a line is built as phrases with a form (A A' A C ...): a motif
// is composed once, re-mapped to each bar's chord, varied, and closed by a
// fill that leads into the next phrase. The three variants are three
// characters (Groove, Melodic, Syncopated), not three random rolls.
namespace wf::bass
{
struct StyleInfo
{
    const char* id;
    const char* name;
    const char* description;
};

const std::vector<StyleInfo>& styles();        // 10 styles; index = Settings::style
const char* characterName (int variant);       // "Groove", "Melodic", "Syncopated"

enum class Tone { byStyle, tr808, sub, synth, reese, pluck };
const char* toneName (Tone);

struct Settings
{
    int style = 0;              // trap
    int bars = 8;               // 4, 8 or 16
    int complexity = 2;         // 1 simple, 2 medium, 3 complex
    int octave = 0;             // -1, 0, +1
    Tone tone = Tone::tr808;
    bool followChords = true;   // false = the tonic in every bar
    int keyTonic = 5;
    bool minor = true;
    double bpm = 120.0;
    double downbeat = 0.0;      // seconds into the audio where bar 1 starts
    int startBar = 0;           // the bar of the audio the line starts on
    std::array<juce::uint32, 3> seeds { 11, 22, 33 };
};

// A note on the 16th-note grid: start and length in 16ths, MIDI pitch,
// velocity, and whether it slides from the previous note.
struct Note
{
    int start = 0, length = 1, pitch = 36, velocity = 100;
    bool glide = false;
};

Tone resolvedTone (const Settings&);   // byStyle -> the style's own sound
bool styleGlides (int style);

// Whole bars of the audio from the downbeat (a loop cut on the beat gives an exact count).
int totalBars (double durationSeconds, double downbeat, double bpm);

// The chord root (pitch class) of each generated bar, from the audio's
// chroma when following the chords.
std::vector<int> computeRoots (const Settings&, const KeyAnalysis* analysis, double durationSeconds);

std::array<std::vector<Note>, 3> generate (const Settings&, const std::vector<int>& roots);

// Standard MIDI file (type 0, 480 ticks per quarter) with tempo and 4/4.
juce::MemoryBlock midiFile (const std::vector<Note>& notes, double bpm, const juce::String& trackName, bool glide);

// The line played by the chosen sound, exactly `bars` long, stereo, not normalised.
juce::AudioBuffer<float> renderBass (const std::vector<Note>& notes, Tone tone, double bpm, int bars, double sampleRate);

// The audio from the chosen start bar, `bars` long (wrapping round the loop),
// with the same channel count as `audio`.
juce::AudioBuffer<float> sampleLoop (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate,
                                     const Settings&);

// "Bas_Fm_120bpm_trap": the start of exported file names.
juce::String fileTag (const Settings&);

// Root pitch of each bar's chord moved one scale step up (clicking a bar).
int nextScaleRoot (int root, int keyTonic, bool minor);
} // namespace wf::bass
