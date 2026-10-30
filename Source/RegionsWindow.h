#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WaveformView.h"

namespace wf
{
// View > Markers & Regions: the active file's markers and regions in one
// table, in time order, with a search box. A click goes to the row in the
// file, a double-click renames it; Play, Rename and Delete act on the row.
struct ListEntry
{
    bool region = false;
    int index = 0;   // in the file's markers or regions
};

struct RegionsCallbacks
{
    std::function<void (ListEntry)> select, play, rename, remove;
    std::function<void()> onClose;
};

class RegionsWindow final : public juce::DocumentWindow
{
public:
    explicit RegionsWindow (RegionsCallbacks callbacks);
    ~RegionsWindow() override;

    // Shows the markers and regions of `fileTitle` (sample positions at `sampleRate`).
    void refresh (const juce::String& fileTitle, const std::vector<juce::int64>& markers, const MarkerNames& markerNames,
                  const std::vector<AudioRegion>& regions, double sampleRate);

    void closeButtonPressed() override;

private:
    class Panel;
    Panel* panel = nullptr;
    RegionsCallbacks callbacks;
};
} // namespace wf
