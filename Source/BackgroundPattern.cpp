#include "BackgroundPattern.h"

namespace wf
{
namespace
{
const juce::Colour baseGrey (0xff686b71);

// Side view of an elephant facing right, about 40 x 30 units.
juce::Path elephantPath()
{
    juce::Path p;
    p.addEllipse (6.0f, 8.0f, 24.0f, 15.0f);              // body
    p.addEllipse (24.0f, 4.0f, 12.0f, 12.0f);             // head

    for (auto x : { 9.0f, 14.0f, 21.0f, 26.0f })          // legs
        p.addRoundedRectangle (x, 19.0f, 4.0f, 8.0f, 1.5f);

    juce::Path trunk;
    trunk.startNewSubPath (33.0f, 12.0f);
    trunk.cubicTo (38.0f, 14.0f, 38.5f, 21.0f, 36.0f, 25.0f);
    juce::PathStrokeType (3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
        .createStrokedPath (trunk, trunk);
    p.addPath (trunk);

    juce::Path tail;
    tail.startNewSubPath (7.0f, 12.0f);
    tail.lineTo (3.0f, 18.0f);
    juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
        .createStrokedPath (tail, tail);
    p.addPath (tail);

    return p;
}

juce::Path earPath()
{
    juce::Path p;
    p.addEllipse (23.0f, 6.0f, 7.0f, 10.0f);
    return p;
}

void addGrain (juce::Image& image, juce::Random& random)
{
    const juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);

    for (int y = 0; y < data.height; ++y)
    {
        for (int x = 0; x < data.width; ++x)
        {
            const auto shift = (float) random.nextInt ({ -7, 8 }) / 255.0f;
            auto c = data.getPixelColour (x, y);
            c = juce::Colour::fromFloatRGBA (juce::jlimit (0.0f, 1.0f, c.getFloatRed()   + shift),
                                             juce::jlimit (0.0f, 1.0f, c.getFloatGreen() + shift),
                                             juce::jlimit (0.0f, 1.0f, c.getFloatBlue()  + shift),
                                             1.0f);
            data.setPixelColour (x, y, c);
        }
    }
}

// Draws `shape` at (x, y) and again shifted by one tile in each direction so
// shapes crossing an edge reappear on the opposite side (seamless tiling).
void drawWrapped (juce::Graphics& g, const juce::Path& shape, juce::AffineTransform placement, float tile)
{
    for (auto dx : { -tile, 0.0f, tile })
        for (auto dy : { -tile, 0.0f, tile })
            g.fillPath (shape, placement.translated (dx, dy));
}
} // namespace

juce::Image makeBackgroundTile (float scale)
{
    const auto pixels = juce::roundToInt ((float) backgroundTileSize * scale);
    juce::Image image (juce::Image::ARGB, pixels, pixels, false);
    juce::Random random (0x57415645); // fixed seed: same pattern every launch

    {
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (scale));

        const auto tile = (float) backgroundTileSize;
        g.fillAll (baseGrey);

        // Soft mottling so the grey isn't flat.
        for (int i = 0; i < 80; ++i)
        {
            const auto r = random.nextFloat() * 50.0f + 20.0f;
            g.setColour ((random.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.025f));
            juce::Path blob;
            blob.addEllipse (-r, -r, r * 2.0f, r * 2.0f);
            drawWrapped (g, blob, juce::AffineTransform::translation (random.nextFloat() * tile, random.nextFloat() * tile), tile);
        }

        const auto elephant = elephantPath();
        const auto ear = earPath();

        // Elephants, on a jittered grid so they spread evenly.
        constexpr int cells = 6;
        const auto cell = tile / (float) cells;

        for (int cy = 0; cy < cells; ++cy)
        {
            for (int cx = 0; cx < cells; ++cx)
            {
                const auto size   = 0.55f + random.nextFloat() * 0.6f;
                const auto mirror = random.nextBool() ? -1.0f : 1.0f;
                const auto angle  = (random.nextFloat() - 0.5f) * 0.6f;
                const auto x = ((float) cx + 0.2f + random.nextFloat() * 0.6f) * cell;
                const auto y = ((float) cy + 0.2f + random.nextFloat() * 0.6f) * cell;

                const auto placement = juce::AffineTransform::translation (-20.0f, -15.0f)
                                           .scaled (size * mirror, size)
                                           .rotated (angle)
                                           .translated (x, y);

                const auto light = random.nextFloat() < 0.7f;
                g.setColour ((light ? juce::Colours::white : juce::Colours::black).withAlpha (light ? 0.13f : 0.10f));
                drawWrapped (g, elephant, placement, tile);

                g.setColour (juce::Colours::black.withAlpha (0.06f));
                drawWrapped (g, ear, placement, tile);
            }
        }

        // Stars scattered between the elephants.
        for (int i = 0; i < 95; ++i)
        {
            const auto outer = 2.5f + random.nextFloat() * 5.5f;
            juce::Path star;
            star.addStar ({}, 5, outer * 0.45f, outer, random.nextFloat() * juce::MathConstants<float>::twoPi);

            g.setColour (juce::Colours::white.withAlpha (0.10f + random.nextFloat() * 0.14f));
            drawWrapped (g, star, juce::AffineTransform::translation (random.nextFloat() * tile, random.nextFloat() * tile), tile);
        }
    }

    addGrain (image, random);
    return image;
}
} // namespace wf
