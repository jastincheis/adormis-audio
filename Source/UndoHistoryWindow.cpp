#include "UndoHistoryWindow.h"

namespace wf
{
namespace
{
const juce::Colour panelColour (0xffd4d0c8);
}

class UndoHistoryWindow::Panel final : public juce::Component,
                                       private juce::ListBoxModel
{
public:
    explicit Panel (UndoHistoryCallbacks& c) : callbacks (c)
    {
        list.setModel (this);
        list.setRowHeight (22);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::white);
        list.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff808080));
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        for (auto* l : { &title, &hint })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::black);
            addAndMakeVisible (l);
        }

        title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        hint.setFont (juce::FontOptions (12.0f));
        hint.setJustificationType (juce::Justification::topLeft);
        hint.setText ("Click a step to go back to just after it (or forward again). Grey steps were undone: "
                      "they can be redone until you make a new edit.",
                      juce::dontSendNotification);

        undoButton.onClick = [this] { if (current > 0 && callbacks.goTo) callbacks.goTo (current - 1); };
        redoButton.onClick = [this] { if (current < (int) steps.size() - 1 && callbacks.goTo) callbacks.goTo (current + 1); };

        for (auto* b : { &undoButton, &redoButton })
            addAndMakeVisible (b);

        setSize (380, 460);
    }

    void refresh (const juce::String& fileTitle, const juce::StringArray& done, const juce::StringArray& undone)
    {
        steps.clear();
        steps.add (fileTitle.isEmpty() ? juce::String ("No file open") : "The file as opened");
        steps.addArray (done);
        steps.addArray (undone);
        current = done.size();
        hasFile = fileTitle.isNotEmpty();
        title.setText (fileTitle.isEmpty() ? juce::String ("No file open") : "History of " + fileTitle, juce::dontSendNotification);
        list.updateContent();
        list.selectRow (current, false, true);
        list.scrollToEnsureRowIsOnscreen (current);
        list.repaint();
        undoButton.setEnabled (current > 0);
        redoButton.setEnabled (current < steps.size() - 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        hint.setBounds (r.removeFromBottom (34));
        auto buttons = r.removeFromBottom (28);
        r.removeFromBottom (8);
        list.setBounds (r);
        redoButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        undoButton.setBounds (buttons.removeFromRight (90));
    }

private:
    int getNumRows() override   { return hasFile ? steps.size() : 0; }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
    {
        if (! juce::isPositiveAndBelow (row, steps.size()))
            return;

        const auto isCurrent = row == current;
        const auto undone = row > current;
        g.fillAll (isCurrent ? juce::Colour (0xff0a246a) : (row % 2 == 0 ? juce::Colours::white : juce::Colour (0xfff0f0f0)));

        g.setColour (isCurrent ? juce::Colours::white : undone ? juce::Colour (0xff9a9a9a) : juce::Colours::black);
        g.setFont (juce::FontOptions (14.0f, row == 0 ? juce::Font::italic : juce::Font::plain));
        const auto number = row == 0 ? juce::String() : juce::String (row) + ".  ";
        g.drawText (number + steps[row] + (undone ? "   (undone)" : ""), 8, 0, width - 12, height, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (row != current && callbacks.goTo)
            callbacks.goTo (row);
    }

    UndoHistoryCallbacks& callbacks;
    juce::StringArray steps;
    int current = 0;
    bool hasFile = false;
    juce::ListBox list;
    juce::Label title, hint;
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" };
};

UndoHistoryWindow::UndoHistoryWindow (UndoHistoryCallbacks c)
    : juce::DocumentWindow ("Undo/Redo History", panelColour, juce::DocumentWindow::closeButton), callbacks (std::move (c))
{
    // Own title bar so it can be moved on window managers without decorations.
    setUsingNativeTitleBar (false);
    panel = new Panel (callbacks);
    setContentOwned (panel, true);
    setResizable (false, false);
    setAlwaysOnTop (true);
    centreWithSize (getWidth(), getHeight());
}

UndoHistoryWindow::~UndoHistoryWindow() = default;

void UndoHistoryWindow::refresh (const juce::String& fileTitle, const juce::StringArray& done, const juce::StringArray& undone)
{
    panel->refresh (fileTitle, done, undone);
}

void UndoHistoryWindow::closeButtonPressed()
{
    if (callbacks.onClose)
        callbacks.onClose();
}
} // namespace wf
