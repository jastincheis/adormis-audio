#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// How the program looks, the same in every window and remembered between
// starts: the waveform colour and the backdrop of the workspace and of every
// waveform (right-click on a waveform > Waveform Colour... / Background...).
namespace wf::appearance
{
juce::Colour defaultWaveColour();
juce::Colour waveColour();
void setWaveColour (juce::Colour);

// Ready textures, drawn in the background colour.
enum Texture { teddyBears = 0, grain, plain, stripes, dots, grid, diamonds, waves, numTextures };
juce::String textureName (int texture);

struct Background
{
    int texture = teddyBears;
    juce::Colour colour;   // what the texture is made of
    juce::File photo;      // when set, shown instead of the texture

    bool operator== (const Background&) const = default;
};

Background defaultBackground();
Background background();
void setBackground (const Background&);

// A photo of any size: loaded once and made smaller when it's huge; it then
// covers the whole main window (cropped to fit, never stretched). False when
// the file isn't an image that can be read.
bool canUsePhoto (const juce::File&);

// Fills `area` of `c` with the backdrop. A photo is laid across the whole
// window, so every waveform shows its own part of it, slightly darkened so
// the waves stay easy to see.
void paintBackground (juce::Graphics& g, juce::Component& c, juce::Rectangle<int> area);

// A small square sample of a texture (the Background picker's buttons).
juce::Image textureThumbnail (int texture, juce::Colour colour, int size, float scale);
} // namespace wf::appearance
