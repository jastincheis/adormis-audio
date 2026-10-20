#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// A panel with a player of its own (a preview, the Bass Creator's tabs...)
// takes Space in its window: true = handled. False lets it go on to the main
// playback.
struct SpaceTarget
{
    virtual ~SpaceTarget() = default;
    virtual bool spacePressed() = 0;
};

// Space plays / stops in every window of the program. Panels that handle
// keys themselves (effects, De-esser, Note Detector, Drum Rebuilder) take it
// first; otherwise the window's showing SpaceTarget gets it, and failing that
// the main playback starts or stops (Shift+Space = Play All). Buttons in those
// windows stop taking the keyboard, so Space doesn't press the last one
// clicked. Message boxes keep Space for their buttons.
class SpaceEverywhere final : private juce::Timer,
                              private juce::KeyListener
{
public:
    // `mainWindow` already has Space through its shortcuts; `play` gets true for Play All.
    SpaceEverywhere (juce::Component& mainWindow, std::function<void (bool all)> play);
    ~SpaceEverywhere() override;

private:
    void timerCallback() override;
    bool keyPressed (const juce::KeyPress&, juce::Component* originator) override;

    juce::Component& mainWindow;
    std::function<void (bool)> play;
    juce::Array<juce::Component::SafePointer<juce::Component>> windows;
};
} // namespace wf
