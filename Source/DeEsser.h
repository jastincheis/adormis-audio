#pragma once

#include "BassFinder.h"

namespace wf
{
// Tools > De-esser: finds the sibilants of the selection (or the whole file)
// and gives each one its own bell to turn it down or up, move its centre and
// widen or narrow it, going from one to the next. Floating, fixed size, in
// the dark theme of the Bass Finder; it uses the same host as the Bass Finder.
std::unique_ptr<juce::DocumentWindow> createDeEsserWindow (BassFinderSource source, BassFinderHost host,
                                                           std::function<void()> onClose);
} // namespace wf
