#include "MixDialog.h"
#include "Settings.h"
#include "SpaceKey.h"

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

class MixPanel final : public juce::Component,
                       public SpaceTarget,
                       private juce::Timer
{
public:
    bool spacePressed() override   { previewButton.onClick(); return true; }   // Space = Preview / Stop preview

    MixPanel (const juce::String& description, std::shared_ptr<MixGains> gainsIn, MixDialogCallbacks callbacksIn)
        : gains (std::move (gainsIn)), callbacks (std::move (callbacksIn))
    {
        info.setText (description, juce::dontSendNotification);
        info.setJustificationType (juce::Justification::topLeft);
        info.setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (info);

        setUpRow (sourceLabel, sourceSlider, "Source volume (dropped audio)", gains->source, "mix.sourceDb");
        setUpRow (destLabel, destSlider, "Destination volume (existing audio)", gains->dest, "mix.destDb");

        previewButton.onClick = [this]
        {
            if (callbacks.onPreview != nullptr)
                callbacks.onPreview (! previewing());

            updatePreviewButton();
        };

        okButton.onClick = [this]
        {
            stopPreview();

            if (callbacks.onOk != nullptr)
                callbacks.onOk();

            close();
        };

        cancelButton.onClick = [this] { close(); };
        okButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
        cancelButton.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));

        for (auto* b : { &previewButton, &okButton, &cancelButton })
            addAndMakeVisible (b);

        setSize (460, 236);
        startTimerHz (8);
    }

    ~MixPanel() override
    {
        // Closing the window any other way must not leave the preview playing.
        stopPreview();
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
        previewButton.setBounds (buttons.removeFromLeft (120));
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        okButton.setBounds (buttons.removeFromRight (90));
    }

private:
    void setUpRow (juce::Label& label, juce::Slider& slider, const juce::String& text, std::atomic<float>& target,
                   const juce::String& rememberKey)
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
        slider.onValueChange = [&slider, &target, rememberKey]
        {
            target = gainFromDb (slider.getValue());
            settings::set (rememberKey, slider.getValue());   // the next mix starts here
        };

        if (const auto last = settings::get (rememberKey); ! last.isVoid())
            slider.setValue (juce::jlimit (minDb, maxDb, (double) last), juce::sendNotificationSync);

        slider.updateText();
        addAndMakeVisible (slider);
    }

    bool previewing() const  { return callbacks.isPreviewing != nullptr && callbacks.isPreviewing(); }

    void stopPreview()
    {
        if (previewing() && callbacks.onPreview != nullptr)
            callbacks.onPreview (false);
    }

    void updatePreviewButton()
    {
        previewButton.setButtonText (previewing() ? "Stop preview" : "Preview");
    }

    void timerCallback() override  { updatePreviewButton(); }

public:
    void close()
    {
        stopPreview();

        if (callbacks.onClose != nullptr)
            callbacks.onClose();
    }

private:
    std::shared_ptr<MixGains> gains;
    MixDialogCallbacks callbacks;
    juce::Label info, sourceLabel, destLabel;
    juce::Slider sourceSlider, destSlider;
    juce::TextButton previewButton { "Preview" }, okButton { "OK" }, cancelButton { "Cancel" };
};

class MixWindow final : public juce::DocumentWindow
{
public:
    MixWindow (const juce::String& description, std::shared_ptr<MixGains> gains, MixDialogCallbacks callbacks)
        : juce::DocumentWindow ("Mix", juce::Colour (0xffd4d0c8), juce::DocumentWindow::closeButton)
    {
        // Our own title bar: tiling window managers give native dialogs none,
        // which would make the window impossible to drag away.
        setUsingNativeTitleBar (false);
        setContentOwned (new MixPanel (description, std::move (gains), std::move (callbacks)), true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<MixPanel*> (getContentComponent()))
            panel->close();
    }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createMixWindow (const juce::String& description,
                                                       std::shared_ptr<MixGains> gains,
                                                       MixDialogCallbacks callbacks)
{
    return std::make_unique<MixWindow> (description, std::move (gains), std::move (callbacks));
}
} // namespace wf
