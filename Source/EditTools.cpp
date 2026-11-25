#include "EditTools.h"

namespace wf::edittools
{
juce::AudioBuffer<float> interpolate (const juce::AudioBuffer<float>& audio, juce::Range<int> range)
{
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    juce::AudioBuffer<float> out (audio.getNumChannels(), range.getLength());
    const auto n = range.getLength();

    for (int c = 0; c < audio.getNumChannels(); ++c)
    {
        const auto* s = audio.getReadPointer (c);
        auto at = [&] (int i) { return (double) s[juce::jlimit (0, audio.getNumSamples() - 1, i)]; };
        const auto a = range.getStart() - 1, b = range.getEnd();     // the good samples on each side
        const auto p0 = at (a), p1 = at (b);
        const auto span = (double) (n + 1);
        const auto m0 = (at (a) - at (a - 1)) * span, m1 = (at (b + 1) - at (b)) * span;   // their slopes
        auto* d = out.getWritePointer (c);

        for (int i = 0; i < n; ++i)
        {
            const auto t = (i + 1) / span, t2 = t * t, t3 = t2 * t;   // cubic Hermite
            d[i] = (float) ((2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1);
        }
    }

    return out;
}

juce::AudioBuffer<float> replaceWithNeighbour (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate)
{
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    const auto n = range.getLength();
    juce::AudioBuffer<float> out (audio.getNumChannels(), n);

    // the same length from just before, or else from just after
    auto from = range.getStart() - n;
    if (from < 0)
        from = std::min (range.getEnd(), audio.getNumSamples() - n);
    from = std::max (0, from);

    const auto fade = std::min (n / 2, (int) (rate * 0.003));

    for (int c = 0; c < audio.getNumChannels(); ++c)
    {
        out.copyFrom (c, 0, audio, c, from, std::min (n, audio.getNumSamples() - from));
        auto* d = out.getWritePointer (c);
        const auto* orig = audio.getReadPointer (c, range.getStart());

        for (int i = 0; i < fade; ++i)
        {
            const auto g = (float) (i + 1) / (float) (fade + 1);
            d[i] = orig[i] * (1.0f - g) + d[i] * g;
            d[n - 1 - i] = orig[n - 1 - i] * (1.0f - g) + d[n - 1 - i] * g;
        }
    }

    return out;
}

juce::AudioBuffer<float> gapperSnipper (const juce::AudioBuffer<float>& part, double rate, bool snip, double every, double length)
{
    const auto period = std::max (2, (int) std::llround (every * rate));
    const auto gap = juce::jlimit (1, snip ? period - 1 : period * 50, (int) std::llround (length * rate));
    const auto fade = std::max (1, (int) (rate * 0.001));
    const auto total = part.getNumSamples();
    std::vector<juce::Range<int>> keep;   // pieces of the input, in order
    std::vector<int> gapsAfter;

    for (int pos = 0; pos < total;)
    {
        const auto take = std::min (snip ? period - gap : period, total - pos);
        keep.push_back ({ pos, pos + take });
        gapsAfter.push_back (! snip && pos + take < total ? gap : 0);
        pos += take + (snip ? gap : 0);
    }

    int outLength = 0;
    for (size_t i = 0; i < keep.size(); ++i)
        outLength += keep[i].getLength() + gapsAfter[i];

    juce::AudioBuffer<float> out (part.getNumChannels(), std::max (1, outLength));
    out.clear();
    int at = 0;

    for (size_t i = 0; i < keep.size(); ++i)
    {
        const auto len = keep[i].getLength();
        const auto f = std::min (fade, len / 2);

        for (int c = 0; c < part.getNumChannels(); ++c)
        {
            out.copyFrom (c, at, part, c, keep[i].getStart(), len);

            if (f > 0)
            {
                if (i > 0)                   out.applyGainRamp (c, at, f, 0.0f, 1.0f);
                if (i + 1 < keep.size())     out.applyGainRamp (c, at + len - f, f, 1.0f, 0.0f);
            }
        }

        at += len + gapsAfter[i];
    }

    return out;
}

std::optional<juce::Range<juce::int64>> find (const juce::AudioBuffer<float>& audio, double rate, juce::int64 from, const FindSettings& s)
{
    const auto n = (juce::int64) audio.getNumSamples();
    const auto channels = audio.getNumChannels();
    auto peakAt = [&] (juce::int64 i) { float m = 0; for (int c = 0; c < channels; ++c) m = std::max (m, std::abs (audio.getSample (c, (int) i))); return m; };
    const auto around = (juce::int64) (rate * 0.005);

    if (s.what == FindWhat::glitch)
    {
        // a sample far off the line through the two before it
        for (auto i = std::max ((juce::int64) 2, from + 1); i < n; ++i)
            for (int c = 0; c < channels; ++c)
            {
                const auto* d = audio.getReadPointer (c);
                const auto predicted = 2.0f * d[i - 1] - d[i - 2];

                if (std::abs (d[i] - predicted) > s.jump)
                    return juce::Range<juce::int64> (std::max ((juce::int64) 0, i - around), std::min (n, i + around));
            }

        return {};
    }

    const auto level = juce::Decibels::decibelsToGain ((float) s.levelDb);

    if (s.what == FindWhat::loud)
    {
        for (auto i = std::max ((juce::int64) 0, from + 1); i < n; ++i)
            if (peakAt (i) > level)
                return juce::Range<juce::int64> (std::max ((juce::int64) 0, i - around), std::min (n, i + around));

        return {};
    }

    // silence: quiet for at least minSeconds (judged on 10 ms blocks)
    const auto block = std::max ((juce::int64) 1, (juce::int64) (rate * 0.01));
    const auto need = std::max ((juce::int64) 1, (juce::int64) (s.minSeconds * rate));
    juce::int64 start = -1;

    for (auto i = std::max ((juce::int64) 0, from + 1); i < n; i += block)
    {
        const auto len = (int) std::min (block, n - i);
        float m = 0;
        for (int c = 0; c < channels; ++c)
            m = std::max (m, audio.getMagnitude (c, (int) i, len));

        if (m < level)
        {
            if (start < 0)
                start = i;
        }
        else
        {
            if (start >= 0 && i - start >= need)
                return juce::Range<juce::int64> (start, i);
            start = -1;
        }
    }

    if (start >= 0 && n - start >= need)
        return juce::Range<juce::int64> (start, n);

    return {};
}
}
