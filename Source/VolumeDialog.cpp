#include "VolumeDialog.h"

namespace wf
{
std::optional<float> parseDecibels (const juce::String& text)
{
    auto t = text.trim().replaceCharacter (',', '.');

    if (t.endsWithIgnoreCase ("db"))
        t = t.dropLastCharacters (2).trim();

    if (t.startsWithChar ('+'))
        t = t.substring (1);

    if (t.isEmpty() || ! t.containsOnly ("-.0123456789") || t.lastIndexOfChar ('-') > 0
        || t.indexOfChar ('.') != t.lastIndexOfChar ('.') || ! t.containsAnyOf ("0123456789"))
        return {};

    const auto value = t.getFloatValue();

    if (value < minVolumeDecibels || value > maxVolumeDecibels)
        return {};

    return value;
}

namespace
{
juce::String formatDecibels (float value)
{
    auto text = juce::String (value, 2);

    while (text.containsChar ('.') && (text.endsWithChar ('0') || text.endsWithChar ('.')))
        text = text.dropLastCharacters (1);

    return value > 0.0f ? "+" + text : text;
}

class VolumePanel final : public juce::Component
{
public:
    VolumePanel (const juce::String& target, float initial, std::function<void (float)> onApplyIn,
                 std::function<void()> onCloseIn)
        : onApply (std::move (onApplyIn)), onClose (std::move (onCloseIn))
    {
        for (auto* l : { &info, &label, &unit, &hint })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::black);
            addAndMakeVisible (l);
        }

        info.setText ("Change the volume of: " + target, juce::dontSendNotification);
        hint.setText ("e.g. 0.1, -0.1, 3, -6   (" + formatDecibels (minVolumeDecibels) + " to "
                          + formatDecibels (maxVolumeDecibels) + " dB)",
                      juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff555555));
        hint.setFont (juce::FontOptions (13.0f));

        field.setText (formatDecibels (initial), false);
        field.setInputRestrictions (12, "+-.,0123456789");
        field.setJustification (juce::Justification::centredRight);
        field.onTextChange = [this] { validate(); };
        field.onReturnKey = [this] { apply(); };
        field.onEscapeKey = [this] { close(); };
        addAndMakeVisible (field);

        okButton.onClick = [this] { apply(); };
        cancelButton.onClick = [this] { close(); };
        okButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
        cancelButton.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
        addAndMakeVisible (okButton);
        addAndMakeVisible (cancelButton);

        setSize (420, 170);
        validate();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        info.setBounds (area.removeFromTop (24));
        area.removeFromTop (8);

        auto row = area.removeFromTop (28);
        label.setBounds (row.removeFromLeft (90));
        field.setBounds (row.removeFromLeft (110));
        unit.setBounds (row.removeFromLeft (40));

        hint.setBounds (area.removeFromTop (22));

        auto buttons = area.removeFromBottom (28);
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        okButton.setBounds (buttons.removeFromRight (90));
    }

    void focusField()
    {
        field.grabKeyboardFocus();
        field.selectAll();
    }

    void close()
    {
        if (onClose != nullptr)
            onClose();
    }

private:
    void validate()
    {
        const auto ok = parseDecibels (field.getText()).has_value();
        okButton.setEnabled (ok);
        field.setColour (juce::TextEditor::outlineColourId, ok ? juce::Colour (0xff808080) : juce::Colours::red);
        field.repaint();
    }

    void apply()
    {
        const auto value = parseDecibels (field.getText());

        if (! value.has_value())
            return;

        close();

        if (onApply != nullptr)
            onApply (*value);
    }

    std::function<void (float)> onApply;
    std::function<void()> onClose;

    juce::Label info, label { {}, "Volume:" }, unit { {}, "dB" }, hint;
    juce::TextEditor field;
    juce::TextButton okButton { "OK" }, cancelButton { "Cancel" };
};

class VolumeWindow final : public juce::DocumentWindow
{
public:
    VolumeWindow (const juce::String& target, float initial, std::function<void (float)> onApply,
                  std::function<void()> onClose)
        : juce::DocumentWindow ("Volume", juce::Colour (0xffd4d0c8), juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        auto* panel = new VolumePanel (target, initial, std::move (onApply), std::move (onClose));
        setContentOwned (panel, true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    void visibilityChanged() override
    {
        juce::DocumentWindow::visibilityChanged();

        if (isShowing())
            if (auto* panel = dynamic_cast<VolumePanel*> (getContentComponent()))
                panel->focusField();
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<VolumePanel*> (getContentComponent()))
            panel->close();
    }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createVolumeWindow (const juce::String& target, float initialDecibels,
                                                          std::function<void (float)> onApply,
                                                          std::function<void()> onClose)
{
    return std::make_unique<VolumeWindow> (target, initialDecibels, std::move (onApply), std::move (onClose));
}
} // namespace wf
