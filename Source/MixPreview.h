#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace wf
{
// Volumes shared between the Mix dialog (message thread) and the preview
// (audio thread), so moving a slider is heard straight away.
struct MixGains
{
    std::atomic<float> source { 1.0f };
    std::atomic<float> dest { 1.0f };
};

// Plays what a mix will sound like without changing the file: a stretch of
// the destination with the prepared source added at `sourceOffset`.
class MixPreviewSource final : public juce::PositionableAudioSource
{
public:
    MixPreviewSource (juce::AudioBuffer<float> destRegion, juce::AudioBuffer<float> preparedSource,
                      int sourceOffset, std::shared_ptr<MixGains> gains);

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    void setNextReadPosition (juce::int64 newPosition) override  { position = newPosition; }
    juce::int64 getNextReadPosition() const override             { return position; }
    juce::int64 getTotalLength() const override                  { return totalLength; }
    bool isLooping() const override                              { return false; }

private:
    juce::AudioBuffer<float> dest, source;
    int sourceOffset;
    std::shared_ptr<MixGains> gains;
    juce::int64 totalLength;
    std::atomic<juce::int64> position { 0 };
};
} // namespace wf
