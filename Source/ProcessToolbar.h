#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

// Bottom strip of one-click buttons (transport, undo/redo, processes).
// Each button invokes an application command and greys out with it.
class ProcessToolbar final : public juce::Component,
                             private juce::ApplicationCommandManagerListener
{
public:
    explicit ProcessToolbar (juce::ApplicationCommandManager& manager);
    ~ProcessToolbar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int preferredHeight = 46;

    // The Volume button opens a quick list (-10 ... +10 dB); picking one calls
    // this. "Custom..." in that list opens the Volume window instead.
    std::function<void (float decibels)> onQuickVolume;

private:
    class IconButton;
    using IconPainter = std::function<void (juce::Graphics&, juce::Rectangle<float>)>;

    void addButton (juce::CommandID id, juce::Colour tint, IconPainter painter);
    void addGap();
    void showVolumeMenu (juce::Component& button);
    void refreshEnabled();

    void applicationCommandInvoked (const juce::ApplicationCommandTarget::InvocationInfo&) override {}
    void applicationCommandListChanged() override  { refreshEnabled(); }

    juce::ApplicationCommandManager& commands;
    std::vector<std::unique_ptr<IconButton>> buttons;
    std::vector<int> gapsBefore;   // button indices that start a new group

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProcessToolbar)
};
