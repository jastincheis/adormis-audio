#pragma once

#include "Effects.h"

// Package D, repair tools with the live preview of the effect windows:
// noise reduction (from a noise print), hum removal, click / crackle removal
// and clipped peak restoration.
namespace wf::fx
{
//==============================================================================
// The noise's average spectrum: one magnitude per FFT bin, per channel.
struct NoiseProfile
{
    double sampleRate = 44100.0;
    int fftSize = 4096;
    std::vector<std::vector<float>> magnitude;   // [channel][bin], fftSize / 2 + 1 bins
    juce::String source;                          // "captured from 0.8 s of Song.wav" / "learned"
};

int noiseFftSize (double sampleRate);   // ~90 ms

// The noise print of `audio` (all of it is taken as noise), or, with
// `quietestOnly`, learned from its quietest moments in every band (when no
// noise-only stretch was captured).
std::shared_ptr<NoiseProfile> makeNoiseProfile (const juce::AudioBuffer<float>& audio, double sampleRate, bool quietestOnly);

// The noise print captured with Tools > Noise Reduction > Capture Noise Print
// (kept until the next capture); null when none.
std::shared_ptr<const NoiseProfile> capturedNoiseProfile();
void setCapturedNoiseProfile (std::shared_ptr<const NoiseProfile>);

struct NoiseReductionParams
{
    std::shared_ptr<const NoiseProfile> profile;
    double reductionDb = 15.0;    // how far the noise goes down
    double thresholdDb = 4.0;     // how far above the noise print still counts as noise
    double smoothing = 0.5;       // 0..1, against "musical noise" (warbling)
    bool noiseOnly = false;       // hear what is taken out
};

//==============================================================================
struct HumParams
{
    double frequency = 50.0;      // mains: 50 Hz (Europe) or 60 Hz (Americas)
    int harmonics = 8;            // the fundamental and its multiples
    double widthHz = 2.0;         // of each notch
    bool removeRumble = false;    // also a high-pass at 30 Hz
};

// The mains hum in `audio` (45..65 Hz, to 0.02 Hz) and how far it stands
// out (dB); empty when there's no clear hum.
struct HumEstimate { double frequency = 50.0, strengthDb = 0.0; };
std::optional<HumEstimate> detectHum (const juce::AudioBuffer<float>& audio, double sampleRate);

//==============================================================================
struct DeclickParams
{
    double sensitivity = 50.0;    // 0..100: higher finds smaller clicks (crackle)
    double maxClickMs = 1.5;      // longer bursts are left alone (they're music)
};

struct ClipRestoreParams
{
    double clipLevelDb = -0.1;    // samples at or above this count as clipped
    double outputDb = -3.0;       // room for the restored peaks
};

// The level the audio was clipped at: its highest sample, when enough
// samples sit right at it (runs of 2+), else empty. `runs` gets their count.
std::optional<double> detectClipLevelDb (const juce::AudioBuffer<float>& audio, int* runs = nullptr);

std::shared_ptr<Processor> makeNoiseReduction();
std::shared_ptr<Processor> makeHumRemoval();
std::shared_ptr<Processor> makeDeclick();
std::shared_ptr<Processor> makeClipRestore();

void setParams (Processor&, const NoiseReductionParams&);
void setParams (Processor&, const HumParams&);
void setParams (Processor&, const DeclickParams&);
void setParams (Processor&, const ClipRestoreParams&);

// Clicks / clipped runs repaired since the last reset (declick, clip restore).
int repairsMade (const Processor&);
} // namespace wf::fx
