#include "WindowCaptureShared.h"

#import <AppKit/AppKit.h>

namespace wf
{
juce::Image captureOnScreen (juce::Component& c)
{
    auto* peer = c.getPeer();
    if (peer == nullptr || ! c.isShowing())
        return {};

    auto* view = (__bridge NSView*) peer->getNativeHandle();
    if (view == nil)
        return {};

    @autoreleasepool
    {
        const auto a = capture_detail::areaInPeer (c, *peer);
        NSRect r = NSMakeRect (a.getX(), a.getY(), a.getWidth(), a.getHeight());

        if (! [view isFlipped])
            r.origin.y = view.bounds.size.height - r.origin.y - r.size.height;

        // Draws the view and its subviews (the plug-in's own NSView among them)
        // into a bitmap; needs no screen-recording permission.
        NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect: r];
        if (rep == nil)
            return {};

        [view cacheDisplayInRect: r toBitmapImageRep: rep];
        NSData* png = [rep representationUsingType: NSBitmapImageFileTypePNG properties: @{}];

        if (png == nil)
            return {};

        return juce::ImageFileFormat::loadFrom (png.bytes, (size_t) png.length);
    }
}
}
