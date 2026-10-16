#include "Loudness.h"

namespace wf
{
namespace
{
constexpr double absoluteGate = -70.0;   // LUFS
constexpr double integratedRelativeGate = -10.0;   // LU below the ungated mean
constexpr double rangeRelativeGate = -20.0;
constexpr int momentarySteps = 4;    // 400 ms of 100 ms steps
constexpr int shortTermSteps = 30;   // 3 s

double toLufs (double energy)   { return energy > 0.0 ? -0.691 + 10.0 * std::log10 (energy) : LoudnessMeter::silence; }
double fromLufs (double lufs)   { return std::pow (10.0, (lufs + 0.691) / 10.0); }
double toDb (double gain)       { return gain > 0.0 ? 20.0 * std::log10 (gain) : LoudnessMeter::silence; }

// Mean energy of the blocks above the absolute gate and `relative` LU below
// their own mean (BS.1770 two-stage gating).
std::optional<double> gatedMean (const std::vector<double>& blocks, double relative, std::vector<double>* passed = nullptr)
{
    const auto absolute = fromLufs (absoluteGate);
    double sum = 0.0;
    size_t count = 0;

    for (auto e : blocks)
        if (e > absolute) { sum += e; ++count; }

    if (count == 0)
        return {};

    const auto threshold = fromLufs (toLufs (sum / (double) count) + relative);
    sum = 0.0;
    count = 0;

    for (auto e : blocks)
        if (e > absolute && e > threshold)
        {
            sum += e;
            ++count;

            if (passed != nullptr)
                passed->push_back (e);
        }

    if (count == 0)
        return {};

    return sum / (double) count;
}

double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;

    for (int k = 1; k < 30; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }

    return sum;
}
} // namespace

//==============================================================================
void LoudnessMeter::prepare (double sampleRate, int numChannels)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    stepLength = juce::jmax (1, juce::roundToInt (rate / 10.0));
    channels.assign ((size_t) juce::jmax (1, numChannels), {});

    // K-weighting for any sample rate (the BS.1770 48 kHz coefficients come
    // out of these analogue prototypes).
    auto shelfFor = [this]
    {
        const auto f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
        const auto k = std::tan (juce::MathConstants<double>::pi * f0 / rate);
        const auto vh = std::pow (10.0, gain / 20.0);
        const auto vb = std::pow (vh, 0.4996667741545416);
        const auto a0 = 1.0 + k / q + k * k;
        Biquad b;
        b.b0 = (vh + vb * k / q + k * k) / a0;
        b.b1 = 2.0 * (k * k - vh) / a0;
        b.b2 = (vh - vb * k / q + k * k) / a0;
        b.a1 = 2.0 * (k * k - 1.0) / a0;
        b.a2 = (1.0 - k / q + k * k) / a0;
        return b;
    };

    auto highPassFor = [this]
    {
        const auto f0 = 38.13547087602444, q = 0.5003270373238773;
        const auto k = std::tan (juce::MathConstants<double>::pi * f0 / rate);
        const auto a0 = 1.0 + k / q + k * k;
        Biquad b;
        b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0;
        b.a1 = 2.0 * (k * k - 1.0) / a0;
        b.a2 = (1.0 - k / q + k * k) / a0;
        return b;
    };

    for (auto& c : channels)
    {
        c.shelf = shelfFor();
        c.highPass = highPassFor();
    }

    // True peak: interpolate to >= 192 kHz (none needed above that).
    oversampling = rate < 96000.0 ? 4 : rate < 192000.0 ? 2 : 1;
    const auto total = oversampling * tapsPerPhase;
    std::vector<double> h ((size_t) total);
    // Centred on a whole tap, so phase 0 lands on the original samples and
    // the others exactly 1/4, 2/4, 3/4 of the way between them.
    const auto centre = total / 2.0;
    const auto beta = 8.0;

    for (int n = 0; n < total; ++n)
    {
        const auto t = (n - centre) / oversampling;
        const auto sinc = std::abs (t) < 1e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
        const auto r = (n - centre) / centre;
        const auto window = besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / besselI0 (beta);
        h[(size_t) n] = sinc * window;
    }

    phases.assign ((size_t) oversampling, std::vector<float> ((size_t) tapsPerPhase));

    for (int p = 0; p < oversampling; ++p)
    {
        double sum = 0.0;

        for (int k = 0; k < tapsPerPhase; ++k)
            sum += h[(size_t) (p + k * oversampling)];

        // Each phase passes DC at unity, newest sample first.
        for (int k = 0; k < tapsPerPhase; ++k)
            phases[(size_t) p][(size_t) k] = (float) (h[(size_t) (p + k * oversampling)] / sum);
    }

    reset();
}

void LoudnessMeter::reset()
{
    for (auto& c : channels)
    {
        c.shelf.z1 = c.shelf.z2 = c.highPass.z1 = c.highPass.z2 = 0.0;
        c.weightedSum = 0.0;
        c.history.assign ((size_t) tapsPerPhase * 2, 0.0f);
        c.historyPos = 0;
        c.truePeak = c.samplePeak = c.sumSquares = 0.0;
    }

    stepFill = 0;
    totalSamples = 0;
    steps.assign ((size_t) shortTermSteps, 0.0);
    stepCount = 0;
    momentaryBlocks.clear();
    shortTermBlocks.clear();
    maxMomentaryLufs = maxShortTermLufs = silence;
}

double LoudnessMeter::truePeakOfSample (Channel& c, float sample)
{
    if (oversampling == 1)
        return std::abs (sample);

    // Ring buffer written twice, so the last `tapsPerPhase` samples are
    // always contiguous (newest at the end).
    c.history[(size_t) c.historyPos] = sample;
    c.history[(size_t) (c.historyPos + tapsPerPhase)] = sample;
    c.historyPos = (c.historyPos + 1) % tapsPerPhase;
    const float* recent = c.history.data() + c.historyPos;   // oldest .. newest

    double peak = 0.0;

    for (const auto& phase : phases)
    {
        float y = 0.0f;

        for (int k = 0; k < tapsPerPhase; ++k)
            y += phase[(size_t) k] * recent[tapsPerPhase - 1 - k];

        peak = juce::jmax (peak, (double) std::abs (y));
    }

    return peak;
}

void LoudnessMeter::process (const juce::AudioBuffer<float>& audio, int start, int numSamples)
{
    const float* pointers[16] {};
    const auto count = juce::jmin (audio.getNumChannels(), 16);

    for (int ch = 0; ch < count; ++ch)
        pointers[ch] = audio.getReadPointer (ch, start);

    process (pointers, count, numSamples);
}

void LoudnessMeter::process (const float* const* input, int numChannels, int numSamples)
{
    const auto count = juce::jmin (numChannels, (int) channels.size());
    int done = 0;

    while (done < numSamples)
    {
        const auto chunk = juce::jmin (numSamples - done, stepLength - stepFill);

        for (int ch = 0; ch < count; ++ch)
        {
            auto& c = channels[(size_t) ch];
            const auto* x = input[ch] + done;
            double weighted = 0.0, squares = 0.0, peak = c.samplePeak, tp = c.truePeak;

            for (int i = 0; i < chunk; ++i)
            {
                const auto s = (double) x[i];
                const auto k = c.highPass.run (c.shelf.run (s));
                weighted += k * k;
                squares += s * s;
                peak = juce::jmax (peak, std::abs (s));
                tp = juce::jmax (tp, truePeakOfSample (c, x[i]));
            }

            c.weightedSum += weighted;
            c.sumSquares += squares;
            c.samplePeak = peak;
            c.truePeak = juce::jmax (tp, peak);
        }

        done += chunk;
        stepFill += chunk;
        totalSamples += chunk;

        if (stepFill >= stepLength)
            finishStep();
    }
}

void LoudnessMeter::finishStep()
{
    double energy = 0.0;

    for (auto& c : channels)
    {
        energy += c.weightedSum / (double) stepLength;
        c.weightedSum = 0.0;
    }

    steps[(size_t) (stepCount % shortTermSteps)] = energy;
    ++stepCount;
    stepFill = 0;

    auto meanOfLast = [this] (int n)
    {
        double sum = 0.0;

        for (int i = 1; i <= n; ++i)
            sum += steps[(size_t) ((stepCount - i) % shortTermSteps)];

        return sum / (double) n;
    };

    if (stepCount >= momentarySteps)
    {
        const auto block = meanOfLast (momentarySteps);
        momentaryBlocks.push_back (block);
        maxMomentaryLufs = juce::jmax (maxMomentaryLufs, toLufs (block));
    }

    if (stepCount >= shortTermSteps)
    {
        const auto block = meanOfLast (shortTermSteps);
        shortTermBlocks.push_back (block);
        maxShortTermLufs = juce::jmax (maxShortTermLufs, toLufs (block));
    }
}

double LoudnessMeter::momentary() const
{
    if (stepCount < momentarySteps)
        return silence;

    double sum = 0.0;

    for (int i = 1; i <= momentarySteps; ++i)
        sum += steps[(size_t) ((stepCount - i) % shortTermSteps)];

    return toLufs (sum / momentarySteps);
}

double LoudnessMeter::shortTerm() const
{
    // Before 3 s have played: over what there is (so it shows right away).
    const auto n = juce::jmin (stepCount, shortTermSteps);

    if (n < momentarySteps)
        return silence;

    double sum = 0.0;

    for (int i = 1; i <= n; ++i)
        sum += steps[(size_t) ((stepCount - i) % shortTermSteps)];

    return toLufs (sum / n);
}

double LoudnessMeter::integrated() const
{
    const auto mean = gatedMean (momentaryBlocks, integratedRelativeGate);
    return mean.has_value() ? toLufs (*mean) : silence;
}

double LoudnessMeter::loudnessRange() const
{
    std::vector<double> passed;

    if (! gatedMean (shortTermBlocks, rangeRelativeGate, &passed).has_value() || passed.size() < 2)
        return 0.0;

    std::sort (passed.begin(), passed.end());
    auto percentile = [&passed] (double p)
    {
        const auto index = (size_t) juce::roundToInt (p * (double) (passed.size() - 1));
        return toLufs (passed[index]);
    };

    return percentile (0.95) - percentile (0.10);
}

double LoudnessMeter::truePeak (int channel) const
{
    return juce::isPositiveAndBelow (channel, (int) channels.size()) ? toDb (channels[(size_t) channel].truePeak) : silence;
}

double LoudnessMeter::truePeak() const
{
    auto peak = silence;

    for (int ch = 0; ch < (int) channels.size(); ++ch)
        peak = juce::jmax (peak, truePeak (ch));

    return peak;
}

double LoudnessMeter::samplePeak (int channel) const
{
    return juce::isPositiveAndBelow (channel, (int) channels.size()) ? toDb (channels[(size_t) channel].samplePeak) : silence;
}

double LoudnessMeter::rms (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, (int) channels.size()) || totalSamples == 0)
        return silence;

    return 10.0 * std::log10 (juce::jmax (1e-20, channels[(size_t) channel].sumSquares / (double) totalSamples));
}

//==============================================================================
std::optional<AudioStatistics> measure (const juce::AudioBuffer<float>& audio, double sampleRate, juce::Range<int> range,
                                        const std::function<bool (float)>& progress)
{
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    const auto numChannels = audio.getNumChannels();

    AudioStatistics result;
    result.length = range.getLength();
    result.channels.resize ((size_t) numChannels);

    LoudnessMeter meter;
    meter.prepare (sampleRate, numChannels);

    constexpr float clipLevel = 0.98855309f;   // -0.1 dBFS
    std::vector<double> sums ((size_t) numChannels, 0.0);
    std::vector<float> peaks ((size_t) numChannels, -1.0f);
    const int chunk = 65536;

    for (int pos = range.getStart(); pos < range.getEnd(); pos += chunk)
    {
        const auto n = juce::jmin (chunk, range.getEnd() - pos);
        meter.process (audio, pos, n);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& s = result.channels[(size_t) ch];
            const auto* x = audio.getReadPointer (ch);
            auto previous = pos > range.getStart() ? x[pos - 1] : x[pos];

            if (pos == range.getStart())
                s.minimum = s.maximum = x[pos];

            for (int i = pos; i < pos + n; ++i)
            {
                const auto v = x[i];
                s.minimum = juce::jmin (s.minimum, v);
                s.maximum = juce::jmax (s.maximum, v);
                sums[(size_t) ch] += v;

                if (std::abs (v) > peaks[(size_t) ch])
                {
                    peaks[(size_t) ch] = std::abs (v);
                    s.peakPosition = i;
                }

                if ((previous < 0.0f) != (v < 0.0f))
                    ++s.zeroCrossings;

                if (std::abs (v) >= clipLevel)
                    ++s.clippedSamples;

                previous = v;
            }
        }

        if (progress != nullptr && ! progress ((float) (pos + n - range.getStart()) / (float) juce::jmax (1, range.getLength())))
            return {};
    }

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& s = result.channels[(size_t) ch];
        s.peakDb = meter.samplePeak (ch);
        s.truePeakDb = meter.truePeak (ch);
        s.rmsDb = meter.rms (ch);
        s.dcOffset = range.isEmpty() ? 0.0 : sums[(size_t) ch] / (double) range.getLength();
    }

    result.integrated = meter.integrated();
    result.range = meter.loudnessRange();
    result.maxMomentary = meter.maxMomentary();
    result.maxShortTerm = meter.maxShortTerm();
    result.truePeakDb = meter.truePeak();
    return result;
}

juce::String formatLufs (double lufs)
{
    return lufs <= absoluteGate ? juce::String ("-inf LUFS") : juce::String (lufs, 1) + " LUFS";
}

juce::String formatDb (double db, const char* unit)
{
    return db <= -150.0 ? "-inf " + juce::String (unit) : juce::String (db, 2) + " " + unit;
}
} // namespace wf
