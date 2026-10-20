#include "WaveColourPicker.h"

namespace wf
{
namespace
{
constexpr int swatchSize = 28;
constexpr int swatchesPerRow = 8;
constexpr int selectorHeight = 260;
constexpr int textureSize = 56;
constexpr int texturesPerRow = 4;
} // namespace

std::vector<juce::Colour> ColourPicker::wavePresets()
{
    return { juce::Colour (0xffd8ecff), juce::Colour (0xff7fc8ff), juce::Colour (0xff6fe3e0), juce::Colour (0xff86e08a),
             juce::Colour (0xffd4ec6a), juce::Colour (0xffffe066), juce::Colour (0xffffb35c), juce::Colour (0xffff7a6b),
             juce::Colour (0xffff6fa8), juce::Colour (0xffd98cff), juce::Colour (0xffa99bff), juce::Colour (0xffc8b89a),
             juce::Colour (0xffffffff), juce::Colour (0xffb8c2cc), juce::Colour (0xff59b8ff), juce::Colour (0xff3fd18b) };
}

std::vector<juce::Colour> ColourPicker::backgroundPresets()
{
    return { juce::Colour (0xff686b71), juce::Colour (0xff3a3d42), juce::Colour (0xff1f2126), juce::Colour (0xff2b3a55),
             juce::Colour (0xff1e4d6b), juce::Colour (0xff23524a), juce::Colour (0xff2f5233), juce::Colour (0xff55602a),
             juce::Colour (0xff6b5a2a), juce::Colour (0xff6b3f26), juce::Colour (0xff6b2a2f), juce::Colour (0xff5a2a4f),
             juce::Colour (0xff45306b), juce::Colour (0xff4d4a63), juce::Colour (0xff7a6e62), juce::Colour (0xff8a8f99) };
}

ColourPicker::ColourPicker (juce::Colour current, juce::Colour defaultColourIn, std::vector<juce::Colour> presetsIn,
                            std::function<void (juce::Colour)> onPickIn, const juce::String& defaultLabel)
    : defaultColour (defaultColourIn), presets (std::move (presetsIn)), onPick (std::move (onPickIn))
{
    for (const auto& c : presets)
    {
        auto* b = swatches.add (new juce::TextButton());
        b->setColour (juce::TextButton::buttonColourId, c);
        b->setColour (juce::TextButton::buttonOnColourId, c);
        b->setTooltip ("#" + c.toDisplayString (false));
        b->onClick = [this, c] { pick (c); };
        addAndMakeVisible (b);
    }

    defaultButton.setButtonText (defaultLabel);
    defaultButton.setTooltip ("Back to the usual colour");
    defaultButton.onClick = [this] { pick (defaultColour); };
    addAndMakeVisible (defaultButton);

    selector.setCurrentColour (current, juce::dontSendNotification);
    selector.addChangeListener (this);
    addAndMakeVisible (selector);

    const auto rows = ((int) presets.size() + swatchesPerRow - 1) / swatchesPerRow;
    setSize (swatchesPerRow * (swatchSize + 4) + 8, rows * (swatchSize + 4) + 8 + 30 + selectorHeight);
}

ColourPicker::~ColourPicker()
{
    selector.removeChangeListener (this);
}

void ColourPicker::resized()
{
    auto area = getLocalBounds().reduced (4);

    for (int i = 0; i < swatches.size(); ++i)
    {
        if (i % swatchesPerRow == 0 && i > 0)
            area.removeFromTop (swatchSize + 4);

        swatches[i]->setBounds (area.getX() + (i % swatchesPerRow) * (swatchSize + 4) + 2, area.getY() + 2, swatchSize, swatchSize);
    }

    area.removeFromTop (swatchSize + 4);
    defaultButton.setBounds (area.removeFromTop (30).reduced (2));
    selector.setBounds (area);
}

void ColourPicker::setColour (juce::Colour c)
{
    settingSelector = true;
    selector.setCurrentColour (c, juce::dontSendNotification);
    settingSelector = false;
}

void ColourPicker::pick (juce::Colour c)
{
    // The picker shows the colour too; its own change call isn't a new pick.
    setColour (c);

    if (onPick)
        onPick (c);
}

void ColourPicker::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (! settingSelector && onPick)
        onPick (selector.getCurrentColour().withAlpha (1.0f));
}

//==============================================================================
BackgroundPicker::BackgroundPicker (std::function<void()> onChangedIn, std::function<void()> onChoosePhotoIn)
    : onChanged (std::move (onChangedIn)), onChoosePhoto (std::move (onChoosePhotoIn)),
      colours (appearance::background().colour, appearance::defaultBackground().colour, ColourPicker::backgroundPresets(),
               [this] (juce::Colour c)
               {
                   // A colour is for the textures: the photo, if any, gives way.
                   auto b = appearance::background();
                   b.colour = c;
                   b.photo = juce::File();
                   apply (b);
               }, "Default colour")
{
    for (int t = 0; t < appearance::numTextures; ++t)
    {
        auto* b = textures.add (new juce::ImageButton (appearance::textureName (t)));
        b->setTooltip (appearance::textureName (t));
        b->onClick = [this, t]
        {
            auto bg = appearance::background();
            bg.texture = t;
            bg.photo = juce::File();
            apply (bg);
        };
        addAndMakeVisible (b);
    }

    photoButton.setTooltip ("A picture of your own, any size: it's fitted to the window");
    photoButton.onClick = [this] { if (onChoosePhoto) onChoosePhoto(); };
    addAndMakeVisible (photoButton);

    noPhotoButton.onClick = [this]
    {
        auto b = appearance::background();
        b.photo = juce::File();
        apply (b);
    };
    addAndMakeVisible (noPhotoButton);

    defaultButton.onClick = [this]
    {
        apply (appearance::defaultBackground());
        colours.setColour (appearance::defaultBackground().colour);
    };
    addAndMakeVisible (defaultButton);

    photoLabel.setFont (juce::FontOptions (12.0f));
    photoLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
    addAndMakeVisible (photoLabel);

    addAndMakeVisible (colours);
    refresh();

    const auto rows = (appearance::numTextures + texturesPerRow - 1) / texturesPerRow;
    setSize (juce::jmax (colours.getWidth(), texturesPerRow * (textureSize + 18) + 8),
             8 + rows * (textureSize + 22) + 34 + 22 + 30 + colours.getHeight() + 4);
}

void BackgroundPicker::apply (appearance::Background b)
{
    appearance::setBackground (b);
    refresh();

    if (onChanged)
        onChanged();
}

void BackgroundPicker::refresh()
{
    const auto b = appearance::background();
    const auto usingPhoto = b.photo != juce::File();

    for (int t = 0; t < textures.size(); ++t)
    {
        const auto image = appearance::textureThumbnail (t, b.colour, textureSize, 2.0f);
        const auto chosen = ! usingPhoto && t == b.texture;
        textures[t]->setImages (false, true, true, image, 1.0f, {}, image, 0.85f, juce::Colours::white.withAlpha (0.15f),
                                image, 1.0f, {});
        textures[t]->getProperties().set ("chosen", chosen);
    }

    photoLabel.setText (usingPhoto ? b.photo.getFileName() : "No photo: a texture in the colour below",
                        juce::dontSendNotification);
    noPhotoButton.setEnabled (usingPhoto);
    repaint();
}

void BackgroundPicker::resized()
{
    auto area = getLocalBounds().reduced (4);
    const auto rows = (appearance::numTextures + texturesPerRow - 1) / texturesPerRow;
    auto grid = area.removeFromTop (rows * (textureSize + 22));
    const auto cellWidth = grid.getWidth() / texturesPerRow;

    for (int t = 0; t < textures.size(); ++t)
    {
        const auto cell = juce::Rectangle<int> (grid.getX() + (t % texturesPerRow) * cellWidth, grid.getY() + (t / texturesPerRow) * (textureSize + 22),
                                                cellWidth, textureSize + 22);
        textures[t]->setBounds (cell.withSizeKeepingCentre (textureSize, textureSize).withY (cell.getY() + 2));
    }

    auto buttons = area.removeFromTop (34).reduced (2, 3);
    photoButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2, 0));
    noPhotoButton.setBounds (buttons.reduced (2, 0));
    photoLabel.setBounds (area.removeFromTop (22));
    defaultButton.setBounds (area.removeFromTop (30).reduced (2));
    colours.setBounds (area.removeFromTop (colours.getHeight()));
}

void BackgroundPicker::paint (juce::Graphics& g)
{
    // Names under the textures, and a frame around the one in use.
    g.setFont (juce::FontOptions (11.0f));

    for (auto* b : textures)
    {
        const auto r = b->getBounds();

        if ((bool) b->getProperties()["chosen"])
        {
            g.setColour (juce::Colour (0xff3478f6));
            g.drawRect (r.expanded (3), 2);
        }

        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawText (b->getName(), r.getX() - 10, r.getBottom() + 3, r.getWidth() + 20, 14, juce::Justification::centred);
    }
}

//==============================================================================
void showWaveColourPicker (juce::Component& parent, juce::Rectangle<int> area, std::function<void()> onChanged)
{
    // Inside the main window: Hyprland moves focus onto separate windows and
    // the call-out would close at once.
    juce::CallOutBox::launchAsynchronously (
        std::make_unique<ColourPicker> (appearance::waveColour(), appearance::defaultWaveColour(), ColourPicker::wavePresets(),
                                        [onChanged = std::move (onChanged)] (juce::Colour c)
                                        {
                                            appearance::setWaveColour (c);
                                            if (onChanged)
                                                onChanged();
                                        }),
        area, &parent);
}

void showBackgroundPicker (juce::Component& parent, juce::Rectangle<int> area, std::function<void()> onChanged,
                           std::function<void()> onChoosePhoto)
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<BackgroundPicker> (std::move (onChanged), std::move (onChoosePhoto)),
                                            area, &parent);
}
} // namespace wf
