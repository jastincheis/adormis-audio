#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace wf
{
// Tempo and key guesses for a stretch of audio (all channels mixed to mono).
// Ported from the Bass Key Finder page: spectral-flux beat tracking and
// Krumhansl key profiles over a tuning-corrected chroma.

struct TempoEstimate
{
    double bpm = 120.0;
    int loopBars = 0;        // > 0 when the audio is exactly that many 4/4 bars long
    double downbeat = 0.0;   // seconds from the start to the first beat of a bar
};

struct KeyEstimate
{
    int tonic = 0;        // 0 = C ... 11 = B
    bool minor = false;
    bool confident = false;
    int tuningCents = 0;  // how far the recording sits from A = 440 Hz
};

// The full key analysis behind detectKey(): the energy per note (tonal
// content and bass below 200 Hz), every key ranked, and the same per moment
// in time so the chord of each bar can be found.
struct ChromaFrame
{
    double time = 0.0;   // seconds from the start of the range
    std::array<float, 12> chroma {}, bass {};
};

struct KeyCandidate
{
    int tonic = 0;
    bool minor = false;
    double score = 0.0;   // correlation with the key profile, about -1.35 ... 1.35
};

struct KeyAnalysis
{
    std::array<double, 12> chroma {}, bass {};
    int tuningCents = 0;
    std::vector<KeyCandidate> keys;   // all 24, best first
    std::vector<ChromaFrame> frames;
};

std::optional<KeyAnalysis> analyseKey (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate);

// The scale steps of a major or natural minor key.
const std::array<int, 7>& scaleSteps (bool minor);

std::optional<TempoEstimate> detectTempo (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate);
std::optional<KeyEstimate> detectKey (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate);

// "C", "C#" / "Db" (flats where the key signature uses them).
juce::String noteName (int pitchClass, bool flats);
bool keyUsesFlats (int tonic, bool minor);
juce::String keyName (int tonic, bool minor);   // "F minor"
juce::String romanianNoteName (int pitchClass, bool flats);   // "Fa", "Si♭"
double noteFrequency (int pitchClass, int octave, int cents);   // A4 = 440 Hz
} // namespace wf
