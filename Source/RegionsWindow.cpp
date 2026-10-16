#include "RegionsWindow.h"

namespace wf
{
namespace
{
const juce::Colour panelColour (0xffd4d0c8);
}

class RegionsWindow::Panel final : public juce::Component,
                                   private juce::TableListBoxModel
{
public:
    explicit Panel (RegionsCallbacks& c) : callbacks (c)
    {
        auto& header = table.getHeader();
        header.addColumn ("#", 1, 36);
        header.addColumn ("Name", 2, 190);
        header.addColumn ("Start", 3, 110);
        header.addColumn ("End", 4, 110);
        header.addColumn ("Length", 5, 110);
        table.setModel (this);
        table.setRowHeight (22);
        addAndMakeVisible (table);

        title.setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (title);

        for (auto* b : { &play, &rename, &remove })
            addAndMakeVisible (b);

        play.onClick = [this] { if (auto row = selectedRow(); row >= 0 && callbacks.play) callbacks.play (row); };
        rename.onClick = [this] { if (auto row = selectedRow(); row >= 0 && callbacks.rename) callbacks.rename (row); };
        remove.onClick = [this] { if (auto row = selectedRow(); row >= 0 && callbacks.remove) callbacks.remove (row); };
        setSize (600, 380);
    }

    void refresh (const juce::String& fileTitle, const std::vector<AudioRegion>& r, double rate)
    {
        regions = r;
        sampleRate = rate;
        title.setText (fileTitle.isEmpty() ? juce::String ("No file open")
                                           : fileTitle + ": " + juce::String ((int) regions.size())
                                                 + (regions.size() == 1 ? " region" : " regions")
                                                 + (regions.empty() ? "  (select some audio and press R)" : ""),
                       juce::dontSendNotification);
        table.updateContent();
        table.repaint();
        updateButtons();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        auto buttons = r.removeFromBottom (28);
        r.removeFromBottom (8);
        table.setBounds (r);
        remove.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        rename.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        play.setBounds (buttons.removeFromRight (90));
    }

private:
    int getNumRows() override   { return (int) regions.size(); }

    void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
    {
        g.fillAll (selected ? juce::Colour (0xff0a246a) : (row % 2 == 0 ? juce::Colours::white : juce::Colour (0xfff0f0f0)));
    }

    void paintCell (juce::Graphics& g, int row, int column, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) regions.size()))
            return;

        const auto& r = regions[(size_t) row];
        auto time = [this] (juce::int64 s) { return WaveformView::formatTime ((double) s / sampleRate); };
        const auto text = column == 1 ? juce::String (row + 1)
                        : column == 2 ? r.name
                        : column == 3 ? time (r.start)
                        : column == 4 ? time (r.end)
                                      : time (r.end - r.start);

        g.setColour (selected ? juce::Colours::white : juce::Colours::black);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (text, 6, 0, width - 8, height, column == 2 ? juce::Justification::centredLeft : juce::Justification::centredRight, true);
    }

    void cellClicked (int row, int, const juce::MouseEvent&) override
    {
        if (callbacks.select) callbacks.select (row);
    }

    void cellDoubleClicked (int row, int, const juce::MouseEvent&) override
    {
        if (callbacks.play) callbacks.play (row);
    }

    void selectedRowsChanged (int) override   { updateButtons(); }

    void deleteKeyPressed (int row) override
    {
        if (callbacks.remove) callbacks.remove (row);
    }

    int selectedRow() const
    {
        const auto row = table.getSelectedRow();
        return juce::isPositiveAndBelow (row, (int) regions.size()) ? row : -1;
    }

    void updateButtons()
    {
        const auto any = selectedRow() >= 0;

        for (auto* b : { &play, &rename, &remove })
            b->setEnabled (any);
    }

    RegionsCallbacks& callbacks;
    std::vector<AudioRegion> regions;
    double sampleRate = 44100.0;
    juce::TableListBox table;
    juce::Label title;
    juce::TextButton play { "Play" }, rename { "Rename..." }, remove { "Delete" };
};

RegionsWindow::RegionsWindow (RegionsCallbacks c)
    : juce::DocumentWindow ("Regions List", panelColour, juce::DocumentWindow::closeButton), callbacks (std::move (c))
{
    // Own title bar so it can be moved on window managers without decorations.
    setUsingNativeTitleBar (false);
    panel = new Panel (callbacks);
    setContentOwned (panel, true);
    setResizable (false, false);
    setAlwaysOnTop (true);
    centreWithSize (getWidth(), getHeight());
}

RegionsWindow::~RegionsWindow() = default;

void RegionsWindow::refresh (const juce::String& fileTitle, const std::vector<AudioRegion>& regions, double sampleRate)
{
    panel->refresh (fileTitle, regions, sampleRate);
}

void RegionsWindow::closeButtonPressed()
{
    if (callbacks.onClose)
        callbacks.onClose();
}
} // namespace wf
