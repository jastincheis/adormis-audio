#include "WindowCaptureShared.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#ifndef PW_RENDERFULLCONTENT
 #define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace wf
{
juce::Image captureOnScreen (juce::Component& c)
{
    auto* peer = c.getPeer();
    if (peer == nullptr || ! c.isShowing())
        return {};

    auto hwnd = (HWND) peer->getNativeHandle();
    RECT rc {};
    if (hwnd == nullptr || ! GetClientRect (hwnd, &rc))
        return {};

    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return {};

    auto screenDC = GetDC (nullptr);
    auto memDC = CreateCompatibleDC (screenDC);

    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    auto bmp = CreateDIBSection (screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    juce::Image whole;

    if (bmp != nullptr && bits != nullptr)
    {
        auto old = SelectObject (memDC, bmp);

        // PW_RENDERFULLCONTENT (Windows 8.1+) asks DWM for the composed window,
        // child windows and GPU-drawn content included.
        if (PrintWindow (hwnd, memDC, PW_CLIENTONLY | PW_RENDERFULLCONTENT))
        {
            GdiFlush();
            whole = juce::Image (juce::Image::RGB, w, h, false);
            juce::Image::BitmapData out (whole, juce::Image::BitmapData::writeOnly);
            const auto* src = static_cast<const juce::uint8*> (bits);

            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const auto* p = src + ((size_t) y * (size_t) w + (size_t) x) * 4;   // B G R X
                    out.setPixelColour (x, y, juce::Colour (p[2], p[1], p[0]));
                }
        }

        SelectObject (memDC, old);
    }

    if (bmp != nullptr)
        DeleteObject (bmp);
    DeleteDC (memDC);
    ReleaseDC (nullptr, screenDC);

    return capture_detail::cropToComponent (whole, c, *peer);
}
}
