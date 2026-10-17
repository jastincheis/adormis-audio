#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Loudness.h"

namespace wf
{
// Sits between the audio device and the player: runs `inner` and copies
// what it sends to the speakers into a FIFO for the meters (no locks, no
// allocation on the audio thread).
class MeterTap final : public juce::AudioIODeviceCallback
{
public:
    explicit MeterTap (juce::AudioIODeviceCallback& innerCallback) : inner (innerCallback) {}

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // Message thread: takes what has been played since the last call (two
    // channels; a mono output is copied to both). Returns how many samples.
    int read (juce::AudioBuffer<float>& destination);

    double getSampleRate() const   { return sampleRate.load(); }
    void setEnabled (bool shouldCopy)   { enabled.store (shouldCopy); }

    // A second copy for View > Spectrum Analysis (its own reader).
    int readAnalysis (juce::AudioBuffer<float>& destination);
    void setAnalysisEnabled (bool shouldCopy)   { analysisEnabled.store (shouldCopy); }

private:
    static constexpr int capacity = 1 << 17;   // > 0.6 s at 192 kHz

    struct Copy
    {
        juce::AbstractFifo fifo { capacity };
        juce::AudioBuffer<float> ring { 2, capacity };

        void write (const float* const* outputs, int numOutputs, int numSamples);
        int read (juce::AudioBuffer<float>& destination);
    };

    juce::AudioIODeviceCallback& inner;
    Copy meters, analysis;
    std::atomic<double> sampleRate { 0.0 };
    std::atomic<bool> enabled { false }, analysisEnabled { false };
};

// The meters docked on the right of the main window: Channel Meters (peak,
// RMS, peak hold, clip lights per channel; View > Channel Meters, Alt+5) and
// Loudness Meters (momentary, short-term, integrated LUFS, loudness range,
// true peak and a minute of history; View > Loudness Meters, Alt+6).
class MeterPanel final : public juce::Component,
                         public juce::SettableTooltipClient,
                         private juce::Timer
{
public:
    explicit MeterPanel (MeterTap& tap);
    ~MeterPanel() override;

    void setSections (bool channelMeters, bool loudnessMeters);
    bool showsChannels() const    { return channelsShown; }
    bool showsLoudness() const    { return loudnessShown; }
    bool showsAnything() const    { return channelsShown || loudnessShown; }

    // Starts a new measurement (playback start, the Reset button).
    void resetLoudness();
    void resetPeaks();

    static constexpr int preferredWidth = 236;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void paintChannels (juce::Graphics&, juce::Rectangle<int> area);
    void paintLoudness (juce::Graphics&, juce::Rectangle<int> area);
    juce::Colour integratedColour (double lufs) const;

    MeterTap& tap;
    LoudnessMeter loudness;
    double preparedRate = 0.0;
    juce::AudioBuffer<float> incoming { 2, 1 << 17 };

    struct ChannelState
    {
        double peak = -100.0;        // shown bar (dB): fast up, slow fall
        double meanSquare = 0.0;     // RMS over ~300 ms
        double hold = -100.0;        // peak-hold line
        double holdTime = 0.0;       // seconds the hold line still stays
        double maxPeak = -100.0;     // since reset (the number above the bar)
        bool clipped = false;
    };

    ChannelState state[2];
    std::vector<float> history;   // short-term loudness, 10 per second, last minute
    int historyTick = 0;

    bool channelsShown = true, loudnessShown = true;
    juce::Rectangle<int> channelsArea, loudnessArea, peakNumbersArea;

    juce::ComboBox target;
    juce::TextButton resetButton { "Reset" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterPanel)
};
} // namespace wf
