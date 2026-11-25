#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace wf
{
    // Pixels of a component as they are on screen, INCLUDING native child windows
    // (a plug-in's editor is usually its own native window, which
    // createComponentSnapshot cannot see). The component must be showing.
    // Returns an invalid image if the platform capture fails.
    juce::Image captureOnScreen (juce::Component& c);
}
