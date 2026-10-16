#pragma once

#include "BassFinder.h"

namespace wf
{
// The effect windows (dark, like the Bass Finder): parametric EQ, compressor,
// delay and reverb. Preview plays the selection (or the whole file) in a loop
// through the effect, and every change is heard at once; Apply writes it
// into the file (undoable), with the tail of a delay or reverb ringing on
// after the selection.
enum class EffectKind { eq, compressor, delay, reverb };

std::unique_ptr<juce::DocumentWindow> createEffectWindow (EffectKind kind, BassFinderSource source, BassFinderHost host,
                                                          std::function<void()> onClose);
} // namespace wf
