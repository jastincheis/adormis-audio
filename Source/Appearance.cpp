#include "Appearance.h"
#include "BackgroundPattern.h"
#include "Settings.h"
#include <map>

namespace wf::appearance
{
namespace
{
const juce::Colour defaultWave (0xffd8ecff);
constexpr int smallTile = 96;           // the simple textures repeat every 96 px
constexpr int maxPhotoSide = 2560;      // bigger photos are made smaller once
constexpr float photoDim = 0.32f;       // black over a photo, so the waves read

// Kept as { "colour": "ffrrggbb" }: the settings hold JSON objects.
juce::Colour readColour (const juce::String& key, juce::Colour fallback)
{
    const auto text = wf::settings::get (key)["colour"].toString();
    return text.isEmpty() ? fallback : juce::Colour::fromString (text);
}

// What's on screen, read from the settings once.
struct State
{
    juce::Colour wave = readColour ("look.waveColour", defaultWave);
    Background background = []
    {
        auto b = defaultBackground();
        const auto v = wf::settings::get ("look.background");

        if (v.isObject())
        {
            b.texture = juce::jlimit (0, numTextures - 1, (int) v["texture"]);
            b.colour = v["colour"].toString().isNotEmpty() ? juce::Colour::fromString (v["colour"].toString()) : b.colour;
            b.photo = v["photo"].toString().isNotEmpty() ? juce::File (v["photo"].toString()) : juce::File();
        }

        return b;
    }();

    std::map<juce::int64, juce::Image> tiles;   // by texture, colour and pixel scale
    juce::File loadedPhotoFile;
    juce::Image photo;                          // the photo, at most maxPhotoSide
    juce::Image cover;                          // the photo cropped to the window's size
    juce::Rectangle<int> coverSize;
};

State& state()
{
    static State s;
    return s;
}

juce::Image loadPhoto (const juce::File& file)
{
    auto image = juce::ImageFileFormat::loadFrom (file);

    if (image.isValid() && juce::jmax (image.getWidth(), image.getHeight()) > maxPhotoSide)
    {
        const auto ratio = (double) maxPhotoSide / juce::jmax (image.getWidth(), image.getHeight());
        image = image.rescaled (juce::roundToInt (image.getWidth() * ratio), juce::roundToInt (image.getHeight() * ratio),
                                juce::Graphics::highResamplingQuality);
    }

    return image.isValid() ? image.convertedToFormat (juce::Image::RGB) : juce::Image();
}

const juce::Image& currentPhoto()
{
    auto& s = state();

    if (s.loadedPhotoFile != s.background.photo)
    {
        s.loadedPhotoFile = s.background.photo;
        s.photo = s.background.photo == juce::File() ? juce::Image() : loadPhoto (s.background.photo);
        s.cover = {};
    }

    return s.photo;
}

// Draws `shape` again one tile over in each direction, so it continues
// across the edges (the tile repeats seamlessly).
void drawWrapped (juce::Graphics& g, const juce::Path& shape, float tile, bool stroke = false, float thickness = 1.0f)
{
    for (auto dx : { -tile, 0.0f, tile })
        for (auto dy : { -tile, 0.0f, tile })
        {
            if (stroke)
                g.strokePath (shape, juce::PathStrokeType (thickness), juce::AffineTransform::translation (dx, dy));
            else
                g.fillPath (shape, juce::AffineTransform::translation (dx, dy));
        }
}

juce::Image makeSmallTile (int texture, juce::Colour colour, float scale)
{
    const auto pixels = juce::roundToInt ((float) smallTile * scale);
    juce::Image image (juce::Image::ARGB, pixels, pixels, false);
    juce::Random random (0x41444f52);
    const auto t = (float) smallTile;

    {
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.fillAll (colour);
        juce::Path p;

        switch (texture)
        {
            case stripes:   // diagonal, every 24 px
                for (float o = -t; o < 2.0f * t; o += 24.0f)
                {
                    p.startNewSubPath (o, 0.0f);
                    p.lineTo (o + t, t);
                }
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.strokePath (p, juce::PathStrokeType (8.0f));
                break;

            case dots:   // offset rows, like a honeycomb
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 4; ++col)
                        p.addEllipse ((float) col * 24.0f + (row % 2 == 0 ? 6.0f : 18.0f) - 3.5f, (float) row * 24.0f + 12.0f - 3.5f, 7.0f, 7.0f);
                g.setColour (juce::Colours::white.withAlpha (0.11f));
                drawWrapped (g, p, t);
                break;

            case grid:
                for (float x = 0.0f; x < t; x += 24.0f)
                {
                    p.addRectangle (x, 0.0f, 1.0f, t);
                    p.addRectangle (0.0f, x, t, 1.0f);
                }
                g.setColour (juce::Colours::white.withAlpha (0.08f));
                g.fillPath (p);
                break;

            case diamonds:   // a checkerboard turned 45 degrees
                for (int row = 0; row <= 4; ++row)
                    for (int col = 0; col <= 4; ++col)
                        if ((row + col) % 2 == 0)
                        {
                            const auto cx = (float) col * 24.0f, cy = (float) row * 24.0f;
                            p.startNewSubPath (cx, cy - 12.0f);
                            p.lineTo (cx + 12.0f, cy);
                            p.lineTo (cx, cy + 12.0f);
                            p.lineTo (cx - 12.0f, cy);
                            p.closeSubPath();
                        }
                g.setColour (juce::Colours::black.withAlpha (0.09f));
                g.fillPath (p);
                break;

            case waves:   // soft horizontal waves, one period per tile
                for (float y = 8.0f; y < t; y += 16.0f)
                {
                    p.startNewSubPath (0.0f, y);

                    for (float x = 2.0f; x <= t; x += 2.0f)
                        p.lineTo (x, y + 4.0f * std::sin (x / t * juce::MathConstants<float>::twoPi));
                }
                g.setColour (juce::Colours::white.withAlpha (0.08f));
                drawWrapped (g, p, t, true, 1.6f);
                break;

            default:
                break;
        }
    }

    wf::addGrain (image, random);
    return image;
}

const juce::Image& tileFor (int texture, juce::Colour colour, float scale)
{
    auto& tiles = state().tiles;
    const auto key = ((juce::int64) texture << 48) ^ ((juce::int64) colour.getARGB() << 12) ^ (juce::int64) juce::roundToInt (scale * 100.0f);

    if (auto it = tiles.find (key); it != tiles.end())
        return it->second;

    if (tiles.size() > 24)
        tiles.clear();

    auto image = texture == teddyBears ? wf::makeBackgroundTile (scale, colour, true)
               : texture == grain      ? wf::makeBackgroundTile (scale, colour, false)
                                       : makeSmallTile (texture, colour, scale);
    return tiles[key] = std::move (image);
}

// The photo cropped to fill `size` (the main window, in physical pixels).
const juce::Image& coverFor (juce::Rectangle<int> size)
{
    auto& s = state();
    const auto& photo = currentPhoto();

    if (photo.isValid() && (s.cover.isNull() || s.coverSize != size))
    {
        s.coverSize = size;
        s.cover = juce::Image (juce::Image::RGB, juce::jmax (1, size.getWidth()), juce::jmax (1, size.getHeight()), false);
        juce::Graphics g (s.cover);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (photo, s.cover.getBounds().toFloat(), juce::RectanglePlacement::fillDestination);
        g.fillAll (juce::Colours::black.withAlpha (photoDim));
    }

    return s.cover;
}

void paintTexture (juce::Graphics& g, juce::Rectangle<int> area, int texture, juce::Colour colour, float scale)
{
    if (texture == plain)
    {
        g.setColour (colour);
        g.fillRect (area);
        return;
    }

    g.setFillType (juce::FillType (tileFor (texture, colour, scale), juce::AffineTransform::scale (1.0f / scale)));
    g.fillRect (area);
}
} // namespace

juce::Colour defaultWaveColour()   { return defaultWave; }
juce::Colour waveColour()          { return state().wave; }

void setWaveColour (juce::Colour c)
{
    state().wave = c;
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("colour", c.toString());
    wf::settings::set ("look.waveColour", c == defaultWave ? juce::var() : juce::var (obj));
}

juce::String textureName (int texture)
{
    static const char* names[] { "Teddy bears", "Grain", "Plain", "Stripes", "Dots", "Grid", "Diamonds", "Waves" };
    return juce::isPositiveAndBelow (texture, (int) numTextures) ? names[texture] : "";
}

Background defaultBackground()
{
    return { teddyBears, wf::backgroundBaseColour(), {} };
}

Background background()   { return state().background; }

void setBackground (const Background& b)
{
    state().background = b;
    state().tiles.clear();

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("texture", b.texture);
    obj->setProperty ("colour", b.colour.toString());
    obj->setProperty ("photo", b.photo.getFullPathName());
    wf::settings::set ("look.background", b == defaultBackground() ? juce::var() : juce::var (obj));
}

bool canUsePhoto (const juce::File& file)
{
    return file.existsAsFile() && juce::ImageFileFormat::findImageFormatForFileExtension (file) != nullptr
        && juce::ImageFileFormat::loadFrom (file).isValid();
}

void paintBackground (juce::Graphics& g, juce::Component& c, juce::Rectangle<int> area)
{
    const auto scale = (float) g.getInternalContext().getPhysicalPixelScaleFactor();
    const auto& b = state().background;

    if (b.photo != juce::File() && currentPhoto().isValid())
    {
        auto* top = c.getTopLevelComponent();
        const auto inTop = top->getLocalArea (&c, area);
        const auto& cover = coverFor ((top->getLocalBounds().toFloat() * scale).getSmallestIntegerContainer());
        const auto src = (inTop.toFloat() * scale).getSmallestIntegerContainer();
        g.drawImage (cover, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                     src.getX(), src.getY(), src.getWidth(), src.getHeight());
        return;
    }

    paintTexture (g, area, b.texture, b.colour, scale);
}

juce::Image textureThumbnail (int texture, juce::Colour colour, int size, float scale)
{
    const auto pixels = juce::roundToInt ((float) size * scale);
    juce::Image image (juce::Image::ARGB, pixels, pixels, true);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));

    if (texture == plain)
        g.fillAll (colour);
    else
    {
        // The real tile, shrunk a little so a bear or two fits in.
        const auto shrink = texture == teddyBears ? 0.45f : 0.75f;
        g.setFillType (juce::FillType (tileFor (texture, colour, scale), juce::AffineTransform::scale (shrink / scale)));
        g.fillRect (0, 0, size, size);
    }

    return image;
}
} // namespace wf::appearance
