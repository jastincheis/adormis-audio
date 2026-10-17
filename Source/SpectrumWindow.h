#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MeterPanel.h"

namespace wf
{
// View > Spectrum Analysis: the frequencies in the audio. While something
// plays it follows what is heard (live); otherwise it shows the selection
// (its average) or, without a selection, the moment at the cursor. A second
// view, the sonogram, draws the selection as time x frequency.
struct SpectrumSource
{
    std::function<const juce::AudioBuffer<float>*()> audio;   // the active file (null: none)
    std::function<double()> sampleRate;
    std::function<juce::Range<juce::int64>()> selection;     // empty: use the cursor
    std::function<juce::int64()> cursor;
    std::function<bool()> playing;
    std::function<juce::String()> title;
    std::function<void()> onClose;
};

class SpectrumWindow final : public juce::DocumentWindow
{
public:
    SpectrumWindow (SpectrumSource source, MeterTap& tap);
    ~SpectrumWindow() override;

    // The file, the selection or the cursor changed: measured again (shortly after).
    void sourceChanged();

    void closeButtonPressed() override;

private:
    class Panel;
    std::unique_ptr<juce::LookAndFeel> look;
    Panel* panel = nullptr;
    std::function<void()> onClose;
};
} // namespace wf
