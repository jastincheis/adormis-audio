#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Loudness after ITU-R BS.1770-4 / EBU R128 (LUFS), loudness range after
// EBU Tech 3342 (LU), true peak after BS.1770 Annex 2 (dBTP), plus plain
// sample peak and RMS per channel. Used live by the meters (fed the audio
// being played) and offline by Statistics and Normalize.
namespace wf
{
class LoudnessMeter
{
public:
    static constexpr double silence = -200.0;   // what a measure is before there's audio

    void prepare (double sampleRate, int numChannels);
    void reset();

    void process (const juce::AudioBuffer<float>& audio, int start, int numSamples);
    void process (const float* const* channels, int numChannels, int numSamples);

    double momentary() const;          // last 400 ms, LUFS
    double shortTerm() const;          // last 3 s, LUFS
    double integrated() const;         // everything since reset, gated, LUFS
    double loudnessRange() const;      // LRA, LU
    double maxMomentary() const   { return maxMomentaryLufs; }
    double maxShortTerm() const   { return maxShortTermLufs; }

    int getNumChannels() const    { return (int) channels.size(); }
    double truePeak (int channel) const;      // dBTP
    double truePeak() const;                  // highest of all channels
    double samplePeak (int channel) const;    // dBFS
    double rms (int channel) const;           // dBFS, since reset (sine at 0 dBFS = -3 dB)
    juce::int64 samplesProcessed() const   { return totalSamples; }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;

        double run (double x)
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    struct Channel
    {
        Biquad shelf, highPass;     // the K-weighting
        double weightedSum = 0.0;   // filtered energy of the 100 ms step being filled
        std::vector<float> history; // last input samples, for true-peak interpolation
        int historyPos = 0;
        double truePeak = 0.0, samplePeak = 0.0, sumSquares = 0.0;
    };

    void finishStep();
    double truePeakOfSample (Channel&, float sample);

    double rate = 48000.0;
    std::vector<Channel> channels;
    int stepLength = 4800;      // 100 ms
    int stepFill = 0;
    juce::int64 totalSamples = 0;

    // Energies (mean square, channel-summed) of the last 30 steps (3 s).
    std::vector<double> steps;
    int stepCount = 0;

    // Gating blocks: 400 ms with 75% overlap (one per step) for the
    // integrated loudness, 3 s ones (one per step) for the range.
    std::vector<double> momentaryBlocks, shortTermBlocks;
    double maxMomentaryLufs = silence, maxShortTermLufs = silence;

    // True peak: polyphase interpolation filter.
    int oversampling = 4;
    int tapsPerPhase = 16;
    std::vector<std::vector<float>> phases;
};

// Everything Tools > Statistics shows, for a range of a buffer.
struct AudioStatistics
{
    struct PerChannel
    {
        float minimum = 0.0f, maximum = 0.0f;
        juce::int64 peakPosition = 0;   // where the highest absolute sample is
        double peakDb = LoudnessMeter::silence, truePeakDb = LoudnessMeter::silence, rmsDb = LoudnessMeter::silence;
        double dcOffset = 0.0;          // mean value (-1..1)
        juce::int64 zeroCrossings = 0;
        juce::int64 clippedSamples = 0; // at or above -0.1 dBFS
    };

    std::vector<PerChannel> channels;
    double integrated = LoudnessMeter::silence, range = 0.0;
    double maxMomentary = LoudnessMeter::silence, maxShortTerm = LoudnessMeter::silence;
    double truePeakDb = LoudnessMeter::silence;
    juce::int64 length = 0;
};

// `progress` gets 0..1 and returns false to cancel (then the result is empty).
std::optional<AudioStatistics> measure (const juce::AudioBuffer<float>& audio, double sampleRate, juce::Range<int> range,
                                        const std::function<bool (float)>& progress = {});

// "-14.2 LUFS"; "-inf" below -70 (the absolute gate).
juce::String formatLufs (double lufs);
juce::String formatDb (double db, const char* unit = "dB");
} // namespace wf
