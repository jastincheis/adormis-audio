#include "FormDialog.h"

namespace wf
{
std::optional<double> parseNumber (const juce::String& text)
{
    const auto t = text.trim().replaceCharacter (',', '.');

    if (t.isEmpty() || ! t.containsOnly ("+-.0123456789") || ! t.containsAnyOf ("0123456789")
        || t.lastIndexOfAnyOf ("+-") > 0 || t.indexOfChar ('.') != t.lastIndexOfChar ('.'))
        return {};

    return t.getDoubleValue();
}

std::optional<double> parseTime (const juce::String& text)
{
    auto parts = juce::StringArray::fromTokens (text.trim(), ":", {});

    if (parts.isEmpty() || parts.size() > 3)
        return {};

    double seconds = 0.0;

    for (int i = 0; i < parts.size(); ++i)
    {
        const auto value = parseNumber (parts[i]);

        // Only the last part may have a fraction; none may be negative.
        if (! value.has_value() || *value < 0.0 || (i < parts.size() - 1 && parts[i].containsAnyOf (".,")))
            return {};

        seconds = seconds * 60.0 + *value;
    }

    return seconds;
}

namespace
{
const juce::Colour panelColour (0xffd4d0c8);

class FormPanel final : public juce::Component
{
public:
    FormPanel (const juce::String& infoText, std::vector<FormField> fieldsIn, const juce::String& hintText,
               std::function<void (const juce::StringArray&)> onOkIn, std::function<void()> onCloseIn)
        : fields (std::move (fieldsIn)), onOk (std::move (onOkIn)), onClose (std::move (onCloseIn))
    {
        info.setText (infoText, juce::dontSendNotification);
        info.setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (info);

        hint.setText (hintText, juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff555555));
        hint.setFont (juce::FontOptions (13.0f));
        hint.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (hint);

        for (auto& field : fields)
        {
            auto* row = rows.add (new Row());
            row->label.setText (field.label, juce::dontSendNotification);
            row->label.setColour (juce::Label::textColourId, juce::Colours::black);
            row->unit.setText (field.unit, juce::dontSendNotification);
            row->unit.setColour (juce::Label::textColourId, juce::Colours::black);
            addAndMakeVisible (row->label);
            addAndMakeVisible (row->unit);

            if (field.choices.isEmpty())
            {
                row->text.setText (field.value, false);
                row->text.setJustification (juce::Justification::centredRight);
                row->text.onTextChange = [this] { validate(); };
                row->text.onReturnKey = [this] { apply(); };
                row->text.onEscapeKey = [this] { close(); };
                addAndMakeVisible (row->text);
            }
            else
            {
                row->list.addItemList (field.choices, 1);
                row->list.setSelectedItemIndex (juce::jmax (0, field.choices.indexOf (field.value)), juce::dontSendNotification);
                addAndMakeVisible (row->list);
            }
        }

        okButton.onClick = [this] { apply(); };
        cancelButton.onClick = [this] { close(); };
        okButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
        cancelButton.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
        addAndMakeVisible (okButton);
        addAndMakeVisible (cancelButton);

        const auto hintLines = hintText.isEmpty() ? 0 : juce::StringArray::fromLines (hintText).size();
        setSize (460, 14 + 32 + (int) fields.size() * 34 + hintLines * 18 + 12 + 28 + 14);
        validate();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        info.setBounds (area.removeFromTop (24));
        area.removeFromTop (8);

        for (int i = 0; i < rows.size(); ++i)
        {
            auto line = area.removeFromTop (28);
            area.removeFromTop (6);
            rows[i]->label.setBounds (line.removeFromLeft (150));

            if (fields[(size_t) i].choices.isEmpty())
            {
                rows[i]->text.setBounds (line.removeFromLeft (130));
                rows[i]->unit.setBounds (line.removeFromLeft (60));
            }
            else
            {
                rows[i]->list.setBounds (line.removeFromLeft (270));
            }
        }

        auto buttons = area.removeFromBottom (28);
        hint.setBounds (area);
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        okButton.setBounds (buttons.removeFromRight (90));
    }

    void focusFirst()
    {
        for (int i = 0; i < rows.size(); ++i)
            if (fields[(size_t) i].choices.isEmpty())
            {
                rows[i]->text.grabKeyboardFocus();
                rows[i]->text.selectAll();
                return;
            }

        okButton.grabKeyboardFocus();
    }

    void close()
    {
        if (onClose != nullptr)
            onClose();
    }

private:
    struct Row
    {
        juce::Label label, unit;
        juce::TextEditor text;
        juce::ComboBox list;
    };

    bool isValid (int i) const
    {
        const auto& field = fields[(size_t) i];
        return ! field.choices.isEmpty() || field.isValid == nullptr || field.isValid (rows[i]->text.getText());
    }

    void validate()
    {
        bool all = true;

        for (int i = 0; i < rows.size(); ++i)
        {
            const auto ok = isValid (i);
            all = all && ok;
            rows[i]->text.setColour (juce::TextEditor::outlineColourId, ok ? juce::Colour (0xff808080) : juce::Colours::red);
            rows[i]->text.repaint();
        }

        okButton.setEnabled (all);
    }

    void apply()
    {
        juce::StringArray values;

        for (int i = 0; i < rows.size(); ++i)
        {
            if (! isValid (i))
                return;

            values.add (fields[(size_t) i].choices.isEmpty() ? rows[i]->text.getText().trim() : rows[i]->list.getText());
        }

        auto callback = onOk;
        close();

        if (callback != nullptr)
            callback (values);
    }

    std::vector<FormField> fields;
    std::function<void (const juce::StringArray&)> onOk;
    std::function<void()> onClose;

    juce::Label info, hint;
    juce::OwnedArray<Row> rows;
    juce::TextButton okButton { "OK" }, cancelButton { "Cancel" };
};

class FormWindow final : public juce::DocumentWindow
{
public:
    FormWindow (const juce::String& title, std::unique_ptr<FormPanel> panel)
        : juce::DocumentWindow (title, panelColour, juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setContentOwned (panel.release(), true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    void visibilityChanged() override
    {
        juce::DocumentWindow::visibilityChanged();

        if (isShowing())
            if (auto* panel = dynamic_cast<FormPanel*> (getContentComponent()))
                panel->focusFirst();
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<FormPanel*> (getContentComponent()))
            panel->close();
    }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createFormWindow (const juce::String& title, const juce::String& info,
                                                        std::vector<FormField> fields, const juce::String& hint,
                                                        std::function<void (const juce::StringArray&)> onOk,
                                                        std::function<void()> onClose)
{
    return std::make_unique<FormWindow> (title, std::make_unique<FormPanel> (info, std::move (fields), hint,
                                                                             std::move (onOk), std::move (onClose)));
}
} // namespace wf
