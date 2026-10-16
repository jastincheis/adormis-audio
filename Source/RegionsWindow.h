#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WaveformView.h"

namespace wf
{
// View > Regions List: the active file's regions in a table. A click selects
// one in the file, a double-click plays it; Rename and Delete act on the row.
struct RegionsCallbacks
{
    std::function<void (int index)> select, play, rename, remove;
    std::function<void()> onClose;
};

class RegionsWindow final : public juce::DocumentWindow
{
public:
    explicit RegionsWindow (RegionsCallbacks callbacks);
    ~RegionsWindow() override;

    // Shows `regions` of `fileTitle` (sample positions at `sampleRate`).
    void refresh (const juce::String& fileTitle, const std::vector<AudioRegion>& regions, double sampleRate);

    void closeButtonPressed() override;

private:
    class Panel;
    Panel* panel = nullptr;
    RegionsCallbacks callbacks;
};
} // namespace wf
