#include "Processes.h"

namespace wf
{
namespace
{
constexpr float normalizeTarget = 0.98855309f; // -0.1 dBFS

void applyGain (juce::AudioBuffer<float>& buffer, juce::Range<int> range, float gain)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch, range.getStart()), gain, range.getLength());
}

float peakLevel (const juce::AudioBuffer<float>& buffer, juce::Range<int> range)
{
    float peak = 0.0f;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, range.getStart(), range.getLength()));

    return peak;
}
} // namespace

void applyProcess (Process process, juce::AudioBuffer<float>& buffer, juce::Range<int> range)
{
    range = range.getIntersectionWith ({ 0, buffer.getNumSamples() });
    const auto length = range.getLength();

    if (length <= 0)
        return;

    switch (process)
    {
        case Process::fadeIn:
        case Process::fadeOut:
        {
            const auto rampDown = process == Process::fadeOut;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* data = buffer.getWritePointer (ch, range.getStart());

                for (int i = 0; i < length; ++i)
                {
                    const auto t = length > 1 ? (float) i / (float) (length - 1) : 1.0f;
                    data[i] *= rampDown ? 1.0f - t : t;
                }
            }
            break;
        }

        case Process::mute:
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.clear (ch, range.getStart(), length);
            break;

        case Process::normalize:
            if (const auto peak = peakLevel (buffer, range); peak > 0.0f)
                applyGain (buffer, range, normalizeTarget / peak);
            break;

        case Process::reverse:
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* data = buffer.getWritePointer (ch, range.getStart());
                std::reverse (data, data + length);
            }
            break;

        case Process::invert:
            applyGain (buffer, range, -1.0f);
            break;

        case Process::dcOffset:
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* data = buffer.getWritePointer (ch, range.getStart());
                double sum = 0.0;

                for (int i = 0; i < length; ++i)
                    sum += data[i];

                juce::FloatVectorOperations::add (data, (float) (-sum / length), length);
            }
            break;

        case Process::gainUp:
            applyGain (buffer, range, juce::Decibels::decibelsToGain (3.0f));
            break;

        case Process::gainDown:
            applyGain (buffer, range, juce::Decibels::decibelsToGain (-3.0f));
            break;
    }
}
} // namespace wf
