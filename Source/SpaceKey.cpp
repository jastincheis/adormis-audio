#include "SpaceKey.h"

namespace wf
{
namespace
{
void keepSpaceFromButtons (juce::Component& root)
{
    for (auto* c : root.getChildren())
    {
        if (dynamic_cast<juce::Button*> (c) != nullptr)
            c->setWantsKeyboardFocus (false);

        if (dynamic_cast<juce::TextEditor*> (c) == nullptr)
            keepSpaceFromButtons (*c);
    }
}

// The visible one: tabs not shown are hidden or out of the tree.
SpaceTarget* findShowingTarget (juce::Component& root)
{
    if (! root.isVisible())
        return nullptr;

    if (auto* t = dynamic_cast<SpaceTarget*> (&root))
        return t;

    for (auto* c : root.getChildren())
        if (auto* t = findShowingTarget (*c))
            return t;

    return nullptr;
}
} // namespace

SpaceEverywhere::SpaceEverywhere (juce::Component& main, std::function<void (bool)> playIn)
    : mainWindow (main), play (std::move (playIn))
{
    startTimer (400);
}

SpaceEverywhere::~SpaceEverywhere()
{
    for (auto& w : windows)
        if (auto* c = w.getComponent())
            c->removeKeyListener (this);
}

// New windows come and go all the time; every 0.4 s the new ones get the
// listener, and buttons added since (rows, tabs) lose the keyboard.
void SpaceEverywhere::timerCallback()
{
    windows.removeIf ([] (auto& w) { return w.getComponent() == nullptr; });

    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
    {
        auto* w = juce::TopLevelWindow::getTopLevelWindow (i);

        if (w == nullptr || w == mainWindow.getTopLevelComponent() || dynamic_cast<juce::AlertWindow*> (w) != nullptr)
            continue;

        if (! windows.contains (w))
        {
            w->addKeyListener (this);
            windows.add (w);
        }

        keepSpaceFromButtons (*w);
    }
}

bool SpaceEverywhere::keyPressed (const juce::KeyPress& key, juce::Component* originator)
{
    const auto mods = key.getModifiers();

    if (key.getKeyCode() != juce::KeyPress::spaceKey || mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    if (originator != nullptr)
        if (auto* target = findShowingTarget (*originator->getTopLevelComponent()))
            if (target->spacePressed())
                return true;

    if (play != nullptr)
        play (mods.isShiftDown());

    return true;
}
} // namespace wf
