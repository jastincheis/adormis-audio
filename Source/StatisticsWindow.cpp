#include "StatisticsWindow.h"
#include "WaveformView.h"

namespace wf
{
namespace
{
const juce::Colour panelColour (0xffd4d0c8);

juce::String cell (const juce::String& text, int width = 16)   { return text.paddedLeft (' ', width); }

juce::String timeOf (juce::int64 position, double sampleRate)
{
    return WaveformView::formatTime ((double) position / sampleRate);
}

class StatisticsPanel final : public juce::Component
{
public:
    StatisticsPanel (const juce::String& what, const AudioStatistics& statsIn, double sampleRate,
                     std::function<void (juce::int64)> goToIn, std::function<void()> onCloseIn)
        : stats (statsIn), goTo (std::move (goToIn)), onClose (std::move (onCloseIn))
    {
        text.setMultiLine (true);
        text.setReadOnly (true);
        text.setCaretVisible (false);
        text.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
        text.setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
        text.setColour (juce::TextEditor::textColourId, juce::Colours::black);
        text.setText (statisticsText (what, stats, sampleRate), false);
        addAndMakeVisible (text);

        const auto numChannels = (int) stats.channels.size();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* button = peakButtons.add (new juce::TextButton (numChannels == 1 ? juce::String ("Go to Peak")
                                                                                   : ch == 0 ? juce::String ("Go to Peak L")
                                                                                             : juce::String ("Go to Peak R")));
            button->setTooltip ("Put the cursor on this channel's highest sample");
            button->onClick = [this, ch]
            {
                if (goTo != nullptr)
                    goTo (stats.channels[(size_t) ch].peakPosition);
            };
            addAndMakeVisible (button);
        }

        copyButton.setTooltip ("Copy the table as text");
        copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (text.getText()); };
        closeButton.onClick = [this] { close(); };
        closeButton.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
        addAndMakeVisible (copyButton);
        addAndMakeVisible (closeButton);

        setSize (580, 470);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto buttons = area.removeFromBottom (28);
        area.removeFromBottom (10);
        text.setBounds (area);

        for (auto* b : peakButtons)
        {
            b->setBounds (buttons.removeFromLeft (110));
            buttons.removeFromLeft (8);
        }

        closeButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        copyButton.setBounds (buttons.removeFromRight (90));
    }

    void close()
    {
        if (onClose != nullptr)
            onClose();
    }

private:
    AudioStatistics stats;
    std::function<void (juce::int64)> goTo;
    std::function<void()> onClose;
    juce::TextEditor text;
    juce::OwnedArray<juce::TextButton> peakButtons;
    juce::TextButton copyButton { "Copy" }, closeButton { "Close" };
};

class StatisticsWindow final : public juce::DocumentWindow
{
public:
    StatisticsWindow (std::unique_ptr<StatisticsPanel> panel)
        : juce::DocumentWindow ("Statistics", panelColour, juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setContentOwned (panel.release(), true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<StatisticsPanel*> (getContentComponent()))
            panel->close();
    }
};
} // namespace

juce::String statisticsText (const juce::String& what, const AudioStatistics& stats, double sampleRate)
{
    const auto numChannels = (int) stats.channels.size();
    juce::String t;
    t << what << "\n" << "Length: " << timeOf (stats.length, sampleRate) << "  (" << juce::String (stats.length) << " samples)\n\n";

    auto row = [&] (const juce::String& name, std::function<juce::String (const AudioStatistics::PerChannel&)> value)
    {
        t << name.paddedRight (' ', 20);

        for (const auto& c : stats.channels)
            t << cell (value (c));

        t << "\n";
    };

    t << juce::String().paddedRight (' ', 20);

    for (int ch = 0; ch < numChannels; ++ch)
        t << cell (numChannels == 1 ? juce::String ("Mono") : ch == 0 ? juce::String ("Left") : ch == 1 ? juce::String ("Right")
                                                                       : "Channel " + juce::String (ch + 1));

    t << "\n";
    row ("Peak", [] (auto& c) { return formatDb (c.peakDb); });
    row ("  at", [&] (auto& c) { return timeOf (c.peakPosition, sampleRate); });
    row ("True peak", [] (auto& c) { return formatDb (c.truePeakDb, "dBTP"); });
    row ("Minimum sample", [] (auto& c) { return juce::String (c.minimum, 5); });
    row ("Maximum sample", [] (auto& c) { return juce::String (c.maximum, 5); });
    row ("RMS", [] (auto& c) { return formatDb (c.rmsDb); });
    row ("DC offset", [] (auto& c) { return juce::String (c.dcOffset * 100.0, 4) + " %"; });
    row ("Zero crossings", [] (auto& c) { return juce::String (c.zeroCrossings); });
    row ("Clipped samples", [] (auto& c) { return juce::String (c.clippedSamples); });

    const auto seconds = (double) stats.length / sampleRate;
    t << "\nLoudness (all channels together, ITU BS.1770 / EBU R128)\n";
    t << juce::String ("Integrated").paddedRight (' ', 20) << cell (formatLufs (stats.integrated)) << "\n";
    t << juce::String ("Loudness range").paddedRight (' ', 20)
      << cell (seconds >= 3.0 ? juce::String (stats.range, 1) + " LU" : juce::String ("(needs 3 s)")) << "\n";
    t << juce::String ("Max momentary").paddedRight (' ', 20) << cell (formatLufs (stats.maxMomentary)) << "\n";
    t << juce::String ("Max short-term").paddedRight (' ', 20)
      << cell (seconds >= 3.0 ? formatLufs (stats.maxShortTerm) : juce::String ("(needs 3 s)")) << "\n";
    t << juce::String ("True peak").paddedRight (' ', 20) << cell (formatDb (stats.truePeakDb, "dBTP")) << "\n";

    if (stats.integrated > -70.0)
    {
        t << "\nGain needed for";

        for (const auto goal : { -14.0, -16.0, -23.0 })
        {
            const auto gain = goal - stats.integrated;
            t << "   " << juce::String (goal, 0) << " LUFS: " << (gain >= 0.0 ? "+" : "") << juce::String (gain, 1) << " dB";
        }

        t << "\n";
    }

    return t;
}

std::unique_ptr<juce::DocumentWindow> createStatisticsWindow (const juce::String& what, const AudioStatistics& stats,
                                                              double sampleRate,
                                                              std::function<void (juce::int64)> goTo,
                                                              std::function<void()> onClose)
{
    return std::make_unique<StatisticsWindow> (std::make_unique<StatisticsPanel> (what, stats, sampleRate, std::move (goTo),
                                                                                  std::move (onClose)));
}
} // namespace wf
