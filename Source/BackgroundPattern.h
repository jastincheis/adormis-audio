#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Textured grey backdrop with faint elephants, teddy bears and stars, drawn as a seamless
// tile so it can fill any area.
namespace wf
{
// Logical tile size; the image is rendered at `scale` for sharp HiDPI output.
constexpr int backgroundTileSize = 520;

// The grey the backdrop is made of (Background > Default).
juce::Colour backgroundBaseColour();

// `base`: the backdrop's colour; without `animals`, only its soft mottling
// and grain (the "Grain" texture).
juce::Image makeBackgroundTile (float scale, juce::Colour base = backgroundBaseColour(), bool animals = true);

// Adds +-7 levels of random noise to every pixel (so flat colour isn't flat).
void addGrain (juce::Image& image, juce::Random& random);
} // namespace wf
