#include "Synthesis.h"

namespace wf::synth
{
namespace
{
constexpr double twoPi = juce::MathConstants<double>::twoPi;

// Short fades (5 ms) so the tone starts and ends at silence.
void edgeFades (juce::AudioBuffer<float>& b, double rate)
{
    const auto n = std::min (b.getNumSamples() / 2, (int) (rate * 0.005));

    if (n <= 0)
        return;

    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        b.applyGainRamp (c, 0, n, 0.0f, 1.0f);
        b.applyGainRamp (c, b.getNumSamples() - n, n, 1.0f, 0.0f);
    }
}

void copyToAll (juce::AudioBuffer<float>& b)
{
    for (int c = 1; c < b.getNumChannels(); ++c)
        b.copyFrom (c, 0, b, 0, 0, b.getNumSamples());
}
}

juce::StringArray waveNames()
{
    return { "Sine", "Square", "Sawtooth", "Triangle", "White noise", "Pink noise" };
}

juce::AudioBuffer<float> simple (const SimpleSettings& s, double rate)
{
    const auto length = std::max (1, (int) std::llround (s.seconds * rate));
    juce::AudioBuffer<float> out (juce::jlimit (1, 2, s.channels), length);
    out.clear();
    auto* d = out.getWritePointer (0);
    const auto amp = juce::Decibels::decibelsToGain (s.amplitudeDb);
    juce::Random random (12345);
    double phase = 0.0;
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;   // pink noise (Paul Kellet)
    const auto f0 = juce::jlimit (1.0, rate * 0.49, s.startHz), f1 = juce::jlimit (1.0, rate * 0.49, s.endHz);

    for (int i = 0; i < length; ++i)
    {
        const auto t = length > 1 ? (double) i / (length - 1) : 0.0;
        const auto hz = f0 * std::pow (f1 / f0, t);   // a glide that sounds even
        double v = 0.0;

        switch (s.wave)
        {
            case Wave::sine:       v = std::sin (twoPi * phase); break;
            case Wave::square:     v = phase < 0.5 ? 1.0 : -1.0; break;
            case Wave::sawtooth:   v = 2.0 * phase - 1.0; break;
            case Wave::triangle:   v = phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase; break;
            case Wave::whiteNoise: v = random.nextDouble() * 2.0 - 1.0; break;
            case Wave::pinkNoise:
            {
                const auto w = random.nextDouble() * 2.0 - 1.0;
                b0 = 0.99886 * b0 + w * 0.0555179;  b1 = 0.99332 * b1 + w * 0.0750759;
                b2 = 0.96900 * b2 + w * 0.1538520;  b3 = 0.86650 * b3 + w * 0.3104856;
                b4 = 0.55000 * b4 + w * 0.5329522;  b5 = -0.7616 * b5 - w * 0.0168980;
                v = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.11;
                b6 = w * 0.115926;
                break;
            }
        }

        d[i] = (float) (juce::jlimit (-1.0, 1.0, v) * amp);
        phase += hz / rate;
        phase -= std::floor (phase);
    }

    copyToAll (out);

    // stereo noise: a second, different noise on the right
    if (out.getNumChannels() == 2 && (s.wave == Wave::whiteNoise || s.wave == Wave::pinkNoise))
    {
        juce::Random r2 (999);
        auto* right = out.getWritePointer (1);

        for (int i = 0; i < length; ++i)   // white decorrelated; pink keeps the left's colour but shifted in time
            right[i] = s.wave == Wave::whiteNoise ? (float) ((r2.nextDouble() * 2.0 - 1.0) * amp) : d[(i + length / 2) % length];
    }

    // pink noise comes out of its filters at no exact level: bring its peak to the one asked for
    if (s.wave == Wave::pinkNoise)
        if (const auto peak = out.getMagnitude (0, length); peak > 0.0f)
            out.applyGain ((float) amp / peak);

    edgeFades (out, rate);
    return out;
}

juce::AudioBuffer<float> fm (const FmSettings& s, double rate)
{
    const auto length = std::max (1, (int) std::llround (s.seconds * rate));
    juce::AudioBuffer<float> out (juce::jlimit (1, 2, s.channels), length);
    auto* d = out.getWritePointer (0);
    const auto amp = juce::Decibels::decibelsToGain (s.amplitudeDb);
    const auto carrier = juce::jlimit (1.0, rate * 0.45, s.carrierHz);
    const auto mod = carrier * juce::jlimit (0.01, 32.0, s.ratio);
    const auto attack = std::max (1.0, s.attackMs * 0.001 * rate);
    const auto sustain = juce::jlimit (0.0, 1.0, s.sustain);
    double pc = 0.0, pm = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto t = length > 1 ? (double) i / (length - 1) : 0.0;
        const auto index = s.indexStart + (s.indexEnd - s.indexStart) * t;

        // level: up over the attack, then sliding down to the sustain level
        const auto sinceAttack = std::max (0.0, (double) i - attack) / rate;
        const auto level = i < attack ? i / attack : sustain + (1.0 - sustain) * std::exp (-sinceAttack * 4.0);

        d[i] = (float) (std::sin (twoPi * pc + index * std::sin (twoPi * pm)) * level * amp);
        pc += carrier / rate;  pc -= std::floor (pc);
        pm += mod / rate;      pm -= std::floor (pm);
    }

    copyToAll (out);

    // the last 10 % fades out, so it ends like a note
    const auto tail = std::max (1, length / 10);
    for (int c = 0; c < out.getNumChannels(); ++c)
        out.applyGainRamp (c, length - tail, tail, 1.0f, 0.0f);

    edgeFades (out, rate);
    return out;
}
}
