#pragma once

#include "BassFinder.h"

namespace wf
{
// The effect windows (dark, like the Bass Finder): parametric EQ, compressor,
// delay, reverb and MP3 Restore. Preview plays the selection (or the whole file) in a loop
// through the effect, and every change is heard at once; Apply writes it
// into the file (undoable), with the tail of a delay or reverb ringing on
// after the selection.
enum class EffectKind { eq, graphicEq, envelope, compressor, delay, reverb, mp3Restore, limiter, gate, chorus, flanger, vibrato, distortion,
                        noiseReduction, humRemoval, declick, clipRestore };

std::unique_ptr<juce::DocumentWindow> createEffectWindow (EffectKind kind, BassFinderSource source, BassFinderHost host,
                                                          std::function<void()> onClose);
} // namespace wf
