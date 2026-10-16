#include "MeterPanel.h"
#include "DarkTheme.h"

namespace wf
{
//==============================================================================
void MeterTap::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                                 int numSamples, const juce::AudioIODeviceCallbackContext& context)
{
    inner.audioDeviceIOCallbackWithContext (inputs, numInputs, outputs, numOutputs, numSamples, context);

    if (! enabled.load() || numOutputs <= 0)
        return;

    const auto toWrite = juce::jmin (numSamples, fifo.getFreeSpace());

    if (toWrite <= 0)
        return;   // the meters fell behind: drop rather than block

    const auto scope = fifo.write (toWrite);

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* source = outputs[juce::jmin (ch, numOutputs - 1)];

        if (source == nullptr)
            continue;

        if (scope.blockSize1 > 0) ring.copyFrom (ch, scope.startIndex1, source, scope.blockSize1);
        if (scope.blockSize2 > 0) ring.copyFrom (ch, scope.startIndex2, source + scope.blockSize1, scope.blockSize2);
    }
}

void MeterTap::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    inner.audioDeviceAboutToStart (device);
    sampleRate.store (device != nullptr ? device->getCurrentSampleRate() : 0.0);
}

void MeterTap::audioDeviceStopped()
{
    inner.audioDeviceStopped();
    sampleRate.store (0.0);
}

int MeterTap::read (juce::AudioBuffer<float>& destination)
{
    const auto toRead = juce::jmin (fifo.getNumReady(), destination.getNumSamples());

    if (toRead <= 0)
        return 0;

    const auto scope = fifo.read (toRead);

    for (int ch = 0; ch < 2; ++ch)
    {
        if (scope.blockSize1 > 0) destination.copyFrom (ch, 0, ring, ch, scope.startIndex1, scope.blockSize1);
        if (scope.blockSize2 > 0) destination.copyFrom (ch, scope.blockSize1, ring, ch, scope.startIndex2, scope.blockSize2);
    }

    return toRead;
}

//==============================================================================
namespace
{
using namespace dark::colours;

constexpr double floorDb = -100.0;
constexpr double fallPerSecond = 24.0;   // dB/s, bars and the hold line
constexpr double holdSeconds = 2.0;
constexpr int historyLength = 600;       // 60 s of short-term values, 10 per second

struct Target
{
    const char* name;
    double lufs;
};

const Target targets[] {
    { "-14 LUFS  Streaming", -14.0 },
    { "-16 LUFS  Apple, podcast", -16.0 },
    { "-23 LUFS  EBU R128 (TV)", -23.0 },
    { "-24 LUFS  ATSC A/85 (US TV)", -24.0 },
};

double toDb (double gain)   { return gain > 1.0e-5 ? 20.0 * std::log10 (gain) : floorDb; }

// IEC 60268-18 meter scale: dB -> 0..1 of the bar height.
float deflection (double db)
{
    double pct;

    if      (db < -70.0) pct = 0.0;
    else if (db < -60.0) pct = (db + 70.0) * 0.25;
    else if (db < -50.0) pct = (db + 60.0) * 0.5 + 2.5;
    else if (db < -40.0) pct = (db + 50.0) * 0.75 + 7.5;
    else if (db < -30.0) pct = (db + 40.0) * 1.5 + 15.0;
    else if (db < -20.0) pct = (db + 30.0) * 2.0 + 30.0;
    else                 pct = (db + 20.0) * 2.5 + 50.0;

    return (float) juce::jlimit (0.0, 1.0, pct / 100.0);
}

juce::String levelText (double db)
{
    return db <= -99.0 ? juce::String ("-inf") : juce::String (db, 1);
}

juce::String lufsText (double lufs)
{
    return lufs <= -70.0 ? juce::String ("-inf") : juce::String (lufs, 1);
}

dark::DarkLook& panelLook()
{
    static dark::DarkLook look;
    return look;
}
} // namespace

MeterPanel::MeterPanel (MeterTap& t) : tap (t)
{
    setLookAndFeel (&panelLook());
    setOpaque (true);

    int id = 1;

    for (const auto& item : targets)
        target.addItem (item.name, id++);

    target.setSelectedId (1, juce::dontSendNotification);
    target.setTooltip ("The loudness the Integrated value is compared with");
    target.onChange = [this] { repaint(); };
    addAndMakeVisible (target);

    resetButton.setTooltip ("Start measuring again (also happens when playback starts)");
    resetButton.onClick = [this] { resetLoudness(); };
    addAndMakeVisible (resetButton);

    setTooltip ("Bars: peak (light) and RMS (solid) of what is playing. The numbers on top are the highest "
                "peak since reset (CLIP = reached 0 dB); click the meters to reset them.");
    history.reserve (historyLength);
    tap.setEnabled (true);
    startTimerHz (30);
}

MeterPanel::~MeterPanel()
{
    tap.setEnabled (false);
    setLookAndFeel (nullptr);
}

void MeterPanel::setSections (bool channelMeters, bool loudnessMeters)
{
    channelsShown = channelMeters;
    loudnessShown = loudnessMeters;
    tap.setEnabled (showsAnything());

    if (showsAnything())
        startTimerHz (30);
    else
        stopTimer();

    resized();
    repaint();
}

void MeterPanel::resetLoudness()
{
    loudness.reset();
    history.clear();
    historyTick = 0;
    resetPeaks();
}

void MeterPanel::resetPeaks()
{
    for (auto& s : state)
    {
        s.maxPeak = s.hold = floorDb;
        s.clipped = false;
    }

    repaint();
}

void MeterPanel::timerCallback()
{
    const auto rate = tap.getSampleRate();

    if (rate > 0.0 && ! juce::approximatelyEqual (rate, preparedRate))
    {
        preparedRate = rate;
        loudness.prepare (rate, 2);
        history.clear();
    }

    constexpr double dt = 1.0 / 30.0;
    const auto n = tap.read (incoming);

    if (n > 0 && preparedRate > 0.0)
    {
        loudness.process (incoming, 0, n);
        const auto alpha = 1.0 - std::exp (-1.0 / (0.3 * preparedRate));

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& s = state[ch];
            const auto* x = incoming.getReadPointer (ch);
            float blockPeak = 0.0f;
            auto ms = s.meanSquare;

            for (int i = 0; i < n; ++i)
            {
                blockPeak = juce::jmax (blockPeak, std::abs (x[i]));
                ms += ((double) x[i] * x[i] - ms) * alpha;
            }

            s.meanSquare = ms;
            const auto db = toDb (blockPeak);
            s.peak = juce::jmax (db, s.peak - fallPerSecond * dt);
            s.maxPeak = juce::jmax (s.maxPeak, db);
            s.clipped = s.clipped || blockPeak >= 0.999f;

            if (db >= s.hold)
            {
                s.hold = db;
                s.holdTime = holdSeconds;
            }
        }

        // One short-term value per 100 ms of audio.
        historyTick += n;
        const auto step = juce::roundToInt (preparedRate / 10.0);

        while (historyTick >= step)
        {
            historyTick -= step;

            if ((int) history.size() >= historyLength)
                history.erase (history.begin());

            history.push_back ((float) loudness.shortTerm());
        }
    }
    else
    {
        for (auto& s : state)
        {
            s.peak = juce::jmax (floorDb, s.peak - fallPerSecond * dt);
            s.meanSquare *= 0.7;
        }
    }

    for (auto& s : state)
    {
        s.holdTime -= dt;

        if (s.holdTime <= 0.0)
            s.hold = juce::jmax (s.peak, s.hold - fallPerSecond * dt);
    }

    repaint();
}

//==============================================================================
void MeterPanel::resized()
{
    auto area = getLocalBounds();
    const auto loudnessHeight = 330;

    if (loudnessShown)
        loudnessArea = channelsShown ? area.removeFromBottom (juce::jmin (loudnessHeight, area.getHeight() / 2 + 60)) : area;
    else
        loudnessArea = {};

    channelsArea = channelsShown ? area : juce::Rectangle<int>();

    target.setVisible (loudnessShown);
    resetButton.setVisible (loudnessShown);

    if (loudnessShown)
    {
        auto inner = loudnessArea.reduced (10, 8);
        auto header = inner.removeFromTop (22);
        header.removeFromLeft (80);
        target.setBounds (header);
        resetButton.setBounds (inner.removeFromBottom (24).removeFromRight (80));
    }
}

void MeterPanel::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (line);
    g.drawVerticalLine (0, 0.0f, (float) getHeight());

    if (channelsShown && ! channelsArea.isEmpty())
        paintChannels (g, channelsArea);

    if (loudnessShown && ! loudnessArea.isEmpty())
    {
        if (channelsShown)
        {
            g.setColour (line);
            g.drawHorizontalLine (loudnessArea.getY(), 0.0f, (float) getWidth());
        }

        paintLoudness (g, loudnessArea);
    }
}

void MeterPanel::paintChannels (juce::Graphics& g, juce::Rectangle<int> area)
{
    area = area.reduced (10, 8);

    g.setColour (ink);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("Channel Meters", area.removeFromTop (20), juce::Justification::centredLeft);

    // Highest peak since reset; red when the channel clipped. Click resets.
    peakNumbersArea = area.removeFromTop (22);
    const auto scaleWidth = 34;
    const auto barWidth = (area.getWidth() - scaleWidth - 12) / 2;
    auto numbers = peakNumbersArea;
    numbers.removeFromLeft (scaleWidth);

    for (int ch = 0; ch < 2; ++ch)
    {
        auto box = numbers.removeFromLeft (barWidth).reduced (1, 2);
        numbers.removeFromLeft (12);
        g.setColour (state[ch].clipped ? bad : surface);
        g.fillRoundedRectangle (box.toFloat(), 3.0f);
        g.setColour (state[ch].clipped ? onAccent : ink);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (state[ch].clipped ? juce::String ("CLIP") : levelText (state[ch].maxPeak), box, juce::Justification::centred);
    }

    auto footer = area.removeFromBottom (36);
    area.removeFromTop (4);
    auto bars = area;
    auto scale = bars.removeFromLeft (scaleWidth);

    // Scale
    g.setFont (juce::FontOptions (11.0f));

    for (const auto db : { 0, -3, -6, -10, -15, -20, -30, -40, -50, -60 })
    {
        const auto y = (float) bars.getBottom() - deflection (db) * (float) bars.getHeight();
        g.setColour (muted);
        g.drawText (juce::String (db), juce::Rectangle<float> (0.0f, y - 6.0f, (float) scale.getWidth() - 4.0f, 12.0f)
                                           .withX ((float) scale.getX()), juce::Justification::centredRight);
        g.setColour (line);
        g.drawHorizontalLine (juce::roundToInt (y), (float) bars.getX(), (float) bars.getRight());
    }

    const juce::Colour green (0xff3fcf6e), yellow (0xfff2d54b), red (0xffff5a52);

    for (int ch = 0; ch < 2; ++ch)
    {
        auto bar = bars.removeFromLeft (barWidth).toFloat().reduced (1.0f, 0.0f);
        bars.removeFromLeft (12);
        const auto& s = state[ch];

        g.setColour (surface);
        g.fillRect (bar);

        juce::ColourGradient gradient (green, 0.0f, bar.getBottom(), red, 0.0f, bar.getY(), false);
        gradient.addColour (deflection (-18.0), green);
        gradient.addColour (deflection (-9.0), yellow);
        gradient.addColour (deflection (-3.0), juce::Colour (0xffff9a3d));

        const auto rmsDb = s.meanSquare > 1e-10 ? 10.0 * std::log10 (s.meanSquare) : floorDb;
        const auto peakTop = bar.getBottom() - deflection (s.peak) * bar.getHeight();
        const auto rmsTop = bar.getBottom() - deflection (rmsDb) * bar.getHeight();

        g.saveState();
        g.setGradientFill (gradient);
        g.setOpacity (0.45f);
        g.fillRect (bar.withTop (peakTop));
        g.setOpacity (1.0f);
        g.fillRect (bar.withTop (rmsTop));
        g.restoreState();

        if (s.hold > -70.0)
        {
            const auto y = bar.getBottom() - deflection (s.hold) * bar.getHeight();
            g.setColour (s.hold >= -0.1 ? red : ink);
            g.fillRect (bar.getX(), y - 1.0f, bar.getWidth(), 2.0f);
        }

        // Under the bar: channel name, then peak / RMS now.
        auto under = footer.withX ((int) bar.getX()).withWidth ((int) bar.getWidth());
        g.setColour (ink);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (ch == 0 ? "L" : "R", under.removeFromTop (16), juce::Justification::centred);
        g.setColour (muted);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (levelText (s.peak) + " / " + levelText (rmsDb), under, juce::Justification::centred);
    }

}

juce::Colour MeterPanel::integratedColour (double lufs) const
{
    if (lufs <= -70.0)
        return muted;

    const auto goal = targets[juce::jmax (0, target.getSelectedItemIndex())].lufs;
    const auto diff = lufs - goal;

    if (std::abs (diff) <= 1.0) return ok;
    return diff > 0.0 ? warn : accent;
}

void MeterPanel::paintLoudness (juce::Graphics& g, juce::Rectangle<int> area)
{
    area = area.reduced (10, 8);
    auto header = area.removeFromTop (22);
    g.setColour (ink);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("Loudness", header, juce::Justification::centredLeft);
    area.removeFromTop (6);
    area.removeFromBottom (30);   // Reset button

    const auto goal = targets[juce::jmax (0, target.getSelectedItemIndex())].lufs;

    auto row = [&] (const juce::String& name, const juce::String& value, const juce::String& unit, juce::Colour colour, float size)
    {
        auto r = area.removeFromTop ((int) size + 8);
        g.setColour (muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (name, r.removeFromLeft (84), juce::Justification::centredLeft);
        auto unitBox = r.removeFromRight (40);
        g.drawText (unit, unitBox, juce::Justification::centredLeft);
        g.setColour (colour);
        g.setFont (juce::FontOptions (size, juce::Font::bold));
        g.drawText (value, r.withTrimmedRight (6), juce::Justification::centredRight);
    };

    const auto integrated = loudness.integrated();
    const auto tp = loudness.truePeak();

    row ("Momentary", lufsText (loudness.momentary()), "LUFS", ink, 15.0f);
    row ("Short-term", lufsText (loudness.shortTerm()), "LUFS", ink, 15.0f);
    row ("Integrated", lufsText (integrated), "LUFS", integratedColour (integrated), 22.0f);

    if (integrated > -70.0)
    {
        const auto diff = integrated - goal;
        g.setColour (integratedColour (integrated));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (std::abs (diff) <= 1.0 ? juce::String ("on target")
                                            : juce::String (std::abs (diff), 1) + (diff > 0.0 ? " LU above target" : " LU below target"),
                    area.removeFromTop (14), juce::Justification::centredRight);
    }
    row ("Range (LRA)", juce::String (loudness.loudnessRange(), 1), "LU", ink, 14.0f);
    row ("True peak", tp <= -99.0 ? juce::String ("-inf") : juce::String (tp, 1), "dBTP", tp > -1.0 ? bad : ink, 14.0f);

    // Last minute of short-term loudness, around the target.
    area.removeFromTop (4);
    auto graph = area.toFloat();

    if (graph.getHeight() < 30.0f)
        return;

    g.setColour (surface);
    g.fillRoundedRectangle (graph, 3.0f);

    const auto top = goal + 8.0, bottom = goal - 22.0;
    auto yOf = [&] (double lufs) { return graph.getY() + (float) ((top - juce::jlimit (bottom, top, lufs)) / (top - bottom)) * graph.getHeight(); };

    const auto goalY = yOf (goal);
    g.setColour (ok.withAlpha (0.6f));
    const float dashes[] { 4.0f, 3.0f };
    g.drawDashedLine (juce::Line<float> (graph.getX(), goalY, graph.getRight(), goalY), dashes, 2);
    g.setColour (muted);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText (lufsText (goal), graph.withHeight (12.0f).withY (goalY - 13.0f).reduced (4.0f, 0.0f), juce::Justification::centredRight);
    g.drawText ("last 60 s", graph.reduced (4.0f, 2.0f), juce::Justification::bottomLeft);

    if (history.size() >= 2)
    {
        juce::Path path;
        const auto dx = graph.getWidth() / (float) (historyLength - 1);
        const auto startX = graph.getRight() - dx * (float) (history.size() - 1);
        bool started = false;

        for (size_t i = 0; i < history.size(); ++i)
        {
            if (history[i] <= -70.0f)
            {
                started = false;
                continue;
            }

            const auto p = juce::Point<float> (startX + dx * (float) i, yOf (history[i]));

            if (started) path.lineTo (p);
            else         path.startNewSubPath (p);

            started = true;
        }

        g.setColour (accent);
        g.strokePath (path, juce::PathStrokeType (1.6f));
    }
}

void MeterPanel::mouseDown (const juce::MouseEvent& e)
{
    if (channelsShown && channelsArea.contains (e.getPosition()))
        resetPeaks();
}
} // namespace wf
