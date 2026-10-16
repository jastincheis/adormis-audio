#include "MoreEffects.h"
#include "Loudness.h"
#include <juce_dsp/juce_dsp.h>

namespace wf::fx
{
namespace
{
// Hands parameters from the message thread to the audio thread: the audio
// thread only ever try-locks, so it never waits.
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

//==============================================================================
// 4x interpolation for true-peak detection: phase 0 is the sample itself
// (8 samples late), phases 1..3 the points a quarter, half and three
// quarters of the way to the next one.
class TruePeakDetector
{
public:
    static constexpr int factor = 4, taps = 16, delay = taps / 2;

    TruePeakDetector()
    {
        constexpr int total = factor * taps;
        const auto centre = total / 2.0;
        auto bessel = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 30; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
            return sum;
        };

        double h[total];

        for (int n = 0; n < total; ++n)
        {
            const auto t = (n - centre) / factor;
            const auto sinc = std::abs (t) < 1e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
            const auto r = (n - centre) / centre;
            h[n] = sinc * bessel (8.0 * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel (8.0);
        }

        for (int p = 0; p < factor; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < taps; ++k) sum += h[p + k * factor];
            for (int k = 0; k < taps; ++k) phases[(size_t) p][(size_t) k] = (float) (h[p + k * factor] / sum);
        }
    }

    void reset()   { history.fill (0.0f); pos = 0; }

    // Feeds one sample; returns the highest absolute value around the sample
    // `delay` steps back.
    float push (float x, bool interpolate)
    {
        history[(size_t) pos] = x;
        history[(size_t) (pos + taps)] = x;
        pos = (pos + 1) % taps;
        const float* recent = history.data() + pos;   // oldest .. newest

        if (! interpolate)
            return std::abs (recent[taps - 1 - delay]);

        float peak = 0.0f;

        for (const auto& phase : phases)
        {
            float y = 0.0f;
            for (int k = 0; k < taps; ++k) y += phase[(size_t) k] * recent[taps - 1 - k];
            peak = std::max (peak, std::abs (y));
        }

        return peak;
    }

private:
    std::array<std::array<float, taps>, factor> phases {};
    std::array<float, taps * 2> history {};
    int pos = 0;
};

//==============================================================================
class LimiterProcessor final : public Processor
{
public:
    void set (const LimiterParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = std::max (1, ch);
        handoff.fetch (params);
        lookahead = std::max (1, juce::roundToInt (0.002 * rate));   // 2 ms
        detectors.assign ((size_t) channels, {});
        delayLines.assign ((size_t) channels, std::vector<float> ((size_t) latencySamples() + 1, 0.0f));
        holdRing.assign ((size_t) lookahead, 1.0);
        boxRing.assign ((size_t) lookahead, 1.0);
        reset();
    }

    int latencySamples() const override   { return TruePeakDetector::delay + lookahead - 1; }

    void reset() override
    {
        for (auto& d : detectors) d.reset();
        for (auto& line : delayLines) std::fill (line.begin(), line.end(), 0.0f);
        std::fill (holdRing.begin(), holdRing.end(), 1.0);
        std::fill (boxRing.begin(), boxRing.end(), 1.0);
        boxSum = (double) lookahead;
        released = 1.0;
        ringPos = delayPos = 0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        handoff.fetch (params);
        const auto count = std::min (channels, audio.getNumChannels());
        const auto inGain = (float) juce::Decibels::decibelsToGain (params.inputDb);
        const auto ceiling = juce::Decibels::decibelsToGain (params.ceilingDb);
        const auto releaseStep = 1.0 - std::exp (-1.0 / (std::max (1.0, params.releaseMs) * 0.001 * rate));
        const auto delaySize = (int) delayLines[0].size();
        float inPeak = 0.0f, outPeak = 0.0f, deepest = 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float detected = 0.0f;

            for (int ch = 0; ch < count; ++ch)
            {
                const auto x = audio.getSample (ch, start + i) * inGain;
                inPeak = std::max (inPeak, std::abs (x));
                delayLines[(size_t) ch][(size_t) delayPos] = x;
                detected = std::max (detected, detectors[(size_t) ch].push (x, params.truePeak));
            }

            // The gain this sample needs, held over the look-ahead...
            const auto needed = detected > ceiling ? ceiling / detected : 1.0;
            holdRing[(size_t) ringPos] = needed;
            auto held = 1.0;
            for (auto v : holdRing) held = std::min (held, v);

            // ... let go slowly (release), never above what's needed ...
            released = std::min (held, released + (1.0 - released) * releaseStep);

            // ... and averaged over the look-ahead, so it ramps down in time.
            boxSum += released - boxRing[(size_t) ringPos];
            boxRing[(size_t) ringPos] = released;
            ringPos = (ringPos + 1) % lookahead;
            const auto gain = (float) std::min (1.0, boxSum / lookahead);
            deepest = std::min (deepest, gain);

            const auto readPos = (delayPos + 1) % delaySize;   // the oldest = `latency` samples back

            for (int ch = 0; ch < count; ++ch)
            {
                const auto out = delayLines[(size_t) ch][(size_t) readPos] * gain;
                outPeak = std::max (outPeak, std::abs (out));
                audio.setSample (ch, start + i, out);
            }

            delayPos = readPos;
        }

        inputLevelDb = toDb (inPeak);
        outputLevelDb = toDb (outPeak);
        gainReductionDb = toDb (deepest);
    }

private:
    ParamHandoff<LimiterParams> handoff;
    LimiterParams params;
    double rate = 44100.0, boxSum = 0.0, released = 1.0;
    int channels = 2, lookahead = 88, ringPos = 0, delayPos = 0;
    std::vector<TruePeakDetector> detectors;
    std::vector<std::vector<float>> delayLines;
    std::vector<double> holdRing, boxRing;
};

//==============================================================================
class GateProcessor final : public Processor
{
public:
    void set (const GateParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = std::max (1, ch);
        handoff.fetch (params);
        reset();
    }

    void reset() override
    {
        envelope = 0.0;
        gainDb = 0.0;
        open = true;
        holdLeft = 0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        handoff.fetch (params);
        const auto count = std::min (channels, audio.getNumChannels());
        const auto envFall = std::exp (-1.0 / (0.01 * rate));
        const auto rangeDb = juce::jlimit (-100.0, 0.0, params.rangeDb);
        const auto attackStep = -rangeDb / std::max (1.0, params.attackMs * 0.001 * rate);
        const auto releaseStep = -rangeDb / std::max (1.0, params.releaseMs * 0.001 * rate);
        const auto openAt = juce::Decibels::decibelsToGain (params.thresholdDb);
        const auto closeAt = juce::Decibels::decibelsToGain (params.thresholdDb - 4.0);
        const auto holdSamples = (int) (params.holdMs * 0.001 * rate);
        float inPeak = 0.0f, outPeak = 0.0f, lowest = 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            double level = 0.0;

            for (int ch = 0; ch < count; ++ch)
                level = std::max (level, (double) std::abs (audio.getSample (ch, start + i)));

            envelope = std::max (level, envelope * envFall);

            if (envelope >= openAt)
            {
                open = true;
                holdLeft = holdSamples;
            }
            else if (envelope < closeAt)
            {
                if (holdLeft > 0) --holdLeft;
                else              open = false;
            }

            // Moves in dB at a steady pace: fully open / closed after the
            // attack / release time.
            const auto targetDb = open ? 0.0 : rangeDb;
            gainDb = targetDb > gainDb ? std::min (targetDb, gainDb + attackStep) : std::max (targetDb, gainDb - releaseStep);
            const auto gain = juce::Decibels::decibelsToGain (gainDb, -100.0);
            lowest = std::min (lowest, (float) gain);

            for (int ch = 0; ch < count; ++ch)
            {
                const auto v = audio.getSample (ch, start + i);
                inPeak = std::max (inPeak, std::abs (v));
                const auto out = v * (float) gain;
                outPeak = std::max (outPeak, std::abs (out));
                audio.setSample (ch, start + i, out);
            }
        }

        inputLevelDb = toDb (inPeak);
        outputLevelDb = toDb (outPeak);
        gainReductionDb = toDb (lowest);
    }

private:
    ParamHandoff<GateParams> handoff;
    GateParams params;
    double rate = 44100.0, envelope = 0.0, gainDb = 0.0;
    bool open = true;
    int channels = 2, holdLeft = 0;
};

//==============================================================================
class ModulationProcessor final : public Processor
{
public:
    void set (const ModulationParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = std::max (1, ch);
        handoff.fetch (params);
        size = juce::nextPowerOfTwo ((int) (0.1 * rate) + 8);
        lines.assign ((size_t) channels, std::vector<float> ((size_t) size, 0.0f));
        lastWet.assign ((size_t) channels, 0.0f);
        reset();
    }

    void reset() override
    {
        for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f);
        std::fill (lastWet.begin(), lastWet.end(), 0.0f);
        writePos = 0;
        phase = 0.0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        handoff.fetch (params);
        const auto count = std::min (channels, audio.getNumChannels());
        const auto voices = params.mode == ModulationParams::chorus ? juce::jlimit (1, 4, params.voices) : 1;
        // Equal-power blend: the dry and the (mostly unrelated) wet sound add
        // up to about the same loudness at any mix.
        const auto mix = params.mode == ModulationParams::vibrato ? 1.0 : juce::jlimit (0.0, 1.0, params.mix);
        const auto dryGain = (float) std::cos (mix * juce::MathConstants<double>::halfPi);
        const auto wetGain = (float) std::sin (mix * juce::MathConstants<double>::halfPi);
        const auto feedback = params.mode == ModulationParams::flanger ? (float) juce::jlimit (-0.95, 0.95, params.feedback) : 0.0f;
        const auto step = params.rateHz / rate;
        const auto baseSamples = params.delayMs * 0.001 * rate, depthSamples = params.depthMs * 0.001 * rate;
        const auto maxDelay = (double) size - 4.0;

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < count; ++ch)
            {
                auto& line = lines[(size_t) ch];
                const auto x = audio.getSample (ch, start + i);
                line[(size_t) writePos] = x + feedback * lastWet[(size_t) ch];

                // The second channel's sweep runs `spread` half a cycle apart.
                const auto channelOffset = ch == 1 ? params.spread * 0.5 : 0.0;
                float wet = 0.0f;

                for (int v = 0; v < voices; ++v)
                {
                    const auto p = phase + channelOffset + (double) v / voices;
                    const auto lfo = 0.5 * (1.0 + std::sin (juce::MathConstants<double>::twoPi * p));
                    const auto d = juce::jlimit (1.0, maxDelay, baseSamples + depthSamples * lfo);
                    wet += read (line, d);
                }

                wet /= std::sqrt ((float) voices);
                lastWet[(size_t) ch] = wet;
                audio.setSample (ch, start + i, dryGain * x + wetGain * wet);
            }

            writePos = (writePos + 1) & (size - 1);
            phase += step;

            if (phase >= 1.0)
                phase -= 1.0;
        }
    }

private:
    // Cubic (Hermite) read `delay` samples behind the write position.
    float read (const std::vector<float>& line, double delay) const
    {
        const auto pos = (double) writePos - delay;
        const auto whole = (int) std::floor (pos);
        const auto t = (float) (pos - whole);
        const auto mask = size - 1;
        const auto y0 = line[(size_t) ((whole - 1) & mask)], y1 = line[(size_t) (whole & mask)];
        const auto y2 = line[(size_t) ((whole + 1) & mask)], y3 = line[(size_t) ((whole + 2) & mask)];
        const auto c1 = 0.5f * (y2 - y0);
        const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    ParamHandoff<ModulationParams> handoff;
    ModulationParams params;
    double rate = 44100.0, phase = 0.0;
    int channels = 2, size = 8192, writePos = 0;
    std::vector<std::vector<float>> lines;
    std::vector<float> lastWet;
};

//==============================================================================
class DistortionProcessor final : public Processor
{
public:
    void set (const DistortionParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int maxBlock) override
    {
        rate = r;
        channels = std::max (1, ch);
        handoff.fetch (params);
        blockSize = std::max (64, maxBlock);
        oversampling = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) channels, 2,
                           juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        oversampling->initProcessing ((size_t) blockSize);
        latency = juce::roundToInt (oversampling->getLatencyInSamples());
        dryLines.assign ((size_t) channels, std::vector<float> ((size_t) latency + 1, 0.0f));
        dry.setSize (channels, blockSize);
        tone.assign ((size_t) channels, 0.0);
        dcIn.assign ((size_t) channels, 0.0);
        dcOut.assign ((size_t) channels, 0.0);
        reset();
    }

    int latencySamples() const override   { return latency; }

    void reset() override
    {
        if (oversampling != nullptr)
            oversampling->reset();

        for (auto& l : dryLines) std::fill (l.begin(), l.end(), 0.0f);
        std::fill (tone.begin(), tone.end(), 0.0);
        std::fill (dcIn.begin(), dcIn.end(), 0.0);
        std::fill (dcOut.begin(), dcOut.end(), 0.0);
        dryPos = 0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        handoff.fetch (params);

        for (int done = 0; done < numSamples; done += blockSize)
            processChunk (audio, start + done, std::min (blockSize, numSamples - done));
    }

private:
    void processChunk (juce::AudioBuffer<float>& audio, int start, int n)
    {
        const auto count = std::min (channels, audio.getNumChannels());
        const auto drive = (float) juce::Decibels::decibelsToGain (params.driveDb);
        const auto output = (float) juce::Decibels::decibelsToGain (params.outputDb);
        const auto mix = (float) juce::jlimit (0.0, 1.0, params.mix);
        const auto toneCoeff = std::exp (-juce::MathConstants<double>::twoPi * juce::jlimit (200.0, rate * 0.49, params.toneHz) / rate);
        const auto dcCoeff = 1.0 - juce::MathConstants<double>::twoPi * 10.0 / rate;
        const auto type = params.type;
        float inPeak = 0.0f, outPeak = 0.0f;

        for (int ch = 0; ch < count; ++ch)
        {
            dry.copyFrom (ch, 0, audio, ch, start, n);
            audio.applyGain (ch, start, n, drive);
            inPeak = std::max (inPeak, audio.getMagnitude (ch, start, n));
        }

        // Shaped at 4x the rate, so the new harmonics don't fold back as noise.
        float* pointers[8] {};

        for (int ch = 0; ch < count && ch < 8; ++ch)
            pointers[ch] = audio.getWritePointer (ch, start);

        juce::dsp::AudioBlock<float> block (pointers, (size_t) std::min (count, 8), (size_t) n);
        auto up = oversampling->processSamplesUp (block);

        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            auto* x = up.getChannelPointer (ch);

            for (size_t i = 0; i < up.getNumSamples(); ++i)
                x[i] = (float) distortionCurve (type, (double) x[i]);
        }

        oversampling->processSamplesDown (block);

        const auto dryLength = (int) dryLines[0].size();
        auto pos = dryPos;

        for (int ch = 0; ch < count && ch < 8; ++ch)
        {
            auto& line = dryLines[(size_t) ch];
            auto* x = audio.getWritePointer (ch, start);
            const auto* d = dry.getReadPointer (ch);
            pos = dryPos;

            for (int i = 0; i < n; ++i)
            {
                // Tone (a gentle low-pass) and, for the lopsided tube curve, DC removal.
                auto y = (double) x[i];
                tone[(size_t) ch] = (1.0 - toneCoeff) * y + toneCoeff * tone[(size_t) ch];
                y = tone[(size_t) ch];
                const auto blocked = y - dcIn[(size_t) ch] + dcCoeff * dcOut[(size_t) ch];
                dcIn[(size_t) ch] = y;
                dcOut[(size_t) ch] = blocked;

                // The dry signal, delayed as much as the oversampling delays the effect.
                line[(size_t) pos] = d[i];
                const auto readPos = (pos + 1) % dryLength;
                const auto delayedDry = line[(size_t) readPos];
                pos = readPos;

                const auto out = ((1.0f - mix) * delayedDry + mix * (float) blocked) * output;
                outPeak = std::max (outPeak, std::abs (out));
                x[i] = out;
            }
        }

        dryPos = pos;
        inputLevelDb = toDb (inPeak);
        outputLevelDb = toDb (outPeak);
    }

    ParamHandoff<DistortionParams> handoff;
    DistortionParams params;
    double rate = 44100.0;
    int channels = 2, blockSize = 4096, latency = 0, dryPos = 0;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    std::vector<std::vector<float>> dryLines;
    juce::AudioBuffer<float> dry;
    std::vector<double> tone, dcIn, dcOut;
};
} // namespace

//==============================================================================
ModulationParams modulationDefaults (ModulationParams::Mode mode)
{
    ModulationParams p;
    p.mode = mode;

    switch (mode)
    {
        case ModulationParams::chorus:  p.rateHz = 0.8;  p.depthMs = 4.0; p.delayMs = 18.0; p.feedback = 0.0; p.mix = 0.5; p.voices = 3; break;
        case ModulationParams::flanger: p.rateHz = 0.25; p.depthMs = 3.0; p.delayMs = 1.0;  p.feedback = 0.6; p.mix = 0.5; p.voices = 1; break;
        case ModulationParams::vibrato: p.rateHz = 5.0;  p.depthMs = 2.0; p.delayMs = 0.5;  p.feedback = 0.0; p.mix = 1.0; p.voices = 1; break;
    }

    return p;
}

const char* distortionTypeName (DistortionParams::Type t)
{
    switch (t)
    {
        case DistortionParams::soft: return "Soft (overdrive)";
        case DistortionParams::hard: return "Hard clip (fuzz)";
        case DistortionParams::tube: return "Tube (warm, uneven)";
        case DistortionParams::fold: return "Wave fold (synth)";
    }

    return "";
}

double distortionCurve (DistortionParams::Type type, double x)
{
    switch (type)
    {
        case DistortionParams::soft: return std::tanh (x);
        case DistortionParams::hard: return juce::jlimit (-1.0, 1.0, x);
        case DistortionParams::tube: return std::tanh (x + 0.3) - std::tanh (0.3);   // even harmonics too
        case DistortionParams::fold: return std::sin (x);
    }

    return x;
}

std::shared_ptr<Processor> makeLimiter()     { return std::make_shared<LimiterProcessor>(); }
std::shared_ptr<Processor> makeGate()        { return std::make_shared<GateProcessor>(); }
std::shared_ptr<Processor> makeModulation()  { return std::make_shared<ModulationProcessor>(); }
std::shared_ptr<Processor> makeDistortion()  { return std::make_shared<DistortionProcessor>(); }

void setParams (Processor& p, const LimiterParams& v)     { if (auto* x = dynamic_cast<LimiterProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const GateParams& v)        { if (auto* x = dynamic_cast<GateProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const ModulationParams& v)  { if (auto* x = dynamic_cast<ModulationProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const DistortionParams& v)  { if (auto* x = dynamic_cast<DistortionProcessor*> (&p)) x->set (v); }

std::optional<double> findLimiterGain (const juce::AudioBuffer<float>& audio, double sampleRate, LimiterParams params,
                                       double targetLufs, const std::function<bool (float)>& progress)
{
    auto loudnessWith = [&] (double gainDb, int step) -> std::optional<double>
    {
        params.inputDb = gainDb;
        auto limiter = makeLimiter();
        setParams (*limiter, params);
        auto stage = [&] (float p) { return progress == nullptr || progress (((float) step + p) / 8.0f); };
        const auto rendered = render (*limiter, audio, sampleRate, 0, [&] (float p) { return stage (p * 0.5f); });

        if (! rendered.has_value())
            return {};

        const auto stats = measure (*rendered, sampleRate, { 0, rendered->getNumSamples() },
                                    [&] (float p) { return stage (0.5f + p * 0.5f); });

        if (! stats.has_value())
            return {};

        return stats->integrated;
    };

    // Start from the plain gain, then secant steps (limiting gives back less
    // than each extra dB, so a few steps are needed).
    const auto plain = measure (audio, sampleRate, { 0, audio.getNumSamples() });

    if (! plain.has_value() || plain->integrated <= -70.0)
        return {};

    double g0 = juce::jlimit (-30.0, 30.0, targetLufs - plain->integrated);
    auto l0 = loudnessWith (g0, 0);

    if (! l0.has_value())
        return {};

    double g1 = juce::jlimit (-30.0, 30.0, g0 + (targetLufs - *l0) * 1.5);
    auto l1 = loudnessWith (g1, 1);

    for (int step = 2; step < 8 && l1.has_value(); ++step)
    {
        if (std::abs (*l1 - targetLufs) < 0.05)
            break;

        const auto slope = std::abs (*l1 - *l0) > 1.0e-4 ? (g1 - g0) / (*l1 - *l0) : 1.5;
        const auto next = juce::jlimit (-30.0, 30.0, g1 + juce::jlimit (0.5, 6.0, slope) * (targetLufs - *l1));
        g0 = g1; l0 = l1;
        g1 = next;
        l1 = loudnessWith (g1, step);
    }

    if (! l1.has_value() || std::abs (*l1 - targetLufs) > 0.3)
        return {};

    return g1;
}
} // namespace wf::fx
