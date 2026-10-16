#include "Recorder.h"

namespace wf
{
InputRecorder::InputRecorder()
{
    for (auto& p : peak)        p = 0.0f;
    for (auto& s : sumSquares)  s = 0.0f;
}

void InputRecorder::setGainDecibels (float decibels)
{
    gainDecibels = decibels;
    gain = juce::Decibels::decibelsToGain (decibels, -100.0f);
}

void InputRecorder::setRecording (bool shouldRecord)
{
    if (shouldRecord)
        fifo.reset();   // a new take starts empty (the audio thread only writes while recording)

    recording = shouldRecord;
}

InputRecorder::Levels InputRecorder::takeLevels()
{
    Levels l;
    const auto count = std::max (1, squaresCount.exchange (0));

    for (int ch = 0; ch < 2; ++ch)
    {
        l.peak[ch] = peak[ch].exchange (0.0f);
        l.rms[ch] = std::sqrt (sumSquares[ch].exchange (0.0f) / (float) count);
    }

    l.clipped = clipped.exchange (false);
    return l;
}

int InputRecorder::drain (juce::AudioBuffer<float>& destination)
{
    const auto ready = fifo.getNumReady();
    destination.setSize (2, ready, false, false, true);

    if (ready == 0)
        return 0;

    int start1, size1, start2, size2;
    fifo.prepareToRead (ready, start1, size1, start2, size2);

    for (int ch = 0; ch < 2; ++ch)
    {
        if (size1 > 0) destination.copyFrom (ch, 0, queue, ch, start1, size1);
        if (size2 > 0) destination.copyFrom (ch, size1, queue, ch, start2, size2);
    }

    fifo.finishedRead (size1 + size2);
    return size1 + size2;
}

juce::String InputRecorder::getDeviceName() const
{
    const juce::ScopedLock sl (nameLock);
    return deviceName;
}

void InputRecorder::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs,
                                                      int numOutputs, int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    const auto g = gain.load();
    auto channels = std::min (2, numInputs);
    bool clip = false;

    if (channels == 2 && ! rightHasSignal && inputs[1] != nullptr)
    {
        for (int i = 0; i < numSamples && ! rightHasSignal; ++i)
            rightHasSignal = std::abs (inputs[1][i]) > 0.0f;   // exactly zero = nothing connected

        if (! rightHasSignal)
            channels = 1;   // the copy below takes the left input for both
    }

    for (int done = 0; done < numSamples;)
    {
        const auto n = std::min (numSamples - done, scratch.getNumSamples());

        // The input with the level applied; one input fills both channels.
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* out = scratch.getWritePointer (ch);
            const auto* in = channels > 0 ? inputs[std::min (ch, channels - 1)] : nullptr;

            if (in == nullptr)
            {
                juce::FloatVectorOperations::clear (out, n);
                continue;
            }

            juce::FloatVectorOperations::multiply (out, in + done, g, n);

            float p = 0.0f, s = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                const auto a = std::abs (out[i]);
                p = std::max (p, a);
                s += out[i] * out[i];
            }

            if (p >= 0.999f)
                clip = true;

            // Keep the highest peak until the meter reads it.
            auto old = peak[ch].load();
            while (p > old && ! peak[ch].compare_exchange_weak (old, p)) {}
            sumSquares[ch] = sumSquares[ch].load() + s;
        }

        squaresCount += n;

        if (recording.load())
        {
            int start1, size1, start2, size2;
            fifo.prepareToWrite (n, start1, size1, start2, size2);

            if (size1 + size2 < n)
                overflow = true;

            for (int ch = 0; ch < 2; ++ch)
            {
                if (size1 > 0) queue.copyFrom (ch, start1, scratch, ch, 0, size1);
                if (size2 > 0) queue.copyFrom (ch, start2, scratch, ch, size1, size2);
            }

            fifo.finishedWrite (size1 + size2);
        }

        done += n;
    }

    if (clip)
        clipped = true;
}

void InputRecorder::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    rightHasSignal = false;   // a new device: find out again whether it is really stereo

    inputChannels = device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 0;
    sampleRate = device != nullptr ? device->getCurrentSampleRate() : 44100.0;

    const juce::ScopedLock sl (nameLock);
    deviceName = device != nullptr ? device->getName() : juce::String();
}

void InputRecorder::audioDeviceStopped()
{
    inputChannels = 0;
}

namespace
{
const juce::Colour panel (0xffd4d0c8);

// Horizontal level bars (L, R) from -60 to 0 dB with peak hold and a clip lamp.
class LevelMeter final : public juce::Component
{
public:
    void update (const InputRecorder::Levels& l, int channelCount, bool active)
    {
        channels = channelCount;
        enabled = active;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto dt = lastUpdate > 0.0 ? (float) (now - lastUpdate) / 1000.0f : 0.0f;
        lastUpdate = now;

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto p = toDb (l.peak[ch]);
            const auto r = toDb (l.rms[ch]);

            // Fast up, about 24 dB per second down.
            peakDb[ch] = p > peakDb[ch] ? p : std::max (p, peakDb[ch] - 24.0f * dt);
            rmsDb[ch] = r > rmsDb[ch] ? r : std::max (r, rmsDb[ch] - 24.0f * dt);

            if (p >= holdDb[ch] || now - holdTime[ch] > 1500.0)
            {
                holdDb[ch] = p;
                holdTime[ch] = now;
            }

            maxDb[ch] = std::max (maxDb[ch], p);
        }

        if (l.clipped)
            clip = true;

        repaint();
    }

    void reset()
    {
        clip = false;

        for (int ch = 0; ch < 2; ++ch)
            maxDb[ch] = minDb;

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1c1f26));
        g.fillRoundedRectangle (area, 6.0f);

        auto inner = area.reduced (10.0f, 8.0f);
        auto lampArea = inner.removeFromRight (54.0f);
        inner.removeFromRight (8.0f);
        auto labels = inner.removeFromLeft (16.0f);
        auto scale = inner.removeFromBottom (16.0f);
        const auto rows = channels >= 2 ? 2 : 1;
        const auto barHeight = (inner.getHeight() - 6.0f * (float) (rows - 1)) / (float) rows;

        for (int ch = 0; ch < rows; ++ch)
        {
            const auto bar = juce::Rectangle<float> (inner.getX(), inner.getY() + (float) ch * (barHeight + 6.0f), inner.getWidth(), barHeight);
            g.setColour (juce::Colour (0xff2c3038));
            g.fillRoundedRectangle (bar, 3.0f);

            g.setColour (juce::Colour (0xffb8bcc6));
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (rows == 1 ? "M" : ch == 0 ? "L" : "R", labels.withY (bar.getY()).withHeight (bar.getHeight()),
                        juce::Justification::centredLeft);

            if (! enabled)
                continue;

            // Peak bar in three zones, RMS as a brighter core.
            const auto peakX = xFor (peakDb[ch], bar);
            const struct { float from, to; juce::Colour colour; } zones[] {
                { minDb, -12.0f, juce::Colour (0xff3ec46d) }, { -12.0f, -3.0f, juce::Colour (0xffe6c229) },
                { -3.0f, 0.0f, juce::Colour (0xffe8463a) } };

            for (auto& z : zones)
            {
                const auto x1 = xFor (z.from, bar), x2 = std::min (xFor (z.to, bar), peakX);

                if (x2 > x1)
                {
                    g.setColour (z.colour.withAlpha (0.75f));
                    g.fillRect (juce::Rectangle<float> (x1, bar.getY() + 2.0f, x2 - x1, bar.getHeight() - 4.0f));
                }
            }

            const auto rmsX = xFor (rmsDb[ch], bar);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (bar.getX(), bar.getCentreY() - 2.0f, rmsX - bar.getX(), 4.0f));

            g.setColour (juce::Colours::white);
            g.fillRect (juce::Rectangle<float> (xFor (holdDb[ch], bar) - 1.0f, bar.getY() + 1.0f, 2.0f, bar.getHeight() - 2.0f));
        }

        // dB scale.
        g.setFont (juce::FontOptions (10.0f));

        for (auto db : { -60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f, 0.0f })
        {
            const auto x = xFor (db, inner);
            g.setColour (juce::Colour (0xff6c7280));
            g.fillRect (x, inner.getY(), 1.0f, inner.getHeight());
            g.setColour (juce::Colour (0xffb8bcc6));
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (x - 15.0f, scale.getY() + 2.0f, 30.0f, 12.0f),
                        juce::Justification::centred);
        }

        // Clip lamp and the highest peak so far (click to reset both).
        auto lamp = lampArea.removeFromTop (lampArea.getHeight() * 0.5f).reduced (2.0f);
        g.setColour (clip ? juce::Colour (0xffff3b30) : juce::Colour (0xff4a2a2a));
        g.fillRoundedRectangle (lamp, 4.0f);
        g.setColour (clip ? juce::Colours::white : juce::Colour (0xff9a7a7a));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("CLIP", lamp, juce::Justification::centred);

        const auto highest = std::max (maxDb[0], rows > 1 ? maxDb[1] : minDb);
        g.setColour (juce::Colour (0xffe8eaf2));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (enabled && highest > minDb ? juce::String (highest, 1) + " dB" : juce::String ("-inf"), lampArea,
                    juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent&) override   { reset(); }

private:
    static constexpr float minDb = -60.0f;

    static float toDb (float gain)   { return juce::jmax (minDb, juce::Decibels::gainToDecibels (gain, minDb)); }

    static float xFor (float db, juce::Rectangle<float> r)
    {
        return r.getX() + r.getWidth() * (juce::jlimit (minDb, 0.0f, db) - minDb) / -minDb;
    }

    int channels = 2;
    bool enabled = false, clip = false;
    float peakDb[2] { minDb, minDb }, rmsDb[2] { minDb, minDb }, holdDb[2] { minDb, minDb }, maxDb[2] { minDb, minDb };
    double holdTime[2] {}, lastUpdate = 0.0;
};

juce::String formatTime (double seconds)
{
    seconds = std::max (0.0, seconds);
    const auto total = (juce::int64) std::floor (seconds);
    const auto ms = (int) std::floor ((seconds - (double) total) * 1000.0);
    return juce::String::formatted ("%02d:%02d:%02d.%03d", (int) (total / 3600), (int) (total / 60 % 60), (int) (total % 60), ms);
}

class RecordPanel final : public juce::Component,
                          private juce::Timer
{
public:
    RecordPanel (InputRecorder& r, RecordCallbacks cb) : recorder (r), callbacks (std::move (cb))
    {
        for (auto* l : { &target, &device, &levelLabel, &channelsLabel, &bitsLabel, &time, &status, &hint })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::black);
            addAndMakeVisible (l);
        }

        target.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        time.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        time.setJustificationType (juce::Justification::centredRight);
        status.setColour (juce::Label::textColourId, juce::Colour (0xff444444));
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff555555));
        hint.setFont (juce::FontOptions (12.0f));
        hint.setText ("Ctrl+R starts and stops recording. Each take is added at the end of the file (Ctrl+Z removes it). "
                      "Click the meter to reset CLIP.",
                      juce::dontSendNotification);
        hint.setJustificationType (juce::Justification::topLeft);
        levelLabel.setText ("Input level:", juce::dontSendNotification);
        channelsLabel.setText ("Channels:", juce::dontSendNotification);
        bitsLabel.setText ("Bits:", juce::dontSendNotification);

        for (int bits : { 8, 12, 14, 16, 24, 32 })
            bitsBox.addItem (bits == 32 ? juce::String ("32-bit float")
                                        : juce::String (bits) + "-bit" + (bits == 12 || bits == 14 ? " (lo-fi)" : ""),
                             bits);

        bitsBox.onChange = [this]
        {
            if (callbacks.setBits != nullptr)
                callbacks.setBits (bitsBox.getSelectedId());
        };
        addAndMakeVisible (bitsBox);

        level.setRange (-40.0, 20.0, 0.1);
        level.setValue (recorder.getGainDecibels(), juce::dontSendNotification);
        level.setDoubleClickReturnValue (true, 0.0);
        level.setTextValueSuffix (" dB");
        level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22);
        level.setColour (juce::Slider::textBoxTextColourId, juce::Colours::black);
        level.onValueChange = [this] { recorder.setGainDecibels ((float) level.getValue()); };
        addAndMakeVisible (level);

        channels.addItem ("Mono", 1);
        channels.addItem ("Stereo", 2);
        channels.onChange = [this]
        {
            if (callbacks.setChannels != nullptr)
                callbacks.setChannels (channels.getSelectedId());
        };
        addAndMakeVisible (channels);

        recordButton.onClick = [this] { if (callbacks.toggleRecording != nullptr) callbacks.toggleRecording(); };
        recordButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffc62828));
        recordButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible (recordButton);

        settingsButton.onClick = [this] { if (callbacks.openAudioSettings != nullptr) callbacks.openAudioSettings(); };
        addAndMakeVisible (settingsButton);

        closeButton.onClick = [this] { close(); };
        addAndMakeVisible (closeButton);
        addAndMakeVisible (meter);

        setSize (600, 330);
        refresh();
        startTimerHz (30);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        auto row = area.removeFromTop (22);
        target.setBounds (row);
        row = area.removeFromTop (22);
        settingsButton.setBounds (row.removeFromRight (120));
        device.setBounds (row);
        area.removeFromTop (8);
        meter.setBounds (area.removeFromTop (92));
        area.removeFromTop (10);

        row = area.removeFromTop (28);
        levelLabel.setBounds (row.removeFromLeft (90));
        level.setBounds (row);
        area.removeFromTop (6);

        row = area.removeFromTop (28);
        channelsLabel.setBounds (row.removeFromLeft (90));
        channels.setBounds (row.removeFromLeft (100));
        row.removeFromLeft (14);
        bitsLabel.setBounds (row.removeFromLeft (38));
        bitsBox.setBounds (row.removeFromLeft (130));
        time.setBounds (row);
        area.removeFromTop (8);

        row = area.removeFromTop (34);
        recordButton.setBounds (row.removeFromLeft (140));
        row.removeFromLeft (10);
        closeButton.setBounds (row.removeFromRight (90));
        status.setBounds (row);
        area.removeFromTop (6);
        hint.setBounds (area);
    }

    void close()
    {
        stopTimer();

        if (callbacks.onClose != nullptr)
            callbacks.onClose();
    }

private:
    void refresh()
    {
        const auto inputs = recorder.getInputChannels();
        const auto rec = recorder.isRecording();

        target.setText ("Recording into: " + (callbacks.fileTitle != nullptr ? callbacks.fileTitle() : juce::String()),
                        juce::dontSendNotification);
        device.setText (inputs > 0 ? "Input: " + recorder.getDeviceName() + "  -  " + juce::String (juce::roundToInt (recorder.getSampleRate()))
                                         + " Hz, " + juce::String (inputs) + (inputs == 1 ? " channel" : " channels")
                                   : juce::String ("No audio input: choose one in Audio Device..."),
                        juce::dontSendNotification);

        const auto empty = callbacks.fileIsEmpty == nullptr || callbacks.fileIsEmpty();
        channels.setEnabled (empty && ! rec);
        channels.setSelectedId (callbacks.fileChannels != nullptr ? callbacks.fileChannels() : 2, juce::dontSendNotification);
        bitsBox.setEnabled (empty && ! rec);
        bitsBox.setSelectedId (callbacks.fileBits != nullptr ? callbacks.fileBits() : 24, juce::dontSendNotification);
        settingsButton.setEnabled (! rec);

        recordButton.setButtonText (rec ? juce::CharPointer_UTF8 ("\xe2\x96\xa0  Stop") : juce::CharPointer_UTF8 ("\xe2\x97\x8f  Record"));
        recordButton.setEnabled (inputs > 0 || rec);

        const auto take = callbacks.takeSeconds != nullptr ? callbacks.takeSeconds() : 0.0;
        const auto total = callbacks.fileSeconds != nullptr ? callbacks.fileSeconds() : 0.0;
        time.setText (rec ? formatTime (take) : formatTime (total), juce::dontSendNotification);
        time.setColour (juce::Label::textColourId, rec ? juce::Colour (0xffc62828) : juce::Colours::black);

        if (recorder.overflowed())
            dropped = true;

        status.setText (rec ? juce::String ("Recording...") + (dropped ? "  (some audio was lost: the computer was too busy)" : "")
                            : inputs > 0 ? juce::String ("Listening to the input. Press Record when ready.")
                                         : juce::String ("No input."),
                        juce::dontSendNotification);
    }

    void timerCallback() override
    {
        meter.update (recorder.takeLevels(), callbacks.fileChannels != nullptr ? callbacks.fileChannels() : 2,
                      recorder.getInputChannels() > 0);
        refresh();
    }

    InputRecorder& recorder;
    RecordCallbacks callbacks;
    bool dropped = false;

    juce::Label target, device, levelLabel, channelsLabel, bitsLabel, time, status, hint;
    LevelMeter meter;
    juce::Slider level { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::ComboBox channels, bitsBox;
    juce::TextButton recordButton, settingsButton { "Audio Device..." }, closeButton { "Close" };
};

class RecordWindow final : public juce::DocumentWindow
{
public:
    RecordWindow (InputRecorder& recorder, RecordCallbacks callbacks)
        : juce::DocumentWindow ("Record", panel, juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setContentOwned (new RecordPanel (recorder, std::move (callbacks)), true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        if (auto* p = dynamic_cast<RecordPanel*> (getContentComponent()))
            p->close();
    }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createRecordWindow (InputRecorder& recorder, RecordCallbacks callbacks)
{
    return std::make_unique<RecordWindow> (recorder, std::move (callbacks));
}
} // namespace wf
