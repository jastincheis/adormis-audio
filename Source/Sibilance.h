#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// De-esser: finds the sibilants ("s", "sh", "z", "ts") in a voice and turns
// each one down (or up) with its own bell filter that only acts while it
// lasts, fading in and out so nothing clicks.
namespace wf::sibilance
{
// One bell (a peaking filter).
struct Band
{
    double frequency = 6000.0;       // centre, Hz
    double gainDb = -6.0;            // < 0 quieter, > 0 louder
    double q = 2.0;                  // higher = narrower
};

struct Sibilant
{
    double start = 0.0, end = 0.0;   // seconds from the start of the audio
    double peakHz = 6000.0;          // where its energy is highest
    double levelDb = -60.0;          // its high-band level

    // Its bells, applied one after the other while it lasts (at least one).
    std::vector<Band> bands { Band {} };

    bool changesSound() const;       // any band with a gain other than 0 dB
};

constexpr int maxBands = 8;

constexpr double minFrequency = 1500.0, maxFrequency = 18000.0;
constexpr double minGainDb = -24.0, maxGainDb = 12.0;
constexpr double minQ = 0.4, maxQ = 12.0;

// `sensitivity` 0..1: higher finds more (and fainter) sibilants. Every one
// found starts with the bell on its peak, `defaultGainDb` deep.
std::vector<Sibilant> detect (const juce::AudioBuffer<float>& audio, double sampleRate, double sensitivity,
                              double defaultGainDb = -6.0);

// The audio with every sibilant's bell applied (0 dB ones are left alone).
juce::AudioBuffer<float> process (const juce::AudioBuffer<float>& audio, double sampleRate,
                                  const std::vector<Sibilant>& items);

// Applies one sibilant's bell to `audio` in place (also used for the preview).
void processOne (juce::AudioBuffer<float>& audio, double sampleRate, const Sibilant& item);

// Magnitude spectrum (dB, Hann, averaged) of [start, end) seconds, one value
// per bin of a 2048-point FFT; empty when too short.
std::vector<float> spectrum (const juce::AudioBuffer<float>& audio, double sampleRate, double start, double end);

// The response of all its bells at `hz` (dB), or of one band.
double bellResponseDb (double hz, const Sibilant& item, double sampleRate);
double bandResponseDb (double hz, const Band& band, double sampleRate);
} // namespace wf::sibilance
