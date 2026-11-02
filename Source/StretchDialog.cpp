#include "StretchDialog.h"
#include "TemplateDialog.h"
#include "SpaceKey.h"

namespace wf
{
StretchSettings StretchRequest::settings() const
{
    StretchSettings s;

    if (fromBpm.has_value() && toBpm.has_value() && ! tape)
        s.timeRatio = *fromBpm / *toBpm;

    s.semitones = changePitch ? semitones : 0.0;
    s.preserveFormants = preserveFormants && ! tape;
    s.tape = tape && changePitch;
    return s;
}

juce::var stretchRequestToVar (const StretchRequest& r)
{
    auto* o = new juce::DynamicObject();

    if (r.toBpm.has_value())
        o->setProperty ("toBpm", *r.toBpm);

    o->setProperty ("changePitch", r.changePitch);
    o->setProperty ("tape", r.tape);
    o->setProperty ("semitones", r.semitones);
    o->setProperty ("formants", r.preserveFormants);
    return juce::var (o);
}

StretchRequest stretchRequestFromVar (const juce::var& v)
{
    StretchRequest r;

    if (! v.isObject())
        return r;

    if (v.hasProperty ("toBpm"))
        r.toBpm = (double) v.getProperty ("toBpm", 120.0);

    r.changePitch = (bool) v.getProperty ("changePitch", false);
    r.tape = (bool) v.getProperty ("tape", false);
    r.semitones = juce::jlimit (-maxPitchSemitones, maxPitchSemitones, (double) v.getProperty ("semitones", 0.0));
    r.preserveFormants = (bool) v.getProperty ("formants", false);
    return r;
}

std::optional<double> parseSemitones (const juce::String& text)
{
    auto t = text.trim().replaceCharacter (',', '.');

    if (t.startsWithChar ('+'))
        t = t.substring (1);

    if (t.isEmpty() || ! t.containsOnly ("-.0123456789") || t.lastIndexOfChar ('-') > 0
        || t.indexOfChar ('.') != t.lastIndexOfChar ('.') || ! t.containsAnyOf ("0123456789"))
        return {};

    const auto value = t.getDoubleValue();
    return std::abs (value) <= maxPitchSemitones ? std::optional<double> (value) : std::nullopt;
}

namespace
{
juce::String formatNumber (double value, int decimals)
{
    auto text = juce::String (value, decimals);

    while (text.containsChar ('.') && (text.endsWithChar ('0') || text.endsWithChar ('.')))
        text = text.dropLastCharacters (1);

    return text;
}

juce::String formatSemitones (double value)
{
    const auto text = formatNumber (value, 2);
    return value > 0.0 ? "+" + text : text;
}

juce::String formatSeconds (double seconds)
{
    return juce::String (seconds, seconds < 10.0 ? 3 : 2) + " s";
}

// Shortest way from one tonic to another: -5 ... +6 semitones.
int shortestShift (int from, int to)
{
    auto d = ((to - from) % 12 + 12) % 12;
    return d > 6 ? d - 12 : d;
}

class StretchPanel final : public juce::Component,
                           public SpaceTarget,
                           private juce::Timer
{
public:
    StretchPanel (const juce::String& target, double lengthIn, const StretchRequest& initial, StretchCallbacks cb)
        : callbacks (std::move (cb)), lengthSeconds (lengthIn)
    {
        for (auto* l : { &info, &fromLabel, &toLabel, &bpm1, &bpm2, &tempoStatus, &lengthInfo, &pitchLabel,
                         &keyLabel, &newKeyLabel, &shiftLabel, &semitoneUnit, &keyStatus, &hint })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::black);
            addAndMakeVisible (l);
        }

        for (auto* l : { &tempoStatus, &keyStatus, &hint })
        {
            l->setColour (juce::Label::textColourId, juce::Colour (0xff555555));
            l->setFont (juce::FontOptions (13.0f));
        }

        info.setText ("Time stretch / pitch of: " + target, juce::dontSendNotification);
        hint.setText ("Leave both tempos empty to change only the pitch.", juce::dontSendNotification);

        for (auto* field : { &fromField, &toField, &shiftField })
        {
            field->setInputRestrictions (8, "+-.,0123456789");
            field->setJustification (juce::Justification::centredRight);
            field->onReturnKey = [this] { apply(); };
            field->onEscapeKey = [this] { close(); };
            addAndMakeVisible (field);
        }

        if (initial.fromBpm.has_value())  fromField.setText (formatNumber (*initial.fromBpm, 2), false);
        if (initial.toBpm.has_value())    toField.setText (formatNumber (*initial.toBpm, 2), false);
        shiftField.setText (formatSemitones (initial.semitones), false);

        fromField.onTextChange = [this] { validate(); };
        toField.onTextChange = [this] { validate(); };
        shiftField.onTextChange = [this] { shiftEdited(); };

        pitchMode.addItem ("Keep the original pitch", 1);
        pitchMode.addItem ("Change the pitch, keep the length", 2);
        pitchMode.addItem ("Like tape: speed and length change too", 3);
        pitchMode.setSelectedId (! initial.changePitch ? 1 : initial.tape ? 3 : 2, juce::dontSendNotification);
        pitchMode.onChange = [this] { updatePitchEnabled(); validate(); };
        addAndMakeVisible (pitchMode);

        for (int tonic = 0; tonic < 12; ++tonic)
            for (bool minor : { false, true })
                fromKey.addItem (keyName (tonic, minor), tonic * 2 + (minor ? 1 : 0) + 1);

        fromKey.setTextWhenNothingSelected ("(unknown)");
        fromKey.onChange = [this] { fillNewKeys(); keyPicked(); };
        toKey.setTextWhenNothingSelected ("-");
        toKey.onChange = [this] { keyPicked(); };
        addAndMakeVisible (fromKey);
        addAndMakeVisible (toKey);

        if (initial.fromKey >= 0)
            fromKey.setSelectedId (initial.fromKey + 1, juce::dontSendNotification);

        fillNewKeys();
        showKeyForShift();

        detectTempoButton.onClick = [this] { runTempoDetection(); };
        detectKeyButton.onClick = [this] { runKeyDetection(); };
        addAndMakeVisible (detectTempoButton);
        addAndMakeVisible (detectKeyButton);

        formants.setToggleState (initial.preserveFormants, juce::dontSendNotification);
        formants.onClick = [this] { validate(); };
        formants.setColour (juce::ToggleButton::textColourId, juce::Colours::black);
        formants.setColour (juce::ToggleButton::tickColourId, juce::Colours::black);
        formants.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff999999));
        addAndMakeVisible (formants);

        previewButton.onClick = [this] { togglePreview(); };
        previewButton.setTooltip ("Plays the first seconds with these settings (Space)");
        previewButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f6fbf));
        previewButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible (previewButton);

        okButton.onClick = [this] { apply(); };
        cancelButton.onClick = [this] { close(); };
        okButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
        cancelButton.addShortcut (juce::KeyPress (juce::KeyPress::escapeKey));
        addAndMakeVisible (okButton);
        addAndMakeVisible (cancelButton);

        setSize (580, 382);
        updatePitchEnabled();
        validate();
        startTimer (100);

        // Read the tempo (and the key) right away when they aren't known yet.
        juce::Component::SafePointer<StretchPanel> safeThis (this);
        const auto needTempo = ! initial.fromBpm.has_value(), needKey = initial.fromKey < 0;

        juce::MessageManager::callAsync ([safeThis, needTempo, needKey]
        {
            if (auto* p = safeThis.getComponent(); p != nullptr && needTempo)
                p->runTempoDetection();

            if (auto* p = safeThis.getComponent(); p != nullptr && needKey)
                p->runKeyDetection();
        });
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        info.setBounds (area.removeFromTop (24));
        area.removeFromTop (8);

        auto row = [&area] { auto r = area.removeFromTop (28); area.removeFromTop (4); return r; };

        auto r = row();
        fromLabel.setBounds (r.removeFromLeft (110));
        fromField.setBounds (r.removeFromLeft (80));
        bpm1.setBounds (r.removeFromLeft (44));
        detectTempoButton.setBounds (r.removeFromLeft (70));
        r.removeFromLeft (8);
        tempoStatus.setBounds (r);

        r = row();
        toLabel.setBounds (r.removeFromLeft (110));
        toField.setBounds (r.removeFromLeft (80));
        bpm2.setBounds (r.removeFromLeft (44));
        lengthInfo.setBounds (r);

        area.removeFromTop (10);
        r = row();
        pitchLabel.setBounds (r.removeFromLeft (110));
        pitchMode.setBounds (r.removeFromLeft (300));

        r = row();
        keyLabel.setBounds (r.removeFromLeft (110));
        fromKey.setBounds (r.removeFromLeft (130));
        r.removeFromLeft (6);
        detectKeyButton.setBounds (r.removeFromLeft (70));
        r.removeFromLeft (8);
        keyStatus.setBounds (r);

        r = row();
        newKeyLabel.setBounds (r.removeFromLeft (110));
        toKey.setBounds (r.removeFromLeft (130));

        r = row();
        shiftLabel.setBounds (r.removeFromLeft (110));
        shiftField.setBounds (r.removeFromLeft (80));
        semitoneUnit.setBounds (r.removeFromLeft (90));

        r = row();
        r.removeFromLeft (106);
        formants.setBounds (r);

        hint.setBounds (area.removeFromTop (22));

        auto buttons = area.removeFromBottom (28);
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        okButton.setBounds (buttons.removeFromRight (90));
        previewButton.setBounds (buttons.removeFromLeft (110));
    }

    void focusField()
    {
        auto& field = fromField.isEmpty() ? fromField : toField;
        field.grabKeyboardFocus();
        field.selectAll();
    }

    void close()
    {
        stopTimer();
        stopPreview();

        if (callbacks.onClose != nullptr)
            callbacks.onClose();
    }

    bool spacePressed() override   { togglePreview(); return true; }

private:
    int previewState() const   { return callbacks.previewState != nullptr ? callbacks.previewState() : 0; }

    void togglePreview()
    {
        if (previewState() != 0)
        {
            stopPreview();
            return;
        }

        startPreview();
    }

    void startPreview()
    {
        restartAt = 0;
        const auto request = read();

        if (request.has_value() && okButton.isEnabled() && callbacks.onPreview != nullptr)
            callbacks.onPreview (*request);
    }

    void stopPreview()
    {
        restartAt = 0;

        if (callbacks.onStopPreview != nullptr)
            callbacks.onStopPreview();
    }

    // The button follows the preview; a change made while it plays is heard
    // again shortly after (once the typing stops).
    void timerCallback() override
    {
        const auto state = previewState();
        previewButton.setButtonText (state == 1 ? "Preparing..." : state == 2 ? "Stop" : "Preview");
        previewButton.setEnabled (state != 0 || okButton.isEnabled());

        if (restartAt != 0 && juce::Time::getMillisecondCounter() >= restartAt)
        {
            restartAt = 0;

            if (state != 0)
                startPreview();
        }
    }

    void valuesChanged()
    {
        if (previewState() != 0)
            restartAt = juce::Time::getMillisecondCounter() + 600;
    }

    void runTempoDetection()
    {
        const auto estimate = callbacks.detectTempo != nullptr ? callbacks.detectTempo() : std::nullopt;

        if (! estimate.has_value())
        {
            tempoStatus.setText ("No clear beat found: type it", juce::dontSendNotification);
            return;
        }

        fromField.setText (formatNumber (estimate->bpm, 2), false);
        tempoStatus.setText (estimate->loopBars > 0
                                 ? "Read: " + formatNumber (estimate->bpm, 2) + " BPM (" + juce::String (estimate->loopBars)
                                       + (estimate->loopBars == 1 ? " bar loop)" : " bars loop)")
                                 : "Read: ~" + formatNumber (estimate->bpm, 2) + " BPM, check it",
                             juce::dontSendNotification);
        validate();

        if (fromField.hasKeyboardFocus (true))
            focusField();
    }

    void runKeyDetection()
    {
        const auto estimate = callbacks.detectKey != nullptr ? callbacks.detectKey() : std::nullopt;

        if (! estimate.has_value())
        {
            keyStatus.setText ("No key found", juce::dontSendNotification);
            return;
        }

        fromKey.setSelectedId (estimate->tonic * 2 + (estimate->minor ? 1 : 0) + 1, juce::sendNotificationSync);
        auto text = juce::String (estimate->confident ? "Read" : "Probably, check it");

        if (estimate->tuningCents != 0)
            text << " (" << (estimate->tuningCents > 0 ? "+" : "") << estimate->tuningCents << " cents)";

        keyStatus.setText (text, juce::dontSendNotification);
    }

    int currentFromKey() const   { return fromKey.getSelectedId() - 1; }

    // The new key list follows the mode of the current key (a shift keeps it).
    void fillNewKeys()
    {
        const auto key = currentFromKey();
        const auto previous = toKey.getSelectedId();
        toKey.clear (juce::dontSendNotification);

        if (key < 0)
            return;

        for (int tonic = 0; tonic < 12; ++tonic)
            toKey.addItem (keyName (tonic, (key & 1) != 0), tonic + 1);

        if (previous > 0)
            toKey.setSelectedId (previous, juce::dontSendNotification);
    }

    // Picking keys writes the shift; the shift is what gets applied.
    void keyPicked()
    {
        const auto key = currentFromKey();

        if (key < 0 || toKey.getSelectedId() <= 0)
            return;

        const auto shift = shortestShift (key / 2, toKey.getSelectedId() - 1);
        const auto current = parseSemitones (shiftField.getText());

        // Keep e.g. +12 or -7 when they already land on the chosen key.
        if (! current.has_value() || std::abs (*current - std::round (*current)) > 1.0e-9
            || ((((int) std::round (*current) - shift) % 12) != 0))
            shiftField.setText (formatSemitones (shift), false);

        validate();
    }

    void shiftEdited()
    {
        showKeyForShift();
        validate();
    }

    void showKeyForShift()
    {
        const auto key = currentFromKey();
        const auto shift = parseSemitones (shiftField.getText());

        if (key < 0 || ! shift.has_value())
            return;

        if (std::abs (*shift - std::round (*shift)) < 1.0e-9)
            toKey.setSelectedId ((((key / 2) + (int) std::round (*shift)) % 12 + 12) % 12 + 1, juce::dontSendNotification);
        else
            toKey.setSelectedId (0, juce::dontSendNotification);   // between two keys
    }

    bool tapeMode() const   { return pitchMode.getSelectedId() == 3; }

    void updatePitchEnabled()
    {
        const auto on = pitchMode.getSelectedId() >= 2;

        for (juce::Component* c : { (juce::Component*) &fromKey, (juce::Component*) &toKey, (juce::Component*) &shiftField,
                                    (juce::Component*) &newKeyLabel, (juce::Component*) &shiftLabel,
                                    (juce::Component*) &semitoneUnit })
            c->setEnabled (on);

        // Like tape, the tempo follows the pitch and the voice can't stay natural.
        formants.setEnabled (on && ! tapeMode());

        for (juce::Component* c : { (juce::Component*) &toField, (juce::Component*) &toLabel, (juce::Component*) &bpm2 })
            c->setEnabled (! tapeMode());

        hint.setText (tapeMode() ? "Like tape: lower = slower and longer, higher = faster and shorter."
                                 : "Leave both tempos empty to change only the pitch.",
                      juce::dontSendNotification);
    }

    std::optional<StretchRequest> read() const
    {
        StretchRequest r;
        const auto fromText = fromField.getText().trim(), toText = toField.getText().trim();

        r.tape = tapeMode();

        if (r.tape)
        {
            // Only the tempo now is used (to show the new one); the new tempo stays as typed.
            r.fromBpm = parseBpm (fromText);
            r.toBpm = parseBpm (toText);
        }
        else if (fromText.isNotEmpty() || toText.isNotEmpty())
        {
            r.fromBpm = parseBpm (fromText);
            r.toBpm = parseBpm (toText);

            if (! r.fromBpm.has_value() || ! r.toBpm.has_value())
                return {};
        }

        r.changePitch = pitchMode.getSelectedId() >= 2;
        r.fromKey = currentFromKey();
        r.preserveFormants = formants.getToggleState();

        const auto shift = parseSemitones (shiftField.getText());

        if (r.changePitch && ! shift.has_value())
            return {};

        r.semitones = shift.value_or (0.0);
        return r;
    }

    void validate()
    {
        const auto fromText = fromField.getText().trim(), toText = toField.getText().trim();
        const auto bothEmpty = fromText.isEmpty() && toText.isEmpty();
        const auto fromOk = bothEmpty || parseBpm (fromText).has_value() || (tapeMode() && fromText.isEmpty());
        const auto toOk = bothEmpty || tapeMode() || parseBpm (toText).has_value();
        const auto shiftOk = pitchMode.getSelectedId() < 2 || parseSemitones (shiftField.getText()).has_value();

        auto outline = [] (juce::TextEditor& f, bool ok)
        {
            f.setColour (juce::TextEditor::outlineColourId, ok ? juce::Colour (0xff808080) : juce::Colours::red);
            f.repaint();
        };

        outline (fromField, fromOk);
        outline (toField, toOk);
        outline (shiftField, shiftOk);

        const auto request = read();
        auto ok = request.has_value();
        juce::String lengthText;

        if (ok)
        {
            const auto settings = request->settings();

            if (settings.tape)
            {
                const auto ratio = settings.lengthRatio();

                if (! settings.changesSomething())
                {
                    ok = false;
                }
                else
                {
                    lengthText = "Length " + formatSeconds (lengthSeconds) + " -> " + formatSeconds (lengthSeconds * ratio);

                    if (request->fromBpm.has_value())
                        lengthText << ", " << formatNumber (*request->fromBpm, 1) << " -> "
                                   << formatNumber (*request->fromBpm / ratio, 1) << " BPM";
                }
            }
            else if (settings.timeRatio < minStretchRatio || settings.timeRatio > maxStretchRatio)
            {
                lengthText = "Too far: at most 4 times slower or faster";
                ok = false;
            }
            else if (! settings.changesSomething())
            {
                lengthText = bothEmpty ? juce::String() : "Same tempo: the length stays";
                ok = false;
            }
            else if (std::abs (settings.timeRatio - 1.0) > 1.0e-9)
            {
                lengthText = "Length " + formatSeconds (lengthSeconds) + " -> " + formatSeconds (lengthSeconds * settings.timeRatio)
                           + " (" + formatNumber (settings.timeRatio * 100.0, 1) + " %)";
            }
        }

        lengthInfo.setText (lengthText, juce::dontSendNotification);
        okButton.setEnabled (ok);
        valuesChanged();
    }

    void apply()
    {
        const auto request = read();

        if (! request.has_value() || ! okButton.isEnabled())
            return;

        auto onApply = callbacks.onApply;   // close() may delete this panel
        close();

        if (onApply != nullptr)
            onApply (*request);
    }

    StretchCallbacks callbacks;
    double lengthSeconds;

    juce::Label info, fromLabel { {}, "Tempo now:" }, toLabel { {}, "New tempo:" }, bpm1 { {}, "BPM" }, bpm2 { {}, "BPM" },
                tempoStatus, lengthInfo, pitchLabel { {}, "Pitch:" }, keyLabel { {}, "Key now:" },
                newKeyLabel { {}, "New key:" }, shiftLabel { {}, "Shift:" }, semitoneUnit { {}, "semitones" },
                keyStatus, hint;
    juce::TextEditor fromField, toField, shiftField;
    juce::ComboBox pitchMode, fromKey, toKey;
    juce::TextButton detectTempoButton { "Read" }, detectKeyButton { "Read" };
    juce::ToggleButton formants { "Keep the voice natural (formants)" };
    juce::TextButton okButton { "OK" }, cancelButton { "Cancel" }, previewButton { "Preview" };
    juce::uint32 restartAt = 0;   // when to play the preview again after a change; 0 = no
};

class StretchWindow final : public juce::DocumentWindow
{
public:
    StretchWindow (const juce::String& target, double lengthSeconds, const StretchRequest& initial, StretchCallbacks callbacks)
        : juce::DocumentWindow ("Time Stretch", juce::Colour (0xffd4d0c8), juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setContentOwned (new StretchPanel (target, lengthSeconds, initial, std::move (callbacks)), true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    void visibilityChanged() override
    {
        juce::DocumentWindow::visibilityChanged();

        if (isShowing())
            if (auto* panel = dynamic_cast<StretchPanel*> (getContentComponent()))
                panel->focusField();
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<StretchPanel*> (getContentComponent()))
            panel->close();
    }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createStretchWindow (const juce::String& target, double lengthSeconds,
                                                           const StretchRequest& initial, StretchCallbacks callbacks)
{
    return std::make_unique<StretchWindow> (target, lengthSeconds, initial, std::move (callbacks));
}
} // namespace wf
