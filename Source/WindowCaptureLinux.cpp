#include "WindowCaptureShared.h"

#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace wf
{
// libX11 is opened at run time (as JUCE itself does), so nothing extra to link.
juce::Image captureOnScreen (juce::Component& c)
{
    auto* peer = c.getPeer();
    if (peer == nullptr || ! c.isShowing())
        return {};

    static void* lib = dlopen ("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (lib == nullptr)
        return {};

    using OpenFn   = Display* (*) (const char*);
    using CloseFn  = int (*) (Display*);
    using AttrFn   = Status (*) (Display*, Window, XWindowAttributes*);
    using GetFn    = XImage* (*) (Display*, Drawable, int, int, unsigned, unsigned, unsigned long, int);

    auto openD   = (OpenFn)   dlsym (lib, "XOpenDisplay");
    auto closeD  = (CloseFn)  dlsym (lib, "XCloseDisplay");
    auto attrs   = (AttrFn)   dlsym (lib, "XGetWindowAttributes");
    auto getImg  = (GetFn)    dlsym (lib, "XGetImage");

    if (openD == nullptr || closeD == nullptr || attrs == nullptr || getImg == nullptr)
        return {};

    auto* d = openD (nullptr);
    if (d == nullptr)
        return {};

    const auto win = (Window) (juce::pointer_sized_uint) peer->getNativeHandle();
    juce::Image whole;
    XWindowAttributes wa {};

    if (win != 0 && attrs (d, win, &wa) && wa.width > 0 && wa.height > 0)
    {
        if (auto* xi = getImg (d, win, 0, 0, (unsigned) wa.width, (unsigned) wa.height, AllPlanes, ZPixmap))
        {
            if (xi->bits_per_pixel == 32)
            {
                whole = juce::Image (juce::Image::RGB, wa.width, wa.height, false);
                juce::Image::BitmapData out (whole, juce::Image::BitmapData::writeOnly);

                for (int y = 0; y < wa.height; ++y)
                {
                    const auto* row = reinterpret_cast<const juce::uint8*> (xi->data) + (size_t) y * (size_t) xi->bytes_per_line;
                    for (int x = 0; x < wa.width; ++x)
                    {
                        const auto* p = row + (size_t) x * 4;   // B G R X (little-endian TrueColor)
                        out.setPixelColour (x, y, juce::Colour (p[2], p[1], p[0]));
                    }
                }
            }

            xi->f.destroy_image (xi);
        }
    }

    closeD (d);
    return capture_detail::cropToComponent (whole, c, *peer);
}
}
