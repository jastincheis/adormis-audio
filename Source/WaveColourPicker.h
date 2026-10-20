#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Appearance.h"

// Right-click on a waveform > Waveform Colour... / Background...: the look of
// every window at once, changing as you pick.
namespace wf
{
// Ready colours to click, a picker for any other one, and Default.
class ColourPicker final : public juce::Component,
                           private juce::ChangeListener
{
public:
    ColourPicker (juce::Colour current, juce::Colour defaultColour, std::vector<juce::Colour> presets,
                  std::function<void (juce::Colour)> onPick, const juce::String& defaultLabel = "Default");
    ~ColourPicker() override;

    void resized() override;
    void setColour (juce::Colour c);   // shown, not sent

    // Light colours for waves, dark ones for backgrounds.
    static std::vector<juce::Colour> wavePresets();
    static std::vector<juce::Colour> backgroundPresets();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void pick (juce::Colour c);

    juce::Colour defaultColour;
    std::vector<juce::Colour> presets;
    std::function<void (juce::Colour)> onPick;
    juce::OwnedArray<juce::TextButton> swatches;
    juce::TextButton defaultButton { "Default" };
    juce::ColourSelector selector { juce::ColourSelector::showColourAtTop | juce::ColourSelector::showColourspace
                                    | juce::ColourSelector::showSliders };
    bool settingSelector = false;
};

// Textures (in the chosen colour), a photo of your own, or a plain colour.
class BackgroundPicker final : public juce::Component
{
public:
    BackgroundPicker (std::function<void()> onChanged, std::function<void()> onChoosePhoto);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void apply (appearance::Background b);
    void refresh();

    std::function<void()> onChanged, onChoosePhoto;
    juce::OwnedArray<juce::ImageButton> textures;
    juce::TextButton photoButton { "Photo..." }, noPhotoButton { "No photo" }, defaultButton { "Default background" };
    juce::Label photoLabel;
    ColourPicker colours;
};

// Open in a call-out next to `area` (in `parent`'s coordinates); `onChanged`
// runs after every pick (repaint the windows).
void showWaveColourPicker (juce::Component& parent, juce::Rectangle<int> area, std::function<void()> onChanged);
void showBackgroundPicker (juce::Component& parent, juce::Rectangle<int> area, std::function<void()> onChanged,
                           std::function<void()> onChoosePhoto);
} // namespace wf
