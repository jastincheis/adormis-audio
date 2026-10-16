#include "Restoration.h"
#include <juce_dsp/juce_dsp.h>

namespace wf::fx
{
namespace
{
template <typename P>
class ParamHandoff
{
public:
    void set (const P& p)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        pending = p;
        dirty = true;
    }

    bool fetch (P& current)
    {
        if (! dirty.load())
            return false;

        const juce::SpinLock::ScopedTryLockType tl (lock);

        if (! tl.isLocked())
            return false;

        current = pending;
        dirty = false;
        return true;
    }

private:
    juce::SpinLock lock;
    P pending {};
    std::atomic<bool> dirty { false };
};

float toDb (float gain)   { return juce::Decibels::gainToDecibels (gain, -100.0f); }

// Periodic Hann (sums to a constant at 75 % overlap).
std::vector<float> hannWindow (int n)
{
    std::vector<float> w ((size_t) n);

    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);

    return w;
}

std::shared_ptr<const NoiseProfile>& capturedSlot()
{
    static std::shared_ptr<const NoiseProfile> slot;
    return slot;
}

//==============================================================================
// Noise reduction: STFT (Hann, 75 % overlap); every bin is turned down by
// how much of it the noise print explains, smoothed in time and frequency.
class NoiseReductionProcessor final : public Processor
{
public:
    void set (const NoiseReductionParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = std::max (1, ch);
        size = noiseFftSize (rate);
        hop = size / 4;
        bins = size / 2 + 1;
        fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 ((double) size)));
        window = hannWindow (size);
        state.assign ((size_t) channels, {});

        for (auto& s : state)
        {
            s.input.assign ((size_t) size, 0.0f);
            s.output.assign ((size_t) size, 0.0f);
            s.ready.assign ((size_t) hop, 0.0f);
            s.gain.assign ((size_t) bins, 1.0f);
            s.noisePower.assign ((size_t) bins, 0.0f);
        }

        work.assign ((size_t) size * 2, 0.0f);
        smoothed.assign ((size_t) bins, 0.0f);
        handoff.fetch (params);
        mapProfile();
        reset();
    }

    int latencySamples() const override   { return size; }

    void reset() override
    {
        for (auto& s : state)
        {
            std::fill (s.input.begin(), s.input.end(), 0.0f);
            std::fill (s.output.begin(), s.output.end(), 0.0f);
            std::fill (s.ready.begin(), s.ready.end(), 0.0f);
            std::fill (s.gain.begin(), s.gain.end(), 1.0f);
        }

        filled = 0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        if (handoff.fetch (params))
            mapProfile();

        const auto count = std::min (channels, audio.getNumChannels());
        float inPeak = 0.0f, outPeak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < count; ++ch)
            {
                auto& s = state[(size_t) ch];
                const auto x = audio.getSample (ch, start + i);
                inPeak = std::max (inPeak, std::abs (x));
                s.input[(size_t) (size - hop + filled)] = x;
                const auto y = s.ready[(size_t) filled];
                outPeak = std::max (outPeak, std::abs (y));
                audio.setSample (ch, start + i, y);
            }

            if (++filled == hop)
            {
                filled = 0;

                for (int ch = 0; ch < count; ++ch)
                    processFrame (state[(size_t) ch]);
            }
        }

        inputLevelDb = toDb (inPeak);
        outputLevelDb = toDb (outPeak);
    }

private:
    struct Channel
    {
        std::vector<float> input, output, ready, gain, noisePower;
    };

    // The print's magnitudes, at this FFT size and rate, as noise power per bin.
    void mapProfile()
    {
        const auto profile = params.profile;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& power = state[(size_t) ch].noisePower;
            std::fill (power.begin(), power.end(), 0.0f);

            if (profile == nullptr || profile->magnitude.empty())
                continue;

            const auto& mag = profile->magnitude[(size_t) std::min (ch, (int) profile->magnitude.size() - 1)];
            const auto profileBins = (int) mag.size();
            const auto scale = std::sqrt ((double) size / profile->fftSize);   // noise grows with sqrt(N)

            for (int k = 0; k < bins; ++k)
            {
                const auto hz = (double) k * rate / size;
                const auto pos = hz * profile->fftSize / profile->sampleRate;
                const auto i = (int) pos;
                const auto frac = (float) (pos - i);
                const auto m = i + 1 < profileBins ? mag[(size_t) i] * (1.0f - frac) + mag[(size_t) i + 1] * frac
                             : i < profileBins ? mag[(size_t) i] : 0.0f;
                // Mean magnitude -> mean power of Rayleigh-distributed noise.
                power[(size_t) k] = (float) (m * scale * m * scale * 4.0 / juce::MathConstants<double>::pi);
            }
        }
    }

    void processFrame (Channel& s)
    {
        for (int i = 0; i < size; ++i)
            work[(size_t) i] = s.input[(size_t) i] * window[(size_t) i];

        std::fill (work.begin() + size, work.end(), 0.0f);
        fft->performRealOnlyForwardTransform (work.data(), true);

        const auto alpha = (float) std::pow (10.0, params.thresholdDb / 10.0);
        const auto floorGain = (float) juce::Decibels::decibelsToGain (-std::abs (params.reductionDb));
        const auto smooth = (float) juce::jlimit (0.0, 1.0, params.smoothing);
        const auto spread = juce::roundToInt (smooth * 3.0f);
        const auto rise = 1.0f - 0.55f * smooth, fall = 1.0f - 0.8f * smooth;
        const auto hasProfile = params.profile != nullptr;

        // Raw gain per bin: how much is more than the noise.
        for (int k = 0; k < bins; ++k)
        {
            const auto re = work[(size_t) (2 * k)], im = work[(size_t) (2 * k + 1)];
            const auto power = re * re + im * im;
            const auto noise = alpha * s.noisePower[(size_t) k];
            smoothed[(size_t) k] = ! hasProfile ? 1.0f : power > noise ? (power - noise) / power : 0.0f;
        }

        // Across neighbouring bins, then over time (rising faster than falling).
        for (int k = 0; k < bins; ++k)
        {
            float sum = 0.0f;
            int n = 0;

            for (int j = std::max (0, k - spread); j <= std::min (bins - 1, k + spread); ++j, ++n)
                sum += smoothed[(size_t) j];

            const auto target = sum / (float) n;
            auto& g = s.gain[(size_t) k];
            g += (target - g) * (target > g ? rise : fall);
        }

        for (int k = 0; k < bins; ++k)
        {
            const auto g = std::max (floorGain, s.gain[(size_t) k]);
            const auto applied = params.noiseOnly ? 1.0f - g : g;
            work[(size_t) (2 * k)] *= applied;
            work[(size_t) (2 * k + 1)] *= applied;
        }

        fft->performRealOnlyInverseTransform (work.data());

        // Overlap-add (Hann twice at 75 % overlap sums to 1.5).
        for (int i = 0; i < size; ++i)
            s.output[(size_t) i] += work[(size_t) i] * window[(size_t) i] / 1.5f;

        std::copy (s.output.begin(), s.output.begin() + hop, s.ready.begin());
        std::move (s.output.begin() + hop, s.output.end(), s.output.begin());
        std::fill (s.output.end() - hop, s.output.end(), 0.0f);
        std::move (s.input.begin() + hop, s.input.end(), s.input.begin());
    }

    ParamHandoff<NoiseReductionParams> handoff;
    NoiseReductionParams params;
    double rate = 44100.0;
    int channels = 2, size = 4096, hop = 1024, bins = 2049, filled = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, work, smoothed;
    std::vector<Channel> state;
};

//==============================================================================
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    double run (double x)
    {
        const auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Biquad notch (double hz, double q, double rate)
{
    const auto w = juce::MathConstants<double>::twoPi * hz / rate;
    const auto alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = 1.0 / a0; b.b1 = -2.0 * cw / a0; b.b2 = 1.0 / a0;
    b.a1 = -2.0 * cw / a0; b.a2 = (1.0 - alpha) / a0;
    return b;
}

Biquad highPass (double hz, double q, double rate)
{
    const auto w = juce::MathConstants<double>::twoPi * hz / rate;
    const auto alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = (1.0 + cw) / 2.0 / a0; b.b1 = -(1.0 + cw) / a0; b.b2 = (1.0 + cw) / 2.0 / a0;
    b.a1 = -2.0 * cw / a0; b.a2 = (1.0 - alpha) / a0;
    return b;
}

class HumProcessor final : public Processor
{
public:
    void set (const HumParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = std::max (1, ch);
        handoff.fetch (params);
        design();
    }

    void reset() override   { design(); }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        if (handoff.fetch (params))
            design();

        const auto count = std::min (channels, audio.getNumChannels());
        float inPeak = 0.0f, outPeak = 0.0f;

        for (int ch = 0; ch < count; ++ch)
        {
            auto& filters = sections[(size_t) ch];
            auto* x = audio.getWritePointer (ch, start);

            for (int i = 0; i < numSamples; ++i)
            {
                inPeak = std::max (inPeak, std::abs (x[i]));
                auto v = (double) x[i];

                for (auto& f : filters)
                    v = f.run (v);

                x[i] = (float) v;
                outPeak = std::max (outPeak, std::abs (x[i]));
            }
        }

        inputLevelDb = toDb (inPeak);
        outputLevelDb = toDb (outPeak);
    }

private:
    void design()
    {
        std::vector<Biquad> filters;
        const auto width = std::max (0.2, params.widthHz);

        for (int h = 1; h <= juce::jlimit (1, 40, params.harmonics); ++h)
        {
            const auto hz = params.frequency * h;

            if (hz >= rate * 0.45)
                break;

            filters.push_back (notch (hz, hz / width, rate));
        }

        if (params.removeRumble)
        {
            // 24 dB per octave Butterworth at 30 Hz.
            filters.push_back (highPass (30.0, 0.5412, rate));
            filters.push_back (highPass (30.0, 1.3066, rate));
        }

        sections.assign ((size_t) channels, filters);
    }

    ParamHandoff<HumParams> handoff;
    HumParams params;
    double rate = 44100.0;
    int channels = 2;
    std::vector<std::vector<Biquad>> sections;
};

//==============================================================================
// Shared by the declicker and the clip restorer: the audio goes through in
// blocks; each block is repaired with one block of (already repaired)
// audio before it and one after it as context. Latency: two blocks.
class BlockRepairProcessor : public Processor
{
public:
    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = std::max (1, ch);
        block = rate > 70000.0 ? 2048 : 1024;
        windows.assign ((size_t) channels, std::vector<float> ((size_t) block * 3, 0.0f));
        ready.assign ((size_t) channels, std::vector<float> ((size_t) block, 0.0f));
        fetchParams();
        reset();
    }

    int latencySamples() const override   { return 2 * block; }

    void reset() override
    {
        for (auto& w : windows) std::fill (w.begin(), w.end(), 0.0f);
        for (auto& r : ready) std::fill (r.begin(), r.end(), 0.0f);
        filled = 0;
        repairs = 0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        fetchParams();
        const auto count = std::min (channels, audio.getNumChannels());
        const auto gain = outputGain();
        float inPeak = 0.0f, outPeak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < count; ++ch)
            {
                const auto x = audio.getSample (ch, start + i);
                inPeak = std::max (inPeak, std::abs (x));
                windows[(size_t) ch][(size_t) (2 * block + filled)] = x;
                const auto y = ready[(size_t) ch][(size_t) filled] * gain;
                outPeak = std::max (outPeak, std::abs (y));
                audio.setSample (ch, start + i, y);
            }

            if (++filled == block)
            {
                filled = 0;

                for (int ch = 0; ch < count; ++ch)
                {
                    auto& w = windows[(size_t) ch];
                    repair (w.data(), block);   // works on w[block .. 2 block)
                    std::copy (w.begin() + block, w.begin() + 2 * block, ready[(size_t) ch].begin());
                    std::move (w.begin() + block, w.end(), w.begin());
                }
            }
        }

        inputLevelDb = toDb (inPeak);
        outputLevelDb = toDb (outPeak);
    }

    std::atomic<int> repairs { 0 };

protected:
    virtual void fetchParams() = 0;
    virtual void repair (float* window, int blockSize) = 0;
    virtual float outputGain() const   { return 1.0f; }

    double rate = 44100.0;
    int channels = 2, block = 1024, filled = 0;
    std::vector<std::vector<float>> windows, ready;
};

// Burg's method: prediction coefficients c[1..order] with
// x[n] ~ sum c[k] x[n-k].
std::vector<double> burg (const float* x, int n, int order)
{
    std::vector<double> f ((size_t) n), b ((size_t) n), a ((size_t) order + 1, 0.0), tmp;
    a[0] = 1.0;

    for (int i = 0; i < n; ++i)
        f[(size_t) i] = b[(size_t) i] = x[i];

    for (int m = 1; m <= order; ++m)
    {
        double num = 0.0, den = 0.0;

        for (int i = m; i < n; ++i)
        {
            num += f[(size_t) i] * b[(size_t) i - 1];
            den += f[(size_t) i] * f[(size_t) i] + b[(size_t) i - 1] * b[(size_t) i - 1];
        }

        const auto k = den > 0.0 ? -2.0 * num / den : 0.0;
        tmp = a;

        for (int i = 1; i <= m; ++i)
            a[(size_t) i] = tmp[(size_t) i] + k * tmp[(size_t) (m - i)];

        for (int i = n - 1; i >= m; --i)
        {
            const auto fi = f[(size_t) i], bi = b[(size_t) i - 1];
            f[(size_t) i] = fi + k * bi;
            b[(size_t) i] = bi + k * fi;
        }
    }

    std::vector<double> c ((size_t) order + 1, 0.0);

    for (int i = 1; i <= order; ++i)
        c[(size_t) i] = -a[(size_t) i];

    return c;
}

class DeclickProcessor final : public BlockRepairProcessor
{
public:
    void set (const DeclickParams& p)   { handoff.set (p); }

protected:
    void fetchParams() override   { handoff.fetch (params); }

    void repair (float* w, int b) override
    {
        constexpr int order = 24;
        const auto total = 3 * b;
        const auto coeffs = burg (w, total, order);

        // Prediction error over the window and its robust spread (median).
        residual.resize ((size_t) total);
        absolute.clear();

        for (int n = order; n < total; ++n)
        {
            double p = 0.0;

            for (int k = 1; k <= order; ++k)
                p += coeffs[(size_t) k] * w[n - k];

            residual[(size_t) n] = (float) (w[n] - p);
            absolute.push_back (std::abs (residual[(size_t) n]));
        }

        if (absolute.empty())
            return;

        std::nth_element (absolute.begin(), absolute.begin() + (long) absolute.size() / 2, absolute.end());
        const auto spread = 1.4826f * absolute[absolute.size() / 2];

        if (spread <= 1.0e-9f)
            return;

        const auto factor = (float) juce::jmap (juce::jlimit (0.0, 100.0, params.sensitivity), 0.0, 100.0, 12.0, 3.5);
        const auto threshold = spread * factor;
        const auto maxLength = std::max (1, (int) (params.maxClickMs * 0.001 * rate));

        // Spans of outliers in the middle block (gaps up to 8 samples join).
        for (int n = b; n < 2 * b; ++n)
        {
            if (std::abs (residual[(size_t) n]) <= threshold)
                continue;

            auto end = n + 1;

            for (int m = n + 1; m < std::min (2 * b + maxLength, total - order) && m - end <= 8; ++m)
                if (std::abs (residual[(size_t) m]) > threshold)
                    end = m + 1;

            const auto first = std::max (order, n - 1), last = std::min (total - order, end + 2);

            if (last - first <= maxLength + 3)
            {
                interpolate (w, first, last, coeffs, order);
                ++repairs;
            }

            n = end;
        }
    }

private:
    // Fills w[first, last) by predicting forwards from before it and
    // backwards from after it, crossfaded.
    static void interpolate (float* w, int first, int last, const std::vector<double>& c, int order)
    {
        const auto length = last - first;
        std::vector<double> forward ((size_t) length), backward ((size_t) length);
        std::vector<double> history ((size_t) (order + length));

        for (int k = 0; k < order; ++k)
            history[(size_t) k] = w[first - order + k];

        for (int i = 0; i < length; ++i)
        {
            double p = 0.0;

            for (int k = 1; k <= order; ++k)
                p += c[(size_t) k] * history[(size_t) (order + i - k)];

            history[(size_t) (order + i)] = p;
            forward[(size_t) i] = p;
        }

        // Backwards: the same model run on reversed time.
        for (int k = 0; k < order; ++k)
            history[(size_t) k] = w[last + order - 1 - k];

        for (int i = 0; i < length; ++i)
        {
            double p = 0.0;

            for (int k = 1; k <= order; ++k)
                p += c[(size_t) k] * history[(size_t) (order + i - k)];

            history[(size_t) (order + i)] = p;
            backward[(size_t) (length - 1 - i)] = p;
        }

        for (int i = 0; i < length; ++i)
        {
            const auto t = (double) (i + 1) / (double) (length + 1);
            w[first + i] = (float) ((1.0 - t) * forward[(size_t) i] + t * backward[(size_t) i]);
        }
    }

    ParamHandoff<DeclickParams> handoff;
    DeclickParams params;
    std::vector<float> residual, absolute;
};

class ClipRestoreProcessor final : public BlockRepairProcessor
{
public:
    void set (const ClipRestoreParams& p)   { handoff.set (p); }

protected:
    void fetchParams() override   { handoff.fetch (params); }
    float outputGain() const override   { return (float) juce::Decibels::decibelsToGain (params.outputDb); }

    void repair (float* w, int b) override
    {
        constexpr int neighbours = 6;
        const auto level = (float) juce::Decibels::decibelsToGain (params.clipLevelDb) * 0.999f;
        const auto total = 3 * b;

        for (int n = b; n < 2 * b; ++n)
        {
            if (std::abs (w[n]) < level || std::abs (w[n - 1]) >= level)
                continue;   // not the start of a run

            const auto positive = w[n] > 0.0f;
            auto end = n;

            while (end < total && std::abs (w[end]) >= level && (w[end] > 0.0f) == positive)
                ++end;

            const auto length = end - n;

            if (length < 2 || length > b / 2 || n - neighbours < 0 || end + neighbours > total)
            {
                n = end;
                continue;
            }

            // Least-squares cubic through the samples either side, centred on the run.
            const auto centre = (n + end - 1) / 2.0;
            double ata[4][4] {}, atb[4] {};

            auto add = [&] (int i)
            {
                const auto t = (i - centre) / (double) (length + neighbours);
                const double basis[4] { 1.0, t, t * t, t * t * t };

                for (int r = 0; r < 4; ++r)
                {
                    atb[r] += basis[r] * w[i];

                    for (int c = 0; c < 4; ++c)
                        ata[r][c] += basis[r] * basis[c];
                }
            };

            for (int i = n - neighbours; i < n; ++i)   add (i);
            for (int i = end; i < end + neighbours; ++i) add (i);

            double coef[4];

            if (! solve4 (ata, atb, coef))
            {
                n = end;
                continue;
            }

            for (int i = n; i < end; ++i)
            {
                const auto t = (i - centre) / (double) (length + neighbours);
                const auto fit = coef[0] + coef[1] * t + coef[2] * t * t + coef[3] * t * t * t;
                const auto magnitude = juce::jlimit ((double) level, (double) level * 4.0, std::abs (fit));
                w[i] = (float) (positive ? magnitude : -magnitude);
            }

            ++repairs;
            n = end;
        }
    }

private:
    static bool solve4 (double a[4][4], double b[4], double x[4])
    {
        double m[4][5];

        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
                m[r][c] = a[r][c];

            m[r][4] = b[r];
        }

        for (int c = 0; c < 4; ++c)
        {
            int pivot = c;

            for (int r = c + 1; r < 4; ++r)
                if (std::abs (m[r][c]) > std::abs (m[pivot][c]))
                    pivot = r;

            if (std::abs (m[pivot][c]) < 1.0e-12)
                return false;

            for (int k = 0; k < 5; ++k)
                std::swap (m[c][k], m[pivot][k]);

            for (int r = 0; r < 4; ++r)
            {
                if (r == c)
                    continue;

                const auto f = m[r][c] / m[c][c];

                for (int k = c; k < 5; ++k)
                    m[r][k] -= f * m[c][k];
            }
        }

        for (int r = 0; r < 4; ++r)
            x[r] = m[r][4] / m[r][r];

        return true;
    }

    ParamHandoff<ClipRestoreParams> handoff;
    ClipRestoreParams params;
};
} // namespace

//==============================================================================
int noiseFftSize (double sampleRate)
{
    return juce::nextPowerOfTwo (juce::roundToInt (sampleRate * 0.09));
}

std::shared_ptr<NoiseProfile> makeNoiseProfile (const juce::AudioBuffer<float>& audio, double sampleRate, bool quietestOnly)
{
    auto profile = std::make_shared<NoiseProfile>();
    profile->sampleRate = sampleRate;
    profile->fftSize = noiseFftSize (sampleRate);
    const auto size = profile->fftSize, bins = size / 2 + 1;
    const auto hop = size / 2;
    const auto frames = audio.getNumSamples() >= size ? (audio.getNumSamples() - size) / hop + 1 : 0;

    if (frames == 0)
        return nullptr;

    juce::dsp::FFT fft (juce::roundToInt (std::log2 ((double) size)));
    const auto window = hannWindow (size);
    std::vector<float> work ((size_t) size * 2);
    // The learned print looks at up to ~2000 frames spread over the audio.
    const auto step = quietestOnly ? std::max (1, frames / 2000) : 1;

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* x = audio.getReadPointer (ch);
        std::vector<double> sum ((size_t) bins, 0.0);
        std::vector<std::vector<float>> perBin (quietestOnly ? (size_t) bins : 0);
        int used = 0;

        for (int f = 0; f < frames; f += step)
        {
            float energy = 0.0f;

            for (int i = 0; i < size; ++i)
            {
                work[(size_t) i] = x[f * hop + i] * window[(size_t) i];
                energy += std::abs (work[(size_t) i]);
            }

            if (energy <= 1.0e-7f)
                continue;   // digital silence says nothing about the noise

            std::fill (work.begin() + size, work.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (work.data(), true);

            for (int k = 0; k < bins; ++k)
            {
                if (quietestOnly)
                    perBin[(size_t) k].push_back (work[(size_t) k]);
                else
                    sum[(size_t) k] += work[(size_t) k];
            }

            ++used;
        }

        std::vector<float> magnitude ((size_t) bins, 0.0f);

        if (used > 0)
        {
            for (int k = 0; k < bins; ++k)
            {
                if (quietestOnly)
                {
                    // The 15th percentile of a band is mostly noise; scaled up
                    // to what its mean would be (Rayleigh: mean / p15 ~ 2.2).
                    auto& v = perBin[(size_t) k];
                    const auto at = v.begin() + (long) (v.size() * 15 / 100);
                    std::nth_element (v.begin(), at, v.end());
                    magnitude[(size_t) k] = *at * 2.2f;
                }
                else
                {
                    magnitude[(size_t) k] = (float) (sum[(size_t) k] / used);
                }
            }
        }

        profile->magnitude.push_back (std::move (magnitude));
    }

    return profile;
}

std::shared_ptr<const NoiseProfile> capturedNoiseProfile()        { return capturedSlot(); }
void setCapturedNoiseProfile (std::shared_ptr<const NoiseProfile> p) { capturedSlot() = std::move (p); }

std::optional<HumEstimate> detectHum (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    // Averaged spectrum with ~0.7 Hz bins, of up to a minute of mono.
    constexpr int order = 16, size = 1 << order;
    const auto length = std::min (audio.getNumSamples(), (int) (sampleRate * 60.0));

    if (length < size)
        return {};

    juce::dsp::FFT fft (order);
    const auto window = hannWindow (size);
    std::vector<float> work ((size_t) size * 2);
    std::vector<double> spectrum ((size_t) size / 2, 0.0);
    int frames = 0;

    for (int start = 0; start + size <= length; start += size / 2, ++frames)
    {
        for (int i = 0; i < size; ++i)
        {
            float v = 0.0f;

            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                v += audio.getSample (ch, start + i);

            work[(size_t) i] = v * window[(size_t) i];
        }

        std::fill (work.begin() + size, work.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (work.data(), true);

        for (int k = 0; k < size / 2; ++k)
            spectrum[(size_t) k] += (double) work[(size_t) k] * work[(size_t) k];
    }

    const auto binHz = sampleRate / size;
    auto powerDb = [&] (double hz)
    {
        const auto pos = hz / binHz;
        const auto i = (int) pos;

        if (i + 1 >= size / 2)
            return -200.0;

        // The strongest of the two bins around it (a peak may sit between them).
        return 10.0 * std::log10 (std::max (spectrum[(size_t) i], spectrum[(size_t) i + 1]) / frames + 1.0e-30);
    };

    // How far `hz` stands out of the band around it (+-8 Hz, without +-2 Hz).
    auto prominence = [&] (double hz)
    {
        std::vector<double> around;

        for (double f = hz - 8.0; f <= hz + 8.0; f += binHz)
            if (std::abs (f - hz) > 2.0)
                around.push_back (powerDb (f));

        if (around.empty())
            return 0.0;

        std::nth_element (around.begin(), around.begin() + (long) around.size() / 2, around.end());
        return powerDb (hz) - around[around.size() / 2];
    };

    double best = 0.0, bestScore = -1.0e9;

    for (double f0 = 45.0; f0 <= 65.0; f0 += 0.05)
    {
        double score = 0.0;

        for (int h = 1; h <= 4; ++h)
            score += std::max (0.0, prominence (f0 * h)) / h;

        if (score > bestScore)
        {
            bestScore = score;
            best = f0;
        }
    }

    // Refine to a hundredth of a Hz: the energy at f0 and its first
    // harmonics, evaluated exactly (Goertzel) on up to 20 s of the audio.
    const auto span = std::min (length, (int) (sampleRate * 20.0));
    std::vector<float> mono ((size_t) span, 0.0f);

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int i = 0; i < span; ++i)
            mono[(size_t) i] += audio.getSample (ch, i);

    auto energyAt = [&] (double hz)
    {
        const auto coeff = 2.0 * std::cos (juce::MathConstants<double>::twoPi * hz / sampleRate);
        double s1 = 0.0, s2 = 0.0;

        for (int i = 0; i < span; ++i)
        {
            const auto s0 = mono[(size_t) i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        return s1 * s1 + s2 * s2 - coeff * s1 * s2;
    };

    double frequency = best, bestEnergy = -1.0;

    for (double f0 = best - 0.3; f0 <= best + 0.3; f0 += 0.01)
    {
        double e = 0.0;

        for (int h = 1; h <= 3; ++h)
            e += energyAt (f0 * h) / h;

        if (e > bestEnergy)
        {
            bestEnergy = e;
            frequency = f0;
        }
    }

    const auto strength = prominence (frequency);

    if (strength < 6.0)
        return {};

    return HumEstimate { std::round (frequency * 100.0) / 100.0, strength };
}

std::optional<double> detectClipLevelDb (const juce::AudioBuffer<float>& audio, int* runs)
{
    float peak = 0.0f;

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        peak = std::max (peak, audio.getMagnitude (ch, 0, audio.getNumSamples()));

    if (peak <= 0.0f)
        return {};

    const auto level = peak * 0.999f;
    int count = 0;

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* x = audio.getReadPointer (ch);
        int run = 0;

        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            if (std::abs (x[i]) >= level)
                ++run;
            else
            {
                if (run >= 2) ++count;
                run = 0;
            }
        }
    }

    if (runs != nullptr)
        *runs = count;

    if (count < 3)
        return {};

    return juce::Decibels::gainToDecibels ((double) peak);
}

std::shared_ptr<Processor> makeNoiseReduction()  { return std::make_shared<NoiseReductionProcessor>(); }
std::shared_ptr<Processor> makeHumRemoval()      { return std::make_shared<HumProcessor>(); }
std::shared_ptr<Processor> makeDeclick()         { return std::make_shared<DeclickProcessor>(); }
std::shared_ptr<Processor> makeClipRestore()     { return std::make_shared<ClipRestoreProcessor>(); }

void setParams (Processor& p, const NoiseReductionParams& v)  { if (auto* x = dynamic_cast<NoiseReductionProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const HumParams& v)             { if (auto* x = dynamic_cast<HumProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const DeclickParams& v)         { if (auto* x = dynamic_cast<DeclickProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const ClipRestoreParams& v)     { if (auto* x = dynamic_cast<ClipRestoreProcessor*> (&p)) x->set (v); }

int repairsMade (const Processor& p)
{
    if (auto* x = dynamic_cast<const BlockRepairProcessor*> (&p))
        return x->repairs.load();

    return 0;
}
} // namespace wf::fx
