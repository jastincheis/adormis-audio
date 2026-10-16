#include "Sibilance.h"
#include <juce_dsp/juce_dsp.h>

namespace wf::sibilance
{
namespace
{
struct Coefficients
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

// RBJ cookbook peaking EQ.
Coefficients peaking (double hz, double gainDb, double q, double rate)
{
    hz = juce::jlimit (20.0, rate * 0.49, hz);
    const auto A = std::pow (10.0, gainDb / 40.0);
    const auto w = juce::MathConstants<double>::twoPi * hz / rate;
    const auto alpha = std::sin (w) / (2.0 * std::max (0.05, q));
    const auto cw = std::cos (w);
    const auto a0 = 1.0 + alpha / A;
    return { (1.0 + alpha * A) / a0, -2.0 * cw / a0, (1.0 - alpha * A) / a0, -2.0 * cw / a0, (1.0 - alpha / A) / a0 };
}

constexpr int fftOrder = 10, fftSize = 1 << fftOrder;

std::vector<float> hann (int n)
{
    std::vector<float> w ((size_t) n);

    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (n - 1));

    return w;
}

std::vector<float> mono (const juce::AudioBuffer<float>& audio)
{
    std::vector<float> m ((size_t) audio.getNumSamples(), 0.0f);

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* in = audio.getReadPointer (ch);

        for (size_t i = 0; i < m.size(); ++i)
            m[i] += in[i] / (float) audio.getNumChannels();
    }

    return m;
}
} // namespace

std::vector<Sibilant> detect (const juce::AudioBuffer<float>& audio, double rate, double sensitivity, double defaultGainDb)
{
    const auto signal = mono (audio);
    const auto hop = std::max (1, (int) std::lround (rate * 0.01));   // 10 ms frames

    if ((int) signal.size() < fftSize)
        return {};

    const auto frames = ((int) signal.size() - fftSize) / hop + 1;
    juce::dsp::FFT fft (fftOrder);
    const auto window = hann (fftSize);
    std::vector<float> work (2 * fftSize);

    const auto binHz = rate / fftSize;
    const auto lowFrom = (int) std::ceil (100.0 / binHz), lowTo = (int) std::floor (3000.0 / binHz);
    const auto highFrom = (int) std::ceil (4000.0 / binHz);
    const auto highTo = std::min (fftSize / 2 - 1, (int) std::floor (std::min (12000.0, rate * 0.45) / binHz));

    std::vector<double> ratio ((size_t) frames), highDb ((size_t) frames);
    double loudest = -200.0;

    for (int f = 0; f < frames; ++f)
    {
        std::fill (work.begin(), work.end(), 0.0f);

        for (int i = 0; i < fftSize; ++i)
            work[(size_t) i] = signal[(size_t) (f * hop + i)] * window[(size_t) i];

        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        double low = 0.0, high = 0.0;

        for (int k = lowFrom; k <= lowTo; ++k)    low += (double) work[(size_t) k] * work[(size_t) k];
        for (int k = highFrom; k <= highTo; ++k)  high += (double) work[(size_t) k] * work[(size_t) k];

        ratio[(size_t) f] = high / (low + high + 1.0e-20);
        highDb[(size_t) f] = 10.0 * std::log10 (high / fftSize + 1.0e-20);
        loudest = std::max (loudest, highDb[(size_t) f]);
    }

    // A frame is sibilant when the hiss dominates and is loud enough. More
    // sensitivity = a lower share and a lower level are enough.
    sensitivity = juce::jlimit (0.0, 1.0, sensitivity);
    const auto ratioNeeded = 0.75 - 0.35 * sensitivity;
    const auto levelNeeded = loudest - (22.0 + 20.0 * sensitivity);
    std::vector<bool> hit ((size_t) frames);

    for (int f = 0; f < frames; ++f)
        hit[(size_t) f] = ratio[(size_t) f] > ratioNeeded && highDb[(size_t) f] > levelNeeded;

    // Runs of frames: gaps of up to 3 frames are bridged; 30 ms ... 600 ms kept.
    std::vector<Sibilant> found;
    const auto frameStart = [&] (int f) { return (f * hop + fftSize / 4.0) / rate; };
    const auto frameEnd = [&] (int f) { return (f * hop + fftSize * 0.75) / rate; };

    for (int f = 0; f < frames;)
    {
        if (! hit[(size_t) f])
        {
            ++f;
            continue;
        }

        auto last = f;
        auto g = f + 1;

        while (g < frames && g - last <= 4)
        {
            if (hit[(size_t) g])
                last = g;

            ++g;
        }

        const auto count = last - f + 1;

        if (count >= 3 && count <= 60)
        {
            Sibilant s;
            s.start = std::max (0.0, frameStart (f) - 0.01);
            s.end = std::min ((double) signal.size() / rate, frameEnd (last) + 0.01);

            double level = -200.0;

            for (int k = f; k <= last; ++k)
                level = std::max (level, highDb[(size_t) k]);

            s.levelDb = level;
            found.push_back (s);
        }

        f = last + 1;
    }

    // Each one's peak frequency (3 ... 12 kHz) from its averaged spectrum.
    for (auto& s : found)
    {
        const auto spec = spectrum (audio, rate, s.start, s.end);

        if (spec.empty())
            continue;

        const auto bins = (int) spec.size();
        const auto hz = rate / ((bins - 1) * 2.0);
        const auto from = std::max (1, (int) std::ceil (3000.0 / hz));
        const auto to = std::min (bins - 2, (int) std::floor (std::min (12000.0, rate * 0.45) / hz));
        auto best = from;

        for (int k = from; k <= to; ++k)
            if (spec[(size_t) k] > spec[(size_t) best])
                best = k;

        s.peakHz = juce::jlimit (minFrequency, maxFrequency, best * hz);
        s.bands = { Band { s.peakHz, defaultGainDb, 2.0 } };
    }

    return found;
}

bool Sibilant::changesSound() const
{
    return std::any_of (bands.begin(), bands.end(), [] (const Band& b) { return std::abs (b.gainDb) >= 0.05; });
}

void processOne (juce::AudioBuffer<float>& audio, double rate, const Sibilant& item)
{
    if (! item.changesSound())
        return;

    std::vector<Coefficients> chain;

    for (const auto& b : item.bands)
        if (std::abs (b.gainDb) >= 0.05)
            chain.push_back (peaking (b.frequency, b.gainDb, b.q, rate));

    const auto total = audio.getNumSamples();
    const auto fade = (int) std::lround (0.008 * rate);
    const auto warm = (int) std::lround (0.02 * rate);
    const auto a = juce::jlimit (0, total, (int) std::lround (item.start * rate));
    const auto b = juce::jlimit (a, total, (int) std::lround (item.end * rate));
    const auto from = std::max (0, a - fade), to = std::min (total, b + fade);
    const auto warmFrom = std::max (0, from - warm);

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        auto* x = audio.getWritePointer (ch);
        struct State { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
        std::vector<State> state (chain.size());

        for (int n = warmFrom; n < to; ++n)
        {
            const auto v = (double) x[n];
            auto y = v;

            for (size_t k = 0; k < chain.size(); ++k)
            {
                const auto& c = chain[k];
                auto& st = state[k];
                const auto out = c.b0 * y + c.b1 * st.x1 + c.b2 * st.x2 - c.a1 * st.y1 - c.a2 * st.y2;
                st.x2 = st.x1; st.x1 = y; st.y2 = st.y1; st.y1 = out;
                y = out;
            }

            if (n < from)
                continue;   // only warming the filter up on the audio before

            // Full effect inside the sibilant, smooth fades at its edges.
            double w = 1.0;

            if (n < a)        w = (double) (n - from) / std::max (1, a - from);
            else if (n >= b)  w = (double) (to - n) / std::max (1, to - b);

            w = std::pow (std::sin (w * juce::MathConstants<double>::halfPi), 2.0);
            x[n] = (float) (v + w * (y - v));
        }
    }
}

juce::AudioBuffer<float> process (const juce::AudioBuffer<float>& audio, double rate, const std::vector<Sibilant>& items)
{
    juce::AudioBuffer<float> out (audio);

    for (const auto& item : items)
        processOne (out, rate, item);

    return out;
}

std::vector<float> spectrum (const juce::AudioBuffer<float>& audio, double rate, double start, double end)
{
    constexpr int order = 11, size = 1 << order;
    const auto a = juce::jlimit (0, audio.getNumSamples(), (int) std::lround (start * rate));
    const auto b = juce::jlimit (a, audio.getNumSamples(), (int) std::lround (end * rate));

    if (b - a < 64)
        return {};

    juce::dsp::FFT fft (order);
    const auto window = hann (size);
    std::vector<float> work (2 * size);
    std::vector<double> sum (size / 2 + 1, 0.0);
    int count = 0;
    const auto hop = size / 4;

    // Short sibilants get one zero-padded frame; longer ones several.
    for (auto pos = a; pos == a || pos + size <= b; pos += hop)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        const auto n = std::min (size, b - pos);

        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const auto* in = audio.getReadPointer (ch, pos);

            for (int i = 0; i < n; ++i)
                work[(size_t) i] += in[i] / (float) audio.getNumChannels()
                                  * (n == size ? window[(size_t) i]
                                               : 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) std::max (1, n - 1)));
        }

        fft.performFrequencyOnlyForwardTransform (work.data(), true);

        for (size_t k = 0; k < sum.size(); ++k)
            sum[k] += (double) work[k] * work[k];

        ++count;

        if (n < size)
            break;
    }

    std::vector<float> db (sum.size());

    for (size_t k = 0; k < sum.size(); ++k)
        db[k] = (float) (10.0 * std::log10 (sum[k] / std::max (1, count) + 1.0e-20));

    return db;
}

double bellResponseDb (double hz, const Sibilant& item, double rate)
{
    double total = 0.0;

    for (const auto& b : item.bands)
        total += bandResponseDb (hz, b, rate);

    return total;
}

double bandResponseDb (double hz, const Band& band, double rate)
{
    const auto c = peaking (band.frequency, band.gainDb, band.q, rate);
    const auto w = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, rate * 0.4999, hz) / rate;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    const auto h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    return 20.0 * std::log10 (std::abs (h) + 1.0e-20);
}
} // namespace wf::sibilance
