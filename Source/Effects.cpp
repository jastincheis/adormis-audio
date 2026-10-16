#include "Effects.h"
#include <juce_dsp/juce_dsp.h>

namespace wf::fx
{
namespace
{
//==============================================================================
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    double responseDb (double hz, double rate) const
    {
        const auto w = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, rate * 0.4999, hz) / rate;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
        const auto h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
        return 20.0 * std::log10 (std::abs (h) + 1.0e-20);
    }
};

Biquad normalise (double b0, double b1, double b2, double a0, double a1, double a2)
{
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

// RBJ cookbook filters.
Biquad design (EqBand::Type type, double hz, double gainDb, double q, double rate)
{
    hz = juce::jlimit (10.0, rate * 0.49, hz);
    q = std::max (0.05, q);
    const auto A = std::pow (10.0, gainDb / 40.0);
    const auto w = juce::MathConstants<double>::twoPi * hz / rate;
    const auto cw = std::cos (w), sw = std::sin (w);
    const auto alpha = sw / (2.0 * q);

    switch (type)
    {
        case EqBand::bell:
            return normalise (1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);

        case EqBand::lowShelf:
        {
            const auto s = 2.0 * std::sqrt (A) * alpha;
            return normalise (A * ((A + 1) - (A - 1) * cw + s), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - s),
                              (A + 1) + (A - 1) * cw + s, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - s);
        }

        case EqBand::highShelf:
        {
            const auto s = 2.0 * std::sqrt (A) * alpha;
            return normalise (A * ((A + 1) + (A - 1) * cw + s), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - s),
                              (A + 1) - (A - 1) * cw + s, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - s);
        }

        case EqBand::lowCut:
            return normalise ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);

        case EqBand::highCut:
            return normalise ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);

        case EqBand::notch:
            return normalise (1, -2 * cw, 1, 1 + alpha, -2 * cw, 1 - alpha);

        case EqBand::bandPass:
            return normalise (alpha, 0, -alpha, 1 + alpha, -2 * cw, 1 - alpha);
    }

    return {};
}

// A band as up to 4 biquads: the cuts of 24 / 36 / 48 dB per octave are
// Butterworth cascades; 12 dB uses the band's own Q (resonance).
int sections (const EqBand& band, double rate, Biquad* out)
{
    if (band.type == EqBand::lowCut || band.type == EqBand::highCut)
    {
        const auto n = juce::jlimit (1, 4, band.slope / 12);

        if (n == 1)
        {
            out[0] = design (band.type, band.frequency, 0.0, band.q, rate);
            return 1;
        }

        const auto order = 2 * n;

        for (int k = 1; k <= n; ++k)
        {
            const auto q = 1.0 / (2.0 * std::cos (juce::MathConstants<double>::pi * (2 * k - 1) / (2.0 * order)));
            out[k - 1] = design (band.type, band.frequency, 0.0, q, rate);
        }

        return n;
    }

    out[0] = design (band.type, band.frequency, band.gainDb, band.q, rate);
    return 1;
}

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

    // Copies new parameters into `current`; true when they changed.
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

//==============================================================================
class EqProcessor final : public Processor
{
public:
    void set (const EqParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = ch;
        handoff.fetch (params);
        rebuild();
        reset();
    }

    void reset() override
    {
        for (auto& s : state)
            s = {};
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        if (handoff.fetch (params))
            rebuild();

        const auto gain = (float) juce::Decibels::decibelsToGain (params.outputDb);

        for (int ch = 0; ch < std::min (channels, audio.getNumChannels()); ++ch)
        {
            auto* d = audio.getWritePointer (ch, start);
            auto* st = state[(size_t) ch].data();

            for (int i = 0; i < numSamples; ++i)
            {
                auto y = (double) d[i];

                for (int k = 0; k < sectionCount; ++k)
                {
                    const auto& c = coeffs[(size_t) k];
                    auto& z = st[k];
                    const auto out = c.b0 * y + z.z1;
                    z.z1 = c.b1 * y - c.a1 * out + z.z2;
                    z.z2 = c.b2 * y - c.a2 * out;
                    y = out;
                }

                d[i] = (float) y * gain;
            }
        }
    }

private:
    void rebuild()
    {
        sectionCount = 0;

        for (int b = 0; b < params.count; ++b)
        {
            const auto& band = params.bands[(size_t) b];

            if (! band.on || (bandHasGain (band.type) && std::abs (band.gainDb) < 0.01))
                continue;

            sectionCount += sections (band, rate, coeffs.data() + sectionCount);
        }
    }

    struct Z { double z1 = 0, z2 = 0; };   // transposed direct form II

    ParamHandoff<EqParams> handoff;
    EqParams params;
    double rate = 44100.0;
    int channels = 2;
    std::array<Biquad, maxEqBands * 4> coeffs {};
    int sectionCount = 0;
    std::array<std::array<Z, maxEqBands * 4>, 8> state {};
};

//==============================================================================
class CompressorProcessor final : public Processor
{
public:
    void set (const CompressorParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = ch;
        handoff.fetch (params);
        reset();
    }

    void reset() override
    {
        envelope = 0.0;
        gainDb = 0.0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        handoff.fetch (params);

        const auto attack = std::exp (-1.0 / (std::max (0.05, params.attackMs) * 0.001 * rate));
        const auto release = std::exp (-1.0 / (std::max (1.0, params.releaseMs) * 0.001 * rate));
        const auto rmsCoeff = std::exp (-1.0 / (0.01 * rate));   // 10 ms RMS window
        const auto makeup = params.makeupDb + (params.autoMakeup ? autoMakeupDb (params) : 0.0);
        const auto count = std::min (channels, audio.getNumChannels());
        float inPeak = 0.0f, outPeak = 0.0f, deepest = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            double level = 0.0;

            for (int ch = 0; ch < count; ++ch)
                level = std::max (level, (double) std::abs (audio.getSample (ch, start + i)));

            if (params.rms)
            {
                envelope = rmsCoeff * envelope + (1.0 - rmsCoeff) * level * level;
                level = std::sqrt (envelope);
            }

            const auto inDb = juce::Decibels::gainToDecibels (level, -120.0);
            const auto targetDb = compressorCurveDb (params, inDb) - inDb;   // <= 0

            // Smooth the gain change: fast when compressing more, slow when letting go.
            const auto coeff = targetDb < gainDb ? attack : release;
            gainDb = coeff * gainDb + (1.0 - coeff) * targetDb;

            const auto g = (float) juce::Decibels::decibelsToGain (gainDb + makeup);

            for (int ch = 0; ch < count; ++ch)
            {
                const auto v = audio.getSample (ch, start + i);
                inPeak = std::max (inPeak, std::abs (v));
                const auto out = v * g;
                outPeak = std::max (outPeak, std::abs (out));
                audio.setSample (ch, start + i, out);
            }

            deepest = std::min (deepest, (float) gainDb);
        }

        inputLevelDb = juce::Decibels::gainToDecibels (inPeak, -100.0f);
        outputLevelDb = juce::Decibels::gainToDecibels (outPeak, -100.0f);
        gainReductionDb = deepest;
    }

private:
    ParamHandoff<CompressorParams> handoff;
    CompressorParams params;
    double rate = 44100.0, envelope = 0.0, gainDb = 0.0;
    int channels = 2;
};

//==============================================================================
// One-pole filters on the echoes / reverb tail.
struct OnePole
{
    double a = 0.0, z = 0.0;

    void lowPass (double hz, double rate)   { a = std::exp (-juce::MathConstants<double>::twoPi * hz / rate); }
    double lp (double x)                    { z = (1.0 - a) * x + a * z; return z; }
    double hp (double x)                    { return x - lp (x); }
};

class DelayProcessor final : public Processor
{
public:
    void set (const DelayParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int) override
    {
        rate = r;
        channels = std::max (1, ch);
        handoff.fetch (params);
        lines.assign ((size_t) std::max (2, channels), std::vector<float> ((size_t) (rate * 4.1) + 1, 0.0f));
        reset();
    }

    void reset() override
    {
        for (auto& l : lines)
            std::fill (l.begin(), l.end(), 0.0f);

        filters.assign (lines.size() * 2, {});
        write = 0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        handoff.fetch (params);

        if (lines.empty())
            return;

        const auto size = (int) lines[0].size();
        const auto delay = juce::jlimit (1, size - 1, (int) std::lround (params.effectiveTimeMs() * 0.001 * rate));
        const auto feedback = juce::jlimit (0.0, 0.97, params.feedback);
        const auto mix = juce::jlimit (0.0, 1.0, params.mix);
        const auto count = std::min (channels, audio.getNumChannels());
        const auto pingPong = params.pingPong && count == 2;

        for (size_t k = 0; k < lines.size(); ++k)
        {
            filters[k * 2].lowPass (params.highCutHz, rate);
            filters[k * 2 + 1].lowPass (params.lowCutHz, rate);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const auto read = (write - delay + size) % size;
            float echoes[2] { lines[0][(size_t) read], lines[std::min<size_t> (1, lines.size() - 1)][(size_t) read] };

            for (int ch = 0; ch < count; ++ch)
            {
                const auto dry = audio.getSample (ch, start + i);
                const auto echo = echoes[ch];

                // Ping-pong: each side's echo feeds the other side (the input enters on the left).
                const auto source = pingPong ? (ch == 0 ? (audio.getSample (0, start + i) + audio.getSample (1, start + i)) * 0.5f : 0.0f) : dry;
                const auto back = pingPong ? echoes[1 - ch] : echo;
                auto fed = (double) source + feedback * back;
                fed = filters[(size_t) ch * 2].lp (fed);
                fed = filters[(size_t) ch * 2 + 1].hp (fed);
                lines[(size_t) ch][(size_t) write] = (float) fed;

                audio.setSample (ch, start + i, (float) (dry * (1.0 - mix * 0.5) + echo * mix));
            }

            write = (write + 1) % size;
        }
    }

private:
    ParamHandoff<DelayParams> handoff;
    DelayParams params;
    double rate = 44100.0;
    int channels = 2, write = 0;
    std::vector<std::vector<float>> lines;
    std::vector<OnePole> filters;
};

//==============================================================================
class ReverbProcessor final : public Processor
{
public:
    void set (const ReverbParams& p)   { handoff.set (p); }

    void prepare (double r, int ch, int maxBlock) override
    {
        rate = r;
        channels = std::max (1, ch);
        handoff.fetch (params);
        reverb.setSampleRate (rate);
        wet.setSize (2, std::max (1, maxBlock));
        preDelay.assign (2, std::vector<float> ((size_t) (rate * 0.21) + 1, 0.0f));
        filters.assign (4, {});
        reset();
        applyParams();
    }

    void reset() override
    {
        reverb.reset();

        for (auto& p : preDelay)
            std::fill (p.begin(), p.end(), 0.0f);

        write = 0;
    }

    void process (juce::AudioBuffer<float>& audio, int start, int numSamples) override
    {
        if (handoff.fetch (params))
            applyParams();

        // Long blocks are done in pieces that fit the wet buffer.
        for (int done = 0; done < numSamples;)
        {
            const auto n = std::min (numSamples - done, wet.getNumSamples());
            processBlock (audio, start + done, n);
            done += n;
        }
    }

private:
    void applyParams()
    {
        juce::Reverb::Parameters p;
        p.roomSize = (float) params.roomSize;
        p.damping = (float) params.damping;
        p.width = (float) params.width;
        p.wetLevel = 1.0f;     // the mix is done here, after the filters
        p.dryLevel = 0.0f;
        p.freezeMode = params.freeze ? 1.0f : 0.0f;
        reverb.setParameters (p);

        for (int ch = 0; ch < 2; ++ch)
        {
            filters[(size_t) ch * 2].lowPass (params.highCutHz, rate);
            filters[(size_t) ch * 2 + 1].lowPass (params.lowCutHz, rate);
        }
    }

    void processBlock (juce::AudioBuffer<float>& audio, int start, int n)
    {
        const auto count = std::min (channels, audio.getNumChannels());
        const auto size = (int) preDelay[0].size();
        const auto delay = juce::jlimit (0, size - 1, (int) std::lround (params.preDelayMs * 0.001 * rate));

        // The input, delayed, into the wet buffer.
        for (int i = 0; i < n; ++i)
        {
            const auto read = (write - delay + size) % size;

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto in = audio.getSample (std::min (ch, count - 1), start + i);
                preDelay[(size_t) ch][(size_t) write] = in;
                wet.setSample (ch, i, preDelay[(size_t) ch][(size_t) read]);
            }

            write = (write + 1) % size;
        }

        if (count >= 2)
            reverb.processStereo (wet.getWritePointer (0), wet.getWritePointer (1), n);
        else
            reverb.processMono (wet.getWritePointer (0), n);

        for (int ch = 0; ch < count; ++ch)
        {
            auto* w = wet.getWritePointer (ch);
            auto& lp = filters[(size_t) ch * 2];
            auto& hp = filters[(size_t) ch * 2 + 1];

            for (int i = 0; i < n; ++i)
            {
                const auto shaped = hp.hp (lp.lp (w[i]));
                const auto dry = audio.getSample (ch, start + i);
                audio.setSample (ch, start + i, (float) (dry * params.dry + shaped * params.wet));
            }
        }
    }

    ParamHandoff<ReverbParams> handoff;
    ReverbParams params;
    juce::Reverb reverb;
    juce::AudioBuffer<float> wet;
    std::vector<std::vector<float>> preDelay;
    std::vector<OnePole> filters;
    double rate = 44100.0;
    int channels = 2, write = 0;
};
} // namespace

//==============================================================================
const char* bandTypeName (EqBand::Type t)
{
    switch (t)
    {
        case EqBand::bell:       return "Bell";
        case EqBand::lowShelf:   return "Low shelf";
        case EqBand::highShelf:  return "High shelf";
        case EqBand::lowCut:     return "Low cut";
        case EqBand::highCut:    return "High cut";
        case EqBand::notch:      return "Notch";
        case EqBand::bandPass:   return "Band pass";
    }

    return "Bell";
}

bool bandHasGain (EqBand::Type t)   { return t == EqBand::bell || t == EqBand::lowShelf || t == EqBand::highShelf; }

double bandResponseDb (const EqBand& band, double hz, double rate)
{
    if (! band.on)
        return 0.0;

    Biquad s[4];
    const auto n = sections (band, rate, s);
    double total = 0.0;

    for (int i = 0; i < n; ++i)
        total += s[i].responseDb (hz, rate);

    return total;
}

double eqResponseDb (const EqParams& p, double hz, double rate)
{
    auto total = p.outputDb;

    for (int b = 0; b < p.count; ++b)
        total += bandResponseDb (p.bands[(size_t) b], hz, rate);

    return total;
}

double compressorCurveDb (const CompressorParams& p, double in)
{
    const auto t = p.thresholdDb, r = std::max (1.0, p.ratio), k = std::max (0.0, p.kneeDb);

    if (2.0 * (in - t) < -k)
        return in;

    if (k > 0.0 && 2.0 * std::abs (in - t) <= k)
    {
        const auto x = in - t + k / 2.0;
        return in + (1.0 / r - 1.0) * x * x / (2.0 * k);
    }

    return t + (in - t) / r;
}

double autoMakeupDb (const CompressorParams& p)
{
    // Half of what a 0 dB peak loses: louder without overshooting.
    return -0.5 * (compressorCurveDb (p, 0.0) - 0.0);
}

const std::vector<DelayNote>& delayNotes()
{
    static const std::vector<DelayNote> notes {
        { "1/2", 2.0 }, { "1/4 dotted", 1.5 }, { "1/4 triplet", 2.0 / 3.0 }, { "1/4", 1.0 }, { "1/8 dotted", 0.75 },
        { "1/8 triplet", 1.0 / 3.0 }, { "1/8", 0.5 }, { "1/16 dotted", 0.375 }, { "1/16", 0.25 }, { "1/32", 0.125 } };
    return notes;
}

double DelayParams::effectiveTimeMs() const
{
    if (! sync)
        return timeMs;

    const auto& notes = delayNotes();
    return 60000.0 / std::max (20.0, bpm) * notes[(size_t) juce::jlimit (0, (int) notes.size() - 1, note)].beats;
}

double suggestedTailSeconds (const DelayParams& p)
{
    // Until the echoes are 60 dB down.
    const auto fb = juce::jlimit (0.01, 0.97, p.feedback);
    const auto repeats = std::log (0.001) / std::log (fb);
    return juce::jlimit (0.2, 20.0, p.effectiveTimeMs() * 0.001 * (repeats + 1.0));
}

double suggestedTailSeconds (const ReverbParams& p)
{
    return juce::jlimit (0.5, 12.0, 0.6 + p.roomSize * p.roomSize * 6.0 + p.preDelayMs * 0.001);
}

std::shared_ptr<Processor> makeEq()          { return std::make_shared<EqProcessor>(); }
std::shared_ptr<Processor> makeCompressor()  { return std::make_shared<CompressorProcessor>(); }
std::shared_ptr<Processor> makeDelay()       { return std::make_shared<DelayProcessor>(); }
std::shared_ptr<Processor> makeReverb()      { return std::make_shared<ReverbProcessor>(); }

void setParams (Processor& p, const EqParams& v)          { if (auto* x = dynamic_cast<EqProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const CompressorParams& v)  { if (auto* x = dynamic_cast<CompressorProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const DelayParams& v)       { if (auto* x = dynamic_cast<DelayProcessor*> (&p)) x->set (v); }
void setParams (Processor& p, const ReverbParams& v)      { if (auto* x = dynamic_cast<ReverbProcessor*> (&p)) x->set (v); }

std::optional<juce::AudioBuffer<float>> render (Processor& processor, const juce::AudioBuffer<float>& audio, double rate,
                                                int tailSamples, const std::function<bool (float)>& progress)
{
    constexpr int block = 4096;
    const auto length = audio.getNumSamples() + std::max (0, tailSamples);
    juce::AudioBuffer<float> out (audio.getNumChannels(), length);
    out.clear();

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        out.copyFrom (ch, 0, audio, ch, 0, audio.getNumSamples());

    processor.prepare (rate, audio.getNumChannels(), block);

    for (int done = 0; done < length; done += block)
    {
        processor.process (out, done, std::min (block, length - done));

        if (progress != nullptr && ! progress ((float) (done + block) / (float) length))
            return {};
    }

    return out;
}
} // namespace wf::fx
