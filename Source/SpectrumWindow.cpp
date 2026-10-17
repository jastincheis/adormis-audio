#include "SpectrumWindow.h"
#include "Analysis.h"
#include "DarkTheme.h"
#include "Settings.h"
#include <juce_dsp/juce_dsp.h>

namespace wf
{
namespace
{
using namespace dark::colours;

constexpr float floorDb = -120.0f;

void styleLabel (juce::Label& l, float size, bool bold = false, juce::Colour colour = ink)
{
    l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    l.setColour (juce::Label::textColourId, colour);
}

void styleToggle (juce::TextButton& b)
{
    b.setClickingTogglesState (true);
    b.setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.3f));
    b.setColour (juce::TextButton::textColourOnId, ink);
}

// "440.0 Hz  A4 +0¢"
juce::String describeFrequency (double hz)
{
    auto text = hz >= 1000.0 ? juce::String (hz / 1000.0, hz >= 10000.0 ? 2 : 3) + " kHz" : juce::String (hz, 1) + " Hz";

    if (hz >= 16.0)
    {
        const auto midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
        const auto note = (int) std::lround (midi);
        const auto cents = (int) std::lround ((midi - note) * 100.0);
        text << "   " << noteName (((note % 12) + 12) % 12, false) << juce::String (note / 12 - 1) << " "
             << (cents >= 0 ? "+" : juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"))) << std::abs (cents)
             << juce::String (juce::CharPointer_UTF8 ("\xc2\xa2"));
    }

    return text;
}

// Power spectrum (one value per bin, mean over Hann windows hopping by half a
// window) of `length` samples from `start`; a full-scale sine reads 0 dB.
std::vector<float> spectrumDb (const juce::AudioBuffer<float>& audio, juce::int64 start, juce::int64 length, int order, int channel,
                               int* windowsUsed = nullptr)
{
    const auto size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) size * 2), window ((size_t) size);
    std::vector<double> power ((size_t) size / 2 + 1, 0.0);

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);

    // at most ~400 windows, spread over the range
    const auto available = std::max ((juce::int64) 1, (length - size) / (size / 2) + 1);
    const auto frames = (int) std::min ((juce::int64) 400, available);
    const auto hop = frames > 1 ? (double) (length - size) / (frames - 1) : 0.0;
    const auto total = (juce::int64) audio.getNumSamples();

    for (int f = 0; f < frames; ++f)
    {
        const auto from = start + (juce::int64) std::llround (f * hop);
        std::fill (work.begin(), work.end(), 0.0f);

        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            if (channel > 0 && ch != std::min (channel - 1, audio.getNumChannels() - 1))
                continue;

            const auto scale = channel > 0 ? 1.0f : 1.0f / (float) audio.getNumChannels();
            const auto* in = audio.getReadPointer (ch);

            for (int i = 0; i < size; ++i)
                if (const auto at = from + i; at >= 0 && at < total)
                    work[(size_t) i] += in[at] * scale * window[(size_t) i];
        }

        fft.performFrequencyOnlyForwardTransform (work.data(), true);

        for (size_t k = 0; k < power.size(); ++k)
            power[k] += (double) work[k] * work[k];
    }

    if (windowsUsed != nullptr)
        *windowsUsed = frames;

    std::vector<float> db (power.size());

    for (size_t k = 0; k < power.size(); ++k)
        db[k] = (float) juce::Decibels::gainToDecibels (std::sqrt (power[k] / frames) * 4.0 / size, (double) floorDb);

    return db;
}

// Fractional-octave smoothing (1 / `fraction` of an octave), on power.
std::vector<float> smooth (const std::vector<float>& db, int fraction)
{
    if (fraction <= 0 || db.size() < 4)
        return db;

    std::vector<double> prefix (db.size() + 1, 0.0);

    for (size_t k = 0; k < db.size(); ++k)
        prefix[k + 1] = prefix[k] + std::pow (10.0, db[k] / 10.0);

    const auto half = std::pow (2.0, 0.5 / fraction);
    std::vector<float> out (db.size());

    for (size_t k = 0; k < db.size(); ++k)
    {
        const auto lo = std::max ((size_t) 1, (size_t) std::floor ((double) k / half));
        const auto hi = std::min (db.size() - 1, std::max (lo, (size_t) std::ceil ((double) k * half)));
        const auto mean = (prefix[hi + 1] - prefix[lo]) / (double) (hi + 1 - lo);
        out[k] = (float) std::max ((double) floorDb, 10.0 * std::log10 (mean + 1.0e-30));
    }

    return out;
}

juce::Colour sonogramColour (float t)   // 0..1: black, blue, purple, orange, yellow, white
{
    static const juce::Colour stops[] { juce::Colour (0xff05060a), juce::Colour (0xff1b2a78), juce::Colour (0xff7a2a9a),
                                        juce::Colour (0xffe0553a), juce::Colour (0xfff7c843), juce::Colour (0xfffffbe6) };
    t = juce::jlimit (0.0f, 1.0f, t) * 5.0f;
    const auto i = std::min (4, (int) t);
    return stops[i].interpolatedWith (stops[i + 1], t - (float) i);
}
} // namespace

//==============================================================================
class SpectrumWindow::Panel final : public juce::Component,
                                    private juce::Timer
{
public:
    Panel (SpectrumSource s, MeterTap& t) : source (std::move (s)), tap (t)
    {
        auto addToggle = [this] (juce::TextButton& b, int group)
        {
            styleToggle (b);
            b.setRadioGroupId (group);
            addAndMakeVisible (b);
        };

        addToggle (spectrumButton, 801);
        addToggle (sonogramButton, 801);
        spectrumButton.onClick = [this] { setSonogram (false); };
        sonogramButton.onClick = [this] { setSonogram (true); };

        channelBox.addItem ("Both channels", 1);
        channelBox.addItem ("Left", 2);
        channelBox.addItem ("Right", 3);
        channelBox.onChange = [this] { channel = channelBox.getSelectedId() - 1; remember(); scheduleMeasure(); };

        for (int o = 11; o <= 15; ++o)
            sizeBox.addItem ("FFT " + juce::String (1 << o), o);

        sizeBox.onChange = [this] { order = sizeBox.getSelectedId(); remember(); scheduleMeasure(); };

        smoothBox.addItem ("No smoothing", 1);
        smoothBox.addItem ("1/12 octave", 12);
        smoothBox.addItem ("1/6 octave", 6);
        smoothBox.addItem ("1/3 octave", 3);
        smoothBox.onChange = [this] { smoothing = smoothBox.getSelectedId() == 1 ? 0 : smoothBox.getSelectedId(); remember(); scheduleMeasure(); };

        for (auto* c : { &channelBox, &sizeBox, &smoothBox })
            addAndMakeVisible (c);

        styleToggle (holdButton);
        holdButton.onClick = [this] { peak.clear(); repaint(); };
        styleToggle (freezeButton);

        for (auto* b : { &holdButton, &freezeButton })
            addAndMakeVisible (b);

        styleLabel (status, 12.0f, false, muted);
        styleLabel (readout, 13.0f, true);
        readout.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (status);
        addAndMakeVisible (readout);

        // last time's choices
        const auto saved = settings::get ("spectrum");
        channel = juce::jlimit (0, 2, (int) saved.getProperty ("channel", 0));
        order = juce::jlimit (11, 15, (int) saved.getProperty ("order", 13));
        smoothing = (int) saved.getProperty ("smoothing", 6);
        holdButton.setToggleState ((bool) saved.getProperty ("hold", true), juce::dontSendNotification);
        sonogram = (bool) saved.getProperty ("sonogram", false);
        channelBox.setSelectedId (channel + 1, juce::dontSendNotification);
        sizeBox.setSelectedId (order, juce::dontSendNotification);
        smoothBox.setSelectedId (smoothing == 0 ? 1 : smoothing, juce::dontSendNotification);
        (sonogram ? sonogramButton : spectrumButton).setToggleState (true, juce::dontSendNotification);

        setSize (900, 520);
        tap.setAnalysisEnabled (true);
        scheduleMeasure();
        startTimerHz (30);
    }

    ~Panel() override
    {
        tap.setAnalysisEnabled (false);
    }

    void sourceChanged()   { scheduleMeasure(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto top = r.removeFromTop (30);
        spectrumButton.setBounds (top.removeFromLeft (100).reduced (2));
        sonogramButton.setBounds (top.removeFromLeft (100).reduced (2));
        top.removeFromLeft (12);
        channelBox.setBounds (top.removeFromLeft (140).reduced (2));
        sizeBox.setBounds (top.removeFromLeft (110).reduced (2));
        smoothBox.setBounds (top.removeFromLeft (130).reduced (2));
        freezeButton.setBounds (top.removeFromRight (80).reduced (2));
        holdButton.setBounds (top.removeFromRight (100).reduced (2));

        auto bottom = r.removeFromBottom (22);
        readout.setBounds (bottom.removeFromRight (bottom.getWidth() / 2));
        status.setBounds (bottom);
        r.removeFromTop (8);
        r.removeFromBottom (4);
        graph = r;
        sonogramImage = {};
        scheduleMeasure();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (bg);
        g.setColour (surface);
        g.fillRoundedRectangle (graph.toFloat(), 8.0f);

        if (sonogram)
            paintSonogram (g);
        else
            paintSpectrum (g);
    }

    void mouseMove (const juce::MouseEvent& e) override   { hover = e.getPosition(); updateReadout(); }
    void mouseExit (const juce::MouseEvent&) override     { hover = {}; updateReadout(); }

private:
    //==========================================================================
    juce::Rectangle<float> plot() const   { return graph.toFloat().reduced (8.0f, 8.0f).withTrimmedLeft (34.0f).withTrimmedBottom (16.0f); }

    double topHz() const   { return std::max (100.0, rate() / 2.0); }
    double rate() const    { return live ? tap.getSampleRate() : source.sampleRate(); }

    float xFor (double hz) const
    {
        const auto a = plot();
        return a.getX() + a.getWidth() * (float) (std::log (std::max (hz, 20.0) / 20.0) / std::log (topHz() / 20.0));
    }

    double hzAt (float x) const
    {
        const auto a = plot();
        return 20.0 * std::pow (topHz() / 20.0, juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth())));
    }

    float yFor (float db) const
    {
        const auto a = plot();
        return a.getY() + a.getHeight() * juce::jlimit (0.0f, 1.0f, -db / -floorDb);
    }

    // The level of `curve` at `hz` (between bins: the nearer one).
    float levelAt (const std::vector<float>& values, double hz) const
    {
        if (values.size() < 2)
            return floorDb;

        const auto bin = hz / (rate() / (double) ((values.size() - 1) * 2));
        return values[(size_t) juce::jlimit (0, (int) values.size() - 1, (int) std::lround (bin))];
    }

    void paintGrid (juce::Graphics& g, bool levels) const
    {
        const auto a = plot();
        g.setFont (juce::FontOptions (10.5f));

        for (double decade = 10.0; decade <= 100000.0; decade *= 10.0)
            for (int m = 1; m <= 9; ++m)
            {
                const auto hz = decade * m;

                if (hz < 20.0 || hz > topHz())
                    continue;

                const auto x = xFor (hz);
                const auto major = m == 1 || m == 2 || m == 5;
                g.setColour (line.withAlpha (major ? 1.0f : 0.4f));
                g.fillRect (x, a.getY(), 1.0f, a.getHeight());

                if (major)
                {
                    g.setColour (muted);
                    g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz),
                                juce::Rectangle<float> (x - 20.0f, a.getBottom() + 2.0f, 40.0f, 13.0f), juce::Justification::centred);
                }
            }

        if (! levels)
            return;

        for (auto db = 0.0f; db >= floorDb; db -= 12.0f)
        {
            const auto y = yFor (db);
            g.setColour (line);
            g.fillRect (a.getX(), y, a.getWidth(), 1.0f);
            g.setColour (muted);
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (a.getX() - 36.0f, y - 7.0f, 32.0f, 14.0f),
                        juce::Justification::centredRight);
        }
    }

    juce::Path curvePath (const std::vector<float>& db, bool closed) const
    {
        juce::Path p;
        const auto a = plot();
        const auto binHz = rate() / (double) ((db.size() - 1) * 2);
        bool started = false;

        // one point per pixel column: the loudest bin in it (peaks stay visible)
        for (int x = 0; x < (int) a.getWidth(); ++x)
        {
            const auto f0 = hzAt (a.getX() + (float) x), f1 = hzAt (a.getX() + (float) x + 1.0f);
            const auto k0 = juce::jlimit (1, (int) db.size() - 1, (int) std::floor (f0 / binHz));
            const auto k1 = juce::jlimit (k0, (int) db.size() - 1, (int) std::ceil (f1 / binHz));
            auto level = floorDb;

            for (int k = k0; k <= k1; ++k)
                level = std::max (level, db[(size_t) k]);

            const auto px = a.getX() + (float) x, py = yFor (level);

            if (! started)
            {
                p.startNewSubPath (px, closed ? a.getBottom() : py);
                started = true;
            }

            p.lineTo (px, py);
        }

        if (closed)
        {
            p.lineTo (a.getRight(), a.getBottom());
            p.closeSubPath();
        }

        return p;
    }

    void paintSpectrum (juce::Graphics& g) const
    {
        paintGrid (g, true);

        if (curve.size() < 2)
        {
            g.setColour (muted);
            g.drawText (source.audio() == nullptr ? "Open a file" : "Too short to measure", graph, juce::Justification::centred);
            return;
        }

        g.setColour (accent.withAlpha (0.25f));
        g.fillPath (curvePath (curve, true));
        g.setColour (accent);
        g.strokePath (curvePath (curve, false), juce::PathStrokeType (1.4f));

        if (holdButton.getToggleState() && peak.size() == curve.size())
        {
            g.setColour (bass.withAlpha (0.8f));
            g.strokePath (curvePath (peak, false), juce::PathStrokeType (1.0f));
        }

        // the strongest frequency, marked
        if (strongestHz > 0.0)
        {
            const auto x = xFor (strongestHz), y = yFor (strongestDb);
            g.setColour (bass);
            g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            const auto text = describeFrequency (strongestHz) + "   " + juce::String (strongestDb, 1) + " dB";
            const auto left = x + 8.0f + 260.0f > plot().getRight();
            g.drawText (text, juce::Rectangle<float> (left ? x - 268.0f : x + 8.0f, y - 18.0f, 260.0f, 16.0f),
                        left ? juce::Justification::centredRight : juce::Justification::centredLeft);
        }

        if (graph.contains (hover))
        {
            g.setColour (ink.withAlpha (0.35f));
            g.fillRect ((float) hover.x, plot().getY(), 1.0f, plot().getHeight());
        }
    }

    void paintSonogram (juce::Graphics& g)
    {
        const auto a = plot();

        if (sonogramImage.isValid())
            g.drawImage (sonogramImage, a, juce::RectanglePlacement::stretchToFit);
        else
        {
            g.setColour (muted);
            g.drawText (source.audio() == nullptr ? "Open a file" : "Too short to measure", graph, juce::Justification::centred);
        }

        // frequency labels down the left side (log axis, as in the spectrum)
        g.setFont (juce::FontOptions (10.5f));

        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
            if (hz < topHz())
            {
                const auto y = a.getBottom() - a.getHeight() * (float) (std::log (hz / 20.0) / std::log (topHz() / 20.0));
                g.setColour (line);
                g.fillRect (a.getX() - 4.0f, y, 4.0f, 1.0f);
                g.setColour (muted);
                g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz),
                            juce::Rectangle<float> (a.getX() - 36.0f, y - 7.0f, 30.0f, 14.0f), juce::Justification::centredRight);
            }
    }

    //==========================================================================
    void setSonogram (bool on)
    {
        sonogram = on;
        remember();
        scheduleMeasure();
        repaint();
    }

    void remember()
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("channel", channel);
        o->setProperty ("order", order);
        o->setProperty ("smoothing", smoothing);
        o->setProperty ("hold", holdButton.getToggleState());
        o->setProperty ("sonogram", sonogram);
        settings::set ("spectrum", juce::var (o.get()));
    }

    // Measuring a long selection takes a moment: done once things settle.
    void scheduleMeasure()   { measureAt = juce::Time::getMillisecondCounter() + 120; }

    void timerCallback() override
    {
        const auto playing = source.playing != nullptr && source.playing() && tap.getSampleRate() > 0.0;

        if (playing)
            readLive();
        else
        {
            if (live)
            {
                live = false;   // playback stopped: back to the selection / cursor
                peak.clear();
                scheduleMeasure();
            }

            juce::AudioBuffer<float> drain (2, 8192);
            while (tap.readAnalysis (drain) > 0) {}
        }

        if (measureAt != 0 && juce::Time::getMillisecondCounter() >= measureAt && ! live)
        {
            measureAt = 0;
            measure();
        }
    }

    void readLive()
    {
        juce::AudioBuffer<float> incoming (2, 8192);
        int n = 0;

        while ((n = tap.readAnalysis (incoming)) > 0)
            for (int ch = 0; ch < 2; ++ch)
            {
                auto& r = liveRing[(size_t) ch];
                const auto* x = incoming.getReadPointer (ch);
                r.insert (r.end(), x, x + n);

                if (r.size() > 65536)
                    r.erase (r.begin(), r.end() - 65536);
            }

        if (! live)
        {
            live = true;
            peak.clear();
            status.setText ("Live: what is playing now", juce::dontSendNotification);
        }

        if (freezeButton.getToggleState() || sonogram)
            return;

        const auto size = 1 << order;

        if ((int) liveRing[0].size() < size)
            return;

        juce::AudioBuffer<float> window (2, size);

        for (int ch = 0; ch < 2; ++ch)
            window.copyFrom (ch, 0, liveRing[(size_t) ch].data() + liveRing[(size_t) ch].size() - (size_t) size, size);

        auto now = smooth (spectrumDb (window, 0, size, order, channel), smoothing);

        // a little averaging so it is readable: up fast, down slowly
        if (curve.size() == now.size())
            for (size_t k = 0; k < now.size(); ++k)
                now[k] = now[k] > curve[k] ? curve[k] * 0.3f + now[k] * 0.7f : curve[k] * 0.8f + now[k] * 0.2f;

        setCurve (std::move (now));
    }

    void measure()
    {
        const auto* audio = source.audio != nullptr ? source.audio() : nullptr;

        if (audio == nullptr || audio->getNumSamples() == 0)
        {
            curve.clear();
            sonogramImage = {};
            status.setText ("No file open", juce::dontSendNotification);
            repaint();
            return;
        }

        const auto sr = source.sampleRate();
        auto range = source.selection();
        const auto fromSelection = ! range.isEmpty();
        const auto size = 1 << order;

        if (! fromSelection)
        {
            // around the cursor (the whole file when sonogram)
            const auto at = source.cursor();
            range = sonogram ? juce::Range<juce::int64> (0, audio->getNumSamples())
                             : juce::Range<juce::int64> (at - size / 2, at + size / 2);
        }

        const auto seconds = [sr] (juce::int64 s) { return juce::String ((double) s / sr, 3) + " s"; };

        if (sonogram)
        {
            makeSonogram (*audio, range, sr);
            status.setText ((fromSelection ? "The selection, " : "The whole file, ") + seconds (range.getStart()) + " to "
                                + seconds (range.getEnd()) + "  (time across, frequency up, brighter = louder)",
                            juce::dontSendNotification);
        }
        else
        {
            int windows = 0;
            setCurve (smooth (spectrumDb (*audio, range.getStart(), std::max ((juce::int64) size, range.getLength()), order, channel, &windows),
                              smoothing));
            status.setText (fromSelection ? "The selection (" + seconds (range.getLength()) + ", average of " + juce::String (windows) + " windows)"
                                          : "At the cursor, " + seconds (source.cursor()) + "  (select something for its average)",
                            juce::dontSendNotification);
        }

        repaint();
    }

    void setCurve (std::vector<float> now)
    {
        curve = std::move (now);

        if (holdButton.getToggleState())
        {
            if (peak.size() != curve.size())
                peak = curve;
            else
                for (size_t k = 0; k < curve.size(); ++k)
                    peak[k] = std::max (peak[k], curve[k]);
        }

        // the strongest bin above 20 Hz, its frequency refined between the bins
        strongestHz = 0.0;
        strongestDb = floorDb;
        const auto binHz = rate() / (double) ((curve.size() - 1) * 2);
        size_t best = 0;

        for (size_t k = 1; k + 1 < curve.size(); ++k)
            if ((double) k * binHz >= 20.0 && curve[k] > strongestDb)
            {
                strongestDb = curve[k];
                best = k;
            }

        if (best > 0 && strongestDb > -90.0f)
        {
            const auto a = curve[best - 1], b = curve[best], c = curve[best + 1];
            const auto den = a - 2.0f * b + c;
            strongestHz = ((double) best + (std::abs (den) > 1.0e-6f ? 0.5 * (a - c) / den : 0.0)) * binHz;
        }

        updateReadout();
        repaint();
    }

    void makeSonogram (const juce::AudioBuffer<float>& audio, juce::Range<juce::int64> range, double sr)
    {
        const auto a = plot();
        const auto columns = juce::jlimit (16, 1600, (int) a.getWidth());
        const auto rows = juce::jlimit (16, 800, (int) a.getHeight());
        const auto size = 1 << std::min (order, 13);
        juce::Image image (juce::Image::RGB, columns, rows, false);
        const auto top = sr / 2.0;

        for (int c = 0; c < columns; ++c)
        {
            const auto centre = range.getStart() + (juce::int64) ((double) range.getLength() * (c + 0.5) / columns);
            const auto db = spectrumDb (audio, centre - size / 2, size, (int) std::log2 (size), channel);
            const auto binHz = sr / size;

            for (int y = 0; y < rows; ++y)
            {
                const auto f0 = 20.0 * std::pow (top / 20.0, (double) (rows - 1 - y) / rows);
                const auto f1 = 20.0 * std::pow (top / 20.0, (double) (rows - y) / rows);
                const auto k0 = juce::jlimit (1, (int) db.size() - 1, (int) std::floor (f0 / binHz));
                const auto k1 = juce::jlimit (k0, (int) db.size() - 1, (int) std::ceil (f1 / binHz));
                auto level = floorDb;

                for (int k = k0; k <= k1; ++k)
                    level = std::max (level, db[(size_t) k]);

                image.setPixelAt (c, y, sonogramColour ((level + 100.0f) / 100.0f));
            }
        }

        sonogramImage = image;
        sonogramRange = range;
        sonogramRate = sr;
    }

    void updateReadout()
    {
        const auto a = plot();

        if (! a.contains (hover.toFloat()))
        {
            readout.setText ({}, juce::dontSendNotification);
            repaint();
            return;
        }

        if (sonogram)
        {
            if (! sonogramImage.isValid())
                return;

            const auto t = sonogramRange.getStart() + (juce::int64) (sonogramRange.getLength() * (hover.x - a.getX()) / a.getWidth());
            const auto hz = 20.0 * std::pow (topHz() / 20.0, (double) ((a.getBottom() - (float) hover.y) / a.getHeight()));
            readout.setText (juce::String ((double) t / sonogramRate, 3) + " s    " + describeFrequency (hz), juce::dontSendNotification);
            return;
        }

        const auto hz = hzAt ((float) hover.x);
        readout.setText (describeFrequency (hz) + (curve.empty() ? juce::String() : "    " + juce::String (levelAt (curve, hz), 1) + " dB"),
                         juce::dontSendNotification);
        repaint();
    }

    SpectrumSource source;
    MeterTap& tap;
    juce::Rectangle<int> graph;
    juce::Point<int> hover;

    int channel = 0, order = 13, smoothing = 6;
    bool sonogram = false, live = false;
    juce::uint32 measureAt = 0;
    std::vector<float> curve, peak;
    std::array<std::vector<float>, 2> liveRing;
    double strongestHz = 0.0;
    float strongestDb = floorDb;
    juce::Image sonogramImage;
    juce::Range<juce::int64> sonogramRange;
    double sonogramRate = 44100.0;

    juce::TextButton spectrumButton { "Spectrum" }, sonogramButton { "Sonogram" }, holdButton { "Peak hold" }, freezeButton { "Freeze" };
    juce::ComboBox channelBox, sizeBox, smoothBox;
    juce::Label status, readout;
};

//==============================================================================
SpectrumWindow::SpectrumWindow (SpectrumSource source, MeterTap& tap)
    : juce::DocumentWindow ("Spectrum Analysis", dark::colours::bg, juce::DocumentWindow::closeButton),
      onClose (source.onClose)
{
    setUsingNativeTitleBar (false);
    look = std::make_unique<dark::DarkLook>();
    setLookAndFeel (look.get());
    panel = new Panel (std::move (source), tap);
    setContentOwned (panel, true);
    dark::adoptLook (*getContentComponent());
    setResizable (false, false);
    setAlwaysOnTop (true);
    centreWithSize (getWidth(), getHeight());
}

SpectrumWindow::~SpectrumWindow()
{
    clearContentComponent();
    setLookAndFeel (nullptr);
}

void SpectrumWindow::sourceChanged()
{
    if (panel != nullptr)
        panel->sourceChanged();
}

void SpectrumWindow::closeButtonPressed()
{
    if (onClose != nullptr)
        onClose();
}
} // namespace wf
