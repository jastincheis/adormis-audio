#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// Asks for one or more files, like juce::FileChooser::launchAsync.
//
// Where the OS has a file dialog it is used. Otherwise (Linux without
// zenity/kdialog) JUCE's own browser is shown in a fixed-size window: JUCE's
// default one is resizable, so tiling window managers (Hyprland) tile it and
// shrink the main window, and when the main window grows back while WaveForge
// is busy opening or saving, JUCE misses that resize and leaves half of it
// unpainted. Fixed-size windows float instead, like the Mix and Save As windows.
class FilePicker
{
public:
    void launch (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                 std::function<void (const juce::Array<juce::File>&)> onResult);

private:
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::WildcardFileFilter> filter;
    std::unique_ptr<juce::FileBrowserComponent> browser;
    std::unique_ptr<juce::FileChooserDialogBox> box;
};
} // namespace wf
