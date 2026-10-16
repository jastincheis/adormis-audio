#include "TemplateDialog.h"

namespace wf
{
juce::int64 TemplateSettings::lengthInSamples() const
{
    return (juce::int64) std::llround ((double) bars * 4.0 * 60.0 / bpm * sampleRate);
}

std::vector<juce::int64> TemplateSettings::markerPositions() const
{
    // A 1/division note lasts 4/division beats; 4/4 has `division` of them per bar.
    const auto step = 60.0 / bpm * 4.0 / (double) division * sampleRate;
    const auto count = bars * division;
    std::vector<juce::int64> positions;
    positions.reserve ((size_t) count);

    for (int i = 0; i < count; ++i)
        positions.push_back ((juce::int64) std::llround (step * i));

    return positions;
}

std::vector<juce::int64> TemplateSettings::barStartPositions() const
{
    // Every `division`-th grid marker, with the same rounding as markerPositions().
    const auto all = markerPositions();
    std::vector<juce::int64> starts;

    for (size_t i = 0; i < all.size(); i += (size_t) division)
        starts.push_back (all[i]);

    return starts;
}

namespace
{
juce::String cleanNumber (juce::String text)
{
    return text.trim().replaceCharacter (',', '.');
}

bool isPlainNumber (const juce::String& t, bool allowDecimals)
{
    return t.isNotEmpty() && t.containsOnly (allowDecimals ? "0123456789." : "0123456789")
        && t.indexOfChar ('.') == t.lastIndexOfChar ('.') && t.containsAnyOf ("0123456789");
}
} // namespace

std::optional<double> parseBpm (const juce::String& text)
{
    auto t = cleanNumber (text);

    if (t.endsWithIgnoreCase ("bpm"))
        t = t.dropLastCharacters (3).trim();

    if (! isPlainNumber (t, true))
        return {};

    const auto value = t.getDoubleValue();
    return value >= 20.0 && value <= 400.0 ? std::optional<double> (value) : std::nullopt;
}

std::optional<int> parseBars (const juce::String& text)
{
    const auto t = cleanNumber (text);

    if (! isPlainNumber (t, false) || t.length() > 3)
        return {};

    const auto value = t.getIntValue();
    return value >= 1 && value <= 999 ? std::optional<int> (value) : std::nullopt;
}

namespace
{
// Whole files live in memory; keep new ones well inside the 2 GB per-file limit.
constexpr double maxTemplateBytes = 2.0 * 1024.0 * 1024.0 * 1024.0;

class TemplatePanel final : public juce::Component
{
public:
    TemplatePanel (const TemplateSettings& initial, std::function<void (TemplateSettings)> onCreateIn,
                   std::function<void()> onCloseIn)
        : onCreate (std::move (onCreateIn)), onClose (std::move (onCloseIn))
    {
        for (auto* l : { &info, &bpmLabel, &gridLabel, &barsLabel, &rateLabel, &channelsLabel, &bitsLabel, &lengthInfo })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::black);
            addAndMakeVisible (l);
        }

        info.setText ("New silent file with markers on the beat (4/4):", juce::dontSendNotification);

        bpmField.setText (juce::String (initial.bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."), false);
        bpmField.setInputRestrictions (8, "0123456789.,");
        barsField.setText (juce::String (initial.bars), false);
        barsField.setInputRestrictions (3, "0123456789");

        for (auto* field : { &bpmField, &barsField })
        {
            field->setJustification (juce::Justification::centredRight);
            field->onTextChange = [this] { update(); };
            field->onReturnKey = [this] { create(); };
            field->onEscapeKey = [this] { close(); };
            addAndMakeVisible (field);
        }

        const int divisions[] = { 4, 8, 16, 32 };
        const char* names[] = { "1/4 (quarter notes)", "1/8 (eighth notes)", "1/16 (sixteenth notes)",
                                "1/32 (thirty-second notes)" };

        for (int i = 0; i < 4; ++i)
        {
            gridBox.addItem (names[i], divisions[i]);
            if (divisions[i] == initial.division)
                gridBox.setSelectedId (divisions[i], juce::dontSendNotification);
        }

        rateBox.addItem ("44.1 kHz", 44100);
        rateBox.addItem ("48 kHz", 48000);
        rateBox.addItem ("96 kHz", 96000);
        rateBox.setSelectedId (juce::roundToInt (initial.sampleRate), juce::dontSendNotification);

        channelsBox.addItem ("Stereo", 2);
        channelsBox.addItem ("Mono", 1);
        channelsBox.setSelectedId (initial.channels, juce::dontSendNotification);

        for (int b : { 8, 12, 14, 16, 24, 32 })
            bitsBox.addItem (b == 32 ? juce::String ("32-bit float")
                                     : juce::String (b) + "-bit" + (b == 12 || b == 14 ? " (lo-fi)" : ""),
                             b);

        bitsBox.setSelectedId (initial.bits, juce::dontSendNotification);

        for (auto* box : { &gridBox, &rateBox, &channelsBox, &bitsBox })
        {
            box->onChange = [this] { update(); };
            addAndMakeVisible (box);
        }

        lengthInfo.setFont (juce::FontOptions (13.0f));

        createButton.onClick = [this] { create(); };
        cancelButton.onClick = [this] { close(); };
        createButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
        cancelButton.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
        addAndMakeVisible (createButton);
        addAndMakeVisible (cancelButton);

        setSize (440, 333);
        update();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        info.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);

        auto row = [&area] (juce::Label& label, juce::Component& field, int width)
        {
            auto r = area.removeFromTop (26);
            label.setBounds (r.removeFromLeft (110));
            field.setBounds (r.removeFromLeft (width));
            area.removeFromTop (7);
        };

        row (bpmLabel, bpmField, 90);
        row (gridLabel, gridBox, 220);
        row (barsLabel, barsField, 90);
        row (rateLabel, rateBox, 120);
        row (channelsLabel, channelsBox, 120);
        row (bitsLabel, bitsBox, 160);

        lengthInfo.setBounds (area.removeFromTop (22));

        auto buttons = area.removeFromBottom (28);
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        createButton.setBounds (buttons.removeFromRight (90));
    }

    void focusField()
    {
        bpmField.grabKeyboardFocus();
        bpmField.selectAll();
    }

    void close()
    {
        if (onClose != nullptr)
            onClose();
    }

private:
    std::optional<TemplateSettings> current() const
    {
        const auto bpm = parseBpm (bpmField.getText());
        const auto bars = parseBars (barsField.getText());

        if (! bpm.has_value() || ! bars.has_value())
            return {};

        TemplateSettings s;
        s.bpm = *bpm;
        s.bars = *bars;
        s.division = gridBox.getSelectedId();
        s.sampleRate = (double) rateBox.getSelectedId();
        s.channels = channelsBox.getSelectedId();
        s.bits = bitsBox.getSelectedId();
        return s;
    }

    void update()
    {
        const auto bpmOk = parseBpm (bpmField.getText()).has_value();
        const auto barsOk = parseBars (barsField.getText()).has_value();
        bpmField.setColour (juce::TextEditor::outlineColourId, bpmOk ? juce::Colour (0xff808080) : juce::Colours::red);
        barsField.setColour (juce::TextEditor::outlineColourId, barsOk ? juce::Colour (0xff808080) : juce::Colours::red);
        bpmField.repaint();
        barsField.repaint();

        auto ok = false;
        juce::String text;

        if (const auto s = current())
        {
            const auto seconds = (double) s->lengthInSamples() / s->sampleRate;
            const auto bytes = (double) s->lengthInSamples() * s->channels * sizeof (float);
            ok = bytes <= maxTemplateBytes;
            text << "Length " << juce::String (seconds, 3) << " s, " << s->bars * s->division << " markers"
                 << (ok ? juce::String() : juce::String ("  - too long"));
        }
        else
        {
            text = "Tempo 20-400 BPM, 1-999 bars";
        }

        lengthInfo.setText (text, juce::dontSendNotification);
        lengthInfo.setColour (juce::Label::textColourId, ok ? juce::Colour (0xff333333) : juce::Colours::red);
        createButton.setEnabled (ok);
    }

    void create()
    {
        const auto s = current();

        if (! s.has_value() || ! createButton.isEnabled())
            return;

        close();

        if (onCreate != nullptr)
            onCreate (*s);
    }

    std::function<void (TemplateSettings)> onCreate;
    std::function<void()> onClose;

    juce::Label info, bpmLabel { {}, "Tempo (BPM):" }, gridLabel { {}, "Markers every:" }, barsLabel { {}, "Length (bars):" },
                rateLabel { {}, "Sample rate:" }, channelsLabel { {}, "Channels:" },
                bitsLabel { {}, "Bits:" }, lengthInfo;
    juce::TextEditor bpmField, barsField;
    juce::ComboBox gridBox, rateBox, channelsBox, bitsBox;
    juce::TextButton createButton { "Create" }, cancelButton { "Cancel" };
};

class TemplateWindow final : public juce::DocumentWindow
{
public:
    TemplateWindow (const TemplateSettings& initial, std::function<void (TemplateSettings)> onCreate,
                    std::function<void()> onClose)
        : juce::DocumentWindow ("New", juce::Colour (0xffd4d0c8), juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setContentOwned (new TemplatePanel (initial, std::move (onCreate), std::move (onClose)), true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    void visibilityChanged() override
    {
        juce::DocumentWindow::visibilityChanged();

        if (isShowing())
            if (auto* panel = dynamic_cast<TemplatePanel*> (getContentComponent()))
                panel->focusField();
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<TemplatePanel*> (getContentComponent()))
            panel->close();
    }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createTemplateWindow (const TemplateSettings& initial,
                                                            std::function<void (TemplateSettings)> onCreate,
                                                            std::function<void()> onClose)
{
    return std::make_unique<TemplateWindow> (initial, std::move (onCreate), std::move (onClose));
}
} // namespace wf
