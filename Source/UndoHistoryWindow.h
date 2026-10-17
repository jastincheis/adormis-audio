#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
// View > Undo/Redo History: the active file's edits by name, oldest first.
// A click on a step goes back (or forward again) to just after it; the
// first row is the file as it was opened. Undone steps stay listed, greyed,
// until a new edit replaces them.
struct UndoHistoryCallbacks
{
    std::function<void (int position)> goTo;   // 0 = as opened, n = after the n-th step
    std::function<void()> onClose;
};

class UndoHistoryWindow final : public juce::DocumentWindow
{
public:
    explicit UndoHistoryWindow (UndoHistoryCallbacks callbacks);
    ~UndoHistoryWindow() override;

    void refresh (const juce::String& fileTitle, const juce::StringArray& done, const juce::StringArray& undone);
    void closeButtonPressed() override;

private:
    class Panel;
    Panel* panel = nullptr;
    UndoHistoryCallbacks callbacks;
};
} // namespace wf
