#include "Analysis.h"
#include <juce_dsp/juce_dsp.h>

namespace wf
{
namespace
{
std::vector<float> mixToMono (const juce::AudioBuffer<float>& audio, juce::Range<int> range)
{
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    std::vector<float> mono ((size_t) range.getLength(), 0.0f);
    const auto channels = audio.getNumChannels();

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* in = audio.getReadPointer (ch, range.getStart());

        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] += in[i] / (float) channels;
    }

    return mono;
}

std::vector<float> hann (int n)
{
    std::vector<float> w ((size_t) n);

    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (n - 1));

    return w;
}

// Magnitudes of the windowed frame starting at `start` (zero past the end);
// `work` holds 2 * size floats, the first size / 2 are the result.
void magnitudes (juce::dsp::FFT& fft, const std::vector<float>& window, const std::vector<float>& mono,
                 size_t start, std::vector<float>& work)
{
    const auto size = window.size();
    std::fill (work.begin(), work.end(), 0.0f);

    for (size_t i = 0; i < size && start + i < mono.size(); ++i)
        work[i] = mono[start + i] * window[i];

    fft.performFrequencyOnlyForwardTransform (work.data(), true);
}

// Whole bars of 4/4 that fit the clip (after any short silence at the start),
// when that agrees with the beat tracker: the tempo of a cut loop is exact.
std::optional<TempoEstimate> loopTempo (const std::vector<float>& mono, double rate, double estimate)
{
    float peak = 0.0f;

    for (size_t i = 0; i < mono.size(); i += 2)
        peak = std::max (peak, std::abs (mono[i]));

    if (peak <= 0.0f)
        return {};

    const auto threshold = peak * 0.003f;
    const auto maxLead = (size_t) std::lround (0.08 * rate);
    size_t lead = 0;

    while (lead < maxLead && lead < mono.size() && std::abs (mono[lead]) < threshold)
        ++lead;

    const auto leadSeconds = lead >= (size_t) std::lround (0.004 * rate) && lead < maxLead ? (double) lead / rate : 0.0;
    const auto duration = (double) mono.size() / rate - leadSeconds;

    std::optional<TempoEstimate> best;
    double bestError = 0.0;

    for (int bars : { 1, 2, 4, 8, 16, 32, 64 })
    {
        const auto bpm = bars * 240.0 / duration;

        if (bpm < 55.0 || bpm > 200.0)
            continue;

        for (int i = 0; i < 3; ++i)
        {
            const auto factor = i == 0 ? 1.0 : i == 1 ? 0.5 : 2.0;
            const auto penalty = i == 0 ? 0.0 : 0.02;   // half / double time is a little less likely
            const auto error = std::abs (std::log2 (bpm / (estimate * factor))) + penalty;

            // Over the whole clip the two tempos may drift apart by at most a
            // quarter of a beat (of the faster one): a clip a little longer than whole bars (a tail
            // after the loop) is not mistaken for a slower loop.
            const auto drift = std::abs (bpm - estimate * factor) / 60.0 * duration / std::min (1.0, factor);

            if (error < 0.035 + penalty && drift < 0.25 && (! best.has_value() || error < bestError))
            {
                best = TempoEstimate { bpm, bars, leadSeconds };
                bestError = error;
            }
        }
    }

    if (best.has_value())
    {
        best->bpm = std::round (best->bpm * 100.0) / 100.0;

        if (std::abs (best->bpm - std::round (best->bpm)) < 0.06)
            best->bpm = std::round (best->bpm);
    }

    return best;
}

constexpr float majorProfile[12] { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
constexpr float minorProfile[12] { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

double correlation (const std::array<double, 12>& x, const std::array<double, 12>& y)
{
    double mx = 0.0, my = 0.0;

    for (int i = 0; i < 12; ++i)
    {
        mx += x[(size_t) i] / 12.0;
        my += y[(size_t) i] / 12.0;
    }

    double sxy = 0.0, sx = 0.0, sy = 0.0;

    for (int i = 0; i < 12; ++i)
    {
        const auto a = x[(size_t) i] - mx, b = y[(size_t) i] - my;
        sxy += a * b;
        sx += a * a;
        sy += b * b;
    }

    return sx > 0.0 && sy > 0.0 ? sxy / std::sqrt (sx * sy) : 0.0;
}
} // namespace

std::optional<TempoEstimate> detectTempo (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate)
{
    const auto mono = mixToMono (audio, range);
    constexpr int order = 10, size = 1 << order, hop = size / 2;

    if (mono.size() < (size_t) size)
        return {};

    const auto frames = (int) ((mono.size() - size) / hop) + 1;

    if (frames < 40)
        return {};

    // Spectral flux on a log scale, plus the part of it below 150 Hz (kick).
    juce::dsp::FFT fft (order);
    const auto window = hann (size);
    std::vector<float> work (2 * size), previous (size / 2, 0.0f), flux ((size_t) frames, 0.0f), low ((size_t) frames, 0.0f);
    const auto lowBin = (int) std::ceil (150.0 * size / rate);

    for (int f = 0; f < frames; ++f)
    {
        magnitudes (fft, window, mono, (size_t) f * hop, work);
        float sum = 0.0f, lowSum = 0.0f;

        for (int k = 1; k < size / 2; ++k)
        {
            const auto m = std::log (1.0f + 100.0f * work[(size_t) k]);
            const auto d = m - previous[(size_t) k];

            if (d > 0.0f)
            {
                sum += d;

                if (k <= lowBin)
                    lowSum += d;
            }

            previous[(size_t) k] = m;
        }

        if (f > 0)
        {
            flux[(size_t) f] = sum;
            low[(size_t) f] = lowSum;
        }
    }

    // Onset strength: flux above its local (0.4 s) average.
    const auto hopSeconds = (double) hop / rate;
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

    // Autocorrelation scored at the beat, 2 and 4 beats, with a gentle preference around 120 BPM.
    const auto maxLag = std::min (frames - 2, (int) std::ceil (4.0 * 60.0 / 70.0 / hopSeconds) + 2);
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
        if (x >= maxLag)
            return 0.0;

        const auto i = (int) x;
        const auto frac = x - i;
        return ac[(size_t) i] * (1.0 - frac) + ac[(size_t) i + 1] * frac;
    };

    double best = 120.0, bestScore = -1.0;

    for (double bpm = 70.0; bpm <= 180.0; bpm += 0.05)
    {
        const auto lag = 60.0 / bpm / hopSeconds;
        const auto prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 0.9, 2.0));
        const auto score = (acAt (lag) + 0.5 * acAt (2.0 * lag) + 0.25 * acAt (4.0 * lag)) * prior;

        if (score > bestScore)
        {
            bestScore = score;
            best = bpm;
        }
    }

    if (bestScore <= 0.0)
        return {};

    // Refine: the onset peaks that fall on the beat grid (within an eighth of
    // a beat), fitted with a straight line of beat number against time. The
    // grid search above is only as fine as its frames; the fit is not.
    {
        std::vector<std::pair<double, float>> peaks;   // frame position (interpolated), strength
        const auto strongest = *std::max_element (onset.begin(), onset.end());

        for (int i = 1; i + 1 < frames; ++i)
        {
            const auto a = onset[(size_t) i - 1], b = onset[(size_t) i], c = onset[(size_t) i + 1];

            if (b >= 0.3f * strongest && b > a && b >= c)
            {
                const auto den = a - 2.0f * b + c;
                peaks.push_back ({ i + (std::abs (den) > 1.0e-9f ? 0.5 * (a - c) / den : 0.0), b });
            }
        }

        if (peaks.size() >= 8)
        {
            auto period = 60.0 / best / hopSeconds;
            auto anchor = std::max_element (peaks.begin(), peaks.end(), [] (auto& x, auto& y) { return x.second < y.second; })->first;

            for (int pass = 0; pass < 3; ++pass)
            {
                double sw = 0, sn = 0, st = 0, snn = 0, snt = 0;
                int used = 0;

                for (auto& [t, strength] : peaks)
                {
                    const auto n = std::round ((t - anchor) / period);

                    if (std::abs (t - anchor - n * period) > period / 8.0)
                        continue;

                    sw += strength; sn += strength * n; st += strength * t; snn += strength * n * n; snt += strength * n * t;
                    ++used;
                }

                const auto den = sw * snn - sn * sn;

                if (used < 8 || den <= 0.0)
                    break;

                const auto slope = (sw * snt - sn * st) / den;

                if (std::abs (slope / period - 1.0) > 0.01)
                    break;

                period = slope;
                anchor = (st - slope * sn) / sw;
            }

            best = 60.0 / period / hopSeconds;
        }
    }

    // whole numbers and halves are the usual tempos; otherwise one decimal
    const auto bpm = std::abs (best - std::round (best)) < 0.2              ? std::round (best)
                   : std::abs (best * 2.0 - std::round (best * 2.0)) < 0.2 ? std::round (best * 2.0) / 2.0
                                                                            : std::round (best * 10.0) / 10.0;

    if (auto loop = loopTempo (mono, rate, best))
        return loop;

    // Beat phase: where the onsets line up best with the beat grid; then which
    // of the 4 beats is the bar's first (strongest low end + onsets).
    const auto period = 60.0 / bpm / hopSeconds;
    int phase = 0;
    double phaseScore = -1.0;

    for (int p = 0; p < (int) period; ++p)
    {
        double sum = 0.0;

        for (double x = p; x < frames; x += period)
            if (const auto i = (int) std::lround (x); i < frames)
                sum += onset[(size_t) i];

        if (sum > phaseScore)
        {
            phaseScore = sum;
            phase = p;
        }
    }

    int barBeat = 0;
    double barScore = -1.0;

    for (int j = 0; j < 4; ++j)
    {
        double sum = 0.0;

        for (double x = phase + j * period; x < frames; x += 4.0 * period)
            if (const auto i = (int) std::lround (x); i < frames)
                sum += low[(size_t) i] + onset[(size_t) i];

        if (sum > barScore)
        {
            barScore = sum;
            barBeat = j;
        }
    }

    const auto bar = 240.0 / bpm;
    auto downbeat = std::fmod ((phase + barBeat * period) * hopSeconds + size / 2.0 / rate, bar);

    if (downbeat > bar - 0.03)
        downbeat -= bar;

    return TempoEstimate { bpm, 0, std::max (0.0, downbeat) };
}

std::optional<KeyAnalysis> analyseKey (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate)
{
    const auto mono = mixToMono (audio, range);
    constexpr int order = 14, size = 1 << order;

    if (mono.size() < (size_t) size / 2)
        return {};

    const auto length = mono.size();
    const auto hop = std::max ((size_t) size / 2, length / 1200);

    // Loudness per frame, to skip the near-silent ones.
    std::vector<float> rms;
    float maxRms = 0.0f;

    for (size_t s = 0; s < std::max ((size_t) 1, length - std::min (length, (size_t) size / 2)); s += hop)
    {
        double e = 0.0;

        for (size_t i = s; i < s + size && i < length; i += 4)
            e += (double) mono[i] * mono[i];

        const auto r = (float) std::sqrt (e / (size / 4.0));
        rms.push_back (r);
        maxRms = std::max (maxRms, r);
    }

    if (maxRms <= 0.0f)
        return {};

    // Spectral peaks folded into 120 bins per octave (10 per semitone), in
    // total and per frame.
    juce::dsp::FFT fft (order);
    const auto window = hann (size);
    std::vector<float> work (2 * size);
    std::array<double, 120> chroma {}, bass {};
    std::vector<std::array<float, 120>> frameChroma (rms.size()), frameBass (rms.size());
    const auto kMin = std::max (2, (int) std::ceil (28.0 * size / rate));
    const auto kMax = std::min (size / 2 - 2, (int) std::floor (2500.0 * size / rate));

    for (size_t f = 0; f < rms.size(); ++f)
    {
        frameChroma[f].fill (0.0f);
        frameBass[f].fill (0.0f);

        if (rms[f] < maxRms * 0.04f)
            continue;

        magnitudes (fft, window, mono, f * hop, work);
        float peak = 0.0f;

        for (int k = kMin - 1; k <= kMax + 1; ++k)
            peak = std::max (peak, work[(size_t) k]);

        if (peak <= 0.0f)
            continue;

        const auto threshold = peak * 0.04f;

        for (int k = kMin; k <= kMax; ++k)
        {
            const auto b = work[(size_t) k];

            if (b < threshold || b <= work[(size_t) k - 1] || b < work[(size_t) k + 1])
                continue;

            const auto a = std::log (work[(size_t) k - 1] + 1e-12), bb = std::log (b + 1e-12),
                       c = std::log (work[(size_t) k + 1] + 1e-12);
            const auto den = a - 2.0 * bb + c;
            const auto p = std::abs (den) > 1.0e-12 ? 0.5 * (a - c) / den : 0.0;
            const auto hz = (k + p) * rate / size;

            if (hz < 28.0)
                continue;

            const auto midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
            const auto index = (size_t) ((((int) std::lround (midi * 10.0)) % 120 + 120) % 120);
            const auto weight = (double) b / peak;

            if (hz < 200.0)
            {
                bass[index] += weight;
                frameBass[f][index] += (float) weight;
            }

            if (hz >= 60.0)
            {
                const auto w2 = weight * (hz < 200.0 ? 0.6 : 1.0);
                chroma[index] += w2;
                frameChroma[f][index] += (float) w2;
            }
        }
    }

    // Tuning: the average offset of all peaks within a semitone.
    double cx = 0.0, cy = 0.0;

    for (size_t i = 0; i < 120; ++i)
    {
        const auto angle = juce::MathConstants<double>::twoPi * (double) (i % 10) / 10.0;
        cx += (chroma[i] + bass[i]) * std::cos (angle);
        cy += (chroma[i] + bass[i]) * std::sin (angle);
    }

    auto deviation = std::atan2 (cy, cx) / juce::MathConstants<double>::twoPi * 10.0;

    if (deviation >= 5.0)  deviation -= 10.0;
    if (deviation < -5.0)  deviation += 10.0;

    std::array<size_t, 120> toPitchClass {};

    for (int i = 0; i < 120; ++i)
        toPitchClass[(size_t) i] = (size_t) ((((int) std::lround ((i - deviation) / 10.0)) % 12 + 12) % 12);

    KeyAnalysis result;
    result.tuningCents = (int) std::lround (deviation * 10.0);

    for (size_t i = 0; i < 120; ++i)
    {
        result.chroma[toPitchClass[i]] += chroma[i];
        result.bass[toPitchClass[i]] += bass[i];
    }

    result.frames.resize (rms.size());

    for (size_t f = 0; f < rms.size(); ++f)
    {
        auto& frame = result.frames[f];
        frame.time = (double) (f * hop + size / 2) / rate;

        for (size_t i = 0; i < 120; ++i)
        {
            frame.chroma[toPitchClass[i]] += frameChroma[f][i];
            frame.bass[toPitchClass[i]] += frameBass[f][i];
        }
    }

    for (int tonic = 0; tonic < 12; ++tonic)
    {
        for (bool minor : { false, true })
        {
            const auto* profile = minor ? minorProfile : majorProfile;
            std::array<double, 12> rotated {};

            for (int i = 0; i < 12; ++i)
                rotated[(size_t) i] = profile[(i - tonic + 12) % 12];

            result.keys.push_back ({ tonic, minor, correlation (result.chroma, rotated) + 0.35 * correlation (result.bass, rotated) });
        }
    }

    std::stable_sort (result.keys.begin(), result.keys.end(), [] (auto& a, auto& b) { return a.score > b.score; });

    // Major and minor on the same tonic score alike when a minor key borrows
    // the major V (the raised 7th of harmonic minor): then the third over the
    // tonic that is actually heard decides.
    const auto best = result.keys[0];
    const auto parallel = std::find_if (result.keys.begin(), result.keys.end(),
                                        [&] (auto& k) { return k.tonic == best.tonic && k.minor != best.minor; });

    if (parallel != result.keys.end() && best.score - parallel->score < 0.3)
    {
        const auto at = [&] (int pc) { return result.chroma[(size_t) (pc % 12)] + result.bass[(size_t) (pc % 12)]; };
        const auto minorThird = at (best.tonic + 3), majorThird = at (best.tonic + 4);

        // only a clearly stronger third overrides the profiles
        if (best.minor ? majorThird > 1.3 * minorThird : minorThird > 1.3 * majorThird)
            std::rotate (result.keys.begin(), parallel, parallel + 1);
    }

    // A major key and its relative minor (C major / A minor) use the same notes,
    // so the profiles barely tell them apart: then the chord the music starts
    // on decides (its root and fifth over the first second, bass counted twice),
    // when that chord is clearly the relative key's tonic.
    const auto top = result.keys[0];
    const auto relativeTonic = (top.tonic + (top.minor ? 3 : 9)) % 12;
    const auto relative = std::find_if (result.keys.begin(), result.keys.end(),
                                        [&] (auto& k) { return k.tonic == relativeTonic && k.minor != top.minor; });

    if (relative != result.keys.end() && top.score - relative->score < 0.35)
    {
        std::optional<double> start;
        std::array<double, 12> home {};

        for (size_t f = 0; f < result.frames.size(); ++f)
        {
            if (rms[f] < maxRms * 0.1f)
                continue;

            const auto& frame = result.frames[f];

            if (! start.has_value())
                start = frame.time;

            if (frame.time > *start + 1.0)
                break;

            for (size_t pc = 0; pc < 12; ++pc)
                home[pc] += 2.0 * frame.bass[pc] + frame.chroma[pc] + frame.chroma[(pc + 7) % 12];
        }

        const auto strongest = (int) (std::max_element (home.begin(), home.end()) - home.begin());

        if (strongest == relativeTonic && home[(size_t) relativeTonic] > 1.5 * home[(size_t) top.tonic])
            std::rotate (result.keys.begin(), relative, relative + 1);
    }

    return result;
}

std::optional<KeyEstimate> detectKey (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate)
{
    const auto analysis = analyseKey (audio, range, rate);

    if (! analysis.has_value() || analysis->keys[0].score <= 0.0)
        return {};

    const auto& best = analysis->keys[0];
    return KeyEstimate { best.tonic, best.minor, best.score - analysis->keys[1].score > 0.04, analysis->tuningCents };
}

const std::array<int, 7>& scaleSteps (bool minor)
{
    static const std::array<int, 7> major { 0, 2, 4, 5, 7, 9, 11 }, naturalMinor { 0, 2, 3, 5, 7, 8, 10 };
    return minor ? naturalMinor : major;
}

juce::String romanianNoteName (int pitchClass, bool flats)
{
    static const char* sharps[] { "Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" };
    static const char* flatNames[] { "Do", "Re\xe2\x99\xad", "Re", "Mi\xe2\x99\xad", "Mi", "Fa", "Sol\xe2\x99\xad", "Sol", "La\xe2\x99\xad", "La", "Si\xe2\x99\xad", "Si" };
    const auto pc = ((pitchClass % 12) + 12) % 12;
    return juce::CharPointer_UTF8 (flats ? flatNames[pc] : sharps[pc]);
}

double noteFrequency (int pitchClass, int octave, int cents)
{
    return 440.0 * std::pow (2.0, (pitchClass + 12 * (octave + 1) - 69 + cents / 100.0) / 12.0);
}

bool keyUsesFlats (int tonic, bool minor)
{
    static constexpr int majorFlats[] { 5, 10, 3, 8, 1, 6 }, minorFlats[] { 2, 7, 0, 5, 10, 3 };
    const auto* list = minor ? minorFlats : majorFlats;
    return std::find (list, list + 6, tonic) != list + 6;
}

juce::String noteName (int pitchClass, bool flats)
{
    static const char* sharps[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* flatNames[] { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    const auto pc = ((pitchClass % 12) + 12) % 12;
    return flats ? flatNames[pc] : sharps[pc];
}

juce::String keyName (int tonic, bool minor)
{
    return noteName (tonic, keyUsesFlats (tonic, minor)) + (minor ? " minor" : " major");
}
} // namespace wf
