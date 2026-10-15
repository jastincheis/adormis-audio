#include "MixDialog.h"

namespace wf
{
namespace
{
// The bottom of the slider range means silence.
constexpr double minDb = -60.0;
constexpr double maxDb = 12.0;

float gainFromDb (double db)
{
    return db <= minDb ? 0.0f : juce::Decibels::decibelsToGain ((float) db);
}

class MixPanel final : public juce::Component
{
public:
    MixPanel (const juce::String& description, std::function<void (MixSettings)> onOkIn)
        : onOk (std::move (onOkIn))
    {
        info.setText (description, juce::dontSendNotification);
        info.setJustificationType (juce::Justification::topLeft);
        info.setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (info);

        setUpRow (sourceLabel, sourceSlider, "Source volume (dropped audio)");
        setUpRow (destLabel, destSlider, "Destination volume (existing audio)");

        okButton.onClick = [this]
        {
            if (onOk != nullptr)
                onOk ({ gainFromDb (sourceSlider.getValue()), gainFromDb (destSlider.getValue()) });

            close();
        };
        cancelButton.onClick = [this] { close(); };
        okButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
        cancelButton.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
        addAndMakeVisible (okButton);
        addAndMakeVisible (cancelButton);

        setSize (440, 236);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        info.setBounds (area.removeFromTop (40));
        area.removeFromTop (6);

        for (auto* row : { &sourceLabel, &destLabel })
        {
            auto* slider = row == &sourceLabel ? &sourceSlider : &destSlider;
            row->setBounds (area.removeFromTop (20));
            slider->setBounds (area.removeFromTop (30));
            area.removeFromTop (8);
        }

        auto buttons = area.removeFromBottom (28);
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        okButton.setBounds (buttons.removeFromRight (90));
    }

private:
    void setUpRow (juce::Label& label, juce::Slider& slider, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (label);

        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
        slider.setRange (minDb, maxDb, 0.1);
        slider.setSkewFactorFromMidPoint (-12.0);
        slider.setDoubleClickReturnValue (true, 0.0);
        slider.setValue (0.0, juce::dontSendNotification);
        slider.textFromValueFunction = [] (double db)
        {
            return db <= minDb ? juce::String ("-inf dB") : juce::String (db, 1) + " dB";
        };
        slider.valueFromTextFunction = [] (const juce::String& entered)
        {
            return entered.trim().startsWithIgnoreCase ("-inf") ? minDb : entered.getDoubleValue();
        };
        slider.updateText();
        addAndMakeVisible (slider);
    }

    void close()
    {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0);
    }

    std::function<void (MixSettings)> onOk;
    juce::Label info, sourceLabel, destLabel;
    juce::Slider sourceSlider, destSlider;
    juce::TextButton okButton { "OK" }, cancelButton { "Cancel" };
};
} // namespace

void showMixDialog (const juce::String& description, std::function<void (MixSettings)> onOk)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new MixPanel (description, std::move (onOk)));
    options.dialogTitle = "Mix";
    options.dialogBackgroundColour = juce::Colour (0xffd4d0c8);
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.escapeKeyTriggersCloseButton = true;
    options.launchAsync();
}
} // namespace wf
