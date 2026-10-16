#pragma once

#include "Effects.h"

// Package C effects, all with the live preview of the effect windows:
// limiter, noise gate, chorus / flanger / vibrato and distortion.
namespace wf::fx
{
// Brickwall limiter with look-ahead: nothing gets over the ceiling (true
// peak, measured 4x oversampled, when `truePeak`). `inputDb` drives it
// harder (louder result).
struct LimiterParams
{
    double inputDb = 0.0, ceilingDb = -1.0, releaseMs = 80.0;
    bool truePeak = true;
};

// Turns the audio down by `rangeDb` while it stays quiet (hiss or hum between
// phrases): opens at the threshold, closes `holdMs` after the level has gone
// 4 dB under it (the gap keeps it from fluttering).
struct GateParams
{
    double thresholdDb = -45.0, rangeDb = -80.0;
    double attackMs = 1.0, holdMs = 60.0, releaseMs = 150.0;
};

// One modulated delay: chorus (a few voices, 15-30 ms), flanger (a short
// delay with feedback) or vibrato (the delayed copy alone: pitch wobble).
struct ModulationParams
{
    enum Mode { chorus, flanger, vibrato };

    Mode mode = chorus;
    double rateHz = 0.8, depthMs = 4.0, delayMs = 18.0;
    double feedback = 0.0;    // -0.95 .. 0.95 (flanger)
    double mix = 0.5;         // 0 = dry only, 1 = effect only
    double spread = 0.5;      // stereo: 0 = same in both channels, 1 = opposite
    int voices = 2;           // chorus: 1..4
};

ModulationParams modulationDefaults (ModulationParams::Mode);

struct DistortionParams
{
    enum Type { soft, hard, tube, fold };

    Type type = soft;
    double driveDb = 12.0, toneHz = 8000.0, outputDb = -6.0, mix = 1.0;
};

const char* distortionTypeName (DistortionParams::Type);

// The distortion's curve: output for input `x` (-1..1, after the drive).
double distortionCurve (DistortionParams::Type, double x);

std::shared_ptr<Processor> makeLimiter();
std::shared_ptr<Processor> makeGate();
std::shared_ptr<Processor> makeModulation();
std::shared_ptr<Processor> makeDistortion();

void setParams (Processor&, const LimiterParams&);
void setParams (Processor&, const GateParams&);
void setParams (Processor&, const ModulationParams&);
void setParams (Processor&, const DistortionParams&);

// Limiter input gain (dB) that brings `audio` to `targetLufs` integrated
// with the given ceiling and release, or empty if it can't (silent, or
// more than +30 dB needed). Renders a few times; `progress` may cancel.
std::optional<double> findLimiterGain (const juce::AudioBuffer<float>& audio, double sampleRate, LimiterParams params,
                                       double targetLufs, const std::function<bool (float)>& progress);
} // namespace wf::fx
