#include "ShortcutsWindow.h"
#include "Commands.h"

namespace
{
class ShortcutsTable final : public juce::Component,
                             private juce::TableListBoxModel
{
public:
    explicit ShortcutsTable (juce::KeyPressMappingSet& mappingsIn) : mappings (mappingsIn)
    {
        // The keys as they are now (Options > Customize Keyboard can change them).
        for (const auto& def : wf::allCommands())
            if (! mappings.getKeyPressesAssignedToCommand (def.id).isEmpty())
                rows.push_back (&def);

        auto& header = table.getHeader();
        header.addColumn ("Menu",     1, 110);
        header.addColumn ("Command",  2, 260);
        header.addColumn ("Shortcut", 3, 220);

        table.setModel (this);
        addAndMakeVisible (table);
        setSize (620, 560);
    }

    void resized() override  { table.setBounds (getLocalBounds()); }

private:
    int getNumRows() override  { return (int) rows.size(); }

    void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
    {
        g.fillAll (selected ? juce::Colour (0xff0a246a)
                            : (row % 2 == 0 ? juce::Colours::white : juce::Colour (0xfff0f0f0)));
    }

    void paintCell (juce::Graphics& g, int row, int column, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) rows.size()))
            return;

        const auto& def = *rows[(size_t) row];
        const auto text = column == 1 ? juce::String (def.category)
                        : column == 2 ? juce::String (def.name)
                                      : wf::shortcutText (mappings.getKeyPressesAssignedToCommand (def.id));

        g.setColour (selected ? juce::Colours::white : juce::Colours::black);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (text, 6, 0, width - 8, height, juce::Justification::centredLeft, true);
    }

    juce::KeyPressMappingSet& mappings;
    juce::TableListBox table;
    std::vector<const wf::CommandDef*> rows;
};
} // namespace

ShortcutsWindow::ShortcutsWindow (juce::KeyPressMappingSet& mappings)
    : juce::DocumentWindow ("Keyboard Shortcuts", juce::Colours::lightgrey, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new ShortcutsTable (mappings), true);
    setResizable (true, false);
    // Stays in front of the main window when you click there.
    setAlwaysOnTop (true);
    centreWithSize (getWidth(), getHeight());
}
