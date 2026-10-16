#include "Mp3Restore.h"
#include <juce_dsp/juce_dsp.h>

namespace wf::fx
{
namespace
{
// Parameters from the message thread to the audio thread without waiting
// (the audio thread only try-locks).
class Handoff
{
public:
    void set (const Mp3RestoreParams& p)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        pending = p;
        dirty = true;
    }

    bool fetch (Mp3RestoreParams& current)
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
    Mp3RestoreParams pending;
    std::atomic<bool> dirty { false };
};

// A biquad (transposed direct form II), RBJ designs.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void design (bool highPass, double hz, double q, double rate)
    {
        const auto w = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, rate * 0.49, hz) / rate;
        const auto cw = std::cos (w), alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
        const auto k = highPass ? (1.0 + cw) / 2.0 : (1.0 - cw) / 2.0;
        b0 = k / a0;
        b1 = (highPass ? -2.0 * k : 2.0 * k) / a0;
        b2 = k / a0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    double run (double x)
    {
        const auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void clear()   { z1 = z2 = 0.0; }
};

constexpr int fftOrder = 11, frameSize = 1 << fftOrder, hop = frameSize / 4;
constexpr int bins = frameSize / 2 + 1;

// One channel's short-time Fourier analysis / resynthesis (75 % overlap,
// square-root Hann windows): `frame` is called on each spectrum.
struct Stft
{
    std::vector<float> in = std::vector<float> (frameSize, 0.0f), out = std::vector<float> (frameSize, 0.0f);
    std::vector<float> env = std::vector<float> (bins, 0.0f);
    float hfEnergy = 0.0f;
    int pos = 0, count = 0;

    void clear()
    {
        std::fill (in.begin(), in.end(), 0.0f);
        std::fill (out.begin(), out.end(), 0.0f);
        std::fill (env.begin(), env.end(), 0.0f);
        hfEnergy = 0.0f;
        pos = count = 0;
    }
};

class Mp3RestoreProcessor final : public Processor
{
public:
    void set (const Mp3RestoreParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int maxBlock) override
    {
        rate = r;
        channels = std::max (1, ch);
        handoff.fetch (params);

        for (int n = 0; n < frameSize; ++n)
            window[(size_t) n] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) n / (float) frameSize));

        // JUCE's inverse transform may or may not divide by the size: measure it.
        std::vector<float> probe (2 * frameSize, 0.0f);
        probe[0] = 1.0f;
        fft.performRealOnlyForwardTransform (probe.data());
        fft.performRealOnlyInverseTransform (probe.data());
        inverseScale = probe[0] != 0.0f ? 1.0f / probe[0] : 1.0f;

        stft.assign ((size_t) channels, {});
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) channels, 2,
                          juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        oversampler->initProcessing ((size_t) maxBlock);
        work.setSize (channels, maxBlock);
        dry.setSize (channels, maxBlock);
        perChannel.assign ((size_t) channels, {});
        dryPos.assign ((size_t) channels, 0);

        // The dry path waits as long as the oversampler takes.
        const auto osLatency = juce::roundToInt (oversampler->getLatencyInSamples());

        for (auto& c : perChannel)
            c.dry.assign ((size_t) osLatency, 0.0f);
        designFilters();
        reset();
    }

    void reset() override
    {
        for (auto& s : stft)
            s.clear();

        for (auto& c : perChannel)
        {
            const auto delay = c.dry.size();
            c = {};
            c.dry.assign (delay, 0.0f);
        }

        std::fill (dryPos.begin(), dryPos.end(), 0);
        designFilters();

        if (oversampler != nullptr)
            oversampler->reset();

        fastEnv = slowEnv = 0.0;
        punchGain = 1.0;
    }

    int latencySamples() const override
    {
        return frameSize + (oversampler != nullptr ? juce::roundToInt (oversampler->getLatencyInSamples()) : 0);
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        if (handoff.fetch (params))
            designFilters();

        const auto count = std::min (channels, audio.getNumChannels());

        for (int done = 0; done < numSamples;)
        {
            const auto n = std::min (numSamples - done, work.getNumSamples());

            // 1. Spectrum: de-swirl, and the copied-up top end.
            for (int ch = 0; ch < count; ++ch)
            {
                auto* d = audio.getWritePointer (ch, start + done);
                auto* w = work.getWritePointer (ch);

                for (int i = 0; i < n; ++i)
                    w[i] = stftSample ((size_t) ch, d[i]);
            }

            // 2. Harmonics and warmth, 4x oversampled so the saturation doesn't
            // alias. Only what they add goes through the oversampler; the sound
            // itself bypasses it (delayed to match), so it isn't filtered at all.
            for (int ch = 0; ch < count; ++ch)
            {
                auto& line = perChannel[(size_t) ch].dry;
                auto* w = work.getWritePointer (ch);
                auto* keep = dry.getWritePointer (ch);

                for (int i = 0; i < n; ++i)
                {
                    keep[i] = line.empty() ? w[i] : line[(size_t) dryPos[(size_t) ch]];

                    if (! line.empty())
                    {
                        line[(size_t) dryPos[(size_t) ch]] = w[i];
                        dryPos[(size_t) ch] = (dryPos[(size_t) ch] + 1) % (int) line.size();
                    }
                }
            }

            juce::dsp::AudioBlock<float> block (work.getArrayOfWritePointers(), (size_t) count, (size_t) n);
            auto up = oversampler->processSamplesUp (block);
            saturate (up);
            oversampler->processSamplesDown (block);

            for (int ch = 0; ch < count; ++ch)
                juce::FloatVectorOperations::add (work.getWritePointer (ch), dry.getReadPointer (ch), n);

            // 3. Punch, output level.
            const auto outGain = juce::Decibels::decibelsToGain (params.outputDb);
            const auto fastA = std::exp (-1.0 / (0.0005 * rate)), fastR = std::exp (-1.0 / (0.015 * rate));
            const auto slowA = std::exp (-1.0 / (0.020 * rate)), slowR = std::exp (-1.0 / (0.150 * rate));
            const auto smooth = std::exp (-1.0 / (0.002 * rate));

            for (int i = 0; i < n; ++i)
            {
                double level = 0.0;

                for (int ch = 0; ch < count; ++ch)
                    level = std::max (level, (double) std::abs (work.getSample (ch, i)));

                fastEnv = (level > fastEnv ? fastA : fastR) * fastEnv + (1.0 - (level > fastEnv ? fastA : fastR)) * level;
                slowEnv = (level > slowEnv ? slowA : slowR) * slowEnv + (1.0 - (level > slowEnv ? slowA : slowR)) * level;

                auto target = 1.0;

                if (params.punch > 0.0 && slowEnv > 1.0e-5)
                    target = juce::jlimit (0.7, 2.0, std::pow (fastEnv / slowEnv, params.punch * 0.8));

                punchGain = smooth * punchGain + (1.0 - smooth) * target;
                const auto g = (float) (punchGain * outGain);

                for (int ch = 0; ch < count; ++ch)
                    audio.setSample (ch, start + done + i, work.getSample (ch, i) * g);
            }

            done += n;
        }
    }

private:
    struct ChannelState
    {
        Biquad bandHigh, bandLow, outHigh1, outHigh2;
        double bandEnv = 0.0;
        std::vector<float> dry;   // delay line for the untouched sound
    };

    void designFilters()
    {
        const auto osRate = rate * 4.0;
        const auto fc = juce::jlimit (2000.0, rate * 0.49, params.cutoffHz);

        for (auto& c : perChannel)
        {
            // The band just below the cut feeds the harmonics...
            c.bandHigh.design (true, fc * 0.5, 0.707, osRate);
            c.bandLow.design (false, fc, 0.707, osRate);
            // ...and only what lands above the cut is kept (24 dB/oct Butterworth).
            c.outHigh1.design (true, fc * 1.04, 0.5412, osRate);
            c.outHigh2.design (true, fc * 1.04, 1.3066, osRate);
        }
    }

    bool hasCut() const   { return params.cutoffHz > 0.0 && params.cutoffHz < rate * 0.47; }

    float stftSample (size_t ch, float x)
    {
        auto& s = stft[ch];
        s.in[(size_t) s.pos] = x;
        const auto y = s.out[(size_t) s.pos];
        s.out[(size_t) s.pos] = 0.0f;
        s.pos = (s.pos + 1) % frameSize;

        if (++s.count == hop)
        {
            s.count = 0;
            frame (s);
        }

        return y;
    }

    void frame (Stft& s)
    {
        for (int n = 0; n < frameSize; ++n)
            data[(size_t) n] = s.in[(size_t) ((s.pos + n) % frameSize)] * window[(size_t) n];

        std::fill (data.begin() + frameSize, data.end(), 0.0f);
        fft.performRealOnlyForwardTransform (data.data());

        const auto hzPerBin = rate / frameSize;
        const auto kc = hasCut() ? juce::jlimit (16, bins - 1, (int) std::lround (params.cutoffHz / hzPerBin)) : bins - 1;
        const auto kTop = juce::jlimit (kc, bins - 1, (int) std::lround (std::min (20000.0, rate * 0.49) / hzPerBin));
        const auto kLo = juce::jlimit (1, kc, (int) std::lround (2500.0 / hzPerBin));

        for (int k = 0; k < bins; ++k)
        {
            mag[(size_t) k] = std::hypot (data[(size_t) (2 * k)], data[(size_t) (2 * k + 1)]);
            phase[(size_t) k] = std::atan2 (data[(size_t) (2 * k + 1)], data[(size_t) (2 * k)]);
        }

        // Smoothed spectrum: ~35 ms in time, +-2 bins in frequency.
        const auto a = (float) std::exp (-(double) hop / (rate * 0.035));
        float energy = 0.0f;

        for (int k = 0; k < bins; ++k)
        {
            s.env[(size_t) k] = a * s.env[(size_t) k] + (1.0f - a) * mag[(size_t) k];

            if (k >= kLo && k < kc)
                energy += mag[(size_t) k] * mag[(size_t) k];
        }

        for (int k = 0; k < bins; ++k)
        {
            float sum = 0.0f;
            int used = 0;

            for (int j = std::max (0, k - 2); j <= std::min (bins - 1, k + 2); ++j, ++used)
                sum += s.env[(size_t) j];

            smooth[(size_t) k] = sum / (float) used;
        }

        // De-swirl: pull each bin of the upper band towards the smoothed
        // spectrum (geometric blend), and give holes a fresh phase. Much less
        // on attacks, so they stay sharp.
        auto d = (float) params.deswirl;

        if (energy > 2.5f * s.hfEnergy)
            d *= 0.25f;

        s.hfEnergy = 0.8f * s.hfEnergy + 0.2f * energy;

        if (d > 0.0f)
        {
            for (int k = kLo; k < kc; ++k)
            {
                const auto m = mag[(size_t) k], target = smooth[(size_t) k];
                mag[(size_t) k] = std::exp ((1.0f - d) * std::log (m + 1.0e-9f) + d * std::log (target + 1.0e-9f));

                if (m < 0.25f * target)
                    phase[(size_t) k] = random.nextFloat() * juce::MathConstants<float>::twoPi;
            }
        }

        // Copy the octave below the cut up above it, tilted down 1.5 dB/kHz.
        if (params.spectralCopy && params.highs > 0.0 && hasCut())
        {
            const auto source = kc / 2, width = kc - source;

            for (int k = kc; k <= kTop; ++k)
            {
                const auto from = source + (k - kc) % width;
                const auto fadeIn = std::min (1.0f, (float) (k - kc + 1) / 6.0f);
                const auto tilt = std::pow (10.0f, -(float) ((k - kc) * hzPerBin / 1000.0 * 1.5) / 20.0f);
                const auto m = mag[(size_t) from] * (float) params.highs * tilt * fadeIn;
                mag[(size_t) k] = std::max (mag[(size_t) k], m);
                phase[(size_t) k] = phase[(size_t) from] + random.nextFloat() * 0.6f;
            }
        }

        for (int k = 0; k < bins; ++k)
        {
            data[(size_t) (2 * k)] = mag[(size_t) k] * std::cos (phase[(size_t) k]);
            data[(size_t) (2 * k + 1)] = mag[(size_t) k] * std::sin (phase[(size_t) k]);
        }

        fft.performRealOnlyInverseTransform (data.data());

        // Square-root Hann twice at 75 % overlap sums to 2.
        const auto scale = inverseScale * 0.5f;

        for (int n = 0; n < frameSize; ++n)
            s.out[(size_t) ((s.pos + n) % frameSize)] += data[(size_t) n] * window[(size_t) n] * scale;
    }

    void saturate (juce::dsp::AudioBlock<float>& up)
    {
        const auto osRate = rate * 4.0;
        const auto envA = std::exp (-1.0 / (0.001 * osRate)), envR = std::exp (-1.0 / (0.030 * osRate));
        const auto harmonics = ! params.spectralCopy && hasCut() ? params.highs : 0.0;
        const auto warmth = params.warmth;
        const auto drive = 1.0 + 3.0 * warmth;
        const auto norm = 1.0 / std::tanh (drive);

        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            auto& c = perChannel[ch];
            auto* d = up.getChannelPointer (ch);

            for (size_t i = 0; i < up.getNumSamples(); ++i)
            {
                double v = d[i];

                if (harmonics > 0.0)
                {
                    // The band below the cut, normalised by its own level so the
                    // harmonics come out the same at any loudness, then scaled back.
                    const auto band = c.bandLow.run (c.bandHigh.run (v));
                    const auto level = std::abs (band);
                    c.bandEnv = (level > c.bandEnv ? envA : envR) * c.bandEnv + (1.0 - (level > c.bandEnv ? envA : envR)) * level;
                    const auto x = band / (c.bandEnv + 1.0e-6);
                    const auto shaped = std::tanh (2.5 * x + 0.6 * x * x) * c.bandEnv;
                    v += harmonics * 0.8 * c.outHigh2.run (c.outHigh1.run (shaped));
                }

                if (warmth > 0.0)
                    v += warmth * (std::tanh (drive * v) * norm - v);

                d[i] = (float) (v - d[i]);   // only what was added
            }
        }
    }

    Handoff handoff;
    Mp3RestoreParams params;
    double rate = 44100.0;
    int channels = 2;

    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window = std::vector<float> (frameSize);
    std::vector<float> data = std::vector<float> (2 * frameSize);
    std::vector<float> mag = std::vector<float> (bins), phase = std::vector<float> (bins), smooth = std::vector<float> (bins);
    float inverseScale = 1.0f;
    std::vector<Stft> stft;
    juce::Random random { 0x3ad5 };

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> work, dry;
    std::vector<ChannelState> perChannel;
    std::vector<int> dryPos;
    double fastEnv = 0.0, slowEnv = 0.0, punchGain = 1.0;
};
} // namespace

std::shared_ptr<Processor> makeMp3Restore()   { return std::make_shared<Mp3RestoreProcessor>(); }

void setParams (Processor& p, const Mp3RestoreParams& params)
{
    if (auto* r = dynamic_cast<Mp3RestoreProcessor*> (&p))
        r->set (params);
}

std::optional<double> detectCutoff (const juce::AudioBuffer<float>& audio, double rate, std::vector<float>* spectrumDb)
{
    constexpr int order = 12, size = 1 << order, half = size / 2 + 1;
    const auto total = audio.getNumSamples();

    if (total < size || audio.getNumChannels() == 0)
        return {};

    juce::dsp::FFT fft (order);
    std::vector<float> work (2 * size), power ((size_t) half, 0.0f);
    const auto frames = std::min (400, total / size);
    const auto step = (double) (total - size) / std::max (1, frames - 1);

    for (int f = 0; f < frames; ++f)
    {
        const auto start = (int) (f * step);
        std::fill (work.begin(), work.end(), 0.0f);

        for (int i = 0; i < size; ++i)
        {
            float v = 0.0f;

            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                v += audio.getSample (ch, start + i);

            work[(size_t) i] = v / (float) audio.getNumChannels()
                             * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1)));
        }

        fft.performFrequencyOnlyForwardTransform (work.data(), true);

        for (int k = 0; k < half; ++k)
            power[(size_t) k] += work[(size_t) k] * work[(size_t) k];
    }

    std::vector<float> db ((size_t) half);

    for (int k = 0; k < half; ++k)
        db[(size_t) k] = juce::Decibels::gainToDecibels (std::sqrt (power[(size_t) k] / (float) frames) * 4.0f / (float) size, -140.0f);

    // Smoothed over +-3 bins.
    std::vector<float> smooth ((size_t) half);

    for (int k = 0; k < half; ++k)
    {
        float sum = 0.0f;
        int used = 0;

        for (int j = std::max (0, k - 3); j <= std::min (half - 1, k + 3); ++j, ++used)
            sum += db[(size_t) j];

        smooth[(size_t) k] = sum / (float) used;
    }

    if (spectrumDb != nullptr)
        *spectrumDb = smooth;

    const auto hzPerBin = rate / size;
    auto binOf = [&] (double hz) { return juce::jlimit (0, half - 1, (int) std::lround (hz / hzPerBin)); };

    // The floor: the quietest top 2 % of the spectrum.
    const auto topStart = binOf (rate * 0.49);
    auto floorDb = 0.0f;
    int used = 0;

    for (int k = (int) (topStart * 0.98); k <= topStart; ++k, ++used)
        floorDb += smooth[(size_t) k];

    floorDb /= (float) std::max (1, used);

    // From the top down, the first place clearly above the floor...
    int k = topStart;

    while (k > binOf (4000.0) && smooth[(size_t) k] < floorDb + 18.0f)
        --k;

    const auto cutoff = (k + 1) * hzPerBin;

    // ...counts as a cut when it's well below Nyquist and the drop is steep.
    if (cutoff > rate * 0.475 || k <= binOf (4000.0))
        return {};

    const auto below = smooth[(size_t) binOf (cutoff - 700.0)], above = smooth[(size_t) binOf (cutoff + 700.0)];

    if (below - above < 20.0f)
        return {};

    return cutoff;
}

juce::String likelyBitrate (double cutoffHz)
{
    if (cutoffHz <= 12000.0) return "64 kbps or lower";
    if (cutoffHz <= 15500.0) return "about 96 kbps";
    if (cutoffHz <= 17200.0) return "about 128 kbps";
    if (cutoffHz <= 18500.0) return "about 160 kbps";
    if (cutoffHz <= 19600.0) return "about 192 kbps";
    return "256 kbps or more";
}
} // namespace wf::fx
