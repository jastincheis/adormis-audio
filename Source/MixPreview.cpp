#include "MixPreview.h"

namespace wf
{
MixPreviewSource::MixPreviewSource (juce::AudioBuffer<float> destRegion, juce::AudioBuffer<float> preparedSource,
                                    int offset, std::shared_ptr<MixGains> sharedGains)
    : dest (std::move (destRegion)),
      source (std::move (preparedSource)),
      sourceOffset (offset),
      gains (std::move (sharedGains)),
      totalLength (juce::jmax ((juce::int64) dest.getNumSamples(), (juce::int64) (offset + source.getNumSamples())))
{
}

void MixPreviewSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    const auto sourceGain = gains->source.load();
    const auto destGain = gains->dest.load();
    const auto start = position.load();
    // A mono destination is heard in the centre (both speakers); the prepared
    // source already has the destination's channel count.
    const auto mono = dest.getNumChannels() == 1;
    const auto channels = mono ? info.buffer->getNumChannels()
                               : juce::jmin (info.buffer->getNumChannels(), juce::jmax (dest.getNumChannels(), source.getNumChannels()));

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* out = info.buffer->getWritePointer (ch, info.startSample);
        const auto from = mono ? 0 : ch;
        const auto* d = from < dest.getNumChannels() ? dest.getReadPointer (from) : nullptr;
        const auto* s = from < source.getNumChannels() ? source.getReadPointer (from) : nullptr;

        for (int i = 0; i < info.numSamples; ++i)
        {
            const auto p = start + i;
            const auto sp = p - sourceOffset;
            const auto inSource = s != nullptr && sp >= 0 && sp < source.getNumSamples();
            const auto destSample = d != nullptr && p < dest.getNumSamples() ? d[p] : 0.0f;

            // The destination volume only applies where the two overlap.
            out[i] = destSample * (inSource ? destGain : 1.0f) + (inSource ? s[sp] * sourceGain : 0.0f);
        }
    }

    position = start + info.numSamples;
}
} // namespace wf
