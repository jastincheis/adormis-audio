#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Textured grey backdrop with faint elephants, teddy bears and stars, drawn as a seamless
// tile so it can fill any area.
namespace wf
{
// Logical tile size; the image is rendered at `scale` for sharp HiDPI output.
constexpr int backgroundTileSize = 520;

juce::Image makeBackgroundTile (float scale);
} // namespace wf
