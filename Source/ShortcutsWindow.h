#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

// Read-only list of every command and its shortcut (Help > Keyboard Shortcuts).
class ShortcutsWindow final : public juce::DocumentWindow
{
public:
    explicit ShortcutsWindow (juce::KeyPressMappingSet& mappings);

    void closeButtonPressed() override  { setVisible (false); }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShortcutsWindow)
};
