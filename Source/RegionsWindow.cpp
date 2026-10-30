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
        header.addColumn ("Type", 2, 60);
        header.addColumn ("Name", 3, 190);
        header.addColumn ("Start", 4, 100);
        header.addColumn ("End", 5, 100);
        header.addColumn ("Length", 6, 100);
        table.setModel (this);
        table.setRowHeight (22);
        addAndMakeVisible (table);

        title.setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (title);

        searchLabel.setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (searchLabel);
        search.setTextToShowWhenEmpty ("name or number", juce::Colours::grey);
        search.onTextChange = [this] { applyFilter(); };
        search.onEscapeKey = [this] { search.clear(); applyFilter(); };
        addAndMakeVisible (search);

        for (auto* b : { &play, &rename, &remove })
            addAndMakeVisible (b);

        play.onClick = [this] { if (auto* e = selectedEntry(); e != nullptr && callbacks.play) callbacks.play (*e); };
        rename.onClick = [this] { if (auto* e = selectedEntry(); e != nullptr && callbacks.rename) callbacks.rename (*e); };
        remove.onClick = [this] { if (auto* e = selectedEntry(); e != nullptr && callbacks.remove) callbacks.remove (*e); };
        setSize (640, 420);
    }

    void refresh (const juce::String& fileTitle, const std::vector<juce::int64>& markers, const MarkerNames& names,
                  const std::vector<AudioRegion>& regions, double rate)
    {
        sampleRate = rate;
        rows.clear();

        for (int i = 0; i < (int) markers.size(); ++i)
        {
            const auto it = names.find (markers[(size_t) i]);
            rows.push_back ({ { false, i }, it != names.end() ? it->second : juce::String(), markers[(size_t) i], -1 });
        }

        for (int i = 0; i < (int) regions.size(); ++i)
        {
            const auto& r = regions[(size_t) i];
            rows.push_back ({ { true, i }, r.name, r.start, r.end });
        }

        std::stable_sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b) { return a.start < b.start; });

        title.setText (fileTitle.isEmpty() ? juce::String ("No file open")
                                           : fileTitle + ": " + juce::String ((int) markers.size())
                                                 + (markers.size() == 1 ? " marker, " : " markers, ")
                                                 + juce::String ((int) regions.size())
                                                 + (regions.size() == 1 ? " region" : " regions")
                                                 + (rows.empty() ? "  (M adds a marker, R a region)" : ""),
                       juce::dontSendNotification);
        applyFilter();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        auto searchRow = r.removeFromTop (24);
        searchLabel.setBounds (searchRow.removeFromLeft (56));
        search.setBounds (searchRow);
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
    struct Row
    {
        ListEntry entry;
        juce::String name;
        juce::int64 start = 0, end = -1;   // end -1: a marker
    };

    // Rows whose name, number or type has the search text in it.
    void applyFilter()
    {
        const auto text = search.getText().trim();
        shown.clear();

        for (const auto& row : rows)
            if (text.isEmpty() || row.name.containsIgnoreCase (text) || number (row) == text
                || typeName (row).startsWithIgnoreCase (text))
                shown.push_back (row);

        table.updateContent();
        table.repaint();
        updateButtons();
    }

    static juce::String number (const Row& row)     { return juce::String (row.entry.index + 1); }
    static juce::String typeName (const Row& row)   { return row.entry.region ? "Region" : "Marker"; }

    int getNumRows() override   { return (int) shown.size(); }

    void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
    {
        g.fillAll (selected ? juce::Colour (0xff0a246a) : (row % 2 == 0 ? juce::Colours::white : juce::Colour (0xfff0f0f0)));
    }

    void paintCell (juce::Graphics& g, int row, int column, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) shown.size()))
            return;

        const auto& r = shown[(size_t) row];
        const auto isMarker = r.end < 0;
        auto time = [this] (juce::int64 s) { return WaveformView::formatTime ((double) s / sampleRate); };
        const auto text = column == 1 ? number (r)
                        : column == 2 ? typeName (r)
                        : column == 3 ? r.name
                        : column == 4 ? time (r.start)
                        : isMarker    ? juce::String()
                        : column == 5 ? time (r.end)
                                      : time (r.end - r.start);

        g.setColour (selected ? juce::Colours::white : juce::Colours::black);
        g.setFont (juce::FontOptions (14.0f));
        const auto left = column == 2 || column == 3;
        g.drawText (text, 6, 0, width - 8, height, left ? juce::Justification::centredLeft : juce::Justification::centredRight, true);
    }

    void cellClicked (int row, int, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, (int) shown.size()) && callbacks.select)
            callbacks.select (shown[(size_t) row].entry);
    }

    void cellDoubleClicked (int row, int, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, (int) shown.size()) && callbacks.rename)
            callbacks.rename (shown[(size_t) row].entry);
    }

    void selectedRowsChanged (int) override   { updateButtons(); }

    void deleteKeyPressed (int row) override
    {
        if (juce::isPositiveAndBelow (row, (int) shown.size()) && callbacks.remove)
            callbacks.remove (shown[(size_t) row].entry);
    }

    const ListEntry* selectedEntry() const
    {
        const auto row = table.getSelectedRow();
        return juce::isPositiveAndBelow (row, (int) shown.size()) ? &shown[(size_t) row].entry : nullptr;
    }

    void updateButtons()
    {
        const auto any = selectedEntry() != nullptr;

        for (auto* b : { &play, &rename, &remove })
            b->setEnabled (any);
    }

    RegionsCallbacks& callbacks;
    std::vector<Row> rows, shown;
    double sampleRate = 44100.0;
    juce::TableListBox table;
    juce::Label title, searchLabel { {}, "Search:" };
    juce::TextEditor search;
    juce::TextButton play { "Play" }, rename { "Rename..." }, remove { "Delete" };
};

RegionsWindow::RegionsWindow (RegionsCallbacks c)
    : juce::DocumentWindow ("Markers & Regions", panelColour, juce::DocumentWindow::closeButton), callbacks (std::move (c))
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

void RegionsWindow::refresh (const juce::String& fileTitle, const std::vector<juce::int64>& markers, const MarkerNames& markerNames,
                             const std::vector<AudioRegion>& regions, double sampleRate)
{
    panel->refresh (fileTitle, markers, markerNames, regions, sampleRate);
}

void RegionsWindow::closeButtonPressed()
{
    if (callbacks.onClose)
        callbacks.onClose();
}
} // namespace wf
