#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// Takes the audio input on the audio thread: applies the input level, keeps
// the meter readings, and while recording queues the samples for the
// message thread (which appends them to the file).
class InputRecorder final : public juce::AudioIODeviceCallback
{
public:
    InputRecorder();

    void setGainDecibels (float decibels);
    float getGainDecibels() const   { return gainDecibels; }

    void setRecording (bool shouldRecord);
    bool isRecording() const        { return recording.load(); }

    // Highest peak / mean square per channel since the last call (meter, 30 Hz).
    struct Levels { float peak[2] {}, rms[2] {}; bool clipped = false; };
    Levels takeLevels();

    // Moves the queued samples into `destination` (resized to them); returns how many.
    int drain (juce::AudioBuffer<float>& destination);

    int getInputChannels() const    { return inputChannels.load(); }
    double getSampleRate() const    { return sampleRate.load(); }
    juce::String getDeviceName() const;
    bool overflowed()               { return overflow.exchange (false); }

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    float gainDecibels = 0.0f;
    std::atomic<float> gain { 1.0f };
    std::atomic<bool> recording { false }, overflow { false }, clipped { false };
    std::atomic<int> inputChannels { 0 };

    // A mono microphone often arrives as two channels with the right one
    // digitally silent; until the right channel carries anything, the left
    // is used for both, so a stereo take has the sound on both sides.
    bool rightHasSignal = false;
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<float> peak[2], sumSquares[2];
    std::atomic<int> squaresCount { 0 };

    juce::AbstractFifo fifo { 1 << 20 };          // ~21 s at 48 kHz between two drains
    juce::AudioBuffer<float> queue { 2, 1 << 20 };
    juce::AudioBuffer<float> scratch { 2, 8192 };

    juce::CriticalSection nameLock;
    juce::String deviceName;
};

struct RecordCallbacks
{
    std::function<void()> toggleRecording;       // the Record / Stop button
    std::function<void()> openAudioSettings;
    std::function<void (int channels)> setChannels;   // 1 or 2; only while the file is empty
    std::function<void (int bits)> setBits;           // 8, 12, 14, 16, 24, 32; only while empty
    std::function<int()> fileBits;
    std::function<void()> onClose;

    // "00:00:12.345" of the take and the whole file, and whether the file is still empty.
    std::function<double()> takeSeconds, fileSeconds;
    std::function<bool()> fileIsEmpty;
    std::function<int()> fileChannels;
    std::function<juce::String()> fileTitle;
};

// The Record window: input meter, input level slider, mono/stereo and Record / Stop.
std::unique_ptr<juce::DocumentWindow> createRecordWindow (InputRecorder& recorder, RecordCallbacks callbacks);
} // namespace wf
