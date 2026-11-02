#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Analysis.h"
#include "TimeStretch.h"
#include "Settings.h"

namespace wf
{
// What the Time Stretch window asks for: the tempo now and the tempo wanted
// (both empty = keep the length), and optionally a new pitch, typed in
// semitones or picked as "from key -> to key".
struct StretchRequest
{
    std::optional<double> fromBpm, toBpm;
    bool changePitch = false;
    int fromKey = -1;            // tonic * 2 + (minor ? 1 : 0); -1 = not known
    double semitones = 0.0;
    bool preserveFormants = false;
    bool tape = false;           // pitch "like tape": the speed and the length change with it

    StretchSettings settings() const;
};

struct StretchCallbacks
{
    std::function<std::optional<TempoEstimate>()> detectTempo;
    std::function<std::optional<KeyEstimate>()> detectKey;
    std::function<void (StretchRequest)> onApply;   // OK pressed
    std::function<void (StretchRequest)> onPreview; // play a few seconds of the result (again, with new values)
    std::function<void()> onStopPreview;
    std::function<int()> previewState;              // 0 off, 1 preparing, 2 playing
    std::function<void()> onClose;                  // the window can go
};

// Floating, non-modal like Volume. `target` names what it applies to;
// `lengthSeconds` is that audio's length, to show the new one. The tempo and
// the key are detected as soon as the window opens when they aren't known.
std::unique_ptr<juce::DocumentWindow> createStretchWindow (const juce::String& target, double lengthSeconds,
                                                           const StretchRequest& initial, StretchCallbacks callbacks);

// Remembered between starts (settings "stretch.last"): the new tempo, the
// pitch choice, the shift and the formants box.
juce::var stretchRequestToVar (const StretchRequest& r);
StretchRequest stretchRequestFromVar (const juce::var& v);

std::optional<double> parseSemitones (const juce::String& text);   // -24 ... +24, decimals allowed
} // namespace wf
