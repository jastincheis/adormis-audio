#include "NoteDetector.h"
#include "Analysis.h"
#include <juce_dsp/juce_dsp.h>

namespace wf
{
namespace
{
struct DetectorConfig
{
    double rate, highPass, lowPass;
    int lowPassStages;
    double minHz, maxHz;
};

// The bass band needs a steep low-pass, otherwise the chords above leak in and
// the tracker locks onto their combined (sub-harmonic) period.
DetectorConfig configFor (NoteSource source)
{
    return source == NoteSource::bass ? DetectorConfig { 8000.0, 26.0, 250.0, 7, 28.0, 300.0 }
                                      : DetectorConfig { 16000.0, 52.0, 1300.0, 2, 55.0, 1100.0 };
}

// Web Audio style biquad (Q in dB for low/high pass, default 1 dB).
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    static Biquad make (bool highPass, double rate, double hz, double qLinear)
    {
        const auto w = juce::MathConstants<double>::twoPi * hz / rate;
        const auto alpha = std::sin (w) / (2.0 * qLinear), cw = std::cos (w), a0 = 1.0 + alpha;
        Biquad f;

        if (highPass)
        {
            f.b0 = (1.0 + cw) / 2.0 / a0;
            f.b1 = -(1.0 + cw) / a0;
        }
        else
        {
            f.b0 = (1.0 - cw) / 2.0 / a0;
            f.b1 = (1.0 - cw) / a0;
        }

        f.b2 = f.b0;
        f.a1 = -2.0 * cw / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    float process (float v)
    {
        const auto y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = v; y2 = y1; y1 = y;
        return (float) y;
    }
};

const double webAudioQ = std::pow (10.0, 1.0 / 20.0);

// Mono, band-limited and resampled to the detector's rate.
std::vector<float> prepare (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate, const DetectorConfig& cfg)
{
    const auto channels = audio.getNumChannels();
    std::vector<float> mono ((size_t) range.getLength(), 0.0f);

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* in = audio.getReadPointer (ch, range.getStart());

        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] += in[i] / (float) channels;
    }

    std::vector<Biquad> filters { Biquad::make (true, rate, cfg.highPass, webAudioQ) };

    for (int i = 0; i < cfg.lowPassStages; ++i)
        filters.push_back (Biquad::make (false, rate, cfg.lowPass, webAudioQ));

    for (auto& v : mono)
        for (auto& f : filters)
            v = f.process (v);

    const auto ratio = rate / cfg.rate;
    std::vector<float> out ((size_t) std::ceil ((double) mono.size() / ratio));
    juce::LagrangeInterpolator interpolator;
    mono.resize (mono.size() + 16, 0.0f);   // the interpolator reads a little ahead
    interpolator.process (ratio, mono.data(), out.data(), (int) out.size());
    return out;
}

// Fractional MIDI note against a reference A (440 Hz, or the recording's own tuning).
double midiOf (double hz, double refHz)   { return 69.0 + 12.0 * std::log2 (hz / refHz); }
double hzOf (double midi, double refHz)   { return refHz * std::pow (2.0, (midi - 69.0) / 12.0); }

// YIN, a frame every 20 ms (or longer for long files: at most ~6000 frames).
std::optional<NoteTrack> track (const std::vector<float>& x, double sr, double fmin, double fmax, double refHz,
                                const std::function<bool (float)>& progress)
{
    const auto window = (int) std::lround (0.04 * sr);
    const auto tauMin = std::max (2, (int) std::floor (sr / fmax));
    const auto tauMax = (int) std::ceil (sr / fmin);
    const auto duration = (double) x.size() / sr;
    const auto hop = std::max ((int) std::lround (0.02 * sr), (int) std::lround (duration / 6000.0 * sr));

    std::vector<float> rms;
    float maxRms = 0.0f;

    for (size_t s = 0; s + (size_t) window + (size_t) tauMax < x.size(); s += (size_t) hop)
    {
        double e = 0.0;

        for (int j = 0; j < window; ++j)
            e += (double) x[s + (size_t) j] * x[s + (size_t) j];

        rms.push_back ((float) std::sqrt (e / window));
        maxRms = std::max (maxRms, rms.back());
    }

    NoteTrack result;
    result.hopSeconds = hop / sr;
    std::vector<double> diff ((size_t) tauMax + 2), norm ((size_t) tauMax + 2);

    for (size_t f = 0; f < rms.size(); ++f)
    {
        const auto s = f * (size_t) hop;
        PitchFrame frame;
        frame.time = ((double) s + window / 2.0) / sr;

        if (f % 200 == 0 && progress != nullptr && ! progress ((float) f / (float) rms.size()))
            return {};

        if (rms[f] >= maxRms * 0.03f)
        {
            double cumulative = 0.0;
            norm[0] = 1.0;

            for (int tau = 1; tau <= tauMax; ++tau)
            {
                double sum = 0.0;
                const auto* a = x.data() + s;
                const auto* b = a + tau;

                for (int j = 0; j < window; ++j)
                {
                    const auto v = (double) a[j] - b[j];
                    sum += v * v;
                }

                diff[(size_t) tau] = sum;
                cumulative += sum;
                norm[(size_t) tau] = cumulative > 0.0 ? sum * tau / cumulative : 1.0;
            }

            int tau = -1;

            for (int t = tauMin; t <= tauMax; ++t)
            {
                if (norm[(size_t) t] < 0.15)
                {
                    while (t + 1 <= tauMax && norm[(size_t) t + 1] < norm[(size_t) t])
                        ++t;

                    tau = t;
                    break;
                }
            }

            if (tau < 0)
            {
                double lowest = 1.0e9;

                for (int t = tauMin; t <= tauMax; ++t)
                    if (norm[(size_t) t] < lowest)
                    {
                        lowest = norm[(size_t) t];
                        tau = t;
                    }
            }

            if (tau > tauMin && tau < tauMax)
            {
                const auto a = norm[(size_t) tau - 1], b = norm[(size_t) tau], c = norm[(size_t) tau + 1];
                const auto den = a - 2.0 * b + c;
                const auto p = std::abs (den) > 1.0e-12 ? 0.5 * (a - c) / den : 0.0;
                const auto hz = sr / (tau + p);
                frame.clarity = (float) (1.0 - b);

                if (1.0 - b > 0.75 && hz >= fmin && hz <= fmax)
                    frame.midi = midiOf (hz, refHz);
            }
        }

        result.frames.push_back (frame);
    }

    return result;
}

// Frames with (nearly) the same pitch become notes of at least 70 ms; each
// note's pitch is the median of its clearest frames.
std::vector<DetectedNote> segment (const std::vector<PitchFrame>& frames, double hop, double refHz)
{
    struct Current { int note; std::vector<double> midis; std::vector<float> clarity; size_t first, last; };
    std::vector<DetectedNote> notes;
    std::optional<Current> current;
    int gap = 0;

    auto close = [&]
    {
        if (! current.has_value())
            return;

        const auto n = current->midis.size();

        if ((double) n * hop >= 0.07)
        {
            // Skip the attack (first two frames) of longer notes, then keep the clearer half.
            const size_t from = n <= 5 ? 0 : 2;
            std::vector<float> clarities (current->clarity.begin() + (long) from, current->clarity.end());
            std::sort (clarities.begin(), clarities.end());
            const auto cut = clarities[clarities.size() / 2];
            std::vector<double> core;

            for (auto i = from; i < n; ++i)
                if (current->clarity[i] >= cut)
                    core.push_back (current->midis[i]);

            std::sort (core.begin(), core.end());
            const auto median = core[core.size() / 2];
            const auto note = (int) std::lround (median);

            DetectedNote d;
            d.start = frames[current->first].time - hop / 2.0;
            d.end = frames[current->last].time + hop / 2.0;
            d.note = note;
            d.cents = (int) std::lround ((median - note) * 100.0);
            d.hz = hzOf (median, refHz);
            notes.push_back (d);
        }

        current.reset();
    };

    for (size_t i = 0; i < frames.size(); ++i)
    {
        const auto& f = frames[i];

        if (! f.midi.has_value())
        {
            if (current.has_value() && ++gap > 1)
                close();

            continue;
        }

        if (current.has_value() && std::abs (*f.midi - current->note) < 0.6)
        {
            current->midis.push_back (*f.midi);
            current->clarity.push_back (f.clarity);
            current->last = i;
            gap = 0;
            continue;
        }

        close();
        current = Current { (int) std::lround (*f.midi), { *f.midi }, { f.clarity }, i, i };
        gap = 0;
    }

    close();
    return notes;
}

// Refines each note from a long, zero-padded spectrum of its steady part: the
// fundamental's peak is measured on its own, so a kick or chord at the same
// time no longer drags the cents value.
void refine (std::vector<DetectedNote>& notes, const std::vector<float>& sig, double sr, double refHz)
{
    const auto order = sr >= 16000.0 ? 14 : 13;
    const auto size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) size * 2);

    for (auto& note : notes)
    {
        const auto a = (int) std::lround ((note.start + 0.05) * sr);
        const auto b = std::min ({ (int) sig.size(), (int) std::lround ((note.end - 0.02) * sr), a + (int) std::lround (0.5 * sr) });
        const auto length = b - a;

        if (a < 0 || length < 0.08 * sr)
            continue;

        std::fill (work.begin(), work.end(), 0.0f);

        for (int i = 0; i < length; ++i)
            work[(size_t) i] = sig[(size_t) (a + i)]
                             * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (length - 1)));

        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        const auto* mag = work.data();

        auto peakNear = [&] (double hz)
        {
            const auto lo = std::max (2, (int) std::floor (hz * std::pow (2.0, -70.0 / 1200.0) * size / sr));
            const auto hi = std::min (size / 2 - 2, (int) std::ceil (hz * std::pow (2.0, 70.0 / 1200.0) * size / sr));
            int k = -1;
            float best = 0.0f;

            for (int i = lo; i <= hi; ++i)
                if (mag[i] > best && mag[i] >= mag[i - 1] && mag[i] >= mag[i + 1])
                {
                    best = mag[i];
                    k = i;
                }

            return std::pair<int, float> { k, best };
        };

        auto f0 = hzOf (note.note + note.cents / 100.0, refHz);
        auto [k, value] = peakNear (f0);

        // The tracker sometimes reports the octave below; if the octave above clearly dominates, use it.
        if (const auto [k2, value2] = peakNear (2.0 * f0); k2 > 0 && value2 > 2.5f * value)
        {
            k = k2;
            value = value2;
            note.note += 12;
            f0 *= 2.0;
        }

        if (k < 0)
            continue;

        float overall = 0.0f;

        for (int i = 2; i < size / 2; ++i)
            overall = std::max (overall, mag[i]);

        if (value < overall * 0.08f)
            continue;

        const auto A = std::log (mag[k - 1] + 1e-12), B = std::log (mag[k] + 1e-12), C = std::log (mag[k + 1] + 1e-12);
        const auto den = A - 2.0 * B + C;
        const auto d = std::abs (den) > 1.0e-12 ? 0.5 * (A - C) / den : 0.0;
        const auto hz = (k + d) * sr / size;
        const auto m = midiOf (hz, refHz);
        const auto rounded = (int) std::lround (m);

        if (std::abs ((m - (note.note + note.cents / 100.0)) * 100.0) <= 60.0)
        {
            note.note = rounded;
            note.cents = (int) std::lround ((m - rounded) * 100.0);
            note.hz = hz;
        }
    }
}

// Zero-phase low band (two Butterworth passes forward and back), so taking it
// out and putting a shifted copy back leaves the rest of the mix untouched.
std::vector<float> lowBand (const float* x, int n, double sr, double hz)
{
    std::vector<float> y (x, x + n);

    for (int pass = 0; pass < 4; ++pass)
    {
        auto f = Biquad::make (false, sr, hz, std::sqrt (0.5));

        if (pass < 2)
            for (int i = 0; i < n; ++i)
                y[(size_t) i] = f.process (y[(size_t) i]);
        else
            for (int i = n - 1; i >= 0; --i)
                y[(size_t) i] = f.process (y[(size_t) i]);
    }

    return y;
}

float readHermite (const std::vector<float>& x, double position)
{
    const auto i = (int) std::floor (position);
    const auto t = (float) (position - i);
    const auto n = (int) x.size();
    auto at = [&] (int k) { return x[(size_t) juce::jlimit (0, n - 1, k)]; };
    const auto p0 = at (i - 1), p1 = at (i), p2 = at (i + 1), p3 = at (i + 2);
    return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
}

// Two-tap delay-line shifter. The taps sit a whole number of the note's
// periods apart, so the crossfade between them is phase-coherent and the
// note starts sample-aligned with the original. Timing never drifts by more
// than about one period and the length never changes.
void shiftRegion (const std::vector<float>& src, std::vector<float>& dst, int a, int b, int cents, double hz,
                  double sr, int fade)
{
    const auto ratio = std::pow (2.0, cents / 1200.0);
    const auto f = hz > 0.0 ? hz : 100.0;
    const auto period = sr / std::max (20.0, f);
    const auto H = period * std::max (1.0, std::ceil (0.008 * f));
    const auto W = 2.0 * H;
    const auto r0 = std::max (0, a - fade), r1 = std::min ((int) src.size(), b + fade);
    auto d = H;

    for (int n = r0; n < r1; ++n)
    {
        const auto d2 = d + H >= W ? d + H - W : d + H;
        const auto w1 = std::pow (std::sin (juce::MathConstants<double>::pi * d / W), 2.0);
        const auto y = w1 * readHermite (src, n + H - d) + (1.0 - w1) * readHermite (src, n + H - d2);

        double edge = 1.0;

        if (n < a)        edge = (double) (n - r0) / std::max (1, a - r0);
        else if (n >= b)  edge = (double) (r1 - n) / std::max (1, r1 - b);

        const auto g = std::pow (std::sin (edge * juce::MathConstants<double>::halfPi), 2.0);
        dst[(size_t) n] = (float) (dst[(size_t) n] * (1.0 - g) + y * g);

        d += 1.0 - ratio;

        if (d >= W)       d -= W;
        else if (d < 0.0) d += W;
    }
}
} // namespace

std::pair<int, int> DetectedNote::corrected() const
{
    const auto m = note + (cents + shift) / 100.0;
    const auto n = (int) std::lround (m);
    return { n, (int) std::lround ((m - n) * 100.0) };
}

std::optional<NoteTrack> detectNotes (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate,
                                      NoteSource source, const std::function<bool (float)>& progress, int referenceCents)
{
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });

    if (range.getLength() < (int) (0.2 * rate))
        return NoteTrack {};

    const auto cfg = configFor (source);
    const auto sig = prepare (audio, range, rate, cfg);
    const auto refHz = 440.0 * std::pow (2.0, referenceCents / 1200.0);
    auto result = track (sig, cfg.rate, cfg.minHz, cfg.maxHz, refHz, progress);

    if (! result.has_value())
        return {};

    result->notes = segment (result->frames, result->hopSeconds, refHz);
    refine (result->notes, sig, cfg.rate, refHz);
    return result;
}

juce::AudioBuffer<float> renderCorrection (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sr,
                                           const std::vector<DetectedNote>& notes, NoteSource source)
{
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    const auto length = range.getLength();
    juce::AudioBuffer<float> out (audio.getNumChannels(), length);
    const auto fade = (int) std::lround (0.012 * sr);
    const auto band = source == NoteSource::bass;

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* x = audio.getReadPointer (ch, range.getStart());
        const auto src = band ? lowBand (x, length, sr, 450.0) : std::vector<float> (x, x + length);
        auto dst = src;

        for (const auto& n : notes)
            if (n.shift != 0)
                shiftRegion (src, dst, (int) std::lround (n.start * sr), (int) std::lround (n.end * sr), n.shift, n.hz, sr, fade);

        auto* o = out.getWritePointer (ch);

        for (int i = 0; i < length; ++i)
            o[i] = band ? x[i] - src[(size_t) i] + dst[(size_t) i] : dst[(size_t) i];
    }

    return out;
}

TuneStatus tuneStatus (int cents, int tolerance)
{
    const auto c = std::abs (cents);
    return c <= tolerance ? TuneStatus::ok : c <= 25 ? TuneStatus::warn : TuneStatus::bad;
}

bool isInKey (int midiNote, int keyTonic, bool minor)
{
    const auto& steps = scaleSteps (minor);
    const auto degree = ((midiNote - keyTonic) % 12 + 12) % 12;
    return std::find (steps.begin(), steps.end(), degree) != steps.end();
}

int autoCorrect (std::vector<DetectedNote>& notes, const AutoCorrect& settings, int tolerance, int keyTonic, bool minor)
{
    const auto k = settings.strength / 100.0;
    int changed = 0;

    for (auto& n : notes)
    {
        const auto m = n.note + n.cents / 100.0;
        std::optional<int> target;

        if (settings.snapToKey && ! isInKey (n.note, keyTonic, minor))
        {
            std::optional<int> best;

            for (auto q = (int) std::floor (m) - 2; q <= (int) std::ceil (m) + 2; ++q)
                if (isInKey (q, keyTonic, minor) && (! best.has_value() || std::abs (q - m) < std::abs (*best - m)))
                    best = q;

            if (best.has_value())
                target = (int) std::lround ((*best - m) * 100.0);
        }
        else if (settings.allNotes || tuneStatus (n.cents, tolerance) != TuneStatus::ok)
        {
            target = (int) std::lround (-n.cents * k);
        }

        if (target.has_value() && *target != n.shift)
        {
            n.shift = *target;
            ++changed;
        }
    }

    return changed;
}

juce::String midiNoteName (int midiNote, bool flats)
{
    return noteName (midiNote, flats) + juce::String ((int) std::floor (midiNote / 12.0) - 1);
}

juce::String formatCents (int cents)
{
    const auto sign = cents > 0 ? juce::String ("+") : cents < 0 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"))
                                                                 : juce::String (juce::CharPointer_UTF8 ("\xc2\xb1"));
    return sign + juce::String (std::abs (cents)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xa2"));
}
} // namespace wf
