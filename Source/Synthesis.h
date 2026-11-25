#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Tools > Synthesis: tones made from nothing.
namespace wf::synth
{
enum class Wave { sine, square, sawtooth, triangle, whiteNoise, pinkNoise };

// Simple Synthesis: one waveform, its frequency sliding from startHz to endHz
// (straight on a musical scale), `amplitudeDb` peak, short fades at both ends
// so it starts and stops without a click.
struct SimpleSettings
{
    Wave wave = Wave::sine;
    double startHz = 440.0, endHz = 440.0;
    double seconds = 1.0, amplitudeDb = -6.0;
    int channels = 2;
};

// FM Synthesis: a carrier modulated by a second oscillator (frequency
// carrierHz * ratio). The modulation depth (index) slides from indexStart to
// indexEnd: high = bright, metallic; 0 = a plain sine. The level follows
// attack, then decays towards `sustain` (0..1) and fades out at the end.
struct FmSettings
{
    double carrierHz = 220.0, ratio = 2.0;
    double indexStart = 5.0, indexEnd = 1.0;
    double seconds = 2.0, amplitudeDb = -6.0;
    double attackMs = 10.0, sustain = 0.4;
    int channels = 2;
};

juce::AudioBuffer<float> simple (const SimpleSettings&, double sampleRate);
juce::AudioBuffer<float> fm (const FmSettings&, double sampleRate);

juce::StringArray waveNames();   // in Wave order
}
