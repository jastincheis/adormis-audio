#include "TimeStretch.h"
#include <rubberband/RubberBandStretcher.h>

namespace wf
{
std::optional<juce::AudioBuffer<float>> stretchAudio (const juce::AudioBuffer<float>& audio, juce::Range<int> range,
                                                      double sampleRate, const StretchSettings& settings,
                                                      const std::function<bool (float)>& progress)
{
    using RB = RubberBand::RubberBandStretcher;

    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    const auto channels = audio.getNumChannels();
    const auto inputLength = range.getLength();
    const auto outputLength = (int) std::llround ((double) inputLength * settings.timeRatio);

    if (channels < 1 || inputLength <= 0 || outputLength <= 0)
        return {};

    // Offline: the whole input is studied first, so the stretch is exact and
    // transients are placed where they belong. Channels together keeps the
    // stereo image of a mix.
    RB::Options options = RB::OptionProcessOffline | RB::OptionEngineFiner | RB::OptionChannelsTogether
                        | (settings.preserveFormants ? RB::OptionFormantPreserved : RB::OptionFormantShifted);

    RB stretcher ((size_t) sampleRate, (size_t) channels, options, settings.timeRatio,
                  std::pow (2.0, settings.semitones / 12.0));

    constexpr int block = 4096;
    stretcher.setExpectedInputDuration ((size_t) inputLength);
    stretcher.setMaxProcessSize (block);

    std::vector<const float*> in ((size_t) channels);

    auto inputAt = [&] (int offset)
    {
        for (int ch = 0; ch < channels; ++ch)
            in[(size_t) ch] = audio.getReadPointer (ch, range.getStart() + offset);

        return in.data();
    };

    // Pass 1 (about a fifth of the time): study.
    for (int done = 0; done < inputLength; done += block)
    {
        const auto count = std::min (block, inputLength - done);
        stretcher.study (inputAt (done), (size_t) count, done + count >= inputLength);

        if (progress != nullptr && ! progress (0.2f * (float) (done + count) / (float) inputLength))
            return {};
    }

    // Pass 2: process and collect the output.
    juce::AudioBuffer<float> output (channels, outputLength);
    output.clear();
    juce::AudioBuffer<float> chunk (channels, block);
    std::vector<float*> out ((size_t) channels);
    int written = 0;

    auto collect = [&]
    {
        for (int available = stretcher.available(); available > 0; available = stretcher.available())
        {
            const auto count = std::min (available, block);

            for (int ch = 0; ch < channels; ++ch)
                out[(size_t) ch] = chunk.getWritePointer (ch);

            const auto got = (int) stretcher.retrieve (out.data(), (size_t) count);
            const auto keep = std::min (got, outputLength - written);

            for (int ch = 0; ch < channels && keep > 0; ++ch)
                output.copyFrom (ch, written, chunk, ch, 0, keep);

            written += std::max (0, keep);
        }
    };

    for (int done = 0; done < inputLength; done += block)
    {
        const auto count = std::min (block, inputLength - done);
        stretcher.process (inputAt (done), (size_t) count, done + count >= inputLength);
        collect();

        if (progress != nullptr && ! progress (0.2f + 0.8f * (float) (done + count) / (float) inputLength))
            return {};
    }

    // Whatever is still in the pipeline after the last block.
    collect();

    return output;
}
} // namespace wf
