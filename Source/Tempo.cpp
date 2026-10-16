#include "Tempo.h"
#include <juce_dsp/juce_dsp.h>

namespace wf
{
namespace
{
// Onset strength (log spectral flux above its local average), one value per
// `hop` samples.
std::vector<float> onsetEnvelope (const juce::AudioBuffer<float>& audio, juce::Range<int> range, int hop, double hopSeconds)
{
    constexpr int order = 10, size = 1 << order;
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    const auto length = range.getLength();

    if (length < size * 2)
        return {};

    std::vector<float> mono ((size_t) length, 0.0f);

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* in = audio.getReadPointer (ch, range.getStart());

        for (int i = 0; i < length; ++i)
            mono[(size_t) i] += in[i];
    }

    std::vector<float> window ((size_t) size);

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));

    juce::dsp::FFT fft (order);
    const auto frames = (length - size) / hop + 1;
    std::vector<float> work (2 * size), previous (size / 2, 0.0f), flux ((size_t) frames, 0.0f);

    for (int f = 0; f < frames; ++f)
    {
        std::fill (work.begin(), work.end(), 0.0f);

        for (int i = 0; i < size; ++i)
            work[(size_t) i] = mono[(size_t) (f * hop + i)] * window[(size_t) i];

        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        float sum = 0.0f;

        for (int k = 1; k < size / 2; ++k)
        {
            const auto m = std::log (1.0f + 100.0f * work[(size_t) k]);
            sum += std::max (0.0f, m - previous[(size_t) k]);
            previous[(size_t) k] = m;
        }

        flux[(size_t) f] = f > 0 ? sum : 0.0f;
    }

    // Keep only what stands above the local (~0.4 s) average.
    const auto w = std::max (1, (int) std::lround (0.4 / hopSeconds));
    std::vector<double> prefix ((size_t) frames + 1, 0.0);

    for (int i = 0; i < frames; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + flux[(size_t) i];

    std::vector<float> onset ((size_t) frames);

    for (int i = 0; i < frames; ++i)
    {
        const auto a = std::max (0, i - w), b = std::min (frames, i + w + 1);
        onset[(size_t) i] = std::max (0.0f, flux[(size_t) i] - (float) ((prefix[(size_t) b] - prefix[(size_t) a]) / (b - a)));
    }

    return onset;
}
} // namespace

std::optional<double> measureTempo (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate)
{
    // ~5.8 ms steps whatever the sample rate.
    const auto hop = std::max (64, juce::roundToInt (rate * 256.0 / 44100.0));
    const auto hopSeconds = (double) hop / rate;
    const auto onset = onsetEnvelope (audio, range, hop, hopSeconds);
    const auto frames = (int) onset.size();
    constexpr double minBpm = 60.0, maxBpm = 200.0;

    // Needs a few bars: 4 beats at the slowest tempo, twice over.
    if (frames < (int) (8.0 * 60.0 / minBpm / hopSeconds) / 2)
        return {};

    // Autocorrelation up to 4 beats of the slowest tempo.
    const auto maxLag = std::min (frames - 2, (int) std::ceil (4.0 * 60.0 / minBpm / hopSeconds) + 2);
    std::vector<double> ac ((size_t) maxLag + 2, 0.0);

    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double s = 0.0;

        for (int i = 0; i + lag < frames; ++i)
            s += (double) onset[(size_t) i] * onset[(size_t) (i + lag)];

        ac[(size_t) lag] = s / (frames - lag);
    }

    auto acAt = [&] (double x)
    {
        if (x < 1.0 || x >= maxLag)
            return 0.0;

        const auto i = (int) x;
        const auto frac = x - i;
        return ac[(size_t) i] * (1.0 - frac) + ac[(size_t) i + 1] * frac;
    };

    // Every candidate tempo: its beat, 2, 3 and 4 beats (the bar), and half a
    // beat (the eighths most music has; this is what tells 80 from 120 when
    // the hi-hat plays eighths). A gentle preference for 80..160.
    double best = 0.0, bestScore = -1.0, total = 0.0;
    int count = 0;

    for (double bpm = minBpm; bpm <= maxBpm; bpm += 0.05)
    {
        const auto lag = 60.0 / bpm / hopSeconds;
        const auto prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 115.0) / 0.85, 2.0));
        const auto score = (acAt (lag) + 0.5 * acAt (2.0 * lag) + 0.33 * acAt (3.0 * lag) + 0.25 * acAt (4.0 * lag)
                            + 0.4 * acAt (0.5 * lag)) * prior;
        total += score;
        ++count;

        if (score > bestScore)
        {
            bestScore = score;
            best = bpm;
        }
    }

    // No clear peak: no steady beat.
    if (bestScore <= 0.0 || bestScore < 1.25 * total / count)
        return {};

    // Refine: the tempo whose beat grid, at its best phase, collects the most
    // onset strength over the whole range (precise to a few hundredths over
    // a minute of music).
    auto onsetAt = [&] (double x)
    {
        const auto i = (int) x;

        if (i < 0 || i + 1 >= frames)
            return 0.0;

        const auto frac = x - i;
        return (double) onset[(size_t) i] * (1.0 - frac) + (double) onset[(size_t) i + 1] * frac;
    };

    double refined = best, refinedScore = -1.0;

    for (double bpm = best * 0.985; bpm <= best * 1.015; bpm += 0.01)
    {
        const auto period = 60.0 / bpm / hopSeconds;

        for (double phase = 0.0; phase < period; phase += 0.5)
        {
            double sum = 0.0;

            for (double x = phase; x < frames; x += period)
                sum += onsetAt (x) + 0.5 * onsetAt (x + period * 0.5);

            if (sum > refinedScore)
            {
                refinedScore = sum;
                refined = bpm;
            }
        }
    }

    // To 0.1 BPM; whole numbers when it's that close to one.
    const auto rounded = std::round (refined);
    return std::abs (refined - rounded) < 0.15 ? rounded : std::round (refined * 10.0) / 10.0;
}
} // namespace wf
