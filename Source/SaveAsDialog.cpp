#include "SaveAsDialog.h"
#include "Mp3Encoder.h"

namespace wf
{
namespace
{
struct Quality
{
    juce::String label;
    SaveFormat format;
};

struct FormatChoice
{
    juce::String label;
    juce::String extension;
    std::vector<Quality> qualities;
    int defaultQuality = 0;
};

std::vector<FormatChoice> formatChoices()
{
    std::vector<FormatChoice> choices {
        { "WAV (uncompressed)", ".wav",
          { { "16-bit (CD quality)",  { ".wav", 16, false, 0 } },
            { "24-bit (studio)",      { ".wav", 24, false, 0 } },
            { "32-bit float",         { ".wav", 32, true, 0 } } }, 0 },

        { "FLAC (lossless, about half the size)", ".flac",
          { { "16-bit (CD quality)",  { ".flac", 16, false, 0 } },
            { "24-bit (studio)",      { ".flac", 24, false, 0 } } }, 0 },

        { "AIFF (uncompressed, Apple)", ".aiff",
          { { "16-bit (CD quality)",  { ".aiff", 16, false, 0 } },
            { "24-bit (studio)",      { ".aiff", 24, false, 0 } } }, 0 },
    };

    if (mp3EncodingAvailable())
        choices.push_back ({ "MP3 (compressed)", ".mp3",
                             { { "128 kbps",              { ".mp3", 16, false, 128 } },
                               { "192 kbps",              { ".mp3", 16, false, 192 } },
                               { "256 kbps",              { ".mp3", 16, false, 256 } },
                               { "320 kbps (best)",       { ".mp3", 16, false, 320 } } }, 3 });

    choices.push_back ({ "OGG Vorbis (compressed)", ".ogg",
                         { { "128 kbps",                  { ".ogg", 16, false, 128 } },
                           { "192 kbps",                  { ".ogg", 16, false, 192 } },
                           { "256 kbps",                  { ".ogg", 16, false, 256 } },
                           { "320 kbps",                  { ".ogg", 16, false, 320 } } }, 1 });
    return choices;
}

// MP3 (MPEG-1 layer III) tops out at 48 kHz.
double maxRateFor (const juce::String& extension)
{
    return extension == ".mp3" ? 48000.0 : 192000.0;
}

juce::String rateText (double rate)
{
    return juce::String (rate / 1000.0, juce::approximatelyEqual (std::fmod (rate, 1000.0), 0.0) ? 0 : 1) + " kHz";
}

class SaveAsPanel final : public juce::Component
{
public:
    SaveAsPanel (const juce::String& fileTitle, const juce::String& suggested, double fileRateIn,
                 std::function<void (SaveFormat)> onChosenIn, std::function<void()> onCloseIn)
        : choices (formatChoices()), fileRate (fileRateIn), onChosen (std::move (onChosenIn)), onClose (std::move (onCloseIn))
    {
        info.setText ("Save \"" + fileTitle + "\" as:", juce::dontSendNotification);
        info.setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (info);

        for (auto* l : { &formatLabel, &qualityLabel, &rateLabel })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::black);
            addAndMakeVisible (l);
        }

        int selected = 1;
        for (int i = 0; i < (int) choices.size(); ++i)
        {
            formatBox.addItem (choices[(size_t) i].label, i + 1);

            if (choices[(size_t) i].extension.equalsIgnoreCase (suggested)
                || (suggested.equalsIgnoreCase (".aif") && choices[(size_t) i].extension == ".aiff"))
                selected = i + 1;
        }

        formatBox.onChange = [this] { fillQualities(); fillRates(); };
        formatBox.setSelectedId (selected, juce::dontSendNotification);
        addAndMakeVisible (formatBox);
        addAndMakeVisible (qualityBox);
        addAndMakeVisible (rateBox);
        fillQualities();
        fillRates();

        saveButton.onClick = [this]
        {
            const auto& choice = choices[(size_t) (formatBox.getSelectedId() - 1)];
            const auto quality = juce::jlimit (0, (int) choice.qualities.size() - 1, qualityBox.getSelectedId() - 1);
            auto format = choice.qualities[(size_t) quality].format;
            format.sampleRate = selectedRate();

            close();

            if (onChosen != nullptr)
                onChosen (format);
        };
        cancelButton.onClick = [this] { close(); };
        saveButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
        cancelButton.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
        addAndMakeVisible (saveButton);
        addAndMakeVisible (cancelButton);

        setSize (420, 224);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        info.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);

        auto row = [&area] (juce::Label& label, juce::ComboBox& box)
        {
            auto r = area.removeFromTop (26);
            label.setBounds (r.removeFromLeft (90));
            box.setBounds (r);
            area.removeFromTop (8);
        };

        row (formatLabel, formatBox);
        row (qualityLabel, qualityBox);
        row (rateLabel, rateBox);

        auto buttons = area.removeFromBottom (28);
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        saveButton.setBounds (buttons.removeFromRight (110));
    }

    void close()
    {
        if (onClose != nullptr)
            onClose();
    }

private:
    void fillQualities()
    {
        qualityBox.clear (juce::dontSendNotification);
        const auto& choice = choices[(size_t) (formatBox.getSelectedId() - 1)];

        for (int i = 0; i < (int) choice.qualities.size(); ++i)
            qualityBox.addItem (choice.qualities[(size_t) i].label, i + 1);

        qualityBox.setSelectedId (choice.defaultQuality + 1, juce::dontSendNotification);
    }

    // "Same as file" (id 1, rate 0) plus the common rates the format can hold;
    // keeps the current pick when the new format still allows it.
    void fillRates()
    {
        const auto previous = selectedRate();
        const auto maxRate = maxRateFor (choices[(size_t) (formatBox.getSelectedId() - 1)].extension);

        rateBox.clear (juce::dontSendNotification);
        rates.clear();

        if (fileRate <= maxRate)
        {
            rates.push_back (0.0);
            rateBox.addItem ("Same as file (" + rateText (fileRate) + ")", 1);
        }

        for (auto rate : { 44100.0, 48000.0, 96000.0 })
            if (rate <= maxRate)
            {
                rates.push_back (rate);
                rateBox.addItem (rateText (rate), (int) rates.size());
            }

        auto pick = 1;
        for (int i = 0; i < (int) rates.size(); ++i)
            if (juce::approximatelyEqual (rates[(size_t) i], previous))
                pick = i + 1;

        rateBox.setSelectedId (pick, juce::dontSendNotification);
    }

    double selectedRate() const
    {
        const auto index = rateBox.getSelectedId() - 1;
        return juce::isPositiveAndBelow (index, (int) rates.size()) ? rates[(size_t) index] : 0.0;
    }

    std::vector<FormatChoice> choices;
    double fileRate;
    std::vector<double> rates;   // per rateBox item; 0 = same as file
    std::function<void (SaveFormat)> onChosen;
    std::function<void()> onClose;

    juce::Label info, formatLabel { {}, "Format:" }, qualityLabel { {}, "Quality:" }, rateLabel { {}, "Sample rate:" };
    juce::ComboBox formatBox, qualityBox, rateBox;
    juce::TextButton saveButton { "Save..." }, cancelButton { "Cancel" };
};

class SaveAsWindow final : public juce::DocumentWindow
{
public:
    SaveAsWindow (const juce::String& fileTitle, const juce::String& suggested, double fileRate,
                  std::function<void (SaveFormat)> onChosen, std::function<void()> onClose)
        : juce::DocumentWindow ("Save As", juce::Colour (0xffd4d0c8), juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setContentOwned (new SaveAsPanel (fileTitle, suggested, fileRate, std::move (onChosen), std::move (onClose)), true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<SaveAsPanel*> (getContentComponent()))
            panel->close();
    }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createSaveAsWindow (const juce::String& fileTitle, const juce::String& suggested,
                                                          double fileRate,
                                                          std::function<void (SaveFormat)> onChosen,
                                                          std::function<void()> onClose)
{
    return std::make_unique<SaveAsWindow> (fileTitle, suggested, fileRate, std::move (onChosen), std::move (onClose));
}
} // namespace wf
