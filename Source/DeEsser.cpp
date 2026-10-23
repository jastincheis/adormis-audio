#include "DeEsser.h"
#include "DarkTheme.h"
#include "Sibilance.h"
#include "SpaceKey.h"

namespace wf
{
namespace
{
namespace colours = dark::colours;
using sibilance::Sibilant;

juce::String formatTime (double t)
{
    t = std::max (0.0, t);
    const auto m = (int) std::floor (t / 60.0);
    return juce::String (m) + ":" + juce::String (t - m * 60.0, 2).paddedLeft ('0', 5);
}

juce::String formatHz (double hz)
{
    return hz >= 1000.0 ? juce::String (hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
}

juce::String formatDb (double db)
{
    return (db > 0.05 ? "+" : "") + juce::String (db, 1) + " dB";
}

void styleLabel (juce::Label& l, float size, bool bold = false, juce::Colour colour = colours::ink)
{
    l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    l.setColour (juce::Label::textColourId, colour);
    l.setMinimumHorizontalScale (0.7f);
}

void paintCard (juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour (colours::surface);
    g.fillRoundedRectangle (r.toFloat(), 10.0f);
    g.setColour (colours::line);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
}

//==============================================================================
struct Model
{
    BassFinderSource source;
    BassFinderHost host;
    std::vector<Sibilant> items;
    int current = -1;
    int band = 0;                   // the picked point of the current sibilant
    std::vector<int> selection { 0 };   // points moved together (always includes `band`)
    int contextMode = 0;            // 0 only the S, 1 +-0.5 s, 2 +-2 s, 3 from just before it to the end
    double sensitivity = 0.5, defaultGain = -6.0;
    bool playing = false;
    std::shared_ptr<juce::AudioBuffer<float>> playedAudio;   // what is playing, for the live spectrum
    double playOffset = 0.0;
    juce::int64 playLength = 0;

    std::function<void()> changed;   // items or the current one changed

    double rate() const       { return source.sampleRate; }
    double duration() const   { return source.audio.getNumSamples() / source.sampleRate; }
    Sibilant* currentItem()   { return juce::isPositiveAndBelow (current, (int) items.size()) ? &items[(size_t) current] : nullptr; }

    bool isSelected (int index) const
    {
        return std::find (selection.begin(), selection.end(), index) != selection.end();
    }

    void selectOnly (int index)   { band = index; selection = { index }; }

    void selectAllPoints()
    {
        selection.clear();

        if (auto* s = currentItem())
            for (int i = 0; i < (int) s->bands.size(); ++i)
                selection.push_back (i);
    }

    // Applies `fn` to every selected point of the current sibilant.
    void forSelected (const std::function<void (sibilance::Band&)>& fn)
    {
        auto* s = currentItem();

        if (s == nullptr)
            return;

        for (auto i : selection)
            if (juce::isPositiveAndBelow (i, (int) s->bands.size()))
                fn (s->bands[(size_t) i]);
    }

    sibilance::Band* currentBand()
    {
        auto* s = currentItem();

        if (s == nullptr || s->bands.empty())
            return nullptr;

        band = juce::jlimit (0, (int) s->bands.size() - 1, band);
        return &s->bands[(size_t) band];
    }

    void detect()
    {
        stop();
        items = sibilance::detect (source.audio, rate(), sensitivity, defaultGain);
        current = items.empty() ? -1 : 0;
        select (current);
    }

    void select (int index)
    {
        current = items.empty() ? -1 : juce::jlimit (0, (int) items.size() - 1, index);
        band = 0;
        selection = { 0 };

        if (auto* s = currentItem(); s != nullptr && source.showRange != nullptr)
            source.showRange (s->start, s->end);

        if (changed != nullptr)
            changed();
    }

    void play (juce::AudioBuffer<float> audio, double offset)
    {
        if (host.play == nullptr)
            return;

        stop();
        playedAudio = std::make_shared<juce::AudioBuffer<float>> (audio);
        auto player = std::make_unique<LayeredPlayer> (std::move (audio), juce::AudioBuffer<float>(), false);
        playLength = player->getLength();
        playOffset = offset;
        playing = true;
        host.play (std::move (player), rate(), std::max (1, source.audio.getNumChannels()));
    }

    void stop()
    {
        if (playing && host.stop != nullptr)
            host.stop();

        playing = false;
    }

    std::optional<double> playSeconds() const
    {
        if (! playing || host.playPosition == nullptr)
            return {};

        const auto p = host.playPosition();

        if (p < 0 || p >= playLength)
            return {};

        return playOffset + (double) p / rate();
    }

    // The current sibilant with a little audio around it, original or processed.
    void playCurrent (bool processed)
    {
        auto* s = currentItem();

        if (s == nullptr)
            return;

        static constexpr double around[] { 0.0, 0.5, 2.0, 0.5 };
        const auto context = around[juce::jlimit (0, 3, contextMode)];
        const auto t0 = std::max (0.0, s->start - context);
        const auto t1 = contextMode == 3 ? duration() : std::min (duration(), s->end + context);
        const auto a = (int) std::lround (t0 * rate()), b = (int) std::lround (t1 * rate());
        juce::AudioBuffer<float> slice (source.audio.getNumChannels(), b - a);

        for (int ch = 0; ch < slice.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, source.audio, ch, a, b - a);

        if (processed)
        {
            auto shifted = *s;
            shifted.start -= t0;
            shifted.end -= t0;
            sibilance::processOne (slice, rate(), shifted);
        }

        play (std::move (slice), t0);
    }
};

//==============================================================================
// The current sibilant's spectrum (grey before, orange after) with its bell
// on top. Drag the ball: left / right moves the centre, up / down the gain;
// the wheel (or shift + drag up / down) makes it narrower or wider.
class SpectrumView final : public juce::Component
{
public:
    explicit SpectrumView (Model& m) : model (m) {}

    std::function<void()> onEdited;

    // What is playing right now: its spectrum (same scale as "before") and
    // the position, drawn in the graph while it plays.
    void setLive (const std::vector<float>& spectrumDb, std::optional<double> seconds)
    {
        if (spectrumDb.empty())
        {
            live.clear();
        }
        else
        {
            // Rises at once, falls about 40 dB per second (30 frames per second).
            if (live.size() != spectrumDb.size())
                live = spectrumDb;

            for (size_t k = 0; k < live.size(); ++k)
                live[k] = std::max (spectrumDb[k], live[k] - 1.3f);
        }

        liveTime = seconds;
        repaint();
    }

    void refresh()
    {
        before.clear();
        after.clear();

        if (auto* s = model.currentItem())
        {
            before = sibilance::spectrum (model.source.audio, model.rate(), s->start, s->end);

            const auto pad = 0.02;
            const auto t0 = std::max (0.0, s->start - pad), t1 = std::min (model.duration(), s->end + pad);
            const auto a = (int) std::lround (t0 * model.rate()), b = (int) std::lround (t1 * model.rate());
            juce::AudioBuffer<float> slice (model.source.audio.getNumChannels(), std::max (0, b - a));

            for (int ch = 0; ch < slice.getNumChannels(); ++ch)
                slice.copyFrom (ch, 0, model.source.audio, ch, a, b - a);

            auto shifted = *s;
            shifted.start -= t0;
            shifted.end -= t0;
            sibilance::processOne (slice, model.rate(), shifted);
            after = sibilance::spectrum (slice, model.rate(), s->start - t0, s->end - t0);
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = plotArea();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

        // Grid: frequencies and the bell's dB scale.
        g.setFont (juce::FontOptions (11.0f));

        for (double hz : { 1000.0, 2000.0, 3000.0, 5000.0, 7000.0, 10000.0, 15000.0, 20000.0 })
        {
            if (hz > maxHz())
                continue;

            const auto x = xFor (hz);
            g.setColour (colours::line);
            g.fillRect (x, area.getY(), 1.0f, area.getHeight());
            g.setColour (colours::muted);
            g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz),
                        juce::Rectangle<float> (x - 20.0f, area.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centred);
        }

        for (double db : { -24.0, -18.0, -12.0, -6.0, 0.0, 6.0, 12.0 })
        {
            const auto y = yForGain (db);
            g.setColour (std::abs (db) < 0.01 ? colours::muted.withAlpha (0.6f) : colours::line);
            g.fillRect (area.getX(), y, area.getWidth(), 1.0f);
            g.setColour (colours::muted);
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (area.getRight() + 4.0f, y - 7.0f, 30.0f, 14.0f),
                        juce::Justification::centredLeft);
        }

        auto* s = model.currentItem();

        if (s == nullptr)
        {
            g.setColour (colours::muted);
            g.setFont (juce::FontOptions (15.0f));
            g.drawText ("No sibilants found. Try a higher sensitivity and Detect again.", area, juce::Justification::centred);
            return;
        }

        // Spectra, scaled so the loudest bin of "before" sits near the top.
        if (! before.empty())
        {
            float top = -200.0f;
            const auto hzPerBin = model.rate() / ((before.size() - 1) * 2.0);

            for (size_t k = 1; k < before.size(); ++k)
                if (k * hzPerBin >= minHz)
                    top = std::max (top, before[k]);

            auto curve = [&] (const std::vector<float>& spec, juce::Colour colour, bool fill)
            {
                juce::Path p;
                bool started = false;

                for (size_t k = 1; k < spec.size(); ++k)
                {
                    const auto hz = k * hzPerBin;

                    if (hz < minHz || hz > maxHz())
                        continue;

                    const auto y = area.getY() + area.getHeight() * juce::jlimit (0.0f, 1.0f, (top + 3.0f - spec[k]) / 72.0f);

                    if (! started)
                    {
                        p.startNewSubPath (xFor (hz), area.getBottom());
                        p.lineTo (xFor (hz), y);
                        started = true;
                    }
                    else
                    {
                        p.lineTo (xFor (hz), y);
                    }
                }

                if (! started)
                    return;

                if (fill)
                {
                    auto filled = p;
                    filled.lineTo (xFor (std::min (maxHz(), hzPerBin * (spec.size() - 1))), area.getBottom());
                    filled.closeSubPath();
                    g.setColour (colour.withAlpha (0.18f));
                    g.fillPath (filled);
                }

                g.setColour (colour);
                g.strokePath (p, juce::PathStrokeType (1.3f));
            };

            curve (before, colours::muted, true);

            if (! after.empty())
                curve (after, colours::bass, false);

            if (! live.empty() && live.size() == before.size())
                curve (live, colours::ok, false);
        }

        // Where playback is, along the bottom of the graph: the S span and a cursor.
        if (liveTime.has_value())
        {
            const auto barArea = juce::Rectangle<float> (area.getX(), area.getBottom() - 10.0f, area.getWidth(), 6.0f);
            const auto [t0, t1] = playSpan (*s);
            auto X = [&] (double t) { return barArea.getX() + barArea.getWidth() * (float) juce::jlimit (0.0, 1.0, (t - t0) / std::max (1.0e-6, t1 - t0)); };

            g.setColour (colours::line);
            g.fillRoundedRectangle (barArea, 3.0f);
            g.setColour (colours::accent.withAlpha (0.5f));
            g.fillRoundedRectangle (juce::Rectangle<float> (X (s->start), barArea.getY(), X (s->end) - X (s->start), barArea.getHeight()), 3.0f);

            const auto x = X (*liveTime);
            g.setColour (colours::ok);
            g.fillRect (x - 1.5f, barArea.getY() - 6.0f, 3.0f, barArea.getHeight() + 8.0f);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            const auto inside = *liveTime >= s->start && *liveTime <= s->end;
            g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 ")) + formatTime (*liveTime) + (inside ? "  in the S" : ""),
                        juce::Rectangle<float> (area.getRight() - 170.0f, area.getY() + 6.0f, 164.0f, 16.0f), juce::Justification::centredRight);
        }

        // The bell's response.
        juce::Path bell;

        for (int x = 0; x <= (int) area.getWidth(); x += 2)
        {
            const auto hz = hzFor (area.getX() + (float) x);
            const auto y = yForGain (sibilance::bellResponseDb (hz, *s, model.rate()));

            if (x == 0)
                bell.startNewSubPath (area.getX(), y);
            else
                bell.lineTo (area.getX() + (float) x, y);
        }

        auto bellFill = bell;
        bellFill.lineTo (area.getRight(), yForGain (0.0));
        bellFill.lineTo (area.getX(), yForGain (0.0));
        bellFill.closeSubPath();
        g.setColour (colours::accent.withAlpha (0.16f));
        g.fillPath (bellFill);
        g.setColour (colours::accent);
        g.strokePath (bell, juce::PathStrokeType (2.2f));

        // With several points, each one's own bell faintly, then the balls.
        if (s->bands.size() > 1)
        {
            for (const auto& b : s->bands)
            {
                juce::Path one;

                for (int x = 0; x <= (int) area.getWidth(); x += 3)
                {
                    const auto y = yForGain (sibilance::bandResponseDb (hzFor (area.getX() + (float) x), b, model.rate()));

                    if (x == 0)
                        one.startNewSubPath (area.getX(), y);
                    else
                        one.lineTo (area.getX() + (float) x, y);
                }

                g.setColour (colours::accent.withAlpha (0.35f));
                g.strokePath (one, juce::PathStrokeType (1.0f));
            }
        }

        for (size_t i = 0; i < s->bands.size(); ++i)
        {
            const auto& b = s->bands[i];
            const auto picked = (int) i == model.band;
            const auto inGroup = model.selection.size() > 1 && model.isSelected ((int) i);
            const auto handle = handleFor (b);
            const auto r = picked || inGroup ? 9.0f : 6.5f;

            if (inGroup)
            {
                g.setColour (colours::warn);
                g.drawEllipse (handle.x - r - 3.0f, handle.y - r - 3.0f, (r + 3.0f) * 2.0f, (r + 3.0f) * 2.0f, 2.0f);
            }

            g.setColour (picked || inGroup ? colours::accent : colours::accent.withAlpha (0.7f));
            g.fillEllipse (handle.x - r, handle.y - r, r * 2.0f, r * 2.0f);
            g.setColour (colours::bg);
            g.drawEllipse (handle.x - r, handle.y - r, r * 2.0f, r * 2.0f, 2.0f);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (juce::String ((int) i + 1), juce::Rectangle<float> (handle.x - r, handle.y - r, r * 2.0f, r * 2.0f),
                        juce::Justification::centred);

            if (picked)
            {
                g.setColour (colours::ink);
                g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                g.drawText (formatHz (b.frequency) + "   " + formatDb (b.gainDb) + "   Q " + juce::String (b.q, 1),
                            juce::Rectangle<float> (handle.x - 90.0f, handle.y + (b.gainDb < 0 ? 12.0f : -28.0f), 180.0f, 16.0f),
                            juce::Justification::centred);
            }
        }

        // Legend.
        g.setFont (juce::FontOptions (11.0f));
        auto legend = juce::Rectangle<float> (area.getX() + 8.0f, area.getY() + 6.0f, 360.0f, 14.0f);
        g.setColour (colours::muted);
        g.drawText ("grey: this S before", legend, juce::Justification::centredLeft);
        g.setColour (colours::bass);
        g.drawText ("orange: after", legend.translated (120.0f, 0.0f), juce::Justification::centredLeft);
        g.setColour (colours::accent);
        g.drawText ("blue: the bells (dB on the right)", legend.translated (210.0f, 0.0f), juce::Justification::centredLeft);

        if (! live.empty())
        {
            g.setColour (colours::ok);
            g.drawText ("green: playing now", legend.translated (420.0f, 0.0f), juce::Justification::centredLeft);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* s = model.currentItem();

        if (s == nullptr)
            return;

        dragStart = e.position;
        const auto hit = bandAt (e.position);

        // Right-click on a point removes it (the last one goes back to 0 dB).
        if (e.mods.isPopupMenu())
        {
            if (hit >= 0)
                removeBand (hit);

            return;
        }

        if (hit >= 0 && e.mods.isShiftDown())
        {
            // Shift+click adds a point to the selection (or takes it out).
            if (model.isSelected (hit) && model.selection.size() > 1)
            {
                model.selection.erase (std::find (model.selection.begin(), model.selection.end(), hit));
                model.band = model.selection.front();
            }
            else if (! model.isSelected (hit))
            {
                model.selection.push_back (hit);
                model.band = hit;
            }

            draggingPoint = false;
            edited();
            return;
        }

        if (hit >= 0)
        {
            // Dragging a point of a group moves the whole group.
            if (! model.isSelected (hit))
                model.selectOnly (hit);
            else
                model.band = hit;

            draggingPoint = true;
        }
        else
        {
            draggingPoint = false;

            // A click elsewhere: with a group it just goes back to one point,
            // with one point it moves it there.
            if (model.selection.size() > 1)
            {
                model.selectOnly (model.band);
                edited();
                return;
            }
        }

        // Where every selected point started, for moving them together.
        groupStart.clear();
        model.forSelected ([this] (sibilance::Band& b) { groupStart.push_back (b); });

        if (auto* b = model.currentBand())
        {
            startQ = b->q;
            startBand = *b;
        }

        if (hit < 0)
            setFromPoint (e);
        else
            edited();
    }

    void mouseDrag (const juce::MouseEvent& e) override   { setFromPoint (e); }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const auto factor = std::pow (1.15, wheel.deltaY * 4.0);
        model.forSelected ([factor] (sibilance::Band& b) { b.q = juce::jlimit (sibilance::minQ, sibilance::maxQ, b.q * factor); });
        edited();
    }

    // Double-click on empty space adds a point there; on a point, sets it to 0 dB.
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto* s = model.currentItem();

        if (s == nullptr)
            return;

        if (const auto hit = bandAt (e.position); hit >= 0)
        {
            s->bands[(size_t) hit].gainDb = 0.0;
            model.band = hit;
        }
        else if ((int) s->bands.size() < sibilance::maxBands)
        {
            addBand (hzFor (e.position.x), std::round (gainFor (e.position.y) * 2.0) / 2.0);
        }

        edited();
    }

    // A new point (the picked one) at `hz`, `gainDb`.
    void addBand (double hz, double gainDb)
    {
        auto* s = model.currentItem();

        if (s == nullptr || (int) s->bands.size() >= sibilance::maxBands)
            return;

        s->bands.push_back ({ juce::jlimit (sibilance::minFrequency, std::min (sibilance::maxFrequency, maxHz()), hz),
                              juce::jlimit (sibilance::minGainDb, sibilance::maxGainDb, gainDb), 2.0 });
        model.selectOnly ((int) s->bands.size() - 1);
        edited();
    }

    void removeBand (int index)
    {
        auto* s = model.currentItem();

        if (s == nullptr || ! juce::isPositiveAndBelow (index, (int) s->bands.size()))
            return;

        if (s->bands.size() > 1)
            s->bands.erase (s->bands.begin() + index);
        else
            s->bands[0].gainDb = 0.0;

        model.selectOnly (juce::jlimit (0, (int) s->bands.size() - 1, index - 1));
        edited();
    }

private:
    static constexpr double minHz = 1000.0;
    double maxHz() const   { return std::min (20000.0, model.rate() * 0.48); }

    juce::Point<float> handleFor (const sibilance::Band& b) const   { return { xFor (b.frequency), yForGain (b.gainDb) }; }

    int bandAt (juce::Point<float> p) const
    {
        auto* s = const_cast<Model&> (model).currentItem();

        if (s == nullptr)
            return -1;

        int best = -1;
        auto bestDistance = 14.0f;

        for (size_t i = 0; i < s->bands.size(); ++i)
            if (const auto d = p.getDistanceFrom (handleFor (s->bands[i])); d < bestDistance)
            {
                bestDistance = d;
                best = (int) i;
            }

        return best;
    }

    juce::Rectangle<float> plotArea() const   { return getLocalBounds().toFloat().reduced (14.0f).withTrimmedRight (30.0f).withTrimmedBottom (16.0f); }

    float xFor (double hz) const
    {
        const auto a = plotArea();
        return a.getX() + a.getWidth() * (float) (std::log (hz / minHz) / std::log (maxHz() / minHz));
    }

    double hzFor (float x) const
    {
        const auto a = plotArea();
        return minHz * std::pow (maxHz() / minHz, juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth())));
    }

    float yForGain (double db) const
    {
        const auto a = plotArea();
        return a.getY() + a.getHeight() * (float) ((18.0 - juce::jlimit (-30.0, 18.0, db)) / 48.0);
    }

    double gainFor (float y) const
    {
        const auto a = plotArea();
        return 18.0 - 48.0 * (double) ((y - a.getY()) / a.getHeight());
    }

    void setFromPoint (const juce::MouseEvent& e)
    {
        auto* b = model.currentBand();

        if (b == nullptr || e.mods.isPopupMenu())
            return;

        if (model.selection.size() > 1 && groupStart.size() == model.selection.size())
        {
            // A group: the same gain step and the same move (in octaves) for all.
            const auto gainStep = std::round ((gainFor (e.position.y) - gainFor (dragStart.y)) * 2.0) / 2.0;
            const auto ratio = hzFor (e.position.x) / hzFor (dragStart.x);
            const auto qRatio = std::pow (1.02, dragStart.y - e.position.y);
            size_t i = 0;

            model.forSelected ([&] (sibilance::Band& band)
            {
                const auto& from = groupStart[i++];

                if (e.mods.isShiftDown())
                {
                    band.q = juce::jlimit (sibilance::minQ, sibilance::maxQ, from.q * qRatio);
                }
                else
                {
                    band.gainDb = juce::jlimit (sibilance::minGainDb, sibilance::maxGainDb, from.gainDb + gainStep);
                    band.frequency = juce::jlimit (sibilance::minFrequency, std::min (sibilance::maxFrequency, maxHz()), from.frequency * ratio);
                }
            });
        }
        else if (e.mods.isShiftDown())
        {
            // Up = narrower, down = wider.
            b->q = juce::jlimit (sibilance::minQ, sibilance::maxQ, startQ * std::pow (1.02, dragStart.y - e.position.y));
        }
        else
        {
            b->frequency = juce::jlimit (sibilance::minFrequency, std::min (sibilance::maxFrequency, maxHz()), hzFor (e.position.x));
            b->gainDb = juce::jlimit (sibilance::minGainDb, sibilance::maxGainDb, std::round (gainFor (e.position.y) * 2.0) / 2.0);
        }

        edited();
    }

    void edited()
    {
        if (onEdited != nullptr)
            onEdited();
    }

    // The stretch the progress bar spans: the S, widened to what is played around it.
    std::pair<double, double> playSpan (const Sibilant& s) const
    {
        static constexpr double around[] { 0.0, 0.5, 2.0, 0.5 };
        const auto context = std::max (0.03, around[juce::jlimit (0, 3, model.contextMode)]);
        auto t0 = std::max (0.0, s.start - context), t1 = std::min (model.duration(), s.end + context);

        if (liveTime.has_value())
        {
            t0 = std::min (t0, *liveTime);
            t1 = std::max (t1, *liveTime);
        }

        return { t0, t1 };
    }

    Model& model;
    std::vector<float> before, after, live;
    std::optional<double> liveTime;
    juce::Point<float> dragStart;
    double startQ = 2.0;
    bool draggingPoint = false;
    sibilance::Band startBand;
    std::vector<sibilance::Band> groupStart;
};

// The whole audio with a tick per sibilant (bluer = turned down more,
// orange = turned up); click one to go to it.
// The file (or, without it, the analysed audio) with the sibilants on it.
// A click picks the nearest S; a drag picks another stretch of the file.
class Timeline final : public juce::Component
{
public:
    explicit Timeline (Model& m) : model (m) {}

    std::function<void (int)> onPick;
    std::function<void (double start, double end)> onRange;   // seconds in the file

    void computePeaks()
    {
        if (showsFile())
        {
            peaks = model.source.filePeaks (600);
            repaint();
            return;
        }

        peaks.assign (600, 0.0f);
        const auto& a = model.source.audio;
        const auto total = a.getNumSamples();

        for (size_t c = 0; c < peaks.size() && total > 0; ++c)
        {
            const auto from = (int) ((juce::int64) total * (juce::int64) c / (juce::int64) peaks.size());
            const auto to = (int) ((juce::int64) total * (juce::int64) (c + 1) / (juce::int64) peaks.size());

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                peaks[c] = std::max (peaks[c], a.getMagnitude (ch, from, std::max (1, to - from)));
        }

        repaint();
    }

    void setPlayhead (std::optional<double> t)
    {
        if (t != playhead)
        {
            playhead = t;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (r, 6.0f);

        if (length() <= 0.0)
            return;

        const auto offset = showsFile() ? model.source.startInFile : 0.0;
        auto xAt = [&] (double t) { return r.getX() + r.getWidth() * (float) (t / length()); };

        // The stretch being worked on, when it is not the whole file.
        if (showsFile() && model.source.isSelection)
        {
            const auto area = juce::Rectangle<float>::leftTopRightBottom (xAt (offset), r.getY(), xAt (offset + model.duration()), r.getBottom());
            g.setColour (colours::accent.withAlpha (0.12f));
            g.fillRect (area);
            g.setColour (colours::accent.withAlpha (0.6f));
            g.drawRect (area, 1.0f);
        }

        const auto mid = r.getCentreY();
        g.setColour (colours::muted.withAlpha (0.45f));

        for (size_t c = 0; c < peaks.size(); ++c)
        {
            const auto x = r.getX() + r.getWidth() * (float) c / (float) peaks.size();
            const auto h = peaks[c] * (r.getHeight() * 0.45f);
            g.fillRect (x, mid - h, std::max (1.0f, r.getWidth() / (float) peaks.size()), h * 2.0f);
        }

        for (size_t i = 0; i < model.items.size(); ++i)
        {
            const auto& s = model.items[i];
            const auto x1 = xAt (offset + s.start);
            const auto x2 = xAt (offset + s.end);
            double lowest = 0.0, highest = 0.0;

            for (const auto& b : s.bands)
            {
                lowest = std::min (lowest, b.gainDb);
                highest = std::max (highest, b.gainDb);
            }

            const auto depth = (float) juce::jlimit (0.0, 1.0, -lowest / 12.0);
            const auto colour = ! s.changesSound() ? colours::muted
                              : lowest < -0.05 ? colours::accent.withMultipliedAlpha (0.45f + 0.55f * depth)
                                               : colours::bass;
            g.setColour (colour);
            g.fillRect (juce::Rectangle<float> (x1, r.getY() + 3.0f, std::max (2.0f, x2 - x1), r.getHeight() - 6.0f));

            if ((int) i == model.current)
            {
                g.setColour (colours::ink);
                g.drawRect (juce::Rectangle<float> (x1 - 2.0f, r.getY() + 1.0f, std::max (2.0f, x2 - x1) + 4.0f, r.getHeight() - 2.0f), 2.0f);
            }
        }

        if (playhead.has_value())
        {
            const auto x = xAt (offset + *playhead);
            g.setColour (colours::ok);
            g.fillRect (x - 1.5f, r.getY(), 3.0f, r.getHeight());
        }

        if (dragging)
        {
            const auto x1 = (float) std::min (dragFrom, dragTo), x2 = (float) std::max (dragFrom, dragTo);
            g.setColour (colours::ink.withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x1, r.getY(), x2, r.getBottom()));
            g.setColour (colours::ink);
            g.drawRect (juce::Rectangle<float>::leftTopRightBottom (x1, r.getY(), x2, r.getBottom()), 1.5f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragFrom = dragTo = e.x;
        dragging = false;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! showsFile() || onRange == nullptr)
            return;

        dragTo = juce::jlimit (0, getWidth(), e.x);
        dragging = dragging || std::abs (dragTo - dragFrom) > 4;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragging)
        {
            dragging = false;
            repaint();
            const auto a = secondsAt (std::min (dragFrom, dragTo)), b = secondsAt (std::max (dragFrom, dragTo));

            if (b - a >= 0.1)
                onRange (a, b);

            return;
        }

        // A click: the nearest S, if it is in the stretch being worked on.
        const auto duration = model.duration();
        const auto t = secondsAt (e.x) - (showsFile() ? model.source.startInFile : 0.0);

        if (model.items.empty() || duration <= 0.0 || onPick == nullptr || t < 0.0 || t > duration)
            return;

        int best = 0;
        double bestDistance = 1.0e9;

        for (size_t i = 0; i < model.items.size(); ++i)
        {
            const auto& s = model.items[i];
            const auto d = t < s.start ? s.start - t : t > s.end ? t - s.end : 0.0;

            if (d < bestDistance)
            {
                bestDistance = d;
                best = (int) i;
            }
        }

        onPick (best);
    }

private:
    bool showsFile() const    { return model.source.filePeaks != nullptr && model.source.fileDuration > 0.0; }
    double length() const     { return showsFile() ? model.source.fileDuration : model.duration(); }
    double secondsAt (int x) const   { return getWidth() > 0 ? juce::jlimit (0.0, 1.0, (double) x / getWidth()) * length() : 0.0; }

    Model& model;
    std::vector<float> peaks;
    std::optional<double> playhead;
    int dragFrom = 0, dragTo = 0;
    bool dragging = false;
};

//==============================================================================
class DeEsserPanel final : public juce::Component,
                           public SpaceTarget,
                           private juce::Timer
{
public:
    bool spacePressed() override   { model.playCurrent (true); return true; }   // when the panel hasn't the keyboard

    DeEsserPanel (BassFinderSource source, BassFinderHost host, std::function<void()> onCloseIn)
        : onClose (std::move (onCloseIn)), spectrumView (model), timeline (model)
    {
        model.source = std::move (source);
        model.host = std::move (host);
        model.changed = [this] { refresh(); };

        for (auto* l : { &title, &sensitivityLabel, &depthLabel, &position, &gainLabel, &freqLabel, &qLabel, &status, &hint, &applyNote })
            addAndMakeVisible (l);

        styleLabel (title, 14.0f, true);
        styleLabel (sensitivityLabel, 12.0f, false, colours::muted);
        styleLabel (depthLabel, 12.0f, false, colours::muted);
        styleLabel (position, 15.0f, true);
        position.setJustificationType (juce::Justification::centred);
        styleLabel (gainLabel, 12.0f, true, colours::muted);
        styleLabel (freqLabel, 12.0f, true, colours::muted);
        styleLabel (qLabel, 12.0f, true, colours::muted);
        styleLabel (status, 12.0f, false, colours::muted);
        styleLabel (hint, 11.5f, false, colours::muted);
        hint.setJustificationType (juce::Justification::topLeft);
        hint.setMinimumHorizontalScale (1.0f);   // wrap onto two lines instead of squeezing
        styleLabel (applyNote, 11.5f, false, colours::muted);
        applyNote.setJustificationType (juce::Justification::centredTop);
        applyNote.setText ("Changes every S in the list, not just the one shown", juce::dontSendNotification);

        styleLabel (rangeHint, 11.5f, false, colours::muted);
        rangeHint.setJustificationType (juce::Justification::centredRight);
        rangeHint.setText ("Drag on the bar below to work on another stretch", juce::dontSendNotification);
        addAndMakeVisible (rangeHint);
        showTitle();
        sensitivityLabel.setText ("Sensitivity", juce::dontSendNotification);
        depthLabel.setText ("New ones start at", juce::dontSendNotification);
        gainLabel.setText ("Louder / quieter", juce::dontSendNotification);
        freqLabel.setText ("Centre (left / right)", juce::dontSendNotification);
        qLabel.setText ("Width (open / closed)", juce::dontSendNotification);
        hint.setText ("Left / Right: previous / next S.  Up / Down: selected points 1 dB louder / quieter.  Space: hear the S, Shift+Space: "
                      "original.  Graph: drag a point (centre and gain), wheel or Shift+drag = width, double-click empty space = new point, "
                      "right-click a point = remove.  Ctrl+A or Shift+click: several points, dragged together; Esc: back to one.",
                      juce::dontSendNotification);

        auto setupSlider = [this] (juce::Slider& s, double lo, double hi, double step, const juce::String& suffix)
        {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
            s.setRange (lo, hi, step);
            s.setTextValueSuffix (suffix);
            s.setWantsKeyboardFocus (false);   // the arrows move between sibilants
            addAndMakeVisible (s);
        };

        setupSlider (sensitivity, 0.0, 100.0, 1.0, " %");
        sensitivity.setValue (50.0, juce::dontSendNotification);
        setupSlider (depth, sibilance::minGainDb, 0.0, 0.5, " dB");
        depth.setValue (-6.0, juce::dontSendNotification);
        setupSlider (gain, sibilance::minGainDb, sibilance::maxGainDb, 0.5, " dB");
        gain.setDoubleClickReturnValue (true, 0.0);
        setupSlider (frequency, sibilance::minFrequency, std::min (sibilance::maxFrequency, model.rate() * 0.48), 10.0, " Hz");
        frequency.setSkewFactorFromMidPoint (5500.0);
        setupSlider (q, sibilance::minQ, sibilance::maxQ, 0.1, {});
        q.setSkewFactorFromMidPoint (2.0);
        q.setDoubleClickReturnValue (true, 2.0);

        // With several points selected, the sliders move them all by the same amount.
        gain.onValueChange = [this]
        {
            if (auto* b = model.currentBand())
            {
                const auto step = gain.getValue() - b->gainDb;
                model.forSelected ([step] (sibilance::Band& x) { x.gainDb = juce::jlimit (sibilance::minGainDb, sibilance::maxGainDb, x.gainDb + step); });
                edited();
            }
        };
        frequency.onValueChange = [this]
        {
            if (auto* b = model.currentBand())
            {
                const auto ratio = frequency.getValue() / b->frequency;
                model.forSelected ([ratio] (sibilance::Band& x) { x.frequency = juce::jlimit (sibilance::minFrequency, sibilance::maxFrequency, x.frequency * ratio); });
                edited();
            }
        };
        q.onValueChange = [this]
        {
            if (auto* b = model.currentBand())
            {
                const auto ratio = q.getValue() / b->q;
                model.forSelected ([ratio] (sibilance::Band& x) { x.q = juce::jlimit (sibilance::minQ, sibilance::maxQ, x.q * ratio); });
                edited();
            }
        };

        selectAll.onClick = [this]
        {
            if (model.selection.size() > 1)
                model.selectOnly (model.band);
            else
                model.selectAllPoints();

            refresh();
        };

        addPoint.onClick = [this]
        {
            // A second point an octave-ish away from the picked one, at the same depth.
            if (auto* b = model.currentBand())
                spectrumView.addBand (b->frequency < 7000.0 ? b->frequency * 1.6 : b->frequency / 1.6, b->gainDb);
        };
        removePoint.onClick = [this]
        {
            // Removes every selected point (highest index first).
            auto picked = model.selection;
            std::sort (picked.rbegin(), picked.rend());

            for (auto i : picked)
                spectrumView.removeBand (i);
        };
        prevPoint.onClick = [this] { model.selectOnly (std::max (0, model.band - 1)); refresh(); };
        nextPoint.onClick = [this] { model.selectOnly (model.band + 1); refresh(); };
        for (int i = 0; i < contextButtons.size(); ++i)
        {
            auto* b = contextButtons[i];
            b->setClickingTogglesState (true);
            b->setRadioGroupId (202);
            b->setWantsKeyboardFocus (false);
            b->setColour (juce::TextButton::buttonOnColourId, colours::accent.withAlpha (0.3f));
            b->setColour (juce::TextButton::textColourOnId, colours::ink);
            b->onClick = [this, i] { if (contextButtons[i]->getToggleState()) model.contextMode = i; };
            addAndMakeVisible (b);
        }

        contextButtons[0]->setToggleState (true, juce::dontSendNotification);

        detectButton.onClick = [this]
        {
            model.sensitivity = sensitivity.getValue() / 100.0;
            model.defaultGain = depth.getValue();
            model.detect();
            touched = false;
            timeline.repaint();
            status.setText (juce::String ((int) model.items.size()) + " sibilants found.", juce::dontSendNotification);
        };

        prevButton.onClick = [this] { go (-1); };
        nextButton.onClick = [this] { go (1); };
        playProcessed.onClick = [this] { model.playCurrent (true); };
        playOriginal.onClick = [this] { model.playCurrent (false); };
        copyToAll.onClick = [this]
        {
            if (auto* s = model.currentItem())
            {
                const auto settings = *s;

                for (auto& other : model.items)
                {
                    other.bands = settings.bands;

                    // Each keeps its points placed the same way relative to its own peak.
                    for (auto& b : other.bands)
                        b.frequency = juce::jlimit (sibilance::minFrequency, sibilance::maxFrequency,
                                                    other.peakHz * b.frequency / settings.peakHz);
                }

                touched = true;
                status.setText ("These points copied to all " + juce::String ((int) model.items.size())
                                    + " (placed the same way from each one's own peak).",
                                juce::dontSendNotification);
                refresh();
            }
        };
        leave.onClick = [this]
        {
            if (auto* s = model.currentItem())
            {
                for (auto& b : s->bands)
                    b.gainDb = 0.0;

                edited();
            }
        };
        playAll.onClick = [this] { togglePlayAll (true); };
        playAllOriginal.onClick = [this] { togglePlayAll (false); };
        apply.onClick = [this] { applyToFile(); };
        wholeFile.onClick = [this] { pickRange (0.0, 0.0); };

        apply.setColour (juce::TextButton::buttonColourId, colours::accent);
        apply.setColour (juce::TextButton::textColourOffId, colours::onAccent);
        playProcessed.setColour (juce::TextButton::buttonColourId, colours::accent.withAlpha (0.25f));

        for (auto* b : { &detectButton, &prevButton, &nextButton, &playProcessed, &playOriginal, &copyToAll, &leave, &playAll,
                         &playAllOriginal, &apply, &addPoint, &removePoint, &prevPoint, &nextPoint, &selectAll, &wholeFile })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }

        styleLabel (contextLabel, 12.0f, true, colours::muted);
        contextLabel.setText ("Play", juce::dontSendNotification);
        addAndMakeVisible (contextLabel);
        styleLabel (pointLabel, 12.0f, true, colours::muted);
        addAndMakeVisible (pointLabel);

        spectrumView.onEdited = [this] { edited(); };
        timeline.onPick = [this] (int i) { model.select (i); model.playCurrent (true); };
        timeline.onRange = [this] (double start, double end) { pickRange (start, end); };
        addAndMakeVisible (spectrumView);
        addAndMakeVisible (timeline);

        setWantsKeyboardFocus (true);
        setSize (1100, 706);

        model.detect();
        timeline.computePeaks();
        status.setText (juce::String ((int) model.items.size()) + " sibilants found.", juce::dontSendNotification);
        startTimerHz (30);
    }

    ~DeEsserPanel() override   { model.stop(); }

    void close()
    {
        model.stop();

        if (model.source.showPlayhead != nullptr)
            model.source.showPlayhead (-1.0);

        if (onClose != nullptr)
            onClose();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        auto* s = model.currentItem();

        if (key == juce::KeyPress::leftKey)   { go (-1); return true; }
        if (key == juce::KeyPress::rightKey)  { go (1); return true; }

        if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
        {
            model.selectAllPoints();
            refresh();
            return true;
        }

        if (key == juce::KeyPress::escapeKey && model.selection.size() > 1)
        {
            model.selectOnly (model.band);
            refresh();
            return true;
        }

        if (s != nullptr && (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey))
        {
            const auto step = key == juce::KeyPress::upKey ? 1.0 : -1.0;
            model.forSelected ([step] (sibilance::Band& x) { x.gainDb = juce::jlimit (sibilance::minGainDb, sibilance::maxGainDb, x.gainDb + step); });
            edited();
            return true;
        }

        if (key.getKeyCode() == juce::KeyPress::spaceKey)
        {
            model.playCurrent (! key.getModifiers().isShiftDown());
            return true;
        }

        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::bg);
        paintCard (g, topCard);
        paintCard (g, editCard);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        topCard = area.removeFromTop (112);
        area.removeFromTop (10);
        editCard = area;

        auto t = topCard.reduced (14, 10);
        auto row = t.removeFromTop (24);
        wholeFile.setBounds (row.removeFromRight (110).reduced (0, 1));
        row.removeFromRight (10);
        rangeHint.setBounds (row.removeFromRight (320));
        title.setBounds (row);
        t.removeFromTop (4);
        row = t.removeFromTop (28);
        sensitivityLabel.setBounds (row.removeFromLeft (76));
        sensitivity.setBounds (row.removeFromLeft (230));
        row.removeFromLeft (16);
        depthLabel.setBounds (row.removeFromLeft (112));
        depth.setBounds (row.removeFromLeft (230));
        row.removeFromLeft (16);
        detectButton.setBounds (row.removeFromLeft (130));
        row.removeFromLeft (12);
        status.setBounds (row);
        t.removeFromTop (6);
        timeline.setBounds (t.removeFromTop (34));

        auto e = editCard.reduced (14);
        row = e.removeFromTop (34);
        prevButton.setBounds (row.removeFromLeft (110).reduced (2));
        nextButton.setBounds (row.removeFromRight (110).reduced (2));
        position.setBounds (row);
        e.removeFromTop (6);

        auto bottom = e.removeFromBottom (52);
        auto applyColumn = bottom.removeFromRight (300);
        apply.setBounds (applyColumn.removeFromTop (36).reduced (2));
        applyNote.setBounds (applyColumn);
        bottom = bottom.removeFromTop (36);
        hint.setBounds (e.removeFromBottom (34));
        e.removeFromBottom (6);

        auto controls = e.removeFromRight (330);
        e.removeFromRight (12);
        spectrumView.setBounds (e);

        row = controls.removeFromTop (28);
        prevPoint.setBounds (row.removeFromLeft (30).reduced (1));
        pointLabel.setBounds (row.removeFromLeft (110));
        nextPoint.setBounds (row.removeFromLeft (30).reduced (1));
        row.removeFromLeft (6);
        selectAll.setBounds (row.reduced (2));
        controls.removeFromTop (2);
        row = controls.removeFromTop (28);
        addPoint.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2));
        removePoint.setBounds (row.reduced (2));
        controls.removeFromTop (6);

        for (auto [label, slider] : { std::pair { &gainLabel, &gain }, std::pair { &freqLabel, &frequency }, std::pair { &qLabel, &q } })
        {
            label->setBounds (controls.removeFromTop (16));
            slider->setBounds (controls.removeFromTop (28));
            controls.removeFromTop (6);
        }

        controls.removeFromTop (2);
        row = controls.removeFromTop (32);
        playProcessed.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2));
        playOriginal.setBounds (row.reduced (2));
        controls.removeFromTop (2);
        row = controls.removeFromTop (26);
        contextLabel.setBounds (row.removeFromLeft (34));
        const auto w = row.getWidth() / contextButtons.size();

        for (auto* b : contextButtons)
            b->setBounds (row.removeFromLeft (w).reduced (1));

        controls.removeFromTop (2);
        controls.removeFromTop (2);
        leave.setBounds (controls.removeFromTop (30).reduced (2));
        copyToAll.setBounds (controls.removeFromTop (30).reduced (2));

        playAll.setBounds (bottom.removeFromLeft (210).reduced (2));
        playAllOriginal.setBounds (bottom.removeFromLeft (190).reduced (2));
    }

private:
    void showTitle()
    {
        auto text = "De-esser: " + model.source.title;

        if (model.source.isSelection)
            text << "  " << formatTime (model.source.startInFile) << " - " << formatTime (model.source.startInFile + model.duration());

        title.setText (text, juce::dontSendNotification);
        wholeFile.setEnabled (model.source.isSelection && model.source.otherRange != nullptr);
        rangeHint.setVisible (model.source.otherRange != nullptr);
    }

    // Works on another stretch of the file (end <= start = the whole file),
    // finding its sibilants afresh.
    void pickRange (double start, double end)
    {
        if (model.source.otherRange == nullptr)
            return;

        if (touched)
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Another stretch",
                                                "Your changes to these S's are not applied to the file yet. Leave them and go to the new stretch?",
                                                "Leave them", "Cancel", this,
                                                juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<DeEsserPanel> (this), start, end] (int ok)
                                                {
                                                    if (auto* p = safe.getComponent(); p != nullptr && ok != 0)
                                                    {
                                                        p->touched = false;
                                                        p->pickRange (start, end);
                                                    }
                                                }));
            return;
        }

        model.stop();
        auto next = model.source.otherRange (start, end);

        if (! next.has_value())
        {
            status.setText ("The file is closed.", juce::dontSendNotification);
            return;
        }

        model.source = std::move (*next);
        showTitle();
        model.detect();
        touched = false;
        timeline.computePeaks();
        status.setText (juce::String ((int) model.items.size()) + " sibilants found.", juce::dontSendNotification);
    }

    void go (int direction)
    {
        if (model.items.empty())
            return;

        model.select (juce::jlimit (0, (int) model.items.size() - 1, model.current + direction));
        model.playCurrent (true);
    }

    void edited()
    {
        touched = true;
        refresh();
    }

    bool touched = false;   // S's changed by hand since they were found

    void refresh()
    {
        auto* s = model.currentItem();

        for (auto* c : { (juce::Component*) &gain, (juce::Component*) &frequency, (juce::Component*) &q, (juce::Component*) &prevButton,
                         (juce::Component*) &nextButton, (juce::Component*) &playProcessed, (juce::Component*) &playOriginal,
                         (juce::Component*) &copyToAll, (juce::Component*) &leave, (juce::Component*) &addPoint,
                         (juce::Component*) &removePoint, (juce::Component*) &prevPoint, (juce::Component*) &nextPoint })
            c->setEnabled (s != nullptr);

        if (auto* b = model.currentBand(); s != nullptr && b != nullptr)
        {
            gain.setValue (b->gainDb, juce::dontSendNotification);
            frequency.setValue (b->frequency, juce::dontSendNotification);
            q.setValue (b->q, juce::dontSendNotification);
            pointLabel.setText (model.selection.size() > 1
                                    ? juce::String ((int) model.selection.size()) + " of " + juce::String ((int) s->bands.size()) + " selected"
                                    : "Point " + juce::String (model.band + 1) + " of " + juce::String ((int) s->bands.size()),
                                juce::dontSendNotification);
            selectAll.setButtonText (model.selection.size() > 1 ? "Just this point" : "Select all points");
            selectAll.setEnabled (s->bands.size() > 1 || model.selection.size() > 1);
            prevPoint.setEnabled (model.band > 0);
            nextPoint.setEnabled (model.band < (int) s->bands.size() - 1);
            addPoint.setEnabled ((int) s->bands.size() < sibilance::maxBands);
            prevButton.setEnabled (model.current > 0);
            nextButton.setEnabled (model.current < (int) model.items.size() - 1);
            position.setText ("S " + juce::String (model.current + 1) + " of " + juce::String ((int) model.items.size()) + "   |   "
                                  + formatTime (model.source.startInFile + s->start) + " - " + formatTime (model.source.startInFile + s->end) + "   |   its peak " + formatHz (s->peakHz),
                              juce::dontSendNotification);
        }
        else
        {
            position.setText ("No sibilants", juce::dontSendNotification);
        }

        int changedCount = 0;

        for (auto& item : model.items)
            if (item.changesSound())
                ++changedCount;

        // Says plainly that it changes every S found, not just the one on show.
        const auto where = model.source.isSelection ? " in the selection" : " in the file";
        apply.setEnabled (changedCount > 0);
        apply.setButtonText (changedCount == 1 ? "Apply to the 1 S" + juce::String (where)
                                               : "Apply to all " + juce::String (changedCount) + " S's" + where);
        spectrumView.refresh();
        timeline.repaint();
    }

    // The whole audio (selection, or file), processed or original; again = stop.
    void togglePlayAll (bool processed)
    {
        if (model.playing && playingAll == processed)
        {
            model.stop();
            return;
        }

        playingAll = processed;
        model.play (processed ? sibilance::process (model.source.audio, model.rate(), model.items) : model.source.audio, 0.0);
    }

    bool playingAll = false;

    void applyToFile()
    {
        if (model.source.applyToFile == nullptr)
            return;

        model.stop();
        auto processed = sibilance::process (model.source.audio, model.rate(), model.items);

        if (! model.source.applyToFile (processed))
        {
            status.setText ("The file is closed: can't apply.", juce::dontSendNotification);
            return;
        }

        // The file now has them treated: look again from there.
        model.source.audio = std::move (processed);
        model.detect();
        touched = false;
        timeline.computePeaks();
        status.setText ("Applied to the file (Ctrl+Z undoes it). " + juce::String ((int) model.items.size()) + " sibilants left above the threshold.",
                        juce::dontSendNotification);
    }

    // What plays is shown live in the graph (and as the playhead in the file's
    // own window).
    void timerCallback() override
    {
        auto t = model.playSeconds();

        if (! t.has_value() && model.playing && model.host.playPosition != nullptr && model.host.playPosition() < 0)
            model.playing = false;

        spectrumView.setLive (liveSpectrum(), t);

        if (t != shownInFile && model.source.showPlayhead != nullptr)
        {
            model.source.showPlayhead (t.value_or (-1.0));
            shownInFile = t;
        }
    }

    std::optional<double> shownInFile;

    // The spectrum of the last ~46 ms played.
    std::vector<float> liveSpectrum() const
    {
        if (! model.playing || model.playedAudio == nullptr || model.host.playPosition == nullptr)
            return {};

        const auto p = model.host.playPosition();

        if (p < 0 || p >= model.playLength)
            return {};

        const auto end = (double) p / model.rate();
        return sibilance::spectrum (*model.playedAudio, model.rate(), std::max (0.0, end - 2048.0 / model.rate()), end);
    }

    Model model;
    std::function<void()> onClose;

    juce::TextButton wholeFile { "Whole file" };
    juce::Label rangeHint;
    juce::Label title, sensitivityLabel, depthLabel, position, gainLabel, freqLabel, qLabel, status, hint, pointLabel, applyNote;
    juce::Slider sensitivity, depth, gain, frequency, q;
    juce::TextButton detectButton { "Detect again" }, prevButton { juce::CharPointer_UTF8 ("\xe2\x97\x80  Previous") },
                     nextButton { juce::CharPointer_UTF8 ("Next  \xe2\x96\xb6") },
                     playProcessed { juce::CharPointer_UTF8 ("\xe2\x96\xb6 This S, processed") },
                     playOriginal { juce::CharPointer_UTF8 ("\xe2\x96\xb6 This S, original") },
                     copyToAll { "Use these points for all" }, leave { "Leave this S as it is (0 dB)" },
                     playAll { juce::CharPointer_UTF8 ("\xe2\x96\xb6 / \xe2\x96\xa0  All, processed") },
                     playAllOriginal { juce::CharPointer_UTF8 ("\xe2\x96\xb6 / \xe2\x96\xa0  All, original") }, apply { "Apply to file" },
                     addPoint { "+ Add point" }, removePoint { juce::CharPointer_UTF8 ("\xe2\x88\x92 Remove point") },
                     prevPoint { "<" }, nextPoint { ">" }, selectAll { "Select all points" };
    juce::OwnedArray<juce::TextButton> contextButtons { new juce::TextButton ("Only the S"),
                                                        new juce::TextButton (juce::CharPointer_UTF8 ("\xc2\xb1 0.5 s")),
                                                        new juce::TextButton (juce::CharPointer_UTF8 ("\xc2\xb1 2 s")),
                                                        new juce::TextButton ("From here on") };
    juce::Label contextLabel;
    SpectrumView spectrumView;
    Timeline timeline;
    juce::Rectangle<int> topCard, editCard;
};

class DeEsserWindow final : public juce::DocumentWindow
{
public:
    DeEsserWindow (BassFinderSource source, BassFinderHost host, std::function<void()> onClose)
        : juce::DocumentWindow ("De-esser", colours::bg, juce::DocumentWindow::closeButton)
    {
        // Own title bar (movable without window decorations), fixed size so
        // tiling window managers float it.
        setUsingNativeTitleBar (false);
        setLookAndFeel (&look);
        setContentOwned (new DeEsserPanel (std::move (source), std::move (host), std::move (onClose)), true);
        dark::adoptLook (*getContentComponent());
        setResizable (false, false);
        // Stays in front of the main window when you click there.
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    ~DeEsserWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    void closeButtonPressed() override
    {
        if (auto* p = dynamic_cast<DeEsserPanel*> (getContentComponent()))
            p->close();
    }

    void visibilityChanged() override
    {
        juce::DocumentWindow::visibilityChanged();

        if (isShowing())
            if (auto* p = getContentComponent())
                p->grabKeyboardFocus();
    }

private:
    dark::DarkLook look;
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createDeEsserWindow (BassFinderSource source, BassFinderHost host, std::function<void()> onClose)
{
    return std::make_unique<DeEsserWindow> (std::move (source), std::move (host), std::move (onClose));
}
} // namespace wf
