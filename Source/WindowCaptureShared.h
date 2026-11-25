#pragma once

#include "WindowCapture.h"

namespace wf::capture_detail
{
    // Area of c inside its peer's top-level component, in logical pixels.
    inline juce::Rectangle<int> areaInPeer (juce::Component& c, juce::ComponentPeer& peer)
    {
        return peer.getComponent().getLocalArea (&c, c.getLocalBounds());
    }

    // Crops a capture of the whole peer (physical pixels) down to c's area.
    inline juce::Image cropToComponent (const juce::Image& whole, juce::Component& c, juce::ComponentPeer& peer)
    {
        if (! whole.isValid())
            return {};

        const auto& top = peer.getComponent();
        if (top.getWidth() <= 0 || top.getHeight() <= 0)
            return {};

        const double sx = (double) whole.getWidth()  / top.getWidth();
        const double sy = (double) whole.getHeight() / top.getHeight();
        const auto a = areaInPeer (c, peer);
        const auto r = juce::Rectangle<int> (juce::roundToInt (a.getX() * sx), juce::roundToInt (a.getY() * sy),
                                             juce::roundToInt (a.getWidth() * sx), juce::roundToInt (a.getHeight() * sy))
                           .getIntersection (whole.getBounds());

        if (r.isEmpty())
            return {};

        return whole.getClippedImage (r).createCopy();
    }
}
