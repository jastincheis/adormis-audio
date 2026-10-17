#include "EffectWindow.h"
#include "Mp3Restore.h"
#include "MoreEffects.h"
#include "Restoration.h"
#include "Settings.h"
#include "Analysis.h"
#include "DarkTheme.h"
#include "Effects.h"
#include <juce_dsp/juce_dsp.h>

namespace wf
{
namespace
{
namespace colours = dark::colours;

void styleLabel (juce::Label& l, float size, bool bold = false, juce::Colour colour = colours::ink)
{
    l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    l.setColour (juce::Label::textColourId, colour);
    l.setMinimumHorizontalScale (0.75f);
}

void paintCard (juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour (colours::surface);
    g.fillRoundedRectangle (r.toFloat(), 10.0f);
    g.setColour (colours::line);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
}

juce::String formatHz (double hz)
{
    return hz >= 1000.0 ? juce::String (hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
}

juce::String effectName (EffectKind kind)
{
    switch (kind)
    {
        case EffectKind::eq:         return "Parametric EQ";
        case EffectKind::graphicEq:  return "Graphic EQ";
        case EffectKind::compressor: return "Compressor";
        case EffectKind::delay:      return "Delay / Echo";
        case EffectKind::reverb:     return "Reverb";
        case EffectKind::mp3Restore: return "MP3 Restore";
        case EffectKind::limiter:    return "Limiter";
        case EffectKind::gate:       return "Noise Gate";
        case EffectKind::chorus:     return "Chorus";
        case EffectKind::flanger:    return "Flanger";
        case EffectKind::vibrato:    return "Vibrato";
        case EffectKind::distortion: return "Distortion";
        case EffectKind::noiseReduction: return "Noise Reduction";
        case EffectKind::humRemoval: return "Hum Removal";
        case EffectKind::declick:    return "Click and Crackle Removal";
        case EffectKind::clipRestore: return "Clipped Peak Restoration";
    }

    return "Effect";
}

//==============================================================================
// What the preview hands to the window: the last samples before and after the
// effect, for the live spectrum. Written by the audio thread, read by the UI.
struct Taps
{
    static constexpr int size = 1 << 15;
    juce::AbstractFifo preFifo { size }, postFifo { size };
    std::vector<float> pre = std::vector<float> (size), post = std::vector<float> (size);

    static void push (juce::AbstractFifo& fifo, std::vector<float>& store, const float* data, int n)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (n, s1, n1, s2, n2);
        std::copy (data, data + n1, store.data() + s1);
        std::copy (data + n1, data + n1 + n2, store.data() + s2);
        fifo.finishedWrite (n1 + n2);
    }

    // Moves what is waiting into `ring` (keeps the newest ring.size() samples).
    static void pull (juce::AbstractFifo& fifo, std::vector<float>& store, std::vector<float>& ring)
    {
        const auto ready = fifo.getNumReady();

        if (ready == 0)
            return;

        int s1, n1, s2, n2;
        fifo.prepareToRead (ready, s1, n1, s2, n2);
        auto append = [&ring] (const float* d, int n)
        {
            if (n >= (int) ring.size())
            {
                std::copy (d + n - (int) ring.size(), d + n, ring.begin());
                return;
            }

            std::move (ring.begin() + n, ring.end(), ring.begin());
            std::copy (d, d + n, ring.end() - n);
        };
        append (store.data() + s1, n1);
        append (store.data() + s2, n2);
        fifo.finishedRead (n1 + n2);
    }
};

// Loops the audio through the effect; the window keeps the processor and the
// taps (shared), so they outlive whichever of the two goes first.
class PreviewSource final : public juce::PositionableAudioSource
{
public:
    PreviewSource (std::shared_ptr<const juce::AudioBuffer<float>> a, std::shared_ptr<fx::Processor> p, std::shared_ptr<Taps> t,
                   std::shared_ptr<std::atomic<bool>> b)
        : audio (std::move (a)), processor (std::move (p)), taps (std::move (t)), bypass (std::move (b)) {}

    void prepareToPlay (int block, double rate) override
    {
        const auto n = std::max (block, 4096);
        work.setSize (audio->getNumChannels(), n);
        mono.resize ((size_t) n);
        processor->prepare (rate, audio->getNumChannels(), n);
    }

    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const auto length = audio->getNumSamples();

        if (length == 0)
        {
            info.clearActiveBufferRegion();
            return;
        }

        for (int done = 0; done < info.numSamples;)
        {
            const auto n = std::min (info.numSamples - done, work.getNumSamples());
            auto pos = position.load();

            for (int i = 0; i < n; ++i, ++pos)
                for (int ch = 0; ch < work.getNumChannels(); ++ch)
                    work.setSample (ch, i, audio->getSample (ch, (int) (pos % length)));

            position = pos;
            tap (taps->preFifo, taps->pre, n);

            if (! bypass->load())
                processor->process (work, 0, n);

            tap (taps->postFifo, taps->post, n);

            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->copyFrom (ch, info.startSample + done, work, std::min (ch, work.getNumChannels() - 1), 0, n);

            done += n;
        }
    }

    void setNextReadPosition (juce::int64 p) override   { position = p; }
    juce::int64 getNextReadPosition() const override    { return position; }
    juce::int64 getTotalLength() const override         { return std::numeric_limits<juce::int64>::max(); }
    bool isLooping() const override                     { return true; }

private:
    void tap (juce::AbstractFifo& fifo, std::vector<float>& store, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            float v = 0.0f;

            for (int ch = 0; ch < work.getNumChannels(); ++ch)
                v += work.getSample (ch, i);

            mono[(size_t) i] = v / (float) work.getNumChannels();
        }

        Taps::push (fifo, store, mono.data(), n);
    }

    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    std::shared_ptr<fx::Processor> processor;
    std::shared_ptr<Taps> taps;
    std::shared_ptr<std::atomic<bool>> bypass;
    juce::AudioBuffer<float> work;
    std::vector<float> mono;
    std::atomic<juce::int64> position { 0 };
};

//==============================================================================
// Spectrum (dB per bin) of a 4096-sample ring with a Hann window.
class SpectrumMeter
{
public:
    static constexpr int order = 12, size = 1 << order;

    std::vector<float> ring = std::vector<float> (size, 0.0f);
    std::vector<float> db = std::vector<float> (size / 2 + 1, -120.0f);

    void update (bool decay)
    {
        std::vector<float> work (2 * size, 0.0f);

        for (int i = 0; i < size; ++i)
            work[(size_t) i] = ring[(size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1)));

        fft.performFrequencyOnlyForwardTransform (work.data(), true);

        for (size_t k = 0; k < db.size(); ++k)
        {
            const auto now = juce::Decibels::gainToDecibels (work[k] * 4.0f / (float) size, -120.0f);
            db[k] = decay ? std::max (now, db[k] - 1.5f) : now;   // falls ~45 dB/s at 30 fps
        }
    }

    void clear()   { std::fill (db.begin(), db.end(), -120.0f); std::fill (ring.begin(), ring.end(), 0.0f); }

private:
    juce::dsp::FFT fft { order };
};

//==============================================================================
// What every editor gets: the audio, the live processor, the analyser rings.
struct Shared
{
    BassFinderSource source;
    BassFinderHost host;
    std::shared_ptr<juce::AudioBuffer<float>> audio;
    std::shared_ptr<fx::Processor> processor;
    std::shared_ptr<Taps> taps = std::make_shared<Taps>();
    std::shared_ptr<std::atomic<bool>> bypass = std::make_shared<std::atomic<bool>> (false);
    SpectrumMeter pre, post;
    bool previewing = false;

    double rate() const   { return source.sampleRate; }
};

class Editor : public juce::Component
{
public:
    explicit Editor (Shared& s) : shared (s) {}

    virtual void push() = 0;                                     // UI values -> live processor
    virtual std::shared_ptr<fx::Processor> makeForApply() = 0;   // a fresh processor with the same values
    virtual double tailSeconds() const   { return 0.0; }         // suggested ring-out after the selection
    virtual bool hasTail() const         { return false; }
    virtual void tick() {}                                       // 30 times a second

    // What the window remembers for next time (the controls, by default).
    virtual juce::var getState()                    { return settings::captureControls (*this); }
    virtual void setState (const juce::var& state)  { settings::restoreControls (*this, state); }

protected:
    Shared& shared;
};

// A slider with its name above it.
struct LabelledSlider final : public juce::Component
{
    LabelledSlider (const juce::String& name, double lo, double hi, double step, const juce::String& suffix, double value,
                    std::function<void()> changed, double mid = 0.0)
    {
        styleLabel (label, 12.0f, true, colours::muted);
        label.setText (name, juce::dontSendNotification);
        addAndMakeVisible (label);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 84, 22);
        slider.setRange (lo, hi, step);

        if (mid > lo && mid < hi)
            slider.setSkewFactorFromMidPoint (mid);

        slider.setTextValueSuffix (suffix);
        slider.setValue (value, juce::dontSendNotification);
        slider.setDoubleClickReturnValue (true, value);
        slider.onValueChange = std::move (changed);
        addAndMakeVisible (slider);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (16));
        slider.setBounds (r.removeFromTop (28));
    }

    double value() const   { return slider.getValue(); }

    juce::Label label;
    juce::Slider slider;
};

void styleToggle (juce::TextButton& b)
{
    b.setClickingTogglesState (true);
    b.setColour (juce::TextButton::buttonOnColourId, colours::accent.withAlpha (0.3f));
    b.setColour (juce::TextButton::textColourOnId, colours::ink);
}

//==============================================================================
// Parametric EQ: as many points as you like (up to 32), each a bell, shelf,
// cut, notch or band pass, on a zoomable graph with the live spectrum.
class EqGraph final : public juce::Component
{
public:
    EqGraph (Shared& s, fx::EqParams& p, std::vector<int>& sel) : shared (s), params (p), selection (sel)
    {
        staticPre = spectrumOf (*shared.audio);
    }

    std::function<void()> onChanged;      // a band was moved / added / removed
    std::function<void()> onSelection;    // the picked band changed

    double minHz = 20.0, maxHz = 20000.0, dbRange = 18.0;

    void fit()
    {
        minHz = 20.0;
        maxHz = std::min (20000.0, shared.rate() * 0.48);
        repaint();
    }

    int picked() const   { return selection.empty() ? -1 : selection.back(); }

    void paint (juce::Graphics& g) override
    {
        const auto a = plot();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

        // Grid.
        g.setFont (juce::FontOptions (11.0f));

        for (double decade = 10.0; decade <= 20000.0; decade *= 10.0)
            for (int m = 1; m <= 9; ++m)
            {
                const auto hz = decade * m;

                if (hz < minHz || hz > maxHz)
                    continue;

                const auto x = xFor (hz);
                const auto major = m == 1 || m == 2 || m == 5;
                g.setColour (colours::line.withAlpha (major ? 1.0f : 0.45f));
                g.fillRect (x, a.getY(), 1.0f, a.getHeight());

                if (major)
                {
                    g.setColour (colours::muted);
                    g.drawText (hz >= 1000.0 ? juce::String (hz / 1000.0, 0) + "k" : juce::String ((int) hz),
                                juce::Rectangle<float> (x - 20.0f, a.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centred);
                }
            }

        const auto step = dbRange <= 6.0 ? 2.0 : dbRange <= 12.0 ? 3.0 : dbRange <= 24.0 ? 6.0 : 12.0;

        for (auto db = -dbRange; db <= dbRange + 0.01; db += step)
        {
            const auto y = yFor (db);
            g.setColour (std::abs (db) < 0.01 ? colours::muted.withAlpha (0.7f) : colours::line);
            g.fillRect (a.getX(), y, a.getWidth(), 1.0f);
            g.setColour (colours::muted);
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (a.getRight() + 4.0f, juce::jmin (y - 7.0f, a.getBottom() - 14.0f), 32.0f, 14.0f),
                        juce::Justification::centredLeft);
        }

        // Spectrum: live while previewing (grey = before, orange = after the
        // EQ); otherwise the selection's average, with the EQ curve added.
        const auto hzPerBin = shared.rate() / SpectrumMeter::size;

        auto spectrumPath = [&] (const std::vector<float>& db, double binHz, const std::function<double (double)>& extra)
        {
            juce::Path p;
            bool started = false;

            for (size_t k = 1; k < db.size(); ++k)
            {
                const auto hz = k * binHz;

                if (hz < minHz * 0.9 || hz > maxHz * 1.1)
                    continue;

                // Spectrum scale: 0 dBFS at the top, -96 at the bottom.
                const auto level = db[k] + (extra != nullptr ? extra (hz) : 0.0);
                const auto y = a.getY() + a.getHeight() * (float) juce::jlimit (0.0, 1.0, -(level + 6.0) / 90.0);

                if (! started)
                {
                    p.startNewSubPath (xFor (hz), a.getBottom());
                    started = true;
                }

                p.lineTo (xFor (hz), y);
            }

            p.lineTo (a.getRight(), a.getBottom());
            p.closeSubPath();
            return p;
        };

        if (shared.previewing)
        {
            g.setColour (colours::muted.withAlpha (0.25f));
            g.fillPath (spectrumPath (shared.pre.db, hzPerBin, nullptr));
            g.setColour (colours::bass.withAlpha (0.22f));
            g.fillPath (spectrumPath (shared.post.db, hzPerBin, nullptr));
            g.setColour (colours::bass.withAlpha (0.8f));
            g.strokePath (spectrumPath (shared.post.db, hzPerBin, nullptr), juce::PathStrokeType (1.0f));
        }
        else if (! staticPre.empty())
        {
            const auto binHz = shared.rate() / ((staticPre.size() - 1) * 2.0);
            g.setColour (colours::muted.withAlpha (0.25f));
            g.fillPath (spectrumPath (staticPre, binHz, nullptr));
            g.setColour (colours::bass.withAlpha (0.6f));
            g.strokePath (spectrumPath (staticPre, binHz, [this] (double hz) { return fx::eqResponseDb (params, hz, shared.rate()); }),
                          juce::PathStrokeType (1.0f));
        }

        // Each band faintly, then the whole curve.
        auto curve = [&] (const std::function<double (double)>& db)
        {
            juce::Path p;

            for (int x = 0; x <= (int) a.getWidth(); x += 2)
            {
                const auto y = yFor (db (hzFor (a.getX() + (float) x)));

                if (x == 0)
                    p.startNewSubPath (a.getX(), y);
                else
                    p.lineTo (a.getX() + (float) x, y);
            }

            return p;
        };

        for (int b = 0; b < params.count; ++b)
        {
            const auto& band = params.bands[(size_t) b];
            g.setColour (colourFor (b).withAlpha (band.on ? 0.45f : 0.15f));
            g.strokePath (curve ([&] (double hz) { return fx::bandResponseDb (band, hz, shared.rate()); }), juce::PathStrokeType (1.0f));
        }

        g.setColour (colours::accent);
        g.strokePath (curve ([&] (double hz) { return fx::eqResponseDb (params, hz, shared.rate()); }), juce::PathStrokeType (2.4f));

        for (int b = 0; b < params.count; ++b)
        {
            const auto& band = params.bands[(size_t) b];
            const auto h = handleFor (band);
            const auto selected = std::find (selection.begin(), selection.end(), b) != selection.end();
            const auto r = selected ? 9.0f : 7.0f;

            if (selected && selection.size() > 1)
            {
                g.setColour (colours::warn);
                g.drawEllipse (h.x - r - 3.0f, h.y - r - 3.0f, (r + 3.0f) * 2.0f, (r + 3.0f) * 2.0f, 2.0f);
            }

            g.setColour (colourFor (b).withAlpha (band.on ? 1.0f : 0.35f));
            g.fillEllipse (h.x - r, h.y - r, r * 2.0f, r * 2.0f);
            g.setColour (b == picked() ? colours::ink : colours::bg);
            g.drawEllipse (h.x - r, h.y - r, r * 2.0f, r * 2.0f, b == picked() ? 2.0f : 1.5f);
            g.setColour (colours::bg);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (h.x - r, h.y - r, r * 2.0f, r * 2.0f), juce::Justification::centred);
        }

        if (params.count == 0)
        {
            g.setColour (colours::muted);
            g.setFont (juce::FontOptions (15.0f));
            // Halfway up, so it does not sit on the 0 dB line.
            g.drawText ("Double-click anywhere to add a point", a.withY (yFor (dbRange * 0.5) - 10.0f).withHeight (20.0f),
                        juce::Justification::centred);
        }

        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (shared.previewing ? "live: grey before, orange after the EQ" : "selection's spectrum, orange with the EQ",
                    juce::Rectangle<float> (a.getX() + 8.0f, a.getY() + 4.0f, 300.0f, 14.0f), juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStart = e.position;
        const auto hit = bandAt (e.position);

        if (e.mods.isPopupMenu())
        {
            if (hit >= 0)
                showBandMenu (hit);

            return;
        }

        if (hit < 0)
        {
            dragging = false;

            if (selection.size() > 1)
            {
                const auto keep = picked();
                selection = { keep };
                selected();
            }

            return;
        }

        if (e.mods.isShiftDown())
        {
            const auto it = std::find (selection.begin(), selection.end(), hit);

            if (it != selection.end() && selection.size() > 1)
                selection.erase (it);
            else if (it == selection.end())
                selection.push_back (hit);

            dragging = false;
            selected();
            return;
        }

        if (std::find (selection.begin(), selection.end(), hit) == selection.end())
            selection = { hit };
        else
        {
            // Make it the picked one, keeping the group.
            selection.erase (std::find (selection.begin(), selection.end(), hit));
            selection.push_back (hit);
        }

        dragging = true;
        startBands.clear();

        for (auto i : selection)
            startBands.push_back (params.bands[(size_t) i]);

        selected();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging || startBands.size() != selection.size())
            return;

        const auto gainStep = gainFor (e.position.y) - gainFor (dragStart.y);
        const auto ratio = hzFor (e.position.x) / hzFor (dragStart.x);
        const auto qRatio = std::pow (1.02, dragStart.y - e.position.y);

        for (size_t i = 0; i < selection.size(); ++i)
        {
            auto& band = params.bands[(size_t) selection[i]];
            const auto& from = startBands[i];

            if (e.mods.isShiftDown())
            {
                band.q = juce::jlimit (0.1, 18.0, from.q * qRatio);
            }
            else
            {
                band.frequency = juce::jlimit (10.0, shared.rate() * 0.48, from.frequency * ratio);

                if (fx::bandHasGain (band.type))
                    band.gainDb = juce::jlimit (-36.0, 36.0, std::round ((from.gainDb + gainStep) * 10.0) / 10.0);
            }
        }

        changed();
    }

    void mouseUp (const juce::MouseEvent&) override   { dragging = false; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (bandAt (e.position) >= 0 || params.count >= fx::maxEqPoints)
            return;

        auto& band = params.bands[(size_t) params.count];
        band = {};
        band.frequency = hzFor (e.position.x);
        band.gainDb = std::round (juce::jlimit (-dbRange, dbRange, gainFor (e.position.y)) * 2.0) / 2.0;
        // A point added at the far ends starts as a shelf.
        band.type = band.frequency < 60.0 ? fx::EqBand::lowShelf : band.frequency > 12000.0 ? fx::EqBand::highShelf : fx::EqBand::bell;
        selection = { params.count };
        ++params.count;
        changed();
        selected();
    }

    // Over a point: its width; elsewhere: zoom the frequencies (Ctrl: the dB range).
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (const auto hit = bandAt (e.position); hit >= 0 && ! e.mods.isCtrlDown())
        {
            const auto f = std::pow (1.15, w.deltaY * 4.0);

            for (auto i : (std::find (selection.begin(), selection.end(), hit) != selection.end() ? selection : std::vector<int> { hit }))
                params.bands[(size_t) i].q = juce::jlimit (0.1, 18.0, params.bands[(size_t) i].q * f);

            changed();
            return;
        }

        if (e.mods.isCtrlDown())
        {
            dbRange = juce::jlimit (3.0, 48.0, dbRange * std::pow (0.85, w.deltaY * 4.0));
            repaint();
            return;
        }

        const auto centre = hzFor (e.position.x);
        const auto factor = std::pow (0.85, w.deltaY * 4.0);
        auto lo = centre * std::pow (minHz / centre, factor), hi = centre * std::pow (maxHz / centre, factor);
        const auto top = std::min (20000.0, shared.rate() * 0.48);

        if (hi / lo < 2.0)   // at least an octave in view
            return;

        if (lo < 20.0)   { hi *= 20.0 / lo; lo = 20.0; }
        if (hi > top)    { lo *= top / hi; hi = top; }

        minHz = std::max (20.0, lo);
        maxHz = std::min (top, hi);
        repaint();
    }

    void removeSelected()
    {
        auto picked = selection;
        std::sort (picked.rbegin(), picked.rend());

        for (auto i : picked)
            if (juce::isPositiveAndBelow (i, params.count))
            {
                for (int k = i; k < params.count - 1; ++k)
                    params.bands[(size_t) k] = params.bands[(size_t) k + 1];

                --params.count;
            }

        selection = params.count > 0 ? std::vector<int> { std::min (params.count - 1, picked.empty() ? 0 : picked.back()) } : std::vector<int> {};
        changed();
        selected();
    }

    static juce::Colour colourFor (int band)
    {
        static const juce::Colour palette[] { juce::Colour (0xff8193ff), juce::Colour (0xff4fcf8c), juce::Colour (0xfff2b84b),
                                              juce::Colour (0xffff6b9a), juce::Colour (0xff59d0e8), juce::Colour (0xffc58bff),
                                              juce::Colour (0xffff9a3d), juce::Colour (0xffb8e05a) };
        return palette[band % 8];
    }

private:
    std::vector<float> spectrumOf (const juce::AudioBuffer<float>& audio) const
    {
        // Average over the whole selection, at most ~2000 frames.
        constexpr int order = 12, size = 1 << order;

        if (audio.getNumSamples() < size)
            return {};

        juce::dsp::FFT fft (order);
        std::vector<float> work (2 * size);
        std::vector<double> sum (size / 2 + 1, 0.0);
        const auto frames = std::min (2000, (audio.getNumSamples() - size) / (size / 2) + 1);
        const auto hop = std::max (1, (audio.getNumSamples() - size) / std::max (1, frames - 1));

        for (int f = 0; f < frames; ++f)
        {
            std::fill (work.begin(), work.end(), 0.0f);

            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                for (int i = 0; i < size; ++i)
                    work[(size_t) i] += audio.getSample (ch, f * hop + i) / (float) audio.getNumChannels()
                                      * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1)));

            fft.performFrequencyOnlyForwardTransform (work.data(), true);

            for (size_t k = 0; k < sum.size(); ++k)
                sum[k] += (double) work[k] * work[k];
        }

        std::vector<float> db (sum.size());

        for (size_t k = 0; k < sum.size(); ++k)
            db[k] = (float) juce::Decibels::gainToDecibels (std::sqrt (sum[k] / frames) * 4.0 / size, -120.0);

        return db;
    }

    void showBandMenu (int band)
    {
        juce::PopupMenu m;
        auto& b = params.bands[(size_t) band];

        for (int t = 0; t <= fx::EqBand::bandPass; ++t)
            m.addItem (1 + t, fx::bandTypeName ((fx::EqBand::Type) t), true, b.type == t);

        m.addSeparator();

        if (b.type == fx::EqBand::lowCut || b.type == fx::EqBand::highCut)
        {
            for (int s : { 12, 24, 36, 48 })
                m.addItem (100 + s, juce::String (s) + " dB / octave", true, b.slope == s);

            m.addSeparator();
        }

        m.addItem (200, b.on ? "Turn off (bypass this point)" : "Turn on");
        m.addItem (201, "Remove");

        juce::Component::SafePointer<EqGraph> safeThis (this);
        m.showMenuAsync (juce::PopupMenu::Options().withParentComponent (getTopLevelComponent()).withMousePosition(),
                         [safeThis, band] (int result)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || result == 0 || ! juce::isPositiveAndBelow (band, self->params.count))
                return;

            auto& bb = self->params.bands[(size_t) band];

            if (result >= 1 && result <= 7)       bb.type = (fx::EqBand::Type) (result - 1);
            else if (result > 100 && result < 200) bb.slope = result - 100;
            else if (result == 200)                bb.on = ! bb.on;
            else if (result == 201)
            {
                self->selection = { band };
                self->removeSelected();
                return;
            }

            self->selection = { band };
            self->changed();
            self->selected();
        });
    }

    juce::Rectangle<float> plot() const   { return getLocalBounds().toFloat().reduced (12.0f).withTrimmedRight (32.0f).withTrimmedBottom (16.0f); }

    float xFor (double hz) const
    {
        const auto a = plot();
        return a.getX() + a.getWidth() * (float) (std::log (hz / minHz) / std::log (maxHz / minHz));
    }

    double hzFor (float x) const
    {
        const auto a = plot();
        return minHz * std::pow (maxHz / minHz, juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth())));
    }

    float yFor (double db) const
    {
        const auto a = plot();
        return a.getY() + a.getHeight() * (float) ((dbRange - juce::jlimit (-dbRange * 1.2, dbRange * 1.2, db)) / (2.0 * dbRange));
    }

    double gainFor (float y) const
    {
        const auto a = plot();
        return dbRange - 2.0 * dbRange * (double) ((y - a.getY()) / a.getHeight());
    }

    // Cuts, notches and band passes sit on the 0 dB line (they have no gain).
    juce::Point<float> handleFor (const fx::EqBand& b) const
    {
        return { xFor (b.frequency), yFor (fx::bandHasGain (b.type) ? b.gainDb : 0.0) };
    }

    int bandAt (juce::Point<float> p) const
    {
        int best = -1;
        auto bestDistance = 13.0f;

        for (int b = 0; b < params.count; ++b)
            if (const auto d = p.getDistanceFrom (handleFor (params.bands[(size_t) b])); d < bestDistance)
            {
                bestDistance = d;
                best = b;
            }

        return best;
    }

    void changed()   { repaint(); if (onChanged != nullptr) onChanged(); }
    void selected()  { repaint(); if (onSelection != nullptr) onSelection(); }

    Shared& shared;
    fx::EqParams& params;
    std::vector<int>& selection;
    std::vector<float> staticPre;
    juce::Point<float> dragStart;
    std::vector<fx::EqBand> startBands;
    bool dragging = false;
};

class EqEditor final : public Editor
{
public:
    explicit EqEditor (Shared& s) : Editor (s), graph (s, params, selection)
    {
        graph.onChanged = [this] { push(); refreshControls(); };
        graph.onSelection = [this] { refreshControls(); };
        addAndMakeVisible (graph);

        for (int t = 0; t <= fx::EqBand::bandPass; ++t)
            typeBox.addItem (fx::bandTypeName ((fx::EqBand::Type) t), t + 1);

        typeBox.onChange = [this] { forSelected ([this] (fx::EqBand& b) { b.type = (fx::EqBand::Type) (typeBox.getSelectedId() - 1); }); };

        for (int sl : { 12, 24, 36, 48 })
            slopeBox.addItem (juce::String (sl) + " dB/oct", sl);

        slopeBox.onChange = [this] { forSelected ([this] (fx::EqBand& b) { b.slope = slopeBox.getSelectedId(); }); };
        addAndMakeVisible (typeBox);
        addAndMakeVisible (slopeBox);

        styleToggle (onButton);
        onButton.onClick = [this] { forSelected ([this] (fx::EqBand& b) { b.on = onButton.getToggleState(); }); };
        removeButton.onClick = [this] { graph.removeSelected(); };
        addButton.onClick = [this] { addBandAt (1000.0); };
        selectAll.onClick = [this]
        {
            if (selection.size() > 1 || params.count == 0)
                selection = params.count > 0 ? std::vector<int> { std::max (0, graph.picked()) } : std::vector<int> {};
            else
            {
                selection.clear();

                for (int i = 0; i < params.count; ++i)
                    selection.push_back (i);
            }

            graph.repaint();
            refreshControls();
        };
        fitButton.onClick = [this] { graph.fit(); };

        for (auto* b : { &onButton, &removeButton, &addButton, &selectAll, &fitButton })
            addAndMakeVisible (b);

        for (int i = 0; i < 4; ++i)
        {
            auto* b = rangeButtons.add (new juce::TextButton (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + juce::String (std::array<int, 4> { 6, 12, 24, 48 }[(size_t) i])));
            styleToggle (*b);
            b->setRadioGroupId (301);
            b->onClick = [this, i] { graph.dbRange = std::array<double, 4> { 6.0, 12.0, 24.0, 48.0 }[(size_t) i]; graph.repaint(); };
            addAndMakeVisible (b);
        }

        rangeButtons[1]->setToggleState (true, juce::dontSendNotification);
        graph.dbRange = 12.0;

        // Nothing exists above half the sample rate (11 kHz in a 22 kHz file),
        // so the graph and the Frequency slider end there.
        graph.fit();
        const auto top = std::min (20000.0, shared.rate() * 0.48);
        freq.slider.setRange (20.0, top, 1.0);
        freq.slider.setSkewFactorFromMidPoint (1000.0);

        for (auto* l : { &bandLabel, &typeLabel, &hint })
            addAndMakeVisible (l);

        styleLabel (bandLabel, 13.0f, true);
        styleLabel (typeLabel, 12.0f, true, colours::muted);
        typeLabel.setText ("Shape", juce::dontSendNotification);
        styleLabel (hint, 11.5f, false, colours::muted);
        hint.setMinimumHorizontalScale (1.0f);
        hint.setJustificationType (juce::Justification::topLeft);
        hint.setText ("Double-click: new point.  Drag: move (Shift: width).  Wheel on a point: width; elsewhere: zoom in / out "
                      "(Ctrl+wheel: dB range).  Right-click a point: shape, slope, off, remove.  Shift+click or Select all: move several together.",
                      juce::dontSendNotification);

        addAndMakeVisible (freq);
        addAndMakeVisible (gain);
        addAndMakeVisible (q);
        addAndMakeVisible (output);

        refreshControls();
        push();
    }

    void push() override
    {
        fx::setParams (*shared.processor, params);
    }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeEq();
        fx::setParams (*p, params);
        return p;
    }

    void tick() override   { graph.repaint(); }

    void resized() override
    {
        auto r = getLocalBounds();
        auto controls = r.removeFromBottom (168);
        r.removeFromBottom (8);

        auto top = r.removeFromTop (28);
        fitButton.setBounds (top.removeFromRight (110).reduced (2));
        top.removeFromRight (8);

        for (int i = rangeButtons.size(); --i >= 0;)
            rangeButtons[i]->setBounds (top.removeFromRight (52).reduced (2));

        top.removeFromRight (8);
        selectAll.setBounds (top.removeFromRight (150).reduced (2));
        addButton.setBounds (top.removeFromLeft (120).reduced (2));
        r.removeFromTop (6);
        graph.setBounds (r);

        auto row = controls.removeFromTop (30);
        bandLabel.setBounds (row.removeFromLeft (250));
        typeLabel.setBounds (row.removeFromLeft (46));
        typeBox.setBounds (row.removeFromLeft (150).reduced (2));
        slopeBox.setBounds (row.removeFromLeft (120).reduced (2));
        onButton.setBounds (row.removeFromLeft (80).reduced (2));
        removeButton.setBounds (row.removeFromLeft (110).reduced (2));
        controls.removeFromTop (6);

        row = controls.removeFromTop (46);
        const auto w = row.getWidth() / 4;
        freq.setBounds (row.removeFromLeft (w).reduced (6, 0));
        gain.setBounds (row.removeFromLeft (w).reduced (6, 0));
        q.setBounds (row.removeFromLeft (w).reduced (6, 0));
        output.setBounds (row.reduced (6, 0));
        controls.removeFromTop (8);
        hint.setBounds (controls.removeFromTop (34));
    }

private:
    void addBandAt (double hz)
    {
        if (params.count >= fx::maxEqPoints)
            return;

        auto& b = params.bands[(size_t) params.count];
        b = {};
        b.frequency = hz;
        selection = { params.count };
        ++params.count;
        push();
        graph.repaint();
        refreshControls();
    }

    void forSelected (const std::function<void (fx::EqBand&)>& fn)
    {
        for (auto i : selection)
            if (juce::isPositiveAndBelow (i, params.count))
                fn (params.bands[(size_t) i]);

        push();
        graph.repaint();
        refreshControls();
    }

    void refreshControls()
    {
        const auto b = graph.picked();
        const auto has = juce::isPositiveAndBelow (b, params.count);

        for (auto* c : { (juce::Component*) &typeBox, (juce::Component*) &onButton, (juce::Component*) &removeButton,
                         (juce::Component*) &freq, (juce::Component*) &gain, (juce::Component*) &q })
            c->setEnabled (has);

        selectAll.setEnabled (params.count > 1 || selection.size() > 1);
        selectAll.setButtonText (selection.size() > 1 ? "Just one point" : "Select all points");
        addButton.setEnabled (params.count < fx::maxEqPoints);

        if (! has)
        {
            bandLabel.setText (params.count == 0 ? "No points yet" : "No point picked", juce::dontSendNotification);
            slopeBox.setEnabled (false);
            return;
        }

        const auto& band = params.bands[(size_t) b];
        bandLabel.setText (selection.size() > 1 ? juce::String ((int) selection.size()) + " points selected (moved together)"
                                                : "Point " + juce::String (b + 1) + " of " + juce::String (params.count) + ":  "
                                                      + formatHz (band.frequency),
                           juce::dontSendNotification);
        bandLabel.setColour (juce::Label::textColourId, EqGraph::colourFor (b));
        typeBox.setSelectedId (band.type + 1, juce::dontSendNotification);
        const auto isCut = band.type == fx::EqBand::lowCut || band.type == fx::EqBand::highCut;
        slopeBox.setEnabled (isCut);
        slopeBox.setSelectedId (band.slope, juce::dontSendNotification);
        onButton.setToggleState (band.on, juce::dontSendNotification);
        onButton.setButtonText (band.on ? "On" : "Off");
        freq.slider.setValue (band.frequency, juce::dontSendNotification);
        gain.slider.setValue (band.gainDb, juce::dontSendNotification);
        gain.setEnabled (fx::bandHasGain (band.type));
        q.slider.setValue (band.q, juce::dontSendNotification);
    }

    // The sliders move every selected point by the same amount.
    void sliderMoved (int which)
    {
        const auto b = graph.picked();

        if (! juce::isPositiveAndBelow (b, params.count))
            return;

        const auto& lead = params.bands[(size_t) b];
        const auto ratio = which == 0 ? freq.value() / lead.frequency : which == 2 ? q.value() / lead.q : 1.0;
        const auto step = which == 1 ? gain.value() - lead.gainDb : 0.0;

        forSelected ([&] (fx::EqBand& x)
        {
            if (which == 0) x.frequency = juce::jlimit (10.0, shared.rate() * 0.48, x.frequency * ratio);
            if (which == 1 && fx::bandHasGain (x.type)) x.gainDb = juce::jlimit (-36.0, 36.0, x.gainDb + step);
            if (which == 2) x.q = juce::jlimit (0.1, 18.0, x.q * ratio);
        });
    }

public:
    juce::var getState() override
    {
        juce::Array<juce::var> bands;

        for (int i = 0; i < params.count; ++i)
        {
            const auto& b = params.bands[(size_t) i];
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("type", (int) b.type);
            o->setProperty ("hz", b.frequency);
            o->setProperty ("db", b.gainDb);
            o->setProperty ("q", b.q);
            o->setProperty ("slope", b.slope);
            o->setProperty ("on", b.on);
            bands.add (juce::var (o.get()));
        }

        juce::DynamicObject::Ptr state = new juce::DynamicObject();
        state->setProperty ("bands", bands);
        state->setProperty ("output", params.outputDb);
        state->setProperty ("range", graph.dbRange);
        return juce::var (state.get());
    }

    void setState (const juce::var& state) override
    {
        const auto* bands = state["bands"].getArray();

        if (bands == nullptr)
            return;

        params.count = 0;

        for (const auto& v : *bands)
        {
            if (params.count >= fx::maxEqPoints)
                break;

            auto& b = params.bands[(size_t) params.count++];
            b.type = (fx::EqBand::Type) juce::jlimit (0, (int) fx::EqBand::bandPass, (int) v["type"]);
            b.frequency = juce::jlimit (10.0, shared.rate() * 0.48, (double) v["hz"]);
            b.gainDb = v["db"];
            b.q = v["q"];
            b.slope = v["slope"];
            b.on = v["on"];
        }

        selection = params.count > 0 ? std::vector<int> { 0 } : std::vector<int> {};
        output.slider.setValue ((double) state["output"], juce::sendNotificationSync);

        const auto range = (double) state["range"];
        const double ranges[] { 6.0, 12.0, 24.0, 48.0 };

        for (int i = 0; i < 4; ++i)
            if (juce::approximatelyEqual (ranges[i], range))
            {
                rangeButtons[i]->setToggleState (true, juce::dontSendNotification);
                graph.dbRange = range;
            }

        push();
        refreshControls();
        graph.repaint();
    }

private:
    fx::EqParams params;
    std::vector<int> selection;
    EqGraph graph;
    juce::ComboBox typeBox, slopeBox;
    juce::TextButton onButton { "On" }, removeButton { "Remove" }, addButton { "+ Add point" }, selectAll { "Select all points" },
                     fitButton { "Zoom: all" };
    juce::OwnedArray<juce::TextButton> rangeButtons;
    juce::Label bandLabel, typeLabel, hint;
    LabelledSlider freq { "Frequency", 20.0, 20000.0, 1.0, " Hz", 1000.0, [this] { sliderMoved (0); }, 1000.0 };
    LabelledSlider gain { "Gain", -36.0, 36.0, 0.1, " dB", 0.0, [this] { sliderMoved (1); } };
    LabelledSlider q { "Width (Q)", 0.1, 18.0, 0.01, {}, 1.0, [this] { sliderMoved (2); }, 1.5 };
    LabelledSlider output { "Output", -24.0, 24.0, 0.1, " dB", 0.0, [this] { params.outputDb = output.value(); push(); graph.repaint(); } };
};

//==============================================================================
// Graphic EQ: a row of faders like a hardware graphic equaliser (5 to 31
// bands), with the curve they make and the spectrum above them.
juce::String shortHz (double hz)
{
    if (hz < 1000.0)
        return std::abs (hz - std::round (hz)) < 0.01 ? juce::String (juce::roundToInt (hz)) : juce::String (hz, 1);

    const auto k = hz / 1000.0;

    if (std::abs (k - std::round (k)) < 0.01)
        return juce::String (juce::roundToInt (k)) + "k";

    return juce::String (k, std::abs (k * 10.0 - std::round (k * 10.0)) < 0.01 ? 1 : 2) + "k";
}

class GraphicEqEditor final : public Editor
{
public:
    explicit GraphicEqEditor (Shared& s) : Editor (s)
    {
        staticPre = averageSpectrum (*shared.audio);

        for (int count : { 5, 10, 15, 20, 31 })
        {
            auto* b = countButtons.add (new juce::TextButton (juce::String (count)));
            styleToggle (*b);
            b->setRadioGroupId (501);
            b->onClick = [this, count] { setBandCount (count); };
            addAndMakeVisible (b);
        }

        for (int r : { 12, 15, 24 })
        {
            auto* b = rangeButtons.add (new juce::TextButton (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + juce::String (r) + " dB"));
            styleToggle (*b);
            b->setRadioGroupId (502);
            b->onClick = [this, r] { setRange (r); };
            addAndMakeVisible (b);
        }

        flatButton.onClick = [this] { std::fill (db.begin(), db.end(), 0.0); changed(); };
        presetsButton.onClick = [this] { showPresets(); };

        for (auto* b : { &flatButton, &presetsButton })
            addAndMakeVisible (b);

        for (auto* l : { &countLabel, &rangeLabel, &hint })
            addAndMakeVisible (l);

        styleLabel (countLabel, 12.0f, true, colours::muted);
        styleLabel (rangeLabel, 12.0f, true, colours::muted);
        countLabel.setText ("Sliders", juce::dontSendNotification);
        rangeLabel.setText ("Range", juce::dontSendNotification);
        styleLabel (hint, 11.5f, false, colours::muted);
        hint.setJustificationType (juce::Justification::topLeft);
        hint.setText ("Drag a slider up or down, or drag across several to draw a curve.  Double-click: back to 0.  "
                      "Wheel: half a dB.  Presets: ready-made curves, and your own.",
                      juce::dontSendNotification);

        addAndMakeVisible (output);
        setBandCount (10);
        setRange (12);
    }

    void push() override
    {
        fx::setParams (*shared.processor, params());
    }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeEq();
        fx::setParams (*p, params());
        return p;
    }

    void tick() override   { repaint (curveArea); }

    juce::var getState() override
    {
        juce::Array<juce::var> values;

        for (auto v : db)
            values.add (v);

        juce::DynamicObject::Ptr state = new juce::DynamicObject();
        state->setProperty ("bands", (int) hz.size());
        state->setProperty ("range", range);
        state->setProperty ("db", values);
        state->setProperty ("output", output.value());
        return juce::var (state.get());
    }

    void setState (const juce::var& state) override
    {
        const auto count = (int) state["bands"];
        setBandCount (count == 5 || count == 10 || count == 15 || count == 20 || count == 31 ? count : 10);

        if (const auto* values = state["db"].getArray(); values != nullptr && values->size() == (int) db.size())
            for (size_t i = 0; i < db.size(); ++i)
                db[i] = juce::jlimit (-24.0, 24.0, (double) (*values)[(int) i]);

        const auto r = (int) state["range"];
        setRange (r == 15 || r == 24 ? r : 12);
        output.slider.setValue ((double) state["output"], juce::dontSendNotification);
        changed();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (30);
        countLabel.setBounds (top.removeFromLeft (52));

        for (auto* b : countButtons)
            b->setBounds (top.removeFromLeft (44).reduced (2));

        top.removeFromLeft (16);
        rangeLabel.setBounds (top.removeFromLeft (46));

        for (auto* b : rangeButtons)
            b->setBounds (top.removeFromLeft (70).reduced (2));

        presetsButton.setBounds (top.removeFromRight (120).reduced (2));
        flatButton.setBounds (top.removeFromRight (80).reduced (2));

        auto bottom = r.removeFromBottom (78);
        output.setBounds (bottom.removeFromTop (46).removeFromLeft (360).reduced (6, 0));
        hint.setBounds (bottom.reduced (6, 4));

        r.removeFromTop (8);
        curveArea = r.removeFromTop (juce::jmax (110, r.getHeight() / 3));
        r.removeFromTop (8);
        faderArea = r;
    }

    void paint (juce::Graphics& g) override
    {
        paintCurve (g, curveArea);
        paintFaders (g, faderArea);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto band = bandAt (e.x);

        if (band < 0 || ! faderArea.contains (e.getPosition()))
            return;

        lastBand = band;
        lastValue = valueAt (e.y);
        db[(size_t) band] = lastValue;
        changed();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (lastBand < 0)
            return;

        // Across several sliders: a straight line from the last one to here.
        const auto band = juce::jlimit (0, (int) db.size() - 1, bandAt (juce::jlimit (faderArea.getX(), faderArea.getRight() - 1, e.x)));
        const auto value = valueAt (e.y);
        const auto from = std::min (band, lastBand), to = std::max (band, lastBand);

        for (int b = from; b <= to; ++b)
        {
            const auto t = to == from ? 1.0 : (double) (b - lastBand) / (double) (band - lastBand);
            db[(size_t) b] = lastValue + (value - lastValue) * juce::jlimit (0.0, 1.0, t);
        }

        lastBand = band;
        lastValue = value;
        changed();
    }

    void mouseUp (const juce::MouseEvent&) override   { lastBand = -1; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const auto band = bandAt (e.x); band >= 0 && faderArea.contains (e.getPosition()))
        {
            db[(size_t) band] = 0.0;
            changed();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (const auto band = bandAt (e.x); band >= 0 && faderArea.contains (e.getPosition()))
        {
            db[(size_t) band] = juce::jlimit (-range, range, std::round ((db[(size_t) band] + (w.deltaY > 0 ? 0.5 : -0.5)) * 2.0) / 2.0);
            changed();
        }
    }

private:
    fx::EqParams params() const   { return fx::graphicEqParams (hz, db, output.value(), shared.rate()); }

    void changed()
    {
        push();
        repaint();
    }

    // A new number of sliders keeps the curve's shape.
    void setBandCount (int count)
    {
        const auto oldHz = hz;
        const auto oldDb = db;
        hz = fx::graphicEqFrequencies (count);
        db.assign (hz.size(), 0.0);

        if (oldHz.size() >= 2)
            for (size_t i = 0; i < hz.size(); ++i)
                db[i] = curveAt (oldHz, oldDb, hz[i]);

        for (int i = 0; i < countButtons.size(); ++i)
            countButtons[i]->setToggleState (countButtons[i]->getButtonText().getIntValue() == count, juce::dontSendNotification);

        changed();
    }

    void setRange (int dbRange)
    {
        range = dbRange;

        for (auto& v : db)
            v = juce::jlimit (-range, range, v);

        for (int i = 0; i < rangeButtons.size(); ++i)
            rangeButtons[i]->setToggleState (std::array<int, 3> { 12, 15, 24 }[(size_t) i] == dbRange, juce::dontSendNotification);

        changed();
    }

    // Straight lines between the sliders on a log-frequency axis (flat beyond the ends).
    static double curveAt (const std::vector<double>& fs, const std::vector<double>& values, double f)
    {
        if (f <= fs.front())  return values.front();
        if (f >= fs.back())   return values.back();

        for (size_t i = 0; i + 1 < fs.size(); ++i)
            if (f <= fs[i + 1])
            {
                const auto t = std::log (f / fs[i]) / std::log (fs[i + 1] / fs[i]);
                return values[i] + (values[i + 1] - values[i]) * t;
            }

        return values.back();
    }

    bool bandAvailable (size_t i) const   { return hz[i] < shared.rate() * 0.45; }

    int bandAt (int x) const
    {
        if (hz.empty() || x < faderArea.getX() || x >= faderArea.getRight())
            return -1;

        return juce::jlimit (0, (int) hz.size() - 1, (int) ((x - faderArea.getX()) * (int) hz.size() / std::max (1, faderArea.getWidth())));
    }

    juce::Rectangle<float> track() const
    {
        return faderArea.toFloat().withTrimmedTop (22.0f).withTrimmedBottom (24.0f);
    }

    float yFor (double value) const
    {
        const auto t = track();
        return t.getY() + t.getHeight() * (float) ((range - value) / (2.0 * range));
    }

    double valueAt (int y) const
    {
        const auto t = track();
        const auto v = range - (y - t.getY()) / t.getHeight() * 2.0 * range;
        return juce::jlimit (-range, range, std::round (v * 10.0) / 10.0);
    }

    void paintFaders (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        g.setColour (colours::bg);
        g.fillRoundedRectangle (area.toFloat(), 8.0f);

        const auto t = track();
        const auto n = (int) hz.size();
        const auto w = (float) area.getWidth() / (float) n;
        const auto small = n > 20;

        // dB lines across the whole bank.
        const auto step = range <= 12.0 ? 3.0 : range <= 15.0 ? 5.0 : 6.0;
        g.setFont (juce::FontOptions (10.0f));

        for (auto v = -range; v <= range + 0.01; v += step)
        {
            g.setColour (std::abs (v) < 0.01 ? colours::muted.withAlpha (0.6f) : colours::line);
            g.fillRect (t.getX(), yFor (v), t.getWidth(), 1.0f);
        }

        for (int i = 0; i < n; ++i)
        {
            const auto available = bandAvailable ((size_t) i);
            const auto cx = area.getX() + w * ((float) i + 0.5f);
            const auto value = db[(size_t) i];

            // the slot
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRoundedRectangle (cx - 2.5f, t.getY(), 5.0f, t.getHeight(), 2.5f);

            // the bar from 0 to the value: orange up, blue down
            const auto y0 = yFor (0.0), y1 = yFor (value);
            g.setColour ((value >= 0.0 ? colours::bass : colours::accent).withAlpha (available ? 0.75f : 0.2f));
            g.fillRect (cx - 2.5f, std::min (y0, y1), 5.0f, std::abs (y1 - y0));

            // the knob
            const auto capW = juce::jlimit (10.0f, 30.0f, w * 0.7f);
            const juce::Rectangle<float> cap (cx - capW / 2.0f, y1 - 7.0f, capW, 14.0f);
            g.setColour (available ? juce::Colour (0xffc9ccd6) : juce::Colour (0xff4a4e5a));
            g.fillRoundedRectangle (cap, 3.0f);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillRect (cap.getX() + 2.0f, cap.getCentreY() - 0.5f, cap.getWidth() - 4.0f, 1.5f);

            // value above, frequency below
            g.setColour (available ? colours::ink : colours::muted.withAlpha (0.5f));
            g.setFont (juce::FontOptions (small ? 9.5f : 11.5f, juce::Font::bold));
            g.drawText (shortHz (hz[(size_t) i]), juce::Rectangle<float> (cx - w / 2.0f, t.getBottom() + 4.0f, w, 16.0f),
                        juce::Justification::centred, false);
            g.setFont (juce::FontOptions (small ? 9.0f : 11.0f));
            g.setColour (std::abs (value) < 0.05 ? colours::muted : value > 0 ? colours::bass : colours::accent);
            g.drawText (std::abs (value) < 0.05 ? juce::String ("0") : (value > 0 ? "+" : "") + juce::String (value, 1),
                        juce::Rectangle<float> (cx - w / 2.0f, (float) area.getY() + 3.0f, w, 16.0f), juce::Justification::centred, false);
        }
    }

    void paintCurve (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        g.setColour (colours::bg);
        g.fillRoundedRectangle (area.toFloat(), 8.0f);
        const auto a = area.toFloat().reduced (10.0f, 8.0f).withTrimmedRight (26.0f);
        const auto lo = 20.0, hi = std::min (20000.0, shared.rate() * 0.48);
        auto xFor = [&] (double f) { return a.getX() + a.getWidth() * (float) (std::log (f / lo) / std::log (hi / lo)); };
        auto yFor = [&] (double v) { return a.getCentreY() - a.getHeight() * 0.5f * (float) juce::jlimit (-1.2, 1.2, v / range); };

        g.setColour (colours::line);
        g.fillRect (a.getX(), yFor (0.0), a.getWidth(), 1.0f);
        g.setFont (juce::FontOptions (10.0f));

        for (auto v : { -range, range })
        {
            g.setColour (colours::muted);
            g.drawText ((v > 0 ? "+" : "") + juce::String ((int) v), juce::Rectangle<float> (a.getRight() + 3.0f, yFor (v) - 7.0f, 26.0f, 14.0f),
                        juce::Justification::centredLeft);
        }

        // spectrum: live while previewing (after the EQ), otherwise the selection's average
        auto spectrum = [&] (const std::vector<float>& level, double binHz, bool withEq)
        {
            juce::Path p;
            p.startNewSubPath (a.getX(), a.getBottom());
            const auto eq = params();

            for (size_t k = 1; k < level.size(); ++k)
            {
                const auto f = (double) k * binHz;

                if (f < lo || f > hi)
                    continue;

                const auto l = level[k] + (withEq ? fx::eqResponseDb (eq, f, shared.rate()) : 0.0);
                p.lineTo (xFor (f), a.getY() + a.getHeight() * (float) juce::jlimit (0.0, 1.0, -(l + 6.0) / 90.0));
            }

            p.lineTo (a.getRight(), a.getBottom());
            p.closeSubPath();
            return p;
        };

        if (shared.previewing)
        {
            g.setColour (colours::bass.withAlpha (0.18f));
            g.fillPath (spectrum (shared.post.db, shared.rate() / SpectrumMeter::size, false));
        }
        else if (! staticPre.empty())
        {
            g.setColour (colours::muted.withAlpha (0.18f));
            g.fillPath (spectrum (staticPre, shared.rate() / ((staticPre.size() - 1) * 2.0), true));
        }

        // the curve the sliders make (what is really applied), and the sliders as dots
        const auto eq = params();
        juce::Path curve;

        for (int x = 0; x <= (int) a.getWidth(); x += 2)
        {
            const auto f = lo * std::pow (hi / lo, x / (double) a.getWidth());
            const auto y = yFor (fx::eqResponseDb (eq, f, shared.rate()) - output.value());

            if (x == 0) curve.startNewSubPath (a.getX(), y);
            else        curve.lineTo (a.getX() + (float) x, y);
        }

        g.setColour (colours::bass);
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        for (size_t i = 0; i < hz.size(); ++i)
            if (bandAvailable (i) && hz[i] >= lo && hz[i] <= hi)
            {
                g.setColour (colours::ink);
                g.fillEllipse (xFor (hz[i]) - 3.0f, yFor (db[i]) - 3.0f, 6.0f, 6.0f);
            }
    }

    // Ready-made curves (any number of sliders), and the user's own.
    void showPresets()
    {
        const auto bell = [] (double f, double centre, double octaves) { return std::exp (-0.5 * std::pow (std::log2 (f / centre) / octaves, 2.0)); };
        struct Preset { const char* name; std::function<double (double)> curve; };
        const std::vector<Preset> builtIn {
            { "Flat", [] (double) { return 0.0; } },
            { "Bass boost", [] (double f) { return 6.0 / (1.0 + std::pow (f / 120.0, 2.0)); } },
            { "Remove rumble (low cut)", [] (double f) { return -12.0 / (1.0 + std::pow (f / 60.0, 4.0)); } },
            { "Voice: clearer", [bell] (double f) { return -3.0 * bell (f, 250.0, 0.6) + 4.0 * bell (f, 3000.0, 0.8) - 6.0 / (1.0 + std::pow (f / 70.0, 4.0)); } },
            { "Telephone", [] (double f) { return f < 300.0 ? -24.0 * std::min (1.0, std::log2 (300.0 / f)) : f > 3400.0 ? -24.0 * std::min (1.0, std::log2 (f / 3400.0)) : 0.0; } },
            { "Smile (loudness)", [bell] (double f) { return 6.0 * bell (f, 50.0, 1.2) + 5.0 * bell (f, 12000.0, 1.2) - 2.0 * bell (f, 1000.0, 1.0); } },
            { "Treble: more air", [] (double f) { return 5.0 / (1.0 + std::pow (9000.0 / f, 2.0)); } },
            { "Warm", [bell] (double f) { return 3.0 * bell (f, 150.0, 1.0) - 2.5 * bell (f, 4000.0, 1.0); } },
            { "Less harsh (2-5 kHz down)", [bell] (double f) { return -5.0 * bell (f, 3200.0, 0.7); } },
        };

        juce::PopupMenu menu;
        menu.addSectionHeader ("Ready-made");

        for (const auto& p : builtIn)
            menu.addItem (p.name, [this, curve = p.curve]
            {
                for (size_t i = 0; i < hz.size(); ++i)
                    db[i] = juce::jlimit (-range, range, std::round (curve (hz[i]) * 10.0) / 10.0);

                changed();
            });

        const auto mine = settings::get ("graphicEq.presets");
        juce::PopupMenu load, remove;

        if (const auto* list = mine.getArray())
            for (int i = 0; i < list->size(); ++i)
            {
                const auto preset = (*list)[i];
                load.addItem (preset["name"].toString(), [this, preset] { loadUserPreset (preset); });
                remove.addItem (preset["name"].toString(), [i] { removeUserPreset (i); });
            }

        menu.addSeparator();
        menu.addSectionHeader ("Mine");

        if (load.getNumItems() > 0)
        {
            menu.addSubMenu ("Load", load);
            menu.addSubMenu ("Delete", remove);
        }

        menu.addItem ("Save these sliders as...", [this] { saveUserPreset(); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetsButton).withParentComponent (getTopLevelComponent()));
    }

    void loadUserPreset (const juce::var& preset)
    {
        const auto* fs = preset["hz"].getArray();
        const auto* vs = preset["db"].getArray();

        if (fs == nullptr || vs == nullptr || fs->size() != vs->size() || fs->size() < 2)
            return;

        std::vector<double> f, v;

        for (int i = 0; i < fs->size(); ++i)
        {
            f.push_back ((*fs)[i]);
            v.push_back ((*vs)[i]);
        }

        // same sliders: exactly; otherwise its shape
        for (size_t i = 0; i < hz.size(); ++i)
            db[i] = juce::jlimit (-range, range, curveAt (f, v, hz[i]));

        changed();
    }

    static void removeUserPreset (int index)
    {
        const auto mine = settings::get ("graphicEq.presets");
        juce::Array<juce::var> list;

        if (const auto* old = mine.getArray())
            list = *old;

        list.remove (index);
        settings::set ("graphicEq.presets", list);
    }

    void saveUserPreset()
    {
        auto* box = new juce::AlertWindow ("Save the sliders", "A name for this curve:", juce::MessageBoxIconType::NoIcon);
        box->addTextEditor ("name", "My curve");
        box->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        const juce::Component::SafePointer<GraphicEqEditor> safeThis (this);

        box->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, box] (int result)
        {
            auto* self = safeThis.getComponent();
            const auto name = box->getTextEditorContents ("name").trim();

            if (result != 1 || self == nullptr || name.isEmpty())
                return;

            juce::Array<juce::var> fs, vs;

            for (size_t i = 0; i < self->hz.size(); ++i)
            {
                fs.add (self->hz[i]);
                vs.add (self->db[i]);
            }

            juce::DynamicObject::Ptr preset = new juce::DynamicObject();
            preset->setProperty ("name", name);
            preset->setProperty ("hz", fs);
            preset->setProperty ("db", vs);

            const auto mine = settings::get ("graphicEq.presets");
            juce::Array<juce::var> list;

            if (const auto* old = mine.getArray())
                list = *old;

            list.add (juce::var (preset.get()));
            settings::set ("graphicEq.presets", list);
        }), true);
    }

    // The selection's average spectrum (dB per bin), for the curve view.
    static std::vector<float> averageSpectrum (const juce::AudioBuffer<float>& audio)
    {
        constexpr int order = 12, size = 1 << order;

        if (audio.getNumSamples() < size)
            return {};

        juce::dsp::FFT fft (order);
        std::vector<float> work (2 * size);
        std::vector<double> sum (size / 2 + 1, 0.0);
        const auto frames = std::min (500, (audio.getNumSamples() - size) / (size / 2) + 1);
        const auto hop = std::max (1, (audio.getNumSamples() - size) / std::max (1, frames - 1));

        for (int f = 0; f < frames; ++f)
        {
            std::fill (work.begin(), work.end(), 0.0f);

            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                for (int i = 0; i < size; ++i)
                    work[(size_t) i] += audio.getSample (ch, f * hop + i) / (float) audio.getNumChannels()
                                      * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1)));

            fft.performFrequencyOnlyForwardTransform (work.data(), true);

            for (size_t k = 0; k < sum.size(); ++k)
                sum[k] += (double) work[k] * work[k];
        }

        std::vector<float> level (sum.size());

        for (size_t k = 0; k < sum.size(); ++k)
            level[k] = (float) juce::Decibels::gainToDecibels (std::sqrt (sum[k] / frames) * 4.0 / size, -120.0);

        return level;
    }

    std::vector<double> hz, db;
    double range = 12.0;
    int lastBand = -1;
    double lastValue = 0.0;
    std::vector<float> staticPre;
    juce::Rectangle<int> curveArea, faderArea;
    juce::OwnedArray<juce::TextButton> countButtons, rangeButtons;
    juce::TextButton flatButton { "Flat" }, presetsButton { "Presets" };
    juce::Label countLabel, rangeLabel, hint;
    LabelledSlider output { "Output", -24.0, 24.0, 0.1, " dB", 0.0, [this] { changed(); } };
};

//==============================================================================
// Compressor: the input -> output curve with the live level on it, and meters.
class CompressorEditor final : public Editor
{
public:
    explicit CompressorEditor (Shared& s) : Editor (s)
    {
        for (auto* c : { &threshold, &ratio, &knee, &attack, &release, &makeup })
            addAndMakeVisible (c);

        styleToggle (autoMakeup);
        styleToggle (rms);
        autoMakeup.onClick = [this] { push(); repaint(); };
        rms.onClick = [this] { push(); };
        addAndMakeVisible (autoMakeup);
        addAndMakeVisible (rms);
        push();
    }

    fx::CompressorParams params() const
    {
        fx::CompressorParams p;
        p.thresholdDb = threshold.value();
        p.ratio = ratio.value();
        p.kneeDb = knee.value();
        p.attackMs = attack.value();
        p.releaseMs = release.value();
        p.makeupDb = makeup.value();
        p.autoMakeup = autoMakeup.getToggleState();
        p.rms = rms.getToggleState();
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeCompressor();
        fx::setParams (*p, params());
        return p;
    }

    void tick() override
    {
        const auto decay = [] (float& shown, float now, float fall) { shown = now > shown ? now : std::max (now, shown - fall); };

        if (shared.previewing)
        {
            decay (inDb, shared.processor->inputLevelDb.load(), 1.2f);
            decay (outDb, shared.processor->outputLevelDb.load(), 1.2f);
            grDb = std::min (0.0f, shared.processor->gainReductionDb.load());
        }
        else
        {
            inDb = outDb = -100.0f;
            grDb = 0.0f;
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // Transfer curve.
        const auto a = graphArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);
        const auto inner = a.reduced (28.0f, 16.0f);
        auto X = [&] (double db) { return inner.getX() + inner.getWidth() * (float) ((db + 60.0) / 60.0); };
        auto Y = [&] (double db) { return inner.getBottom() - inner.getHeight() * (float) ((juce::jlimit (-60.0, 6.0, db) + 60.0) / 60.0); };

        g.setFont (juce::FontOptions (10.0f));

        for (int db = -60; db <= 0; db += 12)
        {
            g.setColour (colours::line);
            g.fillRect (X (db), inner.getY(), 1.0f, inner.getHeight());
            g.fillRect (inner.getX(), Y (db), inner.getWidth(), 1.0f);
            g.setColour (colours::muted);
            g.drawText (juce::String (db), juce::Rectangle<float> (X (db) - 15.0f, inner.getBottom() + 2.0f, 30.0f, 12.0f), juce::Justification::centred);
            g.drawText (juce::String (db), juce::Rectangle<float> (inner.getX() - 28.0f, Y (db) - 6.0f, 24.0f, 12.0f), juce::Justification::centredRight);
        }

        const auto p = params();
        const auto make = p.makeupDb + (p.autoMakeup ? fx::autoMakeupDb (p) : 0.0);
        juce::Path curve;

        for (double db = -60.0; db <= 0.0; db += 0.5)
        {
            const auto out = fx::compressorCurveDb (p, db) + make;

            if (db <= -60.0)
                curve.startNewSubPath (X (db), Y (out));
            else
                curve.lineTo (X (db), Y (out));
        }

        g.setColour (colours::muted.withAlpha (0.5f));
        g.drawLine (X (-60.0), Y (-60.0), X (0.0), Y (0.0), 1.0f);
        g.setColour (colours::accent);
        g.strokePath (curve, juce::PathStrokeType (2.4f));
        g.setColour (colours::warn.withAlpha (0.6f));
        g.fillRect (X (p.thresholdDb), inner.getY(), 1.5f, inner.getHeight());

        if (inDb > -60.0f)
        {
            const auto x = X (inDb), y = Y (fx::compressorCurveDb (p, inDb) + make);
            g.setColour (colours::ok);
            g.fillEllipse (x - 6.0f, y - 6.0f, 12.0f, 12.0f);
        }

        g.setColour (colours::muted);
        g.drawText ("input level (dB)  ->  output level", juce::Rectangle<float> (inner.getX(), a.getY() + 2.0f, inner.getWidth(), 12.0f),
                    juce::Justification::centred);

        // Meters: in, gain reduction (downwards), out.
        auto m = meterArea.toFloat();
        const auto w = m.getWidth() / 3.0f;
        meter (g, m.removeFromLeft (w).reduced (6.0f, 0.0f), "In", inDb, false);
        meter (g, m.removeFromLeft (w).reduced (6.0f, 0.0f), "GR", grDb, true);
        meter (g, m.reduced (6.0f, 0.0f), "Out", outDb, false);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        meterArea = r.removeFromRight (150);
        r.removeFromRight (12);
        graphArea = r.removeFromLeft (r.getHeight() + 40);
        r.removeFromLeft (16);

        for (auto* c : { &threshold, &ratio, &knee, &attack, &release, &makeup })
        {
            c->setBounds (r.removeFromTop (46));
            r.removeFromTop (4);
        }

        auto row = r.removeFromTop (30);
        autoMakeup.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2));
        rms.setBounds (row.reduced (2));
    }

private:
    static void meter (juce::Graphics& g, juce::Rectangle<float> r, const char* name, float db, bool reduction)
    {
        auto text = r.removeFromBottom (18.0f);
        g.setColour (colours::bg);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (name, text, juce::Justification::centred);

        if (reduction)
        {
            const auto h = r.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 24.0f);
            g.setColour (colours::warn);
            g.fillRect (r.getX() + 2.0f, r.getY(), r.getWidth() - 4.0f, h);
            g.setColour (colours::ink);
            g.drawText (juce::String (db, 1), r.withHeight (16.0f).translated (0.0f, r.getHeight() - 18.0f), juce::Justification::centred);
            return;
        }

        const auto h = r.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (db > -3.0f ? colours::bad : db > -12.0f ? colours::warn : colours::ok);
        g.fillRect (r.getX() + 2.0f, r.getBottom() - h, r.getWidth() - 4.0f, h);
    }

    LabelledSlider threshold { "Threshold", -60.0, 0.0, 0.1, " dB", -18.0, [this] { push(); } };
    LabelledSlider ratio { "Ratio", 1.0, 20.0, 0.1, " : 1", 4.0, [this] { push(); }, 4.0 };
    LabelledSlider knee { "Knee", 0.0, 24.0, 0.1, " dB", 6.0, [this] { push(); } };
    LabelledSlider attack { "Attack", 0.1, 200.0, 0.1, " ms", 10.0, [this] { push(); }, 15.0 };
    LabelledSlider release { "Release", 5.0, 2000.0, 1.0, " ms", 120.0, [this] { push(); }, 150.0 };
    LabelledSlider makeup { "Makeup gain", -12.0, 24.0, 0.1, " dB", 0.0, [this] { push(); } };
    juce::TextButton autoMakeup { "Auto makeup" }, rms { "RMS (smoother)" };
    juce::Rectangle<int> graphArea, meterArea;
    float inDb = -100.0f, outDb = -100.0f, grDb = 0.0f;
};

//==============================================================================
// Delay: time in ms or as a note value at the song's tempo, with the echoes drawn.
class DelayEditor final : public Editor
{
public:
    explicit DelayEditor (Shared& s) : Editor (s)
    {
        // The tempo of the audio itself (there is no other tempo to follow),
        // for the note values; shown above so it's clear where it came from.
        if (const auto tempo = detectTempo (*shared.audio, { 0, shared.audio->getNumSamples() }, shared.rate()))
        {
            const auto bpm = juce::String (tempo->bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
            bpmField.setText (bpm, false);
            bpmLabel.setText ("Song tempo (BPM): " + bpm + " detected in this audio", juce::dontSendNotification);
        }
        else
        {
            bpmField.setText ("120", false);
            bpmLabel.setText ("Song tempo (BPM): none detected, type it in", juce::dontSendNotification);
        }

        for (auto* c : { &time, &feedback, &mix, &lowCut, &highCut })
            addAndMakeVisible (c);

        styleToggle (sync);
        styleToggle (pingPong);
        sync.onClick = [this] { push(); };
        pingPong.onClick = [this] { push(); };

        for (size_t i = 0; i < fx::delayNotes().size(); ++i)
            noteBox.addItem (fx::delayNotes()[i].name, (int) i + 1);

        noteBox.setSelectedId (4, juce::dontSendNotification);
        noteBox.onChange = [this] { push(); };
        bpmField.setInputRestrictions (6, "0123456789.,");
        bpmField.onTextChange = [this] { push(); };
        styleLabel (bpmLabel, 12.0f, true, colours::muted);

        for (auto* c : { (juce::Component*) &sync, (juce::Component*) &pingPong, (juce::Component*) &noteBox,
                         (juce::Component*) &bpmField, (juce::Component*) &bpmLabel })
            addAndMakeVisible (c);

        push();
    }

    fx::DelayParams params() const
    {
        fx::DelayParams p;
        p.timeMs = time.value();
        p.sync = sync.getToggleState();
        p.bpm = juce::jlimit (20.0, 400.0, bpmField.getText().replaceCharacter (',', '.').getDoubleValue());
        p.note = noteBox.getSelectedId() - 1;
        p.feedback = feedback.value() / 100.0;
        p.mix = mix.value() / 100.0;
        p.pingPong = pingPong.getToggleState();
        p.lowCutHz = lowCut.value();
        p.highCutHz = highCut.value();
        return p;
    }

    void push() override
    {
        const auto p = params();
        fx::setParams (*shared.processor, p);
        time.setEnabled (! p.sync);
        noteBox.setEnabled (p.sync);
        bpmField.setEnabled (p.sync);
        repaint();
    }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeDelay();
        fx::setParams (*p, params());
        return p;
    }

    bool hasTail() const override         { return true; }
    double tailSeconds() const override   { return fx::suggestedTailSeconds (params()); }

    void paint (juce::Graphics& g) override
    {
        const auto a = drawArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);

        const auto p = params();
        const auto t = p.effectiveTimeMs();
        const auto span = std::max (t * 8.0, 1000.0);
        const auto inner = a.reduced (16.0f, 20.0f);
        auto X = [&] (double ms) { return inner.getX() + inner.getWidth() * (float) (ms / span); };

        g.setColour (colours::ink);
        g.fillRect (X (0.0), inner.getY(), 4.0f, inner.getHeight());

        auto level = p.mix;

        for (int k = 1; X (k * t) < inner.getRight() && level > 0.002; ++k)
        {
            const auto h = inner.getHeight() * (float) juce::jlimit (0.0, 1.0, level);
            const auto right = p.pingPong ? k % 2 == 0 : false;
            g.setColour ((right ? colours::bass : colours::accent).withAlpha (0.9f));
            g.fillRect (X (k * t) - 2.0f, inner.getBottom() - h, 4.0f, h);
            level *= p.feedback;
        }

        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("echo every " + juce::String (t, 1) + " ms" + (p.pingPong ? "  (ping-pong: blue left, orange right)" : ""),
                    juce::Rectangle<float> (inner.getX(), a.getY() + 2.0f, inner.getWidth(), 16.0f), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        drawArea = r.removeFromTop (150);
        r.removeFromTop (12);

        auto row = r.removeFromTop (46);
        time.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (6, 0));
        auto syncArea = row.reduced (6, 0);
        bpmLabel.setBounds (syncArea.removeFromTop (16));
        auto line = syncArea.removeFromTop (28);
        sync.setBounds (line.removeFromLeft (172).reduced (2));
        noteBox.setBounds (line.removeFromLeft (118).reduced (2));
        bpmField.setBounds (line.removeFromLeft (64).reduced (2));
        r.removeFromTop (8);

        row = r.removeFromTop (46);
        feedback.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (6, 0));
        mix.setBounds (row.reduced (6, 0));
        r.removeFromTop (8);
        row = r.removeFromTop (46);
        lowCut.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (6, 0));
        highCut.setBounds (row.reduced (6, 0));
        r.removeFromTop (8);
        pingPong.setBounds (r.removeFromTop (30).removeFromLeft (200).reduced (6, 2));
    }

private:
    LabelledSlider time { "Time", 1.0, 2000.0, 0.1, " ms", 375.0, [this] { push(); }, 300.0 };
    LabelledSlider feedback { "Feedback (how many echoes)", 0.0, 95.0, 1.0, " %", 35.0, [this] { push(); } };
    LabelledSlider mix { "Echo level", 0.0, 100.0, 1.0, " %", 30.0, [this] { push(); } };
    LabelledSlider lowCut { "Low cut on the echoes", 20.0, 2000.0, 1.0, " Hz", 80.0, [this] { push(); }, 200.0 };
    LabelledSlider highCut { "High cut on the echoes", 1000.0, 20000.0, 10.0, " Hz", 9000.0, [this] { push(); }, 6000.0 };
    juce::TextButton sync { "Follow the song's tempo" }, pingPong { "Ping-pong (L / R)" };
    juce::ComboBox noteBox;
    juce::TextEditor bpmField;
    juce::Label bpmLabel;
    juce::Rectangle<int> drawArea;
};

//==============================================================================
class ReverbEditor final : public Editor
{
public:
    explicit ReverbEditor (Shared& s) : Editor (s)
    {
        for (auto* c : { &room, &damping, &width, &preDelay, &lowCut, &highCut, &wet, &dry })
            addAndMakeVisible (c);

        styleToggle (freeze);
        freeze.onClick = [this] { push(); };
        addAndMakeVisible (freeze);
        push();
    }

    fx::ReverbParams params() const
    {
        fx::ReverbParams p;
        p.roomSize = room.value() / 100.0;
        p.damping = damping.value() / 100.0;
        p.width = width.value() / 100.0;
        p.preDelayMs = preDelay.value();
        p.lowCutHz = lowCut.value();
        p.highCutHz = highCut.value();
        p.wet = wet.value() / 100.0;
        p.dry = dry.value() / 100.0;
        p.freeze = freeze.getToggleState();
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeReverb();
        fx::setParams (*p, params());
        return p;
    }

    bool hasTail() const override         { return true; }
    double tailSeconds() const override   { return fx::suggestedTailSeconds (params()); }

    void paint (juce::Graphics& g) override
    {
        const auto a = drawArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);

        const auto p = params();
        const auto tail = fx::suggestedTailSeconds (p);
        const auto inner = a.reduced (16.0f, 20.0f);
        const auto span = std::max (tail * 1.2, 1.0);
        auto X = [&] (double s) { return inner.getX() + inner.getWidth() * (float) (s / span); };

        g.setColour (colours::ink);
        g.fillRect (X (0.0), inner.getBottom() - inner.getHeight() * (float) std::min (1.0, p.dry), 4.0f,
                    inner.getHeight() * (float) std::min (1.0, p.dry));

        juce::Path decay;
        const auto start = p.preDelayMs * 0.001;
        decay.startNewSubPath (X (start), inner.getBottom());

        for (double t = start; t <= span; t += span / 200.0)
        {
            const auto env = p.freeze ? p.wet : p.wet * std::pow (0.001, (t - start) / std::max (0.05, tail - start));
            decay.lineTo (X (t), inner.getBottom() - inner.getHeight() * (float) juce::jlimit (0.0, 1.0, env));
        }

        decay.lineTo (X (span), inner.getBottom());
        decay.closeSubPath();
        g.setColour (colours::accent.withAlpha (0.35f));
        g.fillPath (decay);
        g.setColour (colours::accent);
        g.strokePath (decay, juce::PathStrokeType (1.5f));

        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (p.freeze ? "frozen: the tail holds" : "dies away in about " + juce::String (tail, 1) + " s",
                    juce::Rectangle<float> (inner.getX(), a.getY() + 2.0f, inner.getWidth(), 16.0f), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        drawArea = r.removeFromTop (130);
        r.removeFromTop (12);

        for (auto pair : { std::pair { &room, &damping }, std::pair { &width, &preDelay }, std::pair { &lowCut, &highCut },
                           std::pair { &wet, &dry } })
        {
            auto row = r.removeFromTop (46);
            pair.first->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (6, 0));
            pair.second->setBounds (row.reduced (6, 0));
            r.removeFromTop (6);
        }

        freeze.setBounds (r.removeFromTop (30).removeFromLeft (200).reduced (6, 2));
    }

private:
    LabelledSlider room { "Room size", 0.0, 100.0, 1.0, " %", 60.0, [this] { push(); } };
    LabelledSlider damping { "Damping (darker)", 0.0, 100.0, 1.0, " %", 45.0, [this] { push(); } };
    LabelledSlider width { "Stereo width", 0.0, 100.0, 1.0, " %", 100.0, [this] { push(); } };
    LabelledSlider preDelay { "Pre-delay", 0.0, 200.0, 0.1, " ms", 15.0, [this] { push(); } };
    LabelledSlider lowCut { "Low cut on the reverb", 20.0, 1000.0, 1.0, " Hz", 150.0, [this] { push(); }, 150.0 };
    LabelledSlider highCut { "High cut on the reverb", 1000.0, 20000.0, 10.0, " Hz", 9000.0, [this] { push(); }, 6000.0 };
    LabelledSlider wet { "Reverb level (wet)", 0.0, 100.0, 1.0, " %", 30.0, [this] { push(); } };
    LabelledSlider dry { "Original level (dry)", 0.0, 100.0, 1.0, " %", 100.0, [this] { push(); } };
    juce::TextButton freeze { "Freeze (endless)" };
    juce::Rectangle<int> drawArea;
};

//==============================================================================
// MP3 Restore: the average spectrum with the encoder's cut marked, and what
// grows back above it; live spectra while previewing.
class Mp3RestoreEditor final : public Editor
{
public:
    explicit Mp3RestoreEditor (Shared& s) : Editor (s)
    {
        settings::dontRemember (cutoff);   // measured in each file
        cutoff.slider.setRange (4000.0, std::floor (shared.rate() * 0.49), 10.0);
        cutoff.slider.setSkewFactor (1.0);
        detect();

        for (auto* c : { &cutoff, &highs, &deswirl, &warmth, &punch, &output })
            addAndMakeVisible (c);

        for (auto* b : { &harmonicsMode, &copyMode })
        {
            styleToggle (*b);
            b->setRadioGroupId (501);
            b->onClick = [this] { push(); };
            addAndMakeVisible (b);
        }

        harmonicsMode.setToggleState (true, juce::dontSendNotification);
        detectButton.onClick = [this] { detect(); push(); };
        addAndMakeVisible (detectButton);
        push();
    }

    fx::Mp3RestoreParams params() const
    {
        fx::Mp3RestoreParams p;
        p.cutoffHz = cutoff.value();
        p.highs = highs.value() / 100.0;
        p.spectralCopy = copyMode.getToggleState();
        p.deswirl = deswirl.value() / 100.0;
        p.warmth = warmth.value() / 100.0;
        p.punch = punch.value() / 100.0;
        p.outputDb = output.value();
        return p;
    }

    void push() override
    {
        fx::setParams (*shared.processor, params());
        repaint();
    }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeMp3Restore();
        fx::setParams (*p, params());
        return p;
    }

    void tick() override
    {
        if (shared.previewing)
            repaint (drawArea);
    }

    void paint (juce::Graphics& g) override
    {
        const auto a = drawArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);

        const auto plot = a.reduced (40.0f, 22.0f).withTrimmedRight (-10.0f);
        const auto nyquist = shared.rate() * 0.5;
        auto X = [&] (double hz) { return plot.getX() + plot.getWidth() * (float) (hz / nyquist); };
        auto Y = [&] (float db) { return plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 110.0f) / 100.0f); };

        // Grid: every 2 kHz, every 20 dB.
        g.setFont (juce::FontOptions (11.0f));

        for (double hz = 0.0; hz <= nyquist; hz += 2000.0)
        {
            g.setColour (colours::line);
            g.fillRect (X (hz), plot.getY(), 1.0f, plot.getHeight());
            g.setColour (colours::muted);
            g.drawText (juce::String ((int) (hz / 1000.0)) + "k", juce::Rectangle<float> (X (hz) - 15.0f, plot.getBottom() + 2.0f, 30.0f, 14.0f),
                        juce::Justification::centred);
        }

        for (float db = -100.0f; db <= -10.0f; db += 20.0f)
        {
            g.setColour (colours::line);
            g.fillRect (plot.getX(), Y (db), plot.getWidth(), 1.0f);
            g.setColour (colours::muted);
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (a.getX() + 4.0f, Y (db) - 7.0f, 32.0f, 14.0f), juce::Justification::centredRight);
        }

        auto curve = [&] (const std::vector<float>& db, bool fill, juce::Colour colour)
        {
            if (db.size() < 2)
                return;

            const auto hzPerBin = nyquist / (double) (db.size() - 1);
            juce::Path path;
            path.startNewSubPath (X (0.0), Y (db[0]));

            for (size_t k = 1; k < db.size(); ++k)
                path.lineTo (X ((double) k * hzPerBin), Y (db[k]));

            if (fill)
            {
                auto area = path;
                area.lineTo (X (nyquist), plot.getBottom());
                area.lineTo (X (0.0), plot.getBottom());
                area.closeSubPath();
                g.setColour (colour.withAlpha (0.18f));
                g.fillPath (area);
            }

            g.setColour (colour);
            g.strokePath (path, juce::PathStrokeType (1.2f));
        };

        const auto p = params();

        // What grows back: the region above the cut.
        if (p.highs > 0.0 && detected.has_value())
        {
            g.setColour (colours::ok.withAlpha (0.12f + 0.18f * (float) p.highs));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (X (p.cutoffHz), plot.getY(), X (std::min (20000.0, nyquist)), plot.getBottom()));
        }

        if (shared.previewing)
        {
            curve (shared.pre.db, false, colours::muted);
            curve (shared.post.db, false, colours::warn);
        }
        else
        {
            curve (spectrum, true, colours::muted);
        }

        g.setColour (colours::warn);
        g.fillRect (X (p.cutoffHz) - 1.0f, plot.getY(), 2.0f, plot.getHeight());

        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (headline, juce::Rectangle<float> (plot.getX(), a.getY() + 3.0f, plot.getWidth(), 18.0f), juce::Justification::centredLeft);
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (shared.previewing ? "grey: before   orange: after   green: grown back" : "grey: average spectrum   orange line: the cut   green: grown back",
                    juce::Rectangle<float> (plot.getX(), a.getY() + 3.0f, plot.getWidth(), 18.0f), juce::Justification::centredRight);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        drawArea = r.removeFromTop (220);
        r.removeFromTop (12);

        auto row = r.removeFromTop (46);
        cutoff.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (6, 0));
        auto modes = row.reduced (6, 0);
        modes.removeFromTop (16);
        modes = modes.removeFromTop (28);
        detectButton.setBounds (modes.removeFromRight (110).reduced (2));
        harmonicsMode.setBounds (modes.removeFromLeft (modes.getWidth() / 2).reduced (2));
        copyMode.setBounds (modes.reduced (2));
        r.removeFromTop (6);

        for (auto pair : { std::pair { &highs, &deswirl }, std::pair { &warmth, &punch } })
        {
            row = r.removeFromTop (46);
            pair.first->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (6, 0));
            pair.second->setBounds (row.reduced (6, 0));
            r.removeFromTop (6);
        }

        row = r.removeFromTop (46);
        output.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (6, 0));
    }

private:
    void detect()
    {
        detected = fx::detectCutoff (*shared.audio, shared.rate(), &spectrum);

        if (detected.has_value())
        {
            cutoff.slider.setValue (*detected, juce::dontSendNotification);
            headline = "Cut at " + formatHz (*detected) + " (" + fx::likelyBitrate (*detected) + "): the top end will be grown back";
        }
        else
        {
            cutoff.slider.setValue (cutoff.slider.getMaximum(), juce::dontSendNotification);
            headline = "No cut found: the top end looks complete (de-swirl, warmth and punch still apply)";
        }
    }

    LabelledSlider cutoff { "Cut (where the encoder stopped)", 4000.0, 22000.0, 10.0, " Hz", 16000.0, [this] { push(); } };
    LabelledSlider highs { "Grow back the top end", 0.0, 100.0, 1.0, " %", 60.0, [this] { push(); } };
    LabelledSlider deswirl { "De-swirl (birdies, underwater sound)", 0.0, 100.0, 1.0, " %", 40.0, [this] { push(); } };
    LabelledSlider warmth { "Warmth (tape-like saturation)", 0.0, 100.0, 1.0, " %", 15.0, [this] { push(); } };
    LabelledSlider punch { "Punch (sharper attacks)", 0.0, 100.0, 1.0, " %", 0.0, [this] { push(); } };
    LabelledSlider output { "Output", -12.0, 12.0, 0.1, " dB", 0.0, [this] { push(); } };
    juce::TextButton harmonicsMode { "Harmonics (saturation)" }, copyMode { "Spectral copy" }, detectButton { "Detect again" };
    std::optional<double> detected;
    std::vector<float> spectrum;
    juce::String headline;
    juce::Rectangle<int> drawArea;
};

//==============================================================================
//==============================================================================
// Package C editors: limiter, noise gate, chorus / flanger / vibrato, distortion.

// A vertical level meter (dB) or, with `reduction`, gain reduction drawn down from the top.
void levelMeter (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& name, float db, bool reduction,
                 float markDb = 1000.0f)
{
    auto text = r.removeFromBottom (18.0f);
    g.setColour (colours::bg);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (colours::muted);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (name, text, juce::Justification::centred);

    if (reduction)
    {
        const auto h = r.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 24.0f);
        g.setColour (colours::warn);
        g.fillRect (r.getX() + 2.0f, r.getY(), r.getWidth() - 4.0f, h);
        g.setColour (colours::ink);
        g.drawText (juce::String (db, 1), r.withHeight (16.0f).translated (0.0f, r.getHeight() - 18.0f), juce::Justification::centred);
        return;
    }

    const auto h = r.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour (db > -3.0f ? colours::bad : db > -12.0f ? colours::warn : colours::ok);
    g.fillRect (r.getX() + 2.0f, r.getBottom() - h, r.getWidth() - 4.0f, h);

    if (markDb < 100.0f)
    {
        const auto y = r.getBottom() - r.getHeight() * juce::jlimit (0.0f, 1.0f, (markDb + 60.0f) / 60.0f);
        g.setColour (colours::ink);
        g.fillRect (r.getX(), y - 1.0f, r.getWidth(), 2.0f);
    }
}

// Falls back slowly, so the meters can be read.
void meterFall (float& shown, float now, float fall)   { shown = now > shown ? now : std::max (now, shown - fall); }

// The In / reduction / Out meters fed from the processor while previewing.
struct DynamicsMeters
{
    float in = -100.0f, out = -100.0f, reduction = 0.0f;

    void update (const Shared& shared)
    {
        if (shared.previewing)
        {
            meterFall (in, shared.processor->inputLevelDb.load(), 1.2f);
            meterFall (out, shared.processor->outputLevelDb.load(), 1.2f);
            const auto now = std::min (0.0f, shared.processor->gainReductionDb.load());
            reduction = now < reduction ? now : std::min (0.0f, reduction + 0.6f);
        }
        else
        {
            in = out = -100.0f;
            reduction = 0.0f;
        }
    }

    void paint (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& reductionName, float markDb = 1000.0f) const
    {
        const auto w = area.getWidth() / 3.0f;
        levelMeter (g, area.removeFromLeft (w).reduced (6.0f, 0.0f), "In", in, false, markDb);
        levelMeter (g, area.removeFromLeft (w).reduced (6.0f, 0.0f), reductionName, reduction, true);
        levelMeter (g, area.reduced (6.0f, 0.0f), "Out", out, false, markDb);
    }
};

//==============================================================================
// Limiter: louder without going over the ceiling; can find the gain for a
// loudness target (what Normalize can't reach without limiting).
class LimiterEditor final : public Editor
{
public:
    explicit LimiterEditor (Shared& s) : Editor (s)
    {
        for (auto* c : { &input, &ceiling, &release })
            addAndMakeVisible (c);

        styleToggle (truePeak);
        truePeak.setToggleState (true, juce::dontSendNotification);
        truePeak.setTooltip ("Also catch the peaks between samples (what streaming services check, in dBTP)");
        truePeak.onClick = [this] { push(); };
        addAndMakeVisible (truePeak);

        styleLabel (aimLabel, 12.0f, true, colours::muted);
        aimLabel.setText ("Find the gain for a loudness", juce::dontSendNotification);
        addAndMakeVisible (aimLabel);
        aim.addItemList ({ "-14 LUFS  Spotify, YouTube", "-16 LUFS  Apple Music, podcast", "-23 LUFS  EBU R128 (TV)", "-9 LUFS  loud master" }, 1);
        aim.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (aim);
        find.setTooltip ("Tries a few input gains on this audio and keeps the one that reaches the loudness");
        find.onClick = [this] { findGain(); };
        addAndMakeVisible (find);
        styleLabel (result, 12.0f, false, colours::muted);
        addAndMakeVisible (result);
        push();
    }

    fx::LimiterParams params() const
    {
        fx::LimiterParams p;
        p.inputDb = input.value();
        p.ceilingDb = ceiling.value();
        p.releaseMs = release.value();
        p.truePeak = truePeak.getToggleState();
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeLimiter();
        fx::setParams (*p, params());
        return p;
    }

    void tick() override   { meters.update (shared); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto a = infoArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);
        a = a.reduced (16.0f, 12.0f);

        const auto p = params();
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("Turns the audio up by " + juce::String (p.inputDb, 1) + " dB and holds every peak under "
                          + juce::String (p.ceilingDb, 1) + (p.truePeak ? " dBTP" : " dBFS")
                          + ".\nThe peaks are caught 2 ms ahead, so nothing gets through; the more gain,"
                          " the more the peaks are squashed (watch GR).",
                          a.removeFromTop (60.0f).toNearestInt(), juce::Justification::topLeft, 4);

        meters.paint (g, meterArea.toFloat(), "GR", (float) p.ceilingDb);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        meterArea = r.removeFromRight (150);
        r.removeFromRight (16);
        infoArea = r.removeFromTop (90);
        r.removeFromTop (12);

        for (auto* c : { &input, &ceiling, &release })
        {
            c->setBounds (r.removeFromTop (46));
            r.removeFromTop (4);
        }

        truePeak.setBounds (r.removeFromTop (30).removeFromLeft (240).reduced (2));
        r.removeFromTop (12);
        aimLabel.setBounds (r.removeFromTop (18));
        auto row = r.removeFromTop (30);
        aim.setBounds (row.removeFromLeft (260).reduced (2));
        find.setBounds (row.removeFromLeft (110).reduced (2));
        result.setBounds (row.reduced (8, 0));
    }

private:
    void findGain()
    {
        if (shared.host.runJob == nullptr)
            return;

        const double targets[] { -14.0, -16.0, -23.0, -9.0 };
        const auto target = targets[juce::jlimit (0, 3, aim.getSelectedItemIndex())];
        auto audio = shared.audio;
        const auto rate = shared.rate();
        const auto p = params();
        auto gain = std::make_shared<std::optional<double>>();
        juce::Component::SafePointer<LimiterEditor> safeThis (this);
        result.setText ("Searching...", juce::dontSendNotification);

        shared.host.runJob ("Finding the gain for " + juce::String (target, 0) + " LUFS...",
                            [audio, rate, p, target, gain] (std::function<bool (float)> progress)
                            {
                                *gain = fx::findLimiterGain (*audio, rate, p, target, progress);
                                return true;
                            },
                            [safeThis, gain, target] (bool finished)
                            {
                                auto* self = safeThis.getComponent();

                                if (self == nullptr)
                                    return;

                                if (! finished)
                                    self->result.setText ("Cancelled.", juce::dontSendNotification);
                                else if (! gain->has_value())
                                    self->result.setText ("Can't reach it (silent, or needs over +30 dB).", juce::dontSendNotification);
                                else
                                {
                                    self->input.slider.setValue (juce::jlimit (-12.0, 30.0, **gain));
                                    self->result.setText ("Input " + juce::String (**gain, 1) + " dB gives " + juce::String (target, 0)
                                                          + " LUFS.", juce::dontSendNotification);
                                }
                            });
    }

    LabelledSlider input { "Input gain (louder)", -12.0, 30.0, 0.1, " dB", 0.0, [this] { push(); } };
    LabelledSlider ceiling { "Ceiling", -12.0, 0.0, 0.1, " dB", -1.0, [this] { push(); } };
    LabelledSlider release { "Release", 5.0, 1000.0, 1.0, " ms", 80.0, [this] { push(); }, 100.0 };
    juce::TextButton truePeak { "True peak (4x oversampled)" }, find { "Find gain" };
    juce::Label aimLabel, result;
    juce::ComboBox aim;
    juce::Rectangle<int> infoArea, meterArea;
    DynamicsMeters meters;
};

//==============================================================================
// Noise Gate: turns the quiet parts down (hiss, hum, room noise between phrases).
class GateEditor final : public Editor
{
public:
    explicit GateEditor (Shared& s) : Editor (s)
    {
        for (auto* c : { &threshold, &range, &attack, &hold, &release })
            addAndMakeVisible (c);

        push();
    }

    fx::GateParams params() const
    {
        fx::GateParams p;
        p.thresholdDb = threshold.value();
        p.rangeDb = range.value();
        p.attackMs = attack.value();
        p.holdMs = hold.value();
        p.releaseMs = release.value();
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeGate();
        fx::setParams (*p, params());
        return p;
    }

    void tick() override   { meters.update (shared); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto a = infoArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);
        const auto p = params();
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("Below " + juce::String (p.thresholdDb, 1) + " dB the sound is turned down by "
                          + juce::String (-p.rangeDb, 0) + " dB (it opens again at the threshold).\n"
                          "Preview and watch the white line on the In meter: set it just above the noise.",
                          a.reduced (16.0f, 12.0f).toNearestInt(), juce::Justification::topLeft, 4);

        meters.paint (g, meterArea.toFloat(), "Gate", (float) p.thresholdDb);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        meterArea = r.removeFromRight (150);
        r.removeFromRight (16);
        infoArea = r.removeFromTop (70);
        r.removeFromTop (12);

        for (auto* c : { &threshold, &range, &attack, &hold, &release })
        {
            c->setBounds (r.removeFromTop (46));
            r.removeFromTop (4);
        }
    }

private:
    LabelledSlider threshold { "Threshold", -90.0, 0.0, 0.1, " dB", -45.0, [this] { push(); } };
    LabelledSlider range { "Turn down by (range)", -100.0, 0.0, 1.0, " dB", -80.0, [this] { push(); } };
    LabelledSlider attack { "Attack (opening)", 0.1, 100.0, 0.1, " ms", 1.0, [this] { push(); }, 5.0 };
    LabelledSlider hold { "Hold", 0.0, 1000.0, 1.0, " ms", 60.0, [this] { push(); }, 100.0 };
    LabelledSlider release { "Release (closing)", 5.0, 2000.0, 1.0, " ms", 150.0, [this] { push(); }, 200.0 };
    juce::Rectangle<int> infoArea, meterArea;
    DynamicsMeters meters;
};

//==============================================================================
// Chorus, Flanger and Vibrato: one modulated delay; the drawing shows how the
// delay sweeps in each channel over two seconds.
class ModulationEditor final : public Editor
{
public:
    ModulationEditor (Shared& s, fx::ModulationParams::Mode modeIn)
        : Editor (s), mode (modeIn), defaults (fx::modulationDefaults (modeIn)),
          rate { "Speed (rate)", 0.02, 15.0, 0.01, " Hz", defaults.rateHz, [this] { push(); }, 1.0 },
          depth { "Depth", 0.0, 15.0, 0.01, " ms", defaults.depthMs, [this] { push(); }, 3.0 },
          delay { "Delay", 0.1, 40.0, 0.1, " ms", defaults.delayMs, [this] { push(); }, 8.0 },
          feedback { "Feedback", -95.0, 95.0, 1.0, " %", defaults.feedback * 100.0, [this] { push(); } },
          mix { "Effect mix", 0.0, 100.0, 1.0, " %", defaults.mix * 100.0, [this] { push(); } },
          spread { "Stereo spread", 0.0, 100.0, 1.0, " %", defaults.spread * 100.0, [this] { push(); } },
          voices { "Voices", 1.0, 4.0, 1.0, "", (double) defaults.voices, [this] { push(); } }
    {
        for (auto* c : visible())
            addAndMakeVisible (c);

        push();
    }

    fx::ModulationParams params() const
    {
        auto p = defaults;
        p.rateHz = rate.value();
        p.depthMs = depth.value();
        p.delayMs = delay.value();
        p.feedback = feedback.value() / 100.0;
        p.mix = mix.value() / 100.0;
        p.spread = spread.value() / 100.0;
        p.voices = juce::roundToInt (voices.value());
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeModulation();
        fx::setParams (*p, params());
        return p;
    }

    bool hasTail() const override         { return mode == fx::ModulationParams::flanger; }
    double tailSeconds() const override   { return 0.25; }

    void paint (juce::Graphics& g) override
    {
        const auto a = drawArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);
        const auto inner = a.reduced (40.0f, 18.0f);
        const auto p = params();
        // Zoomed onto the sweep: from just under the delay to just over delay + depth.
        const auto margin = std::max (0.2, p.depthMs * 0.15);
        const auto minMs = std::max (0.0, p.delayMs - margin), maxMs = p.delayMs + p.depthMs + margin;
        auto Y = [&] (double ms) { return inner.getBottom() - inner.getHeight() * (float) ((ms - minMs) / (maxMs - minMs)); };

        g.setFont (juce::FontOptions (10.0f));
        g.setColour (colours::muted);
        g.drawText (juce::String (maxMs, 1) + " ms", juce::Rectangle<float> (a.getX() + 2.0f, inner.getY() - 6.0f, 36.0f, 12.0f), juce::Justification::centredRight);
        g.drawText (juce::String (minMs, 1) + " ms", juce::Rectangle<float> (a.getX() + 2.0f, inner.getBottom() - 6.0f, 36.0f, 12.0f), juce::Justification::centredRight);
        g.drawText ("delay over 2 seconds  (L blue, R orange)", juce::Rectangle<float> (inner.getX(), a.getY() + 2.0f, inner.getWidth(), 14.0f),
                    juce::Justification::centredLeft);

        const auto voicesNow = mode == fx::ModulationParams::chorus ? p.voices : 1;

        for (int ch = 0; ch < 2; ++ch)
            for (int v = 0; v < voicesNow; ++v)
            {
                juce::Path path;

                for (int i = 0; i <= 300; ++i)
                {
                    const auto t = 2.0 * i / 300.0;
                    const auto phase = p.rateHz * t + (ch == 1 ? p.spread * 0.5 : 0.0) + (double) v / voicesNow;
                    const auto ms = p.delayMs + p.depthMs * 0.5 * (1.0 + std::sin (juce::MathConstants<double>::twoPi * phase));
                    const juce::Point<float> pt (inner.getX() + inner.getWidth() * (float) (t / 2.0), Y (ms));

                    if (i == 0) path.startNewSubPath (pt);
                    else        path.lineTo (pt);
                }

                g.setColour ((ch == 0 ? colours::accent : colours::bass).withAlpha (v == 0 ? 1.0f : 0.5f));
                g.strokePath (path, juce::PathStrokeType (1.8f));
            }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        drawArea = r.removeFromTop (150);
        r.removeFromTop (12);
        auto items = visible();

        for (size_t i = 0; i < items.size(); i += 2)
        {
            auto row = r.removeFromTop (46);
            items[i]->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (6, 0));

            if (i + 1 < items.size())
                items[i + 1]->setBounds (row.reduced (6, 0));

            r.removeFromTop (6);
        }
    }

private:
    std::vector<juce::Component*> visible()
    {
        switch (mode)
        {
            case fx::ModulationParams::chorus:  return { &rate, &depth, &delay, &voices, &mix, &spread };
            case fx::ModulationParams::flanger: return { &rate, &depth, &delay, &feedback, &mix, &spread };
            case fx::ModulationParams::vibrato: return { &rate, &depth, &spread };
        }

        return {};
    }

    fx::ModulationParams::Mode mode;
    fx::ModulationParams defaults;
    LabelledSlider rate, depth, delay, feedback, mix, spread, voices;
    juce::Rectangle<int> drawArea;
};

//==============================================================================
// Distortion: the shaping curve drawn as in -> out, with the drive applied.
class DistortionEditor final : public Editor
{
public:
    explicit DistortionEditor (Shared& s) : Editor (s)
    {
        for (int t = 0; t < 4; ++t)
            type.addItem (fx::distortionTypeName ((fx::DistortionParams::Type) t), t + 1);

        type.setSelectedId (1, juce::dontSendNotification);
        type.onChange = [this] { push(); };
        styleLabel (typeLabel, 12.0f, true, colours::muted);
        typeLabel.setText ("Type", juce::dontSendNotification);
        addAndMakeVisible (type);
        addAndMakeVisible (typeLabel);

        for (auto* c : { &drive, &tone, &output, &mix })
            addAndMakeVisible (c);

        push();
    }

    fx::DistortionParams params() const
    {
        fx::DistortionParams p;
        p.type = (fx::DistortionParams::Type) juce::jlimit (0, 3, type.getSelectedItemIndex());
        p.driveDb = drive.value();
        p.toneHz = tone.value();
        p.outputDb = output.value();
        p.mix = mix.value() / 100.0;
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeDistortion();
        fx::setParams (*p, params());
        return p;
    }

    void tick() override
    {
        if (shared.previewing)
        {
            meterFall (inDb, shared.processor->inputLevelDb.load(), 1.2f);
            meterFall (outDb, shared.processor->outputLevelDb.load(), 1.2f);
        }
        else
        {
            inDb = outDb = -100.0f;
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto a = graphArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);
        const auto inner = a.reduced (18.0f);
        const auto p = params();
        const auto driveGain = juce::Decibels::decibelsToGain (p.driveDb);
        auto X = [&] (double x) { return inner.getCentreX() + inner.getWidth() * 0.5f * (float) x; };
        auto Y = [&] (double y) { return inner.getCentreY() - inner.getHeight() * 0.5f * (float) juce::jlimit (-1.1, 1.1, y); };

        g.setColour (colours::line);
        g.fillRect (inner.getX(), inner.getCentreY(), inner.getWidth(), 1.0f);
        g.fillRect (inner.getCentreX(), inner.getY(), 1.0f, inner.getHeight());
        g.setColour (colours::muted.withAlpha (0.5f));
        g.drawLine (X (-1.0), Y (-1.0), X (1.0), Y (1.0), 1.0f);

        juce::Path curve;

        for (int i = 0; i <= 400; ++i)
        {
            const auto x = -1.0 + 2.0 * i / 400.0;
            const auto y = fx::distortionCurve (p.type, x * driveGain);
            const juce::Point<float> pt (X (x), Y (y));

            if (i == 0) curve.startNewSubPath (pt);
            else        curve.lineTo (pt);
        }

        g.setColour (colours::accent);
        g.strokePath (curve, juce::PathStrokeType (2.4f));
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("input -> output", inner.withHeight (12.0f), juce::Justification::centredLeft);

        auto m = meterArea.toFloat();
        levelMeter (g, m.removeFromLeft (m.getWidth() / 2.0f).reduced (6.0f, 0.0f), "In", inDb, false);
        levelMeter (g, m.reduced (6.0f, 0.0f), "Out", outDb, false);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        meterArea = r.removeFromRight (100);
        r.removeFromRight (12);
        graphArea = r.removeFromLeft (std::min (r.getHeight(), 300));
        r.removeFromLeft (16);
        typeLabel.setBounds (r.removeFromTop (16));
        type.setBounds (r.removeFromTop (28));
        r.removeFromTop (8);

        for (auto* c : { &drive, &tone, &output, &mix })
        {
            c->setBounds (r.removeFromTop (46));
            r.removeFromTop (4);
        }
    }

private:
    juce::ComboBox type;
    juce::Label typeLabel;
    LabelledSlider drive { "Drive", 0.0, 48.0, 0.1, " dB", 12.0, [this] { push(); } };
    LabelledSlider tone { "Tone (high cut)", 500.0, 20000.0, 10.0, " Hz", 8000.0, [this] { push(); }, 4000.0 };
    LabelledSlider output { "Output", -36.0, 12.0, 0.1, " dB", -6.0, [this] { push(); } };
    LabelledSlider mix { "Mix (effect)", 0.0, 100.0, 1.0, " %", 100.0, [this] { push(); } };
    juce::Rectangle<int> graphArea, meterArea;
    float inDb = -100.0f, outDb = -100.0f;
};

//==============================================================================
// Package D editors: noise reduction, hum removal, click / crackle removal,
// clipped peak restoration.

// A frequency axis (log, 20 Hz .. top) with dB lines, for the repair graphs.
struct LogGraph
{
    juce::Rectangle<float> area;
    double lowHz = 20.0, highHz = 20000.0, topDb = 0.0, bottomDb = -100.0;

    float x (double hz) const
    {
        const auto t = std::log (juce::jlimit (lowHz, highHz, hz) / lowHz) / std::log (highHz / lowHz);
        return area.getX() + area.getWidth() * (float) t;
    }

    float y (double db) const
    {
        return area.getY() + area.getHeight() * (float) ((topDb - juce::jlimit (bottomDb, topDb, db)) / (topDb - bottomDb));
    }

    void paintGrid (juce::Graphics& g) const
    {
        g.setFont (juce::FontOptions (10.0f));

        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            if (hz < lowHz || hz > highHz)
                continue;

            g.setColour (colours::line);
            g.fillRect (x (hz), area.getY(), 1.0f, area.getHeight());
            g.setColour (colours::muted);
            g.drawText (formatHz (hz), juce::Rectangle<float> (x (hz) - 24.0f, area.getBottom() + 1.0f, 48.0f, 12.0f), juce::Justification::centred);
        }

        for (double db = topDb; db >= bottomDb; db -= 20.0)
        {
            g.setColour (colours::line);
            g.fillRect (area.getX(), y (db), area.getWidth(), 1.0f);
            g.setColour (colours::muted);
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (area.getX() - 30.0f, y (db) - 6.0f, 26.0f, 12.0f), juce::Justification::centredRight);
        }
    }

    // A spectrum (dB per FFT bin of `hzPerBin`) as a line, the most per pixel.
    juce::Path line (const std::vector<float>& db, double hzPerBin) const
    {
        juce::Path p;
        bool started = false;
        int lastX = -1;
        float maxDb = -200.0f;

        for (size_t k = 1; k < db.size(); ++k)
        {
            const auto hz = (double) k * hzPerBin;

            if (hz < lowHz || hz > highHz)
                continue;

            const auto px = (int) std::floor (x (hz));
            maxDb = std::max (maxDb, db[k]);

            if (px == lastX)
                continue;

            if (! started) { p.startNewSubPath ((float) px, y (maxDb)); started = true; }
            else           p.lineTo ((float) px, y (maxDb));

            lastX = px;
            maxDb = -200.0f;
        }

        return p;
    }
};

//==============================================================================
class NoiseReductionEditor final : public Editor
{
public:
    explicit NoiseReductionEditor (Shared& s) : Editor (s)
    {
        if (auto captured = fx::capturedNoiseProfile())
            profile = captured;
        else
            learn();

        styleLabel (printLabel, 12.0f, true, colours::muted);
        addAndMakeVisible (printLabel);

        capture.setTooltip ("Select a stretch with only the noise in the file (no music or voice), then click");
        capture.onClick = [this] { captureFromFile(); };
        learnButton.setTooltip ("Estimate the noise from the quietest moments of this audio, band by band");
        learnButton.onClick = [this] { learn(); push(); };
        addAndMakeVisible (capture);
        addAndMakeVisible (learnButton);

        for (auto* c : { &reduction, &threshold, &smoothing })
            addAndMakeVisible (c);

        styleToggle (noiseOnly);
        settings::dontRemember (noiseOnly);   // always starts hearing the result
        noiseOnly.setTooltip ("Hear what is being taken out: there should be no music in it");
        noiseOnly.onClick = [this] { push(); };
        addAndMakeVisible (noiseOnly);
        showPrint();
        push();
    }

    fx::NoiseReductionParams params() const
    {
        fx::NoiseReductionParams p;
        p.profile = profile;
        p.reductionDb = reduction.value();
        p.thresholdDb = threshold.value();
        p.smoothing = smoothing.value() / 100.0;
        p.noiseOnly = noiseOnly.getToggleState();
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeNoiseReduction();
        auto values = params();
        values.noiseOnly = false;   // Apply always removes the noise
        fx::setParams (*p, values);
        return p;
    }

    void tick() override   { if (shared.previewing) repaint(); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (colours::bg);
        g.fillRoundedRectangle (graphArea.toFloat(), 8.0f);
        LogGraph graph;
        graph.area = graphArea.toFloat().reduced (36.0f, 14.0f).withTrimmedBottom (6.0f);
        graph.highHz = std::min (20000.0, shared.rate() * 0.48);
        graph.topDb = -10.0;
        graph.bottomDb = -130.0;
        graph.paintGrid (g);

        if (shared.previewing)
        {
            const auto hzPerBin = shared.rate() / SpectrumMeter::size;
            g.setColour (colours::muted.withAlpha (0.7f));
            g.strokePath (graph.line (shared.pre.db, hzPerBin), juce::PathStrokeType (1.0f));
            g.setColour (colours::accent);
            g.strokePath (graph.line (shared.post.db, hzPerBin), juce::PathStrokeType (1.4f));
        }

        if (profile != nullptr && ! profile->magnitude.empty())
        {
            // The print, scaled to the live meter's FFT size, plus the threshold.
            const auto scale = std::sqrt ((double) SpectrumMeter::size / profile->fftSize) * 4.0 / SpectrumMeter::size;
            std::vector<float> db (profile->magnitude[0].size());

            for (size_t k = 0; k < db.size(); ++k)
            {
                double m = 0.0;

                for (const auto& ch : profile->magnitude)
                    m += ch[k] / (double) profile->magnitude.size();

                db[k] = (float) juce::Decibels::gainToDecibels (m * scale, -200.0) + (float) threshold.value() / 2.0f;
            }

            g.setColour (colours::bass);
            g.strokePath (graph.line (db, profile->sampleRate / profile->fftSize), juce::PathStrokeType (2.0f));
        }

        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (shared.previewing ? "orange: noise print   grey: before   blue: after" : "orange: the noise print (what gets taken out)",
                    graphArea.toFloat().reduced (40.0f, 2.0f).withHeight (14.0f), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        graphArea = r.removeFromTop (190);
        r.removeFromTop (10);
        auto row = r.removeFromTop (28);
        learnButton.setBounds (row.removeFromRight (190).reduced (2));
        capture.setBounds (row.removeFromRight (250).reduced (2));
        printLabel.setBounds (row);
        r.removeFromTop (8);

        for (auto* c : { &reduction, &threshold, &smoothing })
        {
            c->setBounds (r.removeFromTop (46));
            r.removeFromTop (2);
        }

        noiseOnly.setBounds (r.removeFromTop (30).removeFromLeft (260).reduced (2));
    }

private:
    void learn()
    {
        profile = fx::makeNoiseProfile (*shared.audio, shared.rate(), true);

        if (auto p = std::const_pointer_cast<fx::NoiseProfile> (profile))
            p->source = "learned from the quietest moments of this audio";

        showPrint();
    }

    void captureFromFile()
    {
        auto source = shared.host.currentSource != nullptr ? shared.host.currentSource() : std::nullopt;

        if (! source.has_value() || source->audio.getNumSamples() < fx::noiseFftSize (source->sampleRate))
        {
            printLabel.setText ("Select some noise in the file first (at least 0.1 s).", juce::dontSendNotification);
            return;
        }

        auto captured = fx::makeNoiseProfile (source->audio, source->sampleRate, false);

        if (captured == nullptr)
            return;

        captured->source = "captured from " + juce::String ((double) source->audio.getNumSamples() / source->sampleRate, 2)
                         + " s of " + source->title;
        fx::setCapturedNoiseProfile (captured);
        profile = captured;
        showPrint();
        push();
    }

    void showPrint()
    {
        printLabel.setText (profile != nullptr ? "Noise print " + profile->source : juce::String ("No noise print"),
                            juce::dontSendNotification);
    }

    std::shared_ptr<const fx::NoiseProfile> profile;
    juce::Label printLabel;
    juce::TextButton capture { "Capture from the file's selection" }, learnButton { "Learn from this audio" };
    LabelledSlider reduction { "Reduce the noise by", 0.0, 40.0, 0.5, " dB", 15.0, [this] { push(); } };
    LabelledSlider threshold { "Noise threshold (above the print)", -6.0, 12.0, 0.1, " dB", 4.0, [this] { push(); } };
    LabelledSlider smoothing { "Smoothing (less warbling)", 0.0, 100.0, 1.0, " %", 50.0, [this] { push(); } };
    juce::TextButton noiseOnly { "Hear only the noise taken out" };
    juce::Rectangle<int> graphArea;
};

//==============================================================================
class HumEditor final : public Editor
{
public:
    explicit HumEditor (Shared& s) : Editor (s)
    {
        styleLabel (found, 12.0f, true, colours::muted);
        addAndMakeVisible (found);
        settings::dontRemember (frequency);   // found in each file
        addAndMakeVisible (frequency);
        addAndMakeVisible (harmonics);
        addAndMakeVisible (width);

        for (auto* b : { &hz50, &hz60, &findAgain })
            addAndMakeVisible (b);

        hz50.onClick = [this] { frequency.slider.setValue (50.0); };
        hz60.onClick = [this] { frequency.slider.setValue (60.0); };
        findAgain.onClick = [this] { detect(); };
        styleToggle (rumble);
        rumble.setTooltip ("Also cut everything under 30 Hz (handling noise, wind, traffic)");
        rumble.onClick = [this] { push(); };
        addAndMakeVisible (rumble);
        detect();
        push();
    }

    fx::HumParams params() const
    {
        fx::HumParams p;
        p.frequency = frequency.value();
        p.harmonics = juce::roundToInt (harmonics.value());
        p.widthHz = width.value();
        p.removeRumble = rumble.getToggleState();
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeHumRemoval();
        fx::setParams (*p, params());
        return p;
    }

    void tick() override   { if (shared.previewing) repaint(); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (colours::bg);
        g.fillRoundedRectangle (graphArea.toFloat(), 8.0f);
        LogGraph graph;
        graph.area = graphArea.toFloat().reduced (36.0f, 14.0f).withTrimmedBottom (6.0f);
        graph.lowHz = 20.0;
        graph.highHz = std::min (2000.0, shared.rate() * 0.45);
        graph.topDb = 0.0;
        graph.bottomDb = -100.0;
        graph.paintGrid (g);

        if (shared.previewing)
        {
            const auto hzPerBin = shared.rate() / SpectrumMeter::size;
            g.setColour (colours::muted.withAlpha (0.7f));
            g.strokePath (graph.line (shared.pre.db, hzPerBin), juce::PathStrokeType (1.0f));
            g.setColour (colours::accent);
            g.strokePath (graph.line (shared.post.db, hzPerBin), juce::PathStrokeType (1.4f));
        }

        // Where the notches cut.
        const auto p = params();
        g.setColour (colours::bass.withAlpha (0.8f));

        for (int h = 1; h <= p.harmonics; ++h)
        {
            const auto hz = p.frequency * h;

            if (hz > graph.highHz)
                break;

            const auto x0 = graph.x (hz - p.widthHz), x1 = graph.x (hz + p.widthHz);
            g.fillRect (juce::Rectangle<float> (x0, graph.area.getY(), std::max (1.5f, x1 - x0), graph.area.getHeight()));
        }

        if (p.removeRumble)
        {
            g.setColour (colours::bass.withAlpha (0.25f));
            g.fillRect (juce::Rectangle<float> (graph.area.getX(), graph.area.getY(), graph.x (30.0) - graph.area.getX(), graph.area.getHeight()));
        }

        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("orange: the notches" + juce::String (shared.previewing ? "   grey: before   blue: after" : ""),
                    graphArea.toFloat().reduced (40.0f, 2.0f).withHeight (14.0f), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        graphArea = r.removeFromTop (180);
        r.removeFromTop (10);
        found.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        auto row = r.removeFromTop (46);
        findAgain.setBounds (row.removeFromRight (100).withTrimmedTop (16).reduced (2));
        hz60.setBounds (row.removeFromRight (70).withTrimmedTop (16).reduced (2));
        hz50.setBounds (row.removeFromRight (70).withTrimmedTop (16).reduced (2));
        frequency.setBounds (row);
        r.removeFromTop (4);
        harmonics.setBounds (r.removeFromTop (46));
        r.removeFromTop (4);
        width.setBounds (r.removeFromTop (46));
        r.removeFromTop (6);
        rumble.setBounds (r.removeFromTop (30).removeFromLeft (300).reduced (2));
    }

private:
    void detect()
    {
        if (const auto hum = fx::detectHum (*shared.audio, shared.rate()))
        {
            frequency.slider.setValue (hum->frequency, juce::dontSendNotification);
            found.setText ("Hum found at " + juce::String (hum->frequency, 2) + " Hz, " + juce::String (hum->strengthDb, 0)
                           + " dB above the sound around it.", juce::dontSendNotification);
        }
        else
        {
            found.setText ("No clear hum found: set 50 Hz (Europe) or 60 Hz (Americas) by hand.", juce::dontSendNotification);
        }

        push();
    }

    juce::Label found;
    LabelledSlider frequency { "Hum frequency", 40.0, 70.0, 0.01, " Hz", 50.0, [this] { push(); } };
    LabelledSlider harmonics { "Harmonics (the hum and its multiples)", 1.0, 30.0, 1.0, "", 8.0, [this] { push(); } };
    LabelledSlider width { "Notch width", 0.5, 10.0, 0.1, " Hz", 2.0, [this] { push(); } };
    juce::TextButton hz50 { "50 Hz" }, hz60 { "60 Hz" }, findAgain { "Find again" }, rumble { "Also remove rumble (under 30 Hz)" };
    juce::Rectangle<int> graphArea;
};

//==============================================================================
class DeclickEditor final : public Editor
{
public:
    explicit DeclickEditor (Shared& s) : Editor (s)
    {
        addAndMakeVisible (sensitivity);
        addAndMakeVisible (maxLength);
        styleLabel (status, 13.0f, true, colours::ink);
        addAndMakeVisible (status);
        count.setTooltip ("Runs the removal over this audio (without changing it) and counts the clicks");
        count.onClick = [this] { countClicks(); };
        addAndMakeVisible (count);
        push();
    }

    fx::DeclickParams params() const
    {
        fx::DeclickParams p;
        p.sensitivity = sensitivity.value();
        p.maxClickMs = maxLength.value();
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeDeclick();
        fx::setParams (*p, params());
        return p;
    }

    void tick() override
    {
        if (shared.previewing)
            status.setText ("Repaired while previewing: " + juce::String (fx::repairsMade (*shared.processor)) + " clicks",
                            juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        auto a = infoArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("Finds the clicks and crackle (vinyl, bad edits, digital errors) as sudden jumps the music around "
                          "them can't explain, and rebuilds those few samples from what comes before and after.\n"
                          "Higher sensitivity catches smaller ticks (crackle); if drum hits start to lose their attack, "
                          "lower it or shorten the longest click.",
                          a.reduced (16.0f, 12.0f).toNearestInt(), juce::Justification::topLeft, 6);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        infoArea = r.removeFromTop (80);
        r.removeFromTop (12);
        sensitivity.setBounds (r.removeFromTop (46));
        r.removeFromTop (4);
        maxLength.setBounds (r.removeFromTop (46));
        r.removeFromTop (12);
        auto row = r.removeFromTop (30);
        count.setBounds (row.removeFromLeft (150).reduced (2));
        status.setBounds (row.reduced (10, 0));
    }

private:
    void countClicks()
    {
        auto p = fx::makeDeclick();
        fx::setParams (*p, params());
        const auto rendered = fx::render (*p, *shared.audio, shared.rate(), 0, {});
        juce::ignoreUnused (rendered);
        const auto n = fx::repairsMade (*p);
        status.setText (n == 0 ? juce::String ("No clicks found at this sensitivity.")
                               : juce::String (n) + (n == 1 ? " click" : " clicks") + " would be repaired (all channels).",
                        juce::dontSendNotification);
    }

    LabelledSlider sensitivity { "Sensitivity", 0.0, 100.0, 1.0, " %", 50.0, [this] { push(); } };
    LabelledSlider maxLength { "Longest click", 0.1, 5.0, 0.1, " ms", 1.5, [this] { push(); } };
    juce::Label status;
    juce::TextButton count { "Count the clicks" };
    juce::Rectangle<int> infoArea;
};

//==============================================================================
class ClipRestoreEditor final : public Editor
{
public:
    explicit ClipRestoreEditor (Shared& s) : Editor (s)
    {
        styleLabel (found, 13.0f, true, colours::ink);
        addAndMakeVisible (found);
        settings::dontRemember (level);    // measured in each file
        settings::dontRemember (output);   // fitted to each file
        addAndMakeVisible (level);
        addAndMakeVisible (output);
        fit.setTooltip ("Sets the output so the rebuilt peaks stay under 0 dB");
        fit.onClick = [this] { fitOutput(); };
        addAndMakeVisible (fit);

        int runs = 0;

        if (const auto clipped = fx::detectClipLevelDb (*shared.audio, &runs))
        {
            level.slider.setValue (*clipped, juce::dontSendNotification);
            found.setText ("Clipped at " + juce::String (*clipped, 2) + " dB: " + juce::String (runs) + " flattened peaks found.",
                           juce::dontSendNotification);
        }
        else
        {
            found.setText ("No clipping found (no flat runs at the top level). Set the level by hand if you hear it.",
                           juce::dontSendNotification);
        }

        push();
        fitOutput();
    }

    fx::ClipRestoreParams params() const
    {
        fx::ClipRestoreParams p;
        p.clipLevelDb = level.value();
        p.outputDb = output.value();
        return p;
    }

    void push() override   { fx::setParams (*shared.processor, params()); repaint(); }

    std::shared_ptr<fx::Processor> makeForApply() override
    {
        auto p = fx::makeClipRestore();
        fx::setParams (*p, params());
        return p;
    }

    void paint (juce::Graphics& g) override
    {
        auto a = infoArea.toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (a, 8.0f);
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("Where the waveform was cut flat at the top (too loud when recorded or mastered), the missing "
                          "peak is redrawn from the curve of the samples either side. The rebuilt peaks go above the old "
                          "limit, so the output is turned down to make room for them.",
                          a.reduced (16.0f, 12.0f).toNearestInt(), juce::Justification::topLeft, 5);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        infoArea = r.removeFromTop (64);
        r.removeFromTop (12);
        found.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        level.setBounds (r.removeFromTop (46));
        r.removeFromTop (4);
        auto row = r.removeFromTop (46);
        fit.setBounds (row.removeFromRight (170).withTrimmedTop (16).reduced (2));
        output.setBounds (row);
    }

private:
    // Rebuilds the peaks once (without changing anything) to see how high they go.
    void fitOutput()
    {
        auto values = params();
        values.outputDb = 0.0;
        auto p = fx::makeClipRestore();
        fx::setParams (*p, values);

        if (const auto rendered = fx::render (*p, *shared.audio, shared.rate(), 0, {}))
        {
            float peak = 0.0f;

            for (int ch = 0; ch < rendered->getNumChannels(); ++ch)
                peak = std::max (peak, rendered->getMagnitude (ch, 0, rendered->getNumSamples()));

            const auto needed = std::min (0.0, -juce::Decibels::gainToDecibels ((double) peak, -100.0) - 0.3);
            output.slider.setValue (juce::jlimit (-24.0, 0.0, needed));
        }
    }

    juce::Label found;
    LabelledSlider level { "Clipped at (level)", -24.0, 0.0, 0.01, " dB", -0.1, [this] { push(); } };
    LabelledSlider output { "Output (room for the peaks)", -24.0, 0.0, 0.1, " dB", -3.0, [this] { push(); } };
    juce::TextButton fit { "Fit under 0 dB" };
    juce::Rectangle<int> infoArea;
};

class EffectPanel final : public juce::Component,
                          private juce::Timer
{
public:
    EffectPanel (EffectKind kind, BassFinderSource source, BassFinderHost host, std::function<void()> onCloseIn)
        : onClose (std::move (onCloseIn))
    {
        shared.source = std::move (source);
        shared.host = std::move (host);
        shared.audio = std::make_shared<juce::AudioBuffer<float>> (shared.source.audio);

        switch (kind)
        {
            case EffectKind::eq:          shared.processor = fx::makeEq();         editor = std::make_unique<EqEditor> (shared); name = "Parametric EQ"; break;
            case EffectKind::graphicEq:   shared.processor = fx::makeEq();         editor = std::make_unique<GraphicEqEditor> (shared); break;
            case EffectKind::compressor:  shared.processor = fx::makeCompressor(); editor = std::make_unique<CompressorEditor> (shared); name = "Compressor"; break;
            case EffectKind::delay:       shared.processor = fx::makeDelay();      editor = std::make_unique<DelayEditor> (shared); name = "Delay / Echo"; break;
            case EffectKind::reverb:      shared.processor = fx::makeReverb();     editor = std::make_unique<ReverbEditor> (shared); name = "Reverb"; break;
            case EffectKind::mp3Restore:  shared.processor = fx::makeMp3Restore(); editor = std::make_unique<Mp3RestoreEditor> (shared); name = "MP3 Restore"; break;
            case EffectKind::limiter:     shared.processor = fx::makeLimiter();    editor = std::make_unique<LimiterEditor> (shared); break;
            case EffectKind::gate:        shared.processor = fx::makeGate();       editor = std::make_unique<GateEditor> (shared); break;
            case EffectKind::chorus:      shared.processor = fx::makeModulation(); editor = std::make_unique<ModulationEditor> (shared, fx::ModulationParams::chorus); break;
            case EffectKind::flanger:     shared.processor = fx::makeModulation(); editor = std::make_unique<ModulationEditor> (shared, fx::ModulationParams::flanger); break;
            case EffectKind::vibrato:     shared.processor = fx::makeModulation(); editor = std::make_unique<ModulationEditor> (shared, fx::ModulationParams::vibrato); break;
            case EffectKind::distortion:  shared.processor = fx::makeDistortion(); editor = std::make_unique<DistortionEditor> (shared); break;
            case EffectKind::noiseReduction: shared.processor = fx::makeNoiseReduction(); editor = std::make_unique<NoiseReductionEditor> (shared); break;
            case EffectKind::humRemoval:  shared.processor = fx::makeHumRemoval(); editor = std::make_unique<HumEditor> (shared); break;
            case EffectKind::declick:     shared.processor = fx::makeDeclick();    editor = std::make_unique<DeclickEditor> (shared); break;
            case EffectKind::clipRestore: shared.processor = fx::makeClipRestore(); editor = std::make_unique<ClipRestoreEditor> (shared); break;
        }

        name = effectName (kind);

        // The editors pushed their values before the processor existed in some cases.
        editor->push();

        // Last time's settings (the factory ones stay for the Defaults button).
        defaults = editor->getState();
        settingsKey = "effect." + name;

        if (const auto saved = settings::get (settingsKey); ! saved.isVoid())
        {
            editor->setState (saved);
            editor->push();
        }

        defaultsButton.setTooltip ("Back to the original settings of this effect");
        defaultsButton.onClick = [this] { editor->setState (defaults); editor->push(); };
        addAndMakeVisible (defaultsButton);
        addAndMakeVisible (*editor);

        styleLabel (title, 14.0f, true);
        title.setText (name + ":  " + shared.source.title, juce::dontSendNotification);
        styleLabel (status, 12.0f, false, colours::muted);
        styleLabel (tailLabel, 12.0f, true, colours::muted);
        tailLabel.setText ("Ring out after the selection", juce::dontSendNotification);
        addAndMakeVisible (title);
        addAndMakeVisible (status);

        preview.onClick = [this] { togglePreview(); };
        preview.setColour (juce::TextButton::buttonColourId, colours::accent);
        preview.setColour (juce::TextButton::textColourOffId, colours::onAccent);
        styleToggle (bypass);
        bypass.onClick = [this] { *shared.bypass = bypass.getToggleState(); };
        apply.onClick = [this] { applyToFile(); };
        apply.setColour (juce::TextButton::buttonColourId, colours::ok.withAlpha (0.8f));
        apply.setColour (juce::TextButton::textColourOffId, colours::onAccent);
        cancel.onClick = [this] { close(); };

        for (auto* b : { &preview, &bypass, &apply, &cancel })
            addAndMakeVisible (b);

        if (editor->hasTail())
        {
            styleToggle (autoTail);
            autoTail.setToggleState (true, juce::dontSendNotification);
            autoTail.onClick = [this] { tailSlider.setEnabled (! autoTail.getToggleState()); };
            tailSlider.setRange (0.0, 20.0, 0.1);
            tailSlider.setTextValueSuffix (" s");
            tailSlider.setSliderStyle (juce::Slider::LinearHorizontal);
            tailSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
            tailSlider.setEnabled (false);

            for (auto* c : { (juce::Component*) &autoTail, (juce::Component*) &tailSlider, (juce::Component*) &tailLabel })
                addAndMakeVisible (c);
        }

        const auto big = kind == EffectKind::eq || kind == EffectKind::graphicEq;
        const auto height = big ? 700
                          : kind == EffectKind::compressor || kind == EffectKind::gate ? 480
                          : kind == EffectKind::limiter ? 520
                          : kind == EffectKind::noiseReduction ? 560
                          : kind == EffectKind::humRemoval ? 520
                          : kind == EffectKind::declick ? 370
                          : kind == EffectKind::clipRestore ? 370
                          : kind == EffectKind::chorus || kind == EffectKind::flanger ? 480
                          : kind == EffectKind::distortion ? 440
                          : kind == EffectKind::vibrato ? 420
                          : kind == EffectKind::mp3Restore ? 640 : 560;
        setSize (big ? 1080 : 820, height);
        startTimerHz (30);
    }

    ~EffectPanel() override
    {
        stopPreview();
        settings::set (settingsKey, editor->getState());   // remembered for next time
    }

    void close()
    {
        stopPreview();

        if (onClose != nullptr)
            onClose();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::bg);
        paintCard (g, card);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (36);
        r.removeFromBottom (8);

        if (editor->hasTail())
        {
            auto tail = r.removeFromBottom (30);
            tailLabel.setBounds (tail.removeFromLeft (200));
            autoTail.setBounds (tail.removeFromLeft (80).reduced (2));
            tailSlider.setBounds (tail.removeFromLeft (300));
            r.removeFromBottom (6);
        }

        card = r;
        editor->setBounds (r.reduced (14));

        preview.setBounds (bottom.removeFromLeft (170).reduced (2));
        bypass.setBounds (bottom.removeFromLeft (110).reduced (2));
        defaultsButton.setBounds (bottom.removeFromLeft (100).reduced (2));
        cancel.setBounds (bottom.removeFromRight (100).reduced (2));
        apply.setBounds (bottom.removeFromRight (150).reduced (2));
        status.setBounds (bottom.reduced (10, 0));
    }

private:
    void togglePreview()
    {
        if (shared.previewing)
        {
            stopPreview();
            return;
        }

        if (shared.host.play == nullptr)
            return;

        shared.pre.clear();
        shared.post.clear();
        shared.processor->reset();
        auto source = std::make_unique<PreviewSource> (shared.audio, shared.processor, shared.taps, shared.bypass);
        shared.host.play (std::move (source), shared.rate(), std::max (1, shared.audio->getNumChannels()));
        shared.previewing = true;
        preview.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xa0  Stop preview"));
        status.setText ("Previewing in a loop: every change is heard at once.", juce::dontSendNotification);
    }

    void stopPreview()
    {
        if (! shared.previewing)
            return;

        if (shared.host.stop != nullptr)
            shared.host.stop();

        shared.previewing = false;
        preview.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb6  Preview"));

        if (shared.source.showPlayhead != nullptr)
            shared.source.showPlayhead (-1.0);

        status.setText ({}, juce::dontSendNotification);
    }

    void timerCallback() override
    {
        if (editor->hasTail() && autoTail.getToggleState())
            tailSlider.setValue (editor->tailSeconds(), juce::dontSendNotification);

        if (shared.previewing)
        {
            const auto p = shared.host.playPosition != nullptr ? shared.host.playPosition() : -1;

            if (p < 0)
            {
                stopPreview();
            }
            else
            {
                Taps::pull (shared.taps->preFifo, shared.taps->pre, shared.pre.ring);
                Taps::pull (shared.taps->postFifo, shared.taps->post, shared.post.ring);
                shared.pre.update (true);
                shared.post.update (true);

                const auto length = std::max (1, shared.audio->getNumSamples());

                if (shared.source.showPlayhead != nullptr)
                    shared.source.showPlayhead ((double) (p % length) / shared.rate());
            }
        }

        editor->tick();
    }

    void applyToFile()
    {
        auto apply = shared.source.applyWithTail;

        if (apply == nullptr || shared.host.runJob == nullptr)
            return;

        stopPreview();
        auto processor = editor->makeForApply();
        auto audio = shared.audio;
        const auto rate = shared.rate();
        const auto tail = editor->hasTail() ? (int) std::lround (tailSlider.getValue() * rate) : 0;
        auto result = std::make_shared<std::optional<juce::AudioBuffer<float>>>();
        juce::Component::SafePointer<EffectPanel> safeThis (this);

        shared.host.runJob ("Applying " + name + "...",
                            [processor, audio, rate, tail, result] (std::function<bool (float)> progress)
                            {
                                *result = fx::render (*processor, *audio, rate, tail, progress);
                                return result->has_value();
                            },
                            [safeThis, apply, result] (bool finished)
                            {
                                auto* self = safeThis.getComponent();

                                if (! finished || ! result->has_value())
                                {
                                    if (self != nullptr)
                                        self->status.setText ("Cancelled.", juce::dontSendNotification);

                                    return;
                                }

                                if (! apply (**result))
                                {
                                    if (self != nullptr)
                                        self->status.setText ("The file is closed: can't apply.", juce::dontSendNotification);

                                    return;
                                }

                                if (self != nullptr)
                                    self->close();
                            });
    }

    Shared shared;
    std::function<void()> onClose;
    std::unique_ptr<Editor> editor;
    juce::String name;
    juce::Label title, status, tailLabel;
    juce::TextButton preview { juce::CharPointer_UTF8 ("\xe2\x96\xb6  Preview") }, bypass { "Bypass" }, apply { "Apply" },
                     cancel { "Close" }, autoTail { "Auto" };
    juce::Slider tailSlider;
    juce::Rectangle<int> card;
    juce::TextButton defaultsButton { "Defaults" };
    juce::var defaults;
    juce::String settingsKey;
};

class EffectWindow final : public juce::DocumentWindow
{
public:
    EffectWindow (EffectKind kind, BassFinderSource source, BassFinderHost host, std::function<void()> onClose)
        : juce::DocumentWindow ("Effect", colours::bg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (false);
        setLookAndFeel (&look);
        auto* panel = new EffectPanel (kind, std::move (source), std::move (host), std::move (onClose));
        setName (effectName (kind));
        setContentOwned (panel, true);
        dark::adoptLook (*getContentComponent());
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
    }

    ~EffectWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    void closeButtonPressed() override
    {
        if (auto* p = dynamic_cast<EffectPanel*> (getContentComponent()))
            p->close();
    }

private:
    dark::DarkLook look;
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createEffectWindow (EffectKind kind, BassFinderSource source, BassFinderHost host,
                                                          std::function<void()> onClose)
{
    return std::make_unique<EffectWindow> (kind, std::move (source), std::move (host), std::move (onClose));
}
} // namespace wf
