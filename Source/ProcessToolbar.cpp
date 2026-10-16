#include "ProcessToolbar.h"
#include "Commands.h"

namespace
{
constexpr int buttonSize = 34;
constexpr int buttonGap  = 5;
constexpr int groupGap   = 18;

const juce::Colour ink (0xfff7f4ee);

// All icons are drawn in a 24 x 24 box that is scaled into the button.
juce::AffineTransform boxTransform (juce::Rectangle<float> area)
{
    return juce::AffineTransform::scale (area.getWidth() / 24.0f, area.getHeight() / 24.0f)
               .translated (area.getX(), area.getY());
}

void strokeIcon (juce::Graphics& g, const juce::Path& p, juce::Rectangle<float> area, float thickness = 2.2f)
{
    g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                  boxTransform (area));
}

void fillIcon (juce::Graphics& g, const juce::Path& p, juce::Rectangle<float> area)
{
    g.fillPath (p, boxTransform (area));
}

juce::Path arrowHead (juce::Point<float> tip, float angle, float size = 4.5f)
{
    juce::Path p;
    p.addTriangle (0.0f, -size, size * 0.9f, size * 0.55f, -size * 0.9f, size * 0.55f);
    p.applyTransform (juce::AffineTransform::rotation (angle).translated (tip));
    return p;
}

// --- Icons -------------------------------------------------------------------

void iconPlay (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path p;
    p.addTriangle (8.0f, 5.0f, 19.0f, 12.0f, 8.0f, 19.0f);
    p = p.createPathWithRoundedCorners (2.0f);
    fillIcon (g, p, a);
}

void iconStop (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path p;
    p.addRoundedRectangle (6.5f, 6.5f, 11.0f, 11.0f, 2.5f);
    fillIcon (g, p, a);
}

void iconUndo (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path p;
    p.addCentredArc (12.0f, 13.0f, 7.0f, 6.0f, 0.0f, -1.9f, 1.6f, true);
    strokeIcon (g, p, a);
    fillIcon (g, arrowHead ({ 5.5f, 10.5f }, -2.6f), a);
}

void iconRedo (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path p;
    p.addCentredArc (12.0f, 13.0f, 7.0f, 6.0f, 0.0f, 1.9f, -1.6f, true);
    strokeIcon (g, p, a);
    fillIcon (g, arrowHead ({ 18.5f, 10.5f }, 2.6f), a);
}

// Fades: a row of round-topped columns growing or shrinking.
void iconColumns (juce::Graphics& g, juce::Rectangle<float> a, bool rising)
{
    juce::Path p;

    for (int i = 0; i < 5; ++i)
    {
        const auto h = 3.0f + 3.2f * (float) (rising ? i : 4 - i);
        p.addRoundedRectangle (3.5f + (float) i * 3.7f, 20.0f - h, 2.6f, h, 1.3f);
    }

    fillIcon (g, p, a);
}

void iconFadeIn (juce::Graphics& g, juce::Rectangle<float> a)   { iconColumns (g, a, true); }
void iconFadeOut (juce::Graphics& g, juce::Rectangle<float> a)  { iconColumns (g, a, false); }

// Mute: a sleepy "z Z" above a flat line.
void iconMute (juce::Graphics& g, juce::Rectangle<float> a)
{
    auto zig = [] (float x, float y, float s)
    {
        juce::Path z;
        z.startNewSubPath (x, y);
        z.lineTo (x + s, y);
        z.lineTo (x, y + s);
        z.lineTo (x + s, y + s);
        return z;
    };

    strokeIcon (g, zig (5.0f, 8.0f, 4.5f), a, 1.8f);
    strokeIcon (g, zig (11.5f, 4.0f, 7.0f), a, 2.0f);

    juce::Path line;
    line.startNewSubPath (4.0f, 19.0f);
    line.lineTo (20.0f, 19.0f);
    strokeIcon (g, line, a, 2.0f);
}

// Normalize: arrows from the middle pushing out to two ceiling lines.
void iconNormalize (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path p;
    p.startNewSubPath (4.0f, 4.0f);   p.lineTo (20.0f, 4.0f);
    p.startNewSubPath (4.0f, 20.0f);  p.lineTo (20.0f, 20.0f);
    p.startNewSubPath (12.0f, 9.5f);  p.lineTo (12.0f, 14.5f);
    strokeIcon (g, p, a, 1.9f);

    fillIcon (g, arrowHead ({ 12.0f, 7.0f }, 0.0f, 3.6f), a);
    fillIcon (g, arrowHead ({ 12.0f, 17.0f }, juce::MathConstants<float>::pi, 3.6f), a);
}

// Reverse: a hairpin turn.
void iconReverse (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path p;
    p.startNewSubPath (19.0f, 7.0f);
    p.lineTo (10.0f, 7.0f);
    p.addCentredArc (10.0f, 12.0f, 5.0f, 5.0f, 0.0f, 0.0f, -juce::MathConstants<float>::pi, false);
    p.lineTo (17.0f, 17.0f);
    strokeIcon (g, p, a);
    fillIcon (g, arrowHead ({ 19.0f, 17.0f }, juce::MathConstants<float>::halfPi), a);
}

// Invert: a mountain above the line mirrored into a valley below.
void iconInvert (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path solid;
    solid.addTriangle (4.0f, 11.0f, 9.5f, 4.0f, 15.0f, 11.0f);
    fillIcon (g, solid, a);

    juce::Path hollow;
    hollow.addTriangle (9.0f, 13.0f, 14.5f, 20.0f, 20.0f, 13.0f);
    strokeIcon (g, hollow, a, 1.6f);

    juce::Path axis;
    axis.startNewSubPath (2.5f, 12.0f);
    axis.lineTo (21.5f, 12.0f);
    strokeIcon (g, axis, a, 1.0f);
}

// DC offset: a wave pulled back onto the centre line.
void iconDcOffset (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path wave;
    wave.startNewSubPath (3.0f, 8.0f);
    wave.cubicTo (6.0f, 3.0f, 9.0f, 3.0f, 12.0f, 8.0f);
    wave.cubicTo (15.0f, 13.0f, 18.0f, 13.0f, 21.0f, 8.0f);
    strokeIcon (g, wave, a, 1.8f);

    juce::Path centre;
    centre.startNewSubPath (3.0f, 19.0f);
    centre.lineTo (21.0f, 19.0f);
    strokeIcon (g, centre, a, 1.2f);

    fillIcon (g, arrowHead ({ 12.0f, 17.0f }, juce::MathConstants<float>::pi, 3.4f), a);
}

// Volume: a speaker with sound waves (asks for the dB amount).
void iconVolume (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path speaker;
    speaker.startNewSubPath (3.5f, 9.5f);
    speaker.lineTo (7.5f, 9.5f);
    speaker.lineTo (12.5f, 5.0f);
    speaker.lineTo (12.5f, 19.0f);
    speaker.lineTo (7.5f, 14.5f);
    speaker.lineTo (3.5f, 14.5f);
    speaker.closeSubPath();
    fillIcon (g, speaker, a);

    juce::Path waves;
    waves.addCentredArc (13.0f, 12.0f, 3.5f, 3.5f, 0.0f, 0.75f, 2.39f, true);
    waves.addCentredArc (13.0f, 12.0f, 7.0f, 7.0f, 0.0f, 0.75f, 2.39f, true);
    strokeIcon (g, waves, a, 1.8f);
}

// Time stretch: a wave being pulled wider, arrows out on both sides.
void iconStretch (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path wave;
    wave.startNewSubPath (4.0f, 9.0f);
    wave.cubicTo (6.5f, 3.0f, 9.0f, 3.0f, 12.0f, 9.0f);
    wave.cubicTo (15.0f, 15.0f, 17.5f, 15.0f, 20.0f, 9.0f);
    strokeIcon (g, wave, a, 1.8f);

    juce::Path line;
    line.startNewSubPath (5.5f, 19.0f);
    line.lineTo (18.5f, 19.0f);
    strokeIcon (g, line, a, 1.6f);

    fillIcon (g, arrowHead ({ 2.5f, 19.0f }, -juce::MathConstants<float>::halfPi, 3.4f), a);
    fillIcon (g, arrowHead ({ 21.5f, 19.0f }, juce::MathConstants<float>::halfPi, 3.4f), a);
}

// Bass Finder: a bass clef (hook and two dots) over a small magnifier.
void iconBassFinder (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path clef;
    clef.startNewSubPath (5.0f, 8.5f);
    clef.cubicTo (5.0f, 4.0f, 14.0f, 3.5f, 14.0f, 9.0f);
    clef.cubicTo (14.0f, 14.0f, 9.5f, 17.0f, 4.5f, 19.5f);
    strokeIcon (g, clef, a, 2.0f);

    juce::Path dots;
    dots.addEllipse (17.0f, 5.6f, 2.6f, 2.6f);
    dots.addEllipse (17.0f, 10.6f, 2.6f, 2.6f);
    dots.addEllipse (4.4f, 7.0f, 3.4f, 3.4f);
    fillIcon (g, dots, a);
}

// New: a blank page with a beat grid (markers) on it.
void iconNew (juce::Graphics& g, juce::Rectangle<float> a)
{
    juce::Path page;
    page.addRoundedRectangle (4.0f, 3.5f, 16.0f, 17.0f, 2.0f);
    strokeIcon (g, page, a, 1.6f);

    juce::Path grid;
    for (int i = 0; i < 4; ++i)
    {
        const auto x = 7.0f + (float) i * 3.3f;
        grid.startNewSubPath (x, i % 2 == 0 ? 7.5f : 10.0f);
        grid.lineTo (x, 17.0f);
    }
    strokeIcon (g, grid, a, 1.4f);
}

// Group tints.
const juce::Colour fileTint      (0xff5d7682);
const juce::Colour transportTint (0xff3f7f5f);
const juce::Colour historyTint   (0xff5a6378);
const juce::Colour fadeTint      (0xffb0703a);
const juce::Colour levelTint     (0xff4a6fa5);
const juce::Colour shapeTint     (0xff7a5a8c);
const juce::Colour timeTint      (0xff2f7f86);
const juce::Colour bassTint      (0xffc0640f);
} // namespace

//==============================================================================
class ProcessToolbar::IconButton final : public juce::Button
{
public:
    IconButton (const juce::String& name, juce::Colour tintIn, IconPainter painterIn)
        : juce::Button (name), tint (tintIn), painter (std::move (painterIn))
    {
        setTooltip (name);

        // Clicking must not steal focus from the data window being processed.
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        const auto enabled = isEnabled();

        auto base = enabled ? tint : tint.withSaturation (0.0f).withAlpha (0.45f);
        if (enabled && highlighted) base = base.brighter (0.25f);
        if (enabled && down)        base = base.darker (0.25f);

        juce::ColourGradient fill (base.brighter (0.18f), bounds.getTopLeft(),
                                   base.darker (0.18f), bounds.getBottomLeft(), false);
        g.setGradientFill (fill);
        g.fillRoundedRectangle (bounds, 7.0f);

        g.setColour (juce::Colours::black.withAlpha (enabled ? 0.35f : 0.15f));
        g.drawRoundedRectangle (bounds, 7.0f, 1.0f);

        g.setColour (ink.withAlpha (enabled ? 1.0f : 0.55f));
        painter (g, bounds.reduced (6.0f).translated (0.0f, down ? 1.0f : 0.0f));
    }

private:
    juce::Colour tint;
    IconPainter painter;
};

//==============================================================================
ProcessToolbar::ProcessToolbar (juce::ApplicationCommandManager& manager)
    : commands (manager)
{
    addButton (wf::fileNew,        fileTint,      iconNew);
    addGap();
    addButton (wf::optPlay,        transportTint, iconPlay);
    addButton (wf::optStop,        transportTint, iconStop);
    addGap();
    addButton (wf::editUndo,       historyTint,   iconUndo);
    addButton (wf::editRedo,       historyTint,   iconRedo);
    addGap();
    addButton (wf::procFadeIn,     fadeTint,      iconFadeIn);
    addButton (wf::procFadeOut,    fadeTint,      iconFadeOut);
    addButton (wf::procMute,       fadeTint,      iconMute);
    addGap();
    addButton (wf::procNormalize,  levelTint,     iconNormalize);
    addButton (wf::procVolume,     levelTint,     iconVolume);
    addButton (wf::procDcOffset,   levelTint,     iconDcOffset);
    addGap();
    addButton (wf::procReverse,    shapeTint,     iconReverse);
    addButton (wf::procInvert,     shapeTint,     iconInvert);
    addGap();
    addButton (wf::procTimeStretch, timeTint,     iconStretch);
    addButton (wf::toolsBassFinder, bassTint,     iconBassFinder);

    commands.addListener (this);
    refreshEnabled();
}

void ProcessToolbar::showVolumeMenu (juce::Component& button)
{
    // Two columns: quieter (-1 ... -10 dB) on the left, louder (+1 ... +10 dB)
    // on the right. Item ids are 1..20; the value comes from the id.
    constexpr int customId = 1000;
    juce::PopupMenu menu;

    for (int db = 1; db <= 10; ++db)
        menu.addItem (10 + db, juce::String::fromUTF8 ("\xe2\x88\x92") + juce::String (db) + " dB");   // U+2212 minus

    menu.addSeparator();
    menu.addItem (customId, "Custom...");
    menu.addColumnBreak();

    for (int db = 1; db <= 10; ++db)
        menu.addItem (db, "+" + juce::String (db) + " dB");

    const juce::Component::SafePointer<ProcessToolbar> safeThis (this);
    // Drawn inside the main window rather than as its own window: Hyprland
    // (focus follows mouse) moves focus onto a separate menu window, and JUCE
    // then closes the menu as soon as the mouse slips off it.
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&button)
                                                  .withParentComponent (getTopLevelComponent()),
                        [safeThis] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            if (result == customId)
                                safeThis->commands.invokeDirectly (wf::procVolume, true);
                            else if (safeThis->onQuickVolume != nullptr)
                                safeThis->onQuickVolume (result > 10 ? (float) -(result - 10) : (float) result);
                        });
}

ProcessToolbar::~ProcessToolbar()
{
    commands.removeListener (this);
}

void ProcessToolbar::addButton (juce::CommandID id, juce::Colour tint, IconPainter painter)
{
    const auto* def = wf::findCommand (id);
    auto name = def != nullptr ? juce::String (def->name).upToFirstOccurrenceOf ("...", false, false) : juce::String();

    if (def != nullptr && ! def->keys.empty())
        name << "  (" << wf::shortcutText (*def) << ")";

    auto button = std::make_unique<IconButton> (name, tint, std::move (painter));
    if (id == wf::procVolume)
        button->onClick = [this, b = button.get()] { showVolumeMenu (*b); };
    else
        button->onClick = [this, id] { commands.invokeDirectly (id, true); };
    button->setComponentID (juce::String (id));
    addAndMakeVisible (*button);
    buttons.push_back (std::move (button));
}

void ProcessToolbar::addGap()
{
    gapsBefore.push_back ((int) buttons.size());
}

void ProcessToolbar::refreshEnabled()
{
    for (auto& button : buttons)
    {
        const auto id = button->getComponentID().getIntValue();
        juce::ApplicationCommandInfo info (id);
        const auto* target = commands.getTargetForCommand (id, info);
        button->setEnabled (target != nullptr && (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0);
    }
}

void ProcessToolbar::paint (juce::Graphics& g)
{
    juce::ColourGradient fill (juce::Colour (0xffe2ded6), 0.0f, 0.0f,
                               juce::Colour (0xffc9c4ba), 0.0f, (float) getHeight(), false);
    g.setGradientFill (fill);
    g.fillAll();

    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());

    // Thin dividers between groups.
    g.setColour (juce::Colours::black.withAlpha (0.18f));

    for (auto index : gapsBefore)
        if (juce::isPositiveAndBelow (index, (int) buttons.size()))
            g.drawVerticalLine (buttons[(size_t) index]->getX() - groupGap / 2, 8.0f, (float) getHeight() - 8.0f);
}

void ProcessToolbar::resized()
{
    auto x = 10;
    const auto y = (getHeight() - buttonSize) / 2;

    for (int i = 0; i < (int) buttons.size(); ++i)
    {
        if (i > 0)
            x += std::find (gapsBefore.begin(), gapsBefore.end(), i) != gapsBefore.end() ? groupGap : buttonGap;

        buttons[(size_t) i]->setBounds (x, y, buttonSize, buttonSize);
        x += buttonSize;
    }
}
