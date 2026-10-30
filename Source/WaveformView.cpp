#include "WaveformView.h"
#include "Appearance.h"
#include "Settings.h"
#include <numeric>

namespace
{
constexpr int rulerHeight     = 22;
constexpr int scrollBarHeight = 14;
constexpr int regionLaneHeight = 17;
constexpr int laneGap         = 14;   // the drag strip between channels (and under a mono one)
constexpr int minSelectPixels = 4;    // a press must move this far before it selects

const juce::Colour waveSelected    (0xffffffff);
const juce::Colour selectionShade  = juce::Colours::black.withAlpha (0.38f);
const juce::Colour cursorColour    (0xffffffff);
const juce::Colour playheadColour  (0xffffc23a);
const juce::Colour markerColour    (0xffff8a3d);
const juce::Colour barMarkerColour (0xffe8344a);   // a marker that begins a bar
const juce::Colour beatLineColour  (0xff8fe3ff);   // the selection's beats
} // namespace

WaveformView::WaveformView()
{
    scrollBar.addListener (this);
    scrollBar.setAutoHide (false);
    addAndMakeVisible (scrollBar);
    setOpaque (true);
}

void WaveformView::setAudio (const juce::AudioBuffer<float>* newBuffer, double newSampleRate)
{
    buffer = newBuffer;
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    cursor = 0;
    selection = {};
    playhead = -1;
    knownLength = totalSamples();
    zoomToFit();
    notifyChanged();
}

void WaveformView::audioChanged()
{
    // Was the whole file on screen? Then it still is (with what was pasted or
    // added at the start or the end); else the view keeps its zoom.
    const auto shownEnd = viewStart + (double) waveArea().getWidth() * samplesPerPixel;
    const auto wasWhole = viewStart <= 0.5 && shownEnd >= (double) knownLength - samplesPerPixel;
    knownLength = totalSamples();

    cursor = juce::jmin (cursor, totalSamples());
    selection = selection.getIntersectionWith ({ 0, totalSamples() });

    if (wasWhole)
        zoomToFit();
    else
        setZoom (samplesPerPixel, viewStart, (float) waveArea().getX());
}

juce::String WaveformView::formatTime (double seconds)
{
    const auto totalMs = juce::roundToInt (seconds * 1000.0);
    const auto ms = totalMs % 1000;
    const auto s  = (totalMs / 1000) % 60;
    const auto m  = (totalMs / 60000) % 60;
    const auto h  = totalMs / 3600000;

    return juce::String::formatted ("%02d:%02d:%02d.%03d", h, m, s, ms);
}

//==============================================================================
juce::Rectangle<int> WaveformView::waveArea() const
{
    const auto lane = regions != nullptr && ! regions->empty() ? regionLaneHeight : 0;
    return getLocalBounds().withTrimmedTop (rulerHeight + lane).withTrimmedBottom (scrollBarHeight);
}

juce::Rectangle<int> WaveformView::regionLane() const
{
    if (regions == nullptr || regions->empty())
        return {};

    return getLocalBounds().withTrimmedTop (rulerHeight).withHeight (regionLaneHeight);
}

int WaveformView::regionAt (juce::Point<int> position) const
{
    if (! regionLane().contains (position) || regions == nullptr)
        return -1;

    // The shortest region under the point (a short one drawn over a long one).
    int best = -1;

    for (int i = 0; i < (int) regions->size(); ++i)
    {
        const auto& r = (*regions)[(size_t) i];

        if ((float) position.x >= sampleToX ((double) r.start) && (float) position.x <= sampleToX ((double) r.end)
            && (best < 0 || r.end - r.start < (*regions)[(size_t) best].end - (*regions)[(size_t) best].start))
            best = i;
    }

    return best;
}

void WaveformView::paintRegions (juce::Graphics& g, juce::Rectangle<int> lane)
{
    static const juce::Colour palette[] { juce::Colour (0xff4a90d9), juce::Colour (0xff3fb27f), juce::Colour (0xffd9a441),
                                          juce::Colour (0xffb05fd0), juce::Colour (0xffd9534f), juce::Colour (0xff46b8c8) };
    g.setColour (juce::Colour (0xff2b2f36));
    g.fillRect (lane);

    // Longest first, so short regions are drawn on top.
    std::vector<int> order ((size_t) regions->size());
    std::iota (order.begin(), order.end(), 0);
    std::sort (order.begin(), order.end(), [this] (int a, int b)
    {
        const auto& ra = (*regions)[(size_t) a];
        const auto& rb = (*regions)[(size_t) b];
        return ra.end - ra.start > rb.end - rb.start;
    });

    const auto wave = waveArea();

    for (auto i : order)
    {
        const auto& r = (*regions)[(size_t) i];
        const auto x1 = sampleToX ((double) r.start), x2 = sampleToX ((double) r.end);

        if (x2 < (float) lane.getX() || x1 > (float) lane.getRight())
            continue;

        const auto colour = palette[(size_t) i % std::size (palette)];
        const juce::Rectangle<float> bar (x1, (float) lane.getY() + 2.0f, std::max (2.0f, x2 - x1), (float) lane.getHeight() - 4.0f);
        g.setColour (colour.withAlpha (0.85f));
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (r.name, bar.reduced (4.0f, 0.0f).withLeft (std::max (bar.getX() + 4.0f, (float) lane.getX() + 2.0f)),
                    juce::Justification::centredLeft, true);

        // The edges down through the waveform, faint.
        g.setColour (colour.withAlpha (0.55f));
        g.fillRect (x1, (float) wave.getY(), 1.0f, (float) wave.getHeight());
        g.fillRect (x2 - 1.0f, (float) wave.getY(), 1.0f, (float) wave.getHeight());
    }
}

double WaveformView::xToSample (float x) const
{
    return viewStart + (double) (x - (float) waveArea().getX()) * samplesPerPixel;
}

float WaveformView::sampleToX (double sample) const
{
    return (float) waveArea().getX() + (float) ((sample - viewStart) / samplesPerPixel);
}

void WaveformView::clampView()
{
    const auto visible = (double) waveArea().getWidth() * samplesPerPixel;
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, (double) totalSamples() - visible), viewStart);
}

void WaveformView::setZoom (double newSamplesPerPixel, double anchorSample, float anchorX)
{
    const auto width = juce::jmax (1, waveArea().getWidth());
    const auto maxSpp = juce::jmax (1.0 / 16.0, (double) totalSamples() / (double) width);
    samplesPerPixel = juce::jlimit (1.0 / 16.0, maxSpp, newSamplesPerPixel);
    viewStart = anchorSample - (double) (anchorX - (float) waveArea().getX()) * samplesPerPixel;
    clampView();
    updateScrollBar();
    repaint();
}

void WaveformView::zoomIn()
{
    setZoom (samplesPerPixel / 2.0, (double) cursor, juce::jlimit ((float) waveArea().getX(), (float) waveArea().getRight(), sampleToX ((double) cursor)));
}

void WaveformView::zoomOut()
{
    setZoom (samplesPerPixel * 2.0, (double) cursor, juce::jlimit ((float) waveArea().getX(), (float) waveArea().getRight(), sampleToX ((double) cursor)));
}

void WaveformView::zoomToFit()
{
    const auto width = juce::jmax (1, waveArea().getWidth());
    setZoom ((double) juce::jmax ((juce::int64) 1, totalSamples()) / (double) width, 0.0, (float) waveArea().getX());
}

void WaveformView::zoomToSelection()
{
    if (selection.isEmpty())
    {
        setZoom (1.0 / 16.0, (double) cursor, (float) waveArea().getCentreX());
        return;
    }

    const auto width = juce::jmax (1, waveArea().getWidth());
    setZoom ((double) selection.getLength() / (double) width, (double) selection.getStart(), (float) waveArea().getX());
}

void WaveformView::ensureVisible (juce::int64 sample)
{
    const auto visible = (double) waveArea().getWidth() * samplesPerPixel;

    if ((double) sample < viewStart || (double) sample > viewStart + visible)
    {
        viewStart = (double) sample - visible * 0.1;
        clampView();
        updateScrollBar();
        repaint();
    }
}

void WaveformView::centreOn (double sample)
{
    const auto before = viewStart;
    viewStart = sample - (double) waveArea().getWidth() * samplesPerPixel / 2.0;
    clampView();

    if (! juce::approximatelyEqual (before, viewStart))
    {
        updateScrollBar();
        repaint();
    }
}

void WaveformView::updateScrollBar()
{
    scrollBar.setRangeLimits (0.0, (double) juce::jmax ((juce::int64) 1, totalSamples()), juce::dontSendNotification);
    scrollBar.setCurrentRange (viewStart, (double) waveArea().getWidth() * samplesPerPixel, juce::dontSendNotification);
}

void WaveformView::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    viewStart = newRangeStart;
    clampView();
    repaint();
}

//==============================================================================
juce::int64 WaveformView::sampleAt (juce::Point<int> position) const
{
    return juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) position.x));
}

juce::int64 WaveformView::snapDrop (juce::int64 start, juce::int64 length, int& snappedTo) const
{
    constexpr float tolerancePx = 12.0f;
    snappedTo = -1;

    if (! hasAudio())
        return start;

    // Candidates: every marker, plus the start and end of the file.
    std::vector<std::pair<juce::int64, int>> targets { { 0, -2 }, { totalSamples(), -2 } };

    if (markers != nullptr)
        for (int i = 0; i < (int) markers->size(); ++i)
            targets.emplace_back ((*markers)[(size_t) i], i);

    auto best = tolerancePx + 1.0f;
    auto result = start;

    for (const auto& [position, id] : targets)
    {
        // Snap the start of the dropped audio, or its end.
        for (const auto candidate : { position, position - length })
        {
            if (candidate < 0)
                continue;

            const auto distance = std::abs ((float) ((double) (candidate - start) / samplesPerPixel));

            if (distance <= tolerancePx && distance < best)
            {
                best = distance;
                result = candidate;
                snappedTo = id;
            }
        }
    }

    return result;
}

void WaveformView::setDropPreview (juce::Range<juce::int64> range, int snappedTo)
{
    if (range == dropPreview && snappedTo == dropSnappedTo)
        return;

    dropPreview = range;
    dropSnappedTo = snappedTo;
    repaint();
}

void WaveformView::setCursor (juce::int64 sample)
{
    cursor = juce::jlimit ((juce::int64) 0, totalSamples(), sample);
    repaint();
    notifyChanged();
}

juce::Rectangle<int> WaveformView::laneBounds (int channel) const
{
    const auto area = waveArea();
    const auto channels = hasAudio() ? buffer->getNumChannels() : 1;
    const auto gaps = juce::jmax (0, channels - 1);   // gaps only between stereo channels; mono fills the lane
    const auto laneHeight = (area.getHeight() - laneGap * gaps) / juce::jmax (1, channels);
    return { area.getX(), area.getY() + channel * (laneHeight + laneGap), area.getWidth(), laneHeight };
}

WaveformView::DragOptions WaveformView::dragOptions()
{
    DragOptions o;
    const auto step = wf::settings::get ("drag.arrowPixels");

    if (! step.isVoid())    o.arrowPixels = juce::jlimit (1, 200, (int) step);

    return o;
}

bool WaveformView::snapToZeroCrossings = false;
int WaveformView::selectionBeats = 4;
bool WaveformView::showBeatLines = true;

juce::Value& WaveformView::zoomOnCursor()
{
    struct Saver final : juce::Value::Listener
    {
        void valueChanged (juce::Value& v) override { wf::settings::set ("view.zoomOnCursor", (bool) v.getValue()); }
    };

    static juce::Value value;
    static Saver saver;

    if (value.getValue().isVoid())
    {
        const auto stored = wf::settings::get ("view.zoomOnCursor");
        value = ! stored.isVoid() && (bool) stored;
        value.addListener (&saver);
    }

    return value;
}

// The nearest sample (within 50 ms, or a few pixels when zoomed out further)
// where the wave changes sign: on `channel`, or on all channels together
// (their sum) when it is -1. Of the two samples around the crossing, the one
// nearer zero. Unchanged when there is none (silence, a DC offset).
juce::int64 WaveformView::nearestZeroCrossing (juce::int64 sample, int channel) const
{
    if (! hasAudio())
        return sample;

    const auto total = totalSamples();
    const auto reach = (juce::int64) std::max (0.05 * sampleRate, 6.0 * samplesPerPixel);
    const auto channels = buffer->getNumChannels();

    auto valueAt = [&] (juce::int64 i)
    {
        if (channel >= 0)
            return buffer->getSample (std::min (channel, channels - 1), (int) i);

        float sum = 0.0f;

        for (int ch = 0; ch < channels; ++ch)
            sum += buffer->getSample (ch, (int) i);

        return sum;
    };

    if (sample <= 0 || sample >= total)
        return sample;

    for (juce::int64 d = 0; d <= reach; ++d)
        for (auto i : { sample - d, sample + d })
        {
            if (i < 1 || i >= total)
                continue;

            const auto a = valueAt (i - 1), b = valueAt (i);

            if ((a <= 0.0f && b > 0.0f) || (a >= 0.0f && b < 0.0f) || std::abs (b) < 1.0e-9f)
                return std::abs (a) < std::abs (b) ? i - 1 : i;
        }

    return sample;
}

juce::int64 WaveformView::arrowStep() const
{
    return std::max ((juce::int64) 1, (juce::int64) std::llround (dragOptions().arrowPixels * samplesPerPixel));
}

void WaveformView::nudgeCursor (int direction, bool extend)
{
    if (! hasAudio())
        return;

    const auto step = arrowStep();
    const auto from = extend || selection.isEmpty() ? cursor : (direction < 0 ? selection.getStart() : selection.getEnd());
    const auto to = juce::jlimit ((juce::int64) 0, totalSamples(), from + direction * step);
    cursor = to;

    if (extend)
        setSelection (juce::Range<juce::int64>::between (dragAnchor, to), selectedChannel);
    else
    {
        dragAnchor = to;
        setSelection ({});
    }

    // the view follows the cursor smoothly, keeping a little margin
    const auto wave = waveArea();
    const auto margin = std::min (40, wave.getWidth() / 8);
    const auto x = sampleToX ((double) to);

    if (x > (float) (wave.getRight() - margin))
        viewStart = (double) to - (wave.getWidth() - margin) * samplesPerPixel;
    else if (x < (float) (wave.getX() + margin))
        viewStart = (double) to - margin * samplesPerPixel;

    clampView();
    updateScrollBar();
    repaint();
    notifyChanged();
}

void WaveformView::moveSelection (int direction)
{
    if (! hasAudio() || selection.isEmpty())
        return;

    const auto length = selection.getLength();
    const auto start = juce::jlimit ((juce::int64) 0, totalSamples() - length, selection.getStart() + direction * arrowStep());
    const auto shift = start - selection.getStart();
    if (shift == 0)
        return;

    cursor += shift;
    dragAnchor += shift;
    setSelection ({ start, start + length }, selectedChannel);
    ensureVisible (direction < 0 ? start : start + length);
}

void WaveformView::moveEdge (int direction)
{
    const auto side = getPickedEdge();
    if (! hasAudio() || side == 0)
        return;

    // The end moves; it never crosses the other one.
    auto range = selection;
    const auto step = direction * arrowStep();
    if (side < 0)
        range.setStart (juce::jlimit ((juce::int64) 0, range.getEnd() - 1, range.getStart() + step));
    else
        range.setEnd (juce::jlimit (range.getStart() + 1, totalSamples(), range.getEnd() + step));

    const auto edge = side < 0 ? range.getStart() : range.getEnd();
    cursor = edge;
    dragAnchor = side < 0 ? range.getEnd() : range.getStart();
    setSelection (range, selectedChannel);
    pickedFor = selection;
    ensureVisible (edge);
}

int WaveformView::channelZoneAt (juce::Point<int> position) const
{
    if (! hasAudio() || buffer->getNumChannels() < 2)
        return -1;

    const auto first = laneBounds (0);
    const auto lastChannel = buffer->getNumChannels() - 1;
    const auto last = laneBounds (lastChannel);

    // Above the middle of the top lane = left only, below the middle of the
    // bottom lane = right only; between the two middles = both channels.
    if (position.y < first.getCentreY())
        return 0;

    if (position.y >= last.getCentreY())
        return lastChannel;

    return -1;
}

juce::Range<juce::int64> WaveformView::markerSpanAt (juce::int64 sample) const
{
    juce::int64 start = 0, end = totalSamples();

    if (markers != nullptr)
        for (auto m : *markers)
        {
            if (m <= sample) start = juce::jmax (start, m);
            else             end = juce::jmin (end, m);
        }

    return { start, end };
}

int WaveformView::laneAt (juce::Point<int> position) const
{
    if (! hasAudio() || buffer->getNumChannels() < 2)
        return -1;

    for (int ch = 0; ch < buffer->getNumChannels(); ++ch)
    {
        const auto lane = laneBounds (ch);

        if (position.y >= lane.getY() && position.y < lane.getBottom())
            return ch;
    }

    return -1;
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    // An "L" / "R" pointer shows when a drag would pick a single channel.
    static const auto makeCursor = [] (const juce::String& letter)
    {
        juce::Image image (juce::Image::ARGB, 40, 48, true);
        juce::Graphics g (image);
        g.setColour (juce::Colours::white);
        g.fillRect (19, 2, 2, 44);
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.setColour (juce::Colours::black);
        g.drawText (letter, 22, 1, 18, 26, juce::Justification::centred);
        g.setColour (juce::Colours::white);
        g.drawText (letter, 21, 0, 18, 26, juce::Justification::centred);
        return juce::MouseCursor (juce::ScaledImage (image, 2.0), { 10, 12 });
    };
    static const auto leftCursor = makeCursor ("L");
    static const auto rightCursor = makeCursor ("R");

    if (hasAudio() && markerAt (e.getPosition()) >= 0)
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);   // markers can be dragged
        return;
    }

    if (markerAt (e.getPosition()) < 0 && selectionEdgeAt (e.getPosition()) >= 0)
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    const auto zone = waveArea().contains (e.getPosition()) && markerAt (e.getPosition()) < 0
                          ? channelZoneAt (e.getPosition()) : -1;
    setMouseCursor (zone == 0 ? leftCursor : zone > 0 ? rightCursor : juce::MouseCursor::NormalCursor);
}

int WaveformView::selectionEdgeAt (juce::Point<int> position) const
{
    if (selection.isEmpty() || ! waveArea().contains (position))
        return -1;

    // A one-channel selection is grabbed in its own lane.
    if (selectedChannel >= 0 && ! laneBounds (selectedChannel).contains (position))
        return -1;

    constexpr float grab = 5.0f;   // pixels either side of the edge
    const auto dStart = std::abs ((float) position.x - sampleToX ((double) selection.getStart()));
    const auto dEnd = std::abs ((float) position.x - sampleToX ((double) selection.getEnd()));

    if (std::min (dStart, dEnd) > grab)
        return -1;

    return dStart <= dEnd ? 0 : 1;
}

void WaveformView::setSelection (juce::Range<juce::int64> range, int channel)
{
    selection = range.getIntersectionWith ({ 0, totalSamples() });
    selectedChannel = selection.isEmpty() ? -1 : channel;
    repaint();
    notifyChanged();
}

void WaveformView::selectAll()
{
    dragAnchor = 0;
    setSelection ({ 0, totalSamples() });
}

void WaveformView::setPlayhead (juce::int64 sample)
{
    if (sample == playhead)
        return;

    const auto area = waveArea();
    auto repaintAt = [this, area] (juce::int64 s)
    {
        if (s >= 0)
            repaint (juce::roundToInt (sampleToX ((double) s)) - 2, area.getY(), 5, area.getHeight());
    };

    repaintAt (playhead);
    playhead = sample;
    repaintAt (playhead);

    if (onPlayheadMoved)
        onPlayheadMoved();
}

void WaveformView::notifyChanged()
{
    if (onCursorOrSelectionChanged != nullptr)
        onCursorOrSelectionChanged();
}

//==============================================================================
juce::Rectangle<float> WaveformView::markerFlag (int index, float x) const
{
    // The number, then the name if it has one (long names are cut short).
    auto width = 16.0f;

    if (markerNames != nullptr)
        if (const auto it = markerNames->find ((*markers)[(size_t) index]); it != markerNames->end() && it->second.isNotEmpty())
        {
            const juce::Font font (juce::FontOptions (10.5f, juce::Font::bold));
            const auto text = juce::String (index + 1) + " " + it->second;
            width = juce::jlimit (16.0f, 160.0f, (float) juce::GlyphArrangement::getStringWidthInt (font, text) + 8.0f);
        }

    // Flags hang to the right of the line, or to the left at the far edge.
    const auto flipped = x + width > (float) waveArea().getRight();
    return { flipped ? x - width : x, 2.0f, width, 13.0f };
}

int WaveformView::markerAt (juce::Point<int> position) const
{
    // Only the numbered flag in the ruler picks a marker; its line through
    // the waveform is clicked like any other spot.
    if (markers == nullptr || position.y >= rulerHeight)
        return -1;

    // The last one drawn is on top.
    for (int i = (int) markers->size() - 1; i >= 0; --i)
        if (const auto flag = markerFlag (i, sampleToX ((double) (*markers)[(size_t) i]));
            (float) position.x >= flag.getX() - 2.0f && (float) position.x <= flag.getRight() + 2.0f)
            return i;

    return -1;
}

bool WaveformView::isMarkerSelected (juce::int64 position) const
{
    return std::find (selectedMarkers.begin(), selectedMarkers.end(), position) != selectedMarkers.end();
}

void WaveformView::clearMarkerSelection()
{
    if (selectedMarkers.empty())
        return;

    selectedMarkers.clear();
    repaint();
    notifyChanged();
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    pressedOnMarker = false;


    if (! hasAudio())
        return;

    // Right-click on a marker's flag offers rename / delete; anywhere else it
    // opens the window's menu.
    if (e.mods.isPopupMenu() && ! regionLane().contains (e.getPosition()))
    {
        if (const auto index = markerAt (e.getPosition()); index >= 0 && onMarkerMenu != nullptr)
            onMarkerMenu (index);
        else if (onContextMenu != nullptr)
            onContextMenu();

        return;
    }

    // A click on a marker picks it (and puts the cursor there); Delete then
    // removes the marker, not audio.
    if (const auto index = markerAt (e.getPosition()); index >= 0 && ! e.mods.isShiftDown())
    {
        const auto position = (*markers)[(size_t) index];
        selectedMarkers = { position };
        pressedOnMarker = true;
        markerDragIndex = index;
        markerDragPos = position;
        markerDragged = false;
        pressedInSelection = false;
        dragAnchor = position;
        cursor = position;
        setSelection ({});
        return;
    }

    if (! selectedMarkers.empty())
    {
        selectedMarkers.clear();
        repaint();
    }

    // The region strip: a click selects the region, a right-click offers a menu.
    if (regionLane().contains (e.getPosition()))
    {
        if (const auto index = regionAt (e.getPosition()); index >= 0)
        {
            const auto& r = (*regions)[(size_t) index];
            dragAnchor = r.start;
            cursor = r.start;
            setSelection ({ r.start, r.end });

            if (e.mods.isPopupMenu() && onRegionMenu != nullptr)
                onRegionMenu (index);
        }

        pressedOnMarker = true;   // no drag-select from the strip
        return;
    }

    if (! waveArea().contains (e.getPosition()))
    {
        notifyChanged();
        return;
    }

    const auto sample = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));

    // The second press of a double-click between markers selects that span on
    // the lane clicked at once, so dragging before letting go adds the spans
    // to the left or right.
    if (e.getNumberOfClicks() == 2 && e.mods.isLeftButtonDown() && ! e.mods.isShiftDown() && markers != nullptr && ! markers->empty())
    {
        spanDrag = spanDragDone = true;
        pressedInSelection = false;
        spanAnchor = markerSpanAt (sample);
        spanLane = channelZoneAt (e.getPosition());
        dragAnchor = spanAnchor.getStart();
        cursor = spanAnchor.getStart();
        setSelection (spanAnchor, spanLane);
        return;
    }

    // Pressing on an edge of the selection picks it for Shift + arrows and
    // drags it (the other one stays), on the same channel(s); with Shift it
    // is only picked.
    if (const auto edge = selectionEdgeAt (e.getPosition()); edge >= 0)
    {
        pickEdge (edge == 0 ? -1 : 1);
        pressedInSelection = false;
        draggingOut = false;

        if (e.mods.isShiftDown())
        {
            pressedOnMarker = true;   // no drag-select
            return;
        }

        pressedOnEdge = true;
        dragAnchor = edge == 0 ? selection.getEnd() : selection.getStart();
        dragChannel = selectedChannel;
        return;
    }

    // Pressing inside the selection may start dragging it out (decided on drag).
    pressedInSelection = ! e.mods.isShiftDown() && selection.contains (sample)
                         && (selectedChannel < 0 || laneBounds (selectedChannel).contains (e.getPosition()));
    draggingOut = false;

    if (pressedInSelection)
        return;

    if (e.mods.isShiftDown())
    {
        // Shift+click extends the existing selection or cursor to the click.
        const auto to = snapToZeroCrossings ? nearestZeroCrossing (sample, selectedChannel) : sample;
        setSelection (juce::Range<juce::int64>::between (dragAnchor, to), selectedChannel);
        return;
    }

    dragChannel = channelZoneAt (e.getPosition());
    dragAnchor = snapToZeroCrossings ? nearestZeroCrossing (sample, dragChannel) : sample;
    cursor = sample;
    setSelection ({});
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    // Dragging a marker (its flag or its line) moves it left / right.
    if (hasAudio() && pressedOnMarker && markerDragIndex >= 0)
    {
        if (! markerDragged && e.getDistanceFromDragStartX() == 0)
            return;

        markerDragged = true;
        markerDragPos = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));
        cursor = markerDragPos;
        selectedMarkers = { markerDragPos };
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        repaint();
        return;
    }

    if (! hasAudio() || pressedOnMarker)
        return;

    if (spanDrag)
    {
        // Whole spans from the double-clicked one to the one under the mouse;
        // moving onto the other channel takes both.
        const auto sample = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));
        const auto range = spanAnchor.getUnionWith (markerSpanAt (sample));
        const auto lane = laneAt (e.getPosition());
        const auto channel = spanLane >= 0 && lane >= 0 && lane != spanLane ? -1 : spanLane;
        dragAnchor = sample < spanAnchor.getStart() ? spanAnchor.getEnd() : spanAnchor.getStart();
        cursor = sample < spanAnchor.getStart() ? range.getStart() : range.getEnd();
        setSelection (range, channel);
        return;
    }

    if (pressedInSelection)
    {
        if (! draggingOut && e.getDistanceFromDragStart() > 6)
        {
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            {
                draggingOut = true;
                container->startDragging (selectionDragId, this, juce::ScaledImage (makeDragImage()), false);
            }
        }

        return;
    }

    // A press that moved only a few pixels (a trackpad tap, a hand resting
    // on the mouse) is a click, not a tiny selection.
    if (selection.isEmpty() && std::abs (e.getDistanceFromDragStartX()) < minSelectPixels)
        return;

    auto sample = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));

    if (snapToZeroCrossings)
        sample = nearestZeroCrossing (sample, dragChannel);

    cursor = sample;
    setSelection (juce::Range<juce::int64>::between (dragAnchor, sample), dragChannel);
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    spanDrag = false;

    // The dragged end stays picked (the side it ended on, if it crossed over).
    if (pressedOnEdge)
    {
        pressedOnEdge = false;

        if (! selection.isEmpty())
            pickEdge (cursor <= selection.getStart() ? -1 : 1);
    }

    // A plain click inside the selection (no drag) just places the cursor.
    if (pressedInSelection && ! draggingOut && hasAudio())
    {
        const auto sample = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));
        dragAnchor = sample;
        cursor = sample;
        setSelection ({});
    }

    if (markerDragged && markerDragIndex >= 0)
    {
        const auto index = markerDragIndex;
        const auto to = markerDragPos;
        markerDragIndex = -1;
        markerDragged = false;

        if (onMoveMarker != nullptr)
            onMoveMarker (index, to);

        selectedMarkers = { to };
        notifyChanged();
    }

    const auto plainClick = e.mouseWasClicked() && ! pressedOnMarker && ! draggingOut && hasAudio()
                            && waveArea().contains (e.getMouseDownPosition()) && selection.isEmpty();

    markerDragIndex = -1;
    markerDragged = false;
    pressedInSelection = false;
    pressedOnMarker = false;
    draggingOut = false;

    if (plainClick && onCursorClicked != nullptr)
        onCursorClicked (cursor);
}

juce::Image WaveformView::makeDragImage() const
{
    // A small card with a mini waveform and the selection length.
    const auto scale = 2.0f;
    juce::Image image (juce::Image::ARGB, (int) (150 * scale), (int) (44 * scale), true);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));

    const juce::Rectangle<float> card (1.0f, 1.0f, 148.0f, 42.0f);
    g.setColour (juce::Colour (0xee2d3340));
    g.fillRoundedRectangle (card, 6.0f);
    g.setColour (juce::Colour (0xffff8a3d));
    g.drawRoundedRectangle (card, 6.0f, 1.5f);

    if (hasAudio() && ! selection.isEmpty())
    {
        const auto columns = 40;
        const auto step = juce::jmax ((juce::int64) 1, selection.getLength() / columns);
        g.setColour (wf::appearance::waveColour());

        for (int i = 0; i < columns; ++i)
        {
            const auto start = selection.getStart() + i * step;
            const auto count = (int) juce::jmin (step, selection.getEnd() - start);

            if (count <= 0)
                break;

            const auto level = juce::jmin (1.0f, buffer->getMagnitude (0, (int) start, count));
            const auto h = juce::jmax (1.0f, level * 30.0f);
            g.fillRect (8.0f + (float) i * 1.6f, 22.0f - h / 2.0f, 1.0f, h);
        }
    }

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (12.0f));
    const auto label = selectedChannel < 0 ? juce::String ("Selection")
                     : selectedChannel == 0 ? juce::String ("Left (mono)") : juce::String ("Right (mono)");
    g.drawText (label, 76, 6, 70, 16, juce::Justification::centredLeft);
    g.setColour (juce::Colours::white.withAlpha (0.75f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (formatTime ((double) selection.getLength() / sampleRate).substring (3), 76, 22, 70, 14,
                juce::Justification::centredLeft);
    return image;
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! hasAudio())
        return;

    if (const auto index = regionAt (e.getPosition()); index >= 0)
    {
        if (onRenameRegion != nullptr)
            onRenameRegion (index);

        return;
    }

    // Double-click on a marker picks every marker.
    if (markers != nullptr && markerAt (e.getPosition()) >= 0)
    {
        selectedMarkers = *markers;
        repaint();
        notifyChanged();
        return;
    }

    // Already done on the press (see mouseDown).
    if (spanDragDone && e.getNumberOfClicks() == 2)
    {
        spanDragDone = false;
        return;
    }

    spanDragDone = false;

    // In the "L" / "R" zone the double-click selects only that channel.
    const auto channel = waveArea().contains (e.getPosition()) ? channelZoneAt (e.getPosition()) : -1;

    // Triple-click selects the whole file (JUCE calls this again with 3 clicks);
    // without markers a double-click already does.
    if (e.getNumberOfClicks() >= 3 || markers == nullptr || markers->empty())
    {
        dragAnchor = 0;
        setSelection ({ 0, totalSamples() }, channel);
        return;
    }

    // Sound Forge: double-click selects the span between the surrounding
    // markers (or the file start/end).
    const auto span = markerSpanAt (juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x)));
    dragAnchor = span.getStart();
    cursor = span.getStart();
    setSelection (span, channel);
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! hasAudio())
        return;

    // Horizontal wheel/trackpad scrolls; vertical wheel zooms around the mouse
    // (or around the cursor, see zoomOnCursor).
    if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
    {
        viewStart -= (double) wheel.deltaX * (double) waveArea().getWidth() * samplesPerPixel * 0.5;
        clampView();
        updateScrollBar();
        repaint();
        return;
    }

    if (juce::exactlyEqual (wheel.deltaY, 0.0f))
        return;

    const auto factor = wheel.deltaY > 0.0f ? 0.8 : 1.25;

    if ((bool) zoomOnCursor().getValue())
        setZoom (samplesPerPixel * factor, (double) cursor,
                 juce::jlimit ((float) waveArea().getX(), (float) waveArea().getRight(), sampleToX ((double) cursor)));
    else
        setZoom (samplesPerPixel * factor, xToSample ((float) e.x), (float) e.x);
}

//==============================================================================
void WaveformView::resized()
{
    scrollBar.setBounds (getLocalBounds().removeFromBottom (scrollBarHeight));

    if (hasAudio())
        setZoom (samplesPerPixel, viewStart, (float) waveArea().getX());
}

void WaveformView::paint (juce::Graphics& g)
{
    wf::appearance::paintBackground (g, *this, getLocalBounds());

    const auto area = waveArea();
    paintRuler (g, getLocalBounds().removeFromTop (rulerHeight));

    if (const auto lane = regionLane(); ! lane.isEmpty() && hasAudio())
        paintRegions (g, lane);

    if (! hasAudio())
    {
        g.setFont (juce::FontOptions (16.0f));
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawText (emptyText, area.translated (1, 1), juce::Justification::centred);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawText (emptyText, area, juce::Justification::centred);
        return;
    }

    const auto clip = g.getClipBounds().getIntersection (area);

    // Selection shading behind the waveform.
    if (! selection.isEmpty())
    {
        const auto x1 = juce::jmax ((float) area.getX(), sampleToX ((double) selection.getStart()));
        const auto x2 = juce::jmin ((float) area.getRight(), sampleToX ((double) selection.getEnd()));

        if (x2 > x1)
        {
            // Only the selected channel's lane when one channel is picked.
            const auto band = selectedChannel >= 0 ? laneBounds (selectedChannel) : area;
            g.setColour (selectionShade);
            g.fillRect (juce::Rectangle<float> (x1, (float) band.getY(), x2 - x1, (float) band.getHeight()));
        }
    }

    const auto channels = buffer->getNumChannels();

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto lane = laneBounds (ch);
        paintChannel (g, ch, lane, clip.getIntersection (lane));

        // A thin line between the channels (e.g. left / right), in the gap.
        if (ch < channels - 1)
        {
            g.setColour (juce::Colours::white.withAlpha (0.22f));
            g.fillRect (area.getX(), lane.getBottom() + laneGap / 2, area.getWidth(), 1);
        }
    }

    // The selection split into its beats (see selectionBeats).
    if (showBeatLines && ! selection.isEmpty() && selectionBeats > 0)
    {
        const auto band = selectedChannel >= 0 ? laneBounds (selectedChannel) : area;
        const auto step = (double) selection.getLength() / selectionBeats;

        for (int i = 0; i <= selectionBeats; ++i)
        {
            const auto x = sampleToX ((double) selection.getStart() + step * i);

            if (x < (float) area.getX() - 2.0f || x > (float) area.getRight() + 2.0f)
                continue;

            // A dark edge so the line shows on any waveform colour.
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (x - 1.5f, (float) band.getY(), 3.0f, (float) band.getHeight()));
            g.setColour (beatLineColour);
            g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) band.getY(), 1.5f, (float) band.getHeight()));
        }
    }

    // The end picked for Shift + arrows: a brighter, thicker line.
    if (const auto side = getPickedEdge(); side != 0)
    {
        const auto x = sampleToX ((double) (side < 0 ? selection.getStart() : selection.getEnd()));
        const auto band = selectedChannel >= 0 ? laneBounds (selectedChannel) : area;
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRect (juce::Rectangle<float> (x - 2.0f, (float) band.getY(), 4.0f, (float) band.getHeight()));
        g.setColour (juce::Colour (0xffffd54a));
        g.fillRect (juce::Rectangle<float> (x - 1.0f, (float) band.getY(), 2.0f, (float) band.getHeight()));
    }

    // Cursor (only when there is no selection, like Sound Forge).
    if (selection.isEmpty())
    {
        const auto x = sampleToX ((double) cursor);
        g.setColour (cursorColour);
        g.drawVerticalLine (juce::roundToInt (x), (float) area.getY(), (float) area.getBottom());
    }

    paintMarkers (g, getLocalBounds().removeFromTop (rulerHeight), area);
    paintDropPreview (g, area);

    if (playhead >= 0)
    {
        const auto x = sampleToX ((double) playhead);
        g.setColour (playheadColour);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight()));
    }
}

void WaveformView::paintChannel (juce::Graphics& g, int channel, juce::Rectangle<int> lane, juce::Rectangle<int> clip)
{
    if (clip.isEmpty())
        return;

    const auto* data = buffer->getReadPointer (channel);
    const auto total = totalSamples();
    const auto mid = (float) lane.getCentreY();
    const auto lanes = (float) lane.getHeight() * 0.5f - 1.0f;
    const auto half = lanes * levelZoom;
    const auto waveColour = wf::appearance::waveColour();

    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawHorizontalLine (lane.getCentreY(), (float) clip.getX(), (float) clip.getRight());

    if (levelZoom > 1.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("Level x" + juce::String ((int) levelZoom), lane.getX() + 4, lane.getY() + 2, 70, 14,
                    juce::Justification::centredLeft);
    }

    if (samplesPerPixel <= 1.0)
    {
        // Zoomed in past one sample per pixel: join individual samples.
        const auto first = juce::jmax ((juce::int64) 0, (juce::int64) xToSample ((float) clip.getX()) - 1);
        const auto last  = juce::jmin (total - 1, (juce::int64) xToSample ((float) clip.getRight()) + 1);

        juce::Path path;

        for (auto s = first; s <= last; ++s)
        {
            const juce::Point<float> p (sampleToX ((double) s), mid - juce::jlimit (-lanes, lanes, data[s] * half));
            if (s == first) path.startNewSubPath (p); else path.lineTo (p);
        }

        g.setColour (waveColour);
        g.strokePath (path, juce::PathStrokeType (1.2f));

        if (samplesPerPixel <= 0.25)
            for (auto s = first; s <= last; ++s)
                g.fillEllipse (sampleToX ((double) s) - 2.0f, mid - juce::jlimit (-lanes, lanes, data[s] * half) - 2.0f, 4.0f, 4.0f);

        return;
    }

    for (int x = clip.getX(); x < clip.getRight(); ++x)
    {
        const auto s0 = (juce::int64) xToSample ((float) x);
        const auto s1 = juce::jmin (total, (juce::int64) xToSample ((float) (x + 1)) + 1);

        if (s0 >= total || s1 <= 0)
            continue;

        const auto range = juce::FloatVectorOperations::findMinAndMax (data + juce::jmax ((juce::int64) 0, s0),
                                                                         (int) (s1 - juce::jmax ((juce::int64) 0, s0)));
        const auto selected = selection.contains (s0) && (selectedChannel < 0 || selectedChannel == channel);
        g.setColour (selected ? waveSelected : waveColour);

        const auto top = mid - juce::jlimit (-lanes, lanes, range.getEnd() * half);
        const auto bottom = mid - juce::jlimit (-lanes, lanes, range.getStart() * half);
        g.fillRect ((float) x, top, 1.0f, juce::jmax (1.0f, bottom - top));
    }
}

void WaveformView::paintMarkers (juce::Graphics& g, juce::Rectangle<int> rulerArea, juce::Rectangle<int> wave)
{
    if (markers == nullptr)
        return;

    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));

    for (int i = 0; i < (int) markers->size(); ++i)
    {
        const auto dragged = markerDragged && i == markerDragIndex;
        const auto position = dragged ? markerDragPos : (*markers)[(size_t) i];
        const auto x = sampleToX ((double) position);

        if (x < (float) wave.getX() - 20.0f || x > (float) wave.getRight())
            continue;

        const auto picked = dragged || isMarkerSelected (position);
        const auto barStart = barStarts != nullptr
                           && std::binary_search (barStarts->begin(), barStarts->end(), (*markers)[(size_t) i]);
        const auto colour = barStart ? barMarkerColour : markerColour;

        // Dashed line through the waveform; solid and thicker when picked.
        const float dashes[] = { 5.0f, 3.0f };
        g.setColour (colour);

        // A bar start is a solid line, so the bars stand out from the beats.
        if (picked || barStart)
            g.drawLine (x, (float) wave.getY(), x, (float) wave.getBottom(), picked ? 2.0f : 1.6f);
        else
            g.drawDashedLine ({ x, (float) wave.getY(), x, (float) wave.getBottom() }, dashes, 2, 1.2f);

        // Numbered tag hanging from the ruler, like a little flag.
        const auto tag = markerFlag (i, x).translated (0.0f, (float) rulerArea.getY());
        const auto flipped = tag.getX() < x;
        juce::Path flag;
        flag.addRoundedRectangle (tag.getX(), tag.getY(), tag.getWidth(), tag.getHeight(), 3.0f, 3.0f,
                                  flipped, ! flipped, flipped, ! flipped);
        // Picked: white flag with an orange outline and number.
        g.setColour (picked ? juce::Colours::white : colour);
        g.fillPath (flag);

        if (picked)
        {
            g.setColour (colour);
            g.strokePath (flag, juce::PathStrokeType (1.5f));
        }

        g.setColour (picked ? colour : juce::Colours::white);
        auto label = juce::String (i + 1);

        if (markerNames != nullptr)
            if (const auto it = markerNames->find ((*markers)[(size_t) i]); it != markerNames->end() && it->second.isNotEmpty())
                label << " " << it->second;

        g.drawFittedText (label, tag.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 1.0f);

        // While dragging: where it is now.
        if (dragged)
        {
            const auto text = formatTime ((double) position / sampleRate);
            const juce::Rectangle<float> box (flipped ? x - 92.0f : x + 4.0f, (float) wave.getY() + 4.0f, 88.0f, 16.0f);
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (juce::Colours::white);
            g.drawText (text, box, juce::Justification::centred);
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        }
    }
}

void WaveformView::paintDropPreview (juce::Graphics& g, juce::Rectangle<int> wave)
{
    if (dropPreview.isEmpty())
        return;

    const auto x1 = sampleToX ((double) dropPreview.getStart());
    const auto x2 = sampleToX ((double) dropPreview.getEnd());
    const auto top = (float) wave.getY();
    const auto height = (float) wave.getHeight();
    const auto snapped = dropSnappedTo != -1;

    // Where the audio will go.
    g.setColour (markerColour.withAlpha (0.22f));
    g.fillRect (juce::Rectangle<float> (x1, top, juce::jmax (1.0f, x2 - x1), height));

    const float dashes[] = { 4.0f, 3.0f };
    g.setColour (markerColour);
    g.drawDashedLine ({ x2, top, x2, top + height }, dashes, 2, 1.2f);
    g.fillRect (juce::Rectangle<float> (x1 - (snapped ? 1.5f : 1.0f), top, snapped ? 3.0f : 2.0f, height));

    // Label: start time, and what it stuck to.
    auto text = formatTime ((double) dropPreview.getStart() / sampleRate);

    if (dropSnappedTo >= 0)
        text << "  -> Marker " << (dropSnappedTo + 1);
    else if (dropSnappedTo == -2)
        text << (dropPreview.getStart() == 0 ? "  -> Start" : "  -> End");

    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    const auto width = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 12.0f;
    auto labelX = x1 + 4.0f;

    if (labelX + width > (float) wave.getRight())
        labelX = x1 - width - 4.0f;

    const juce::Rectangle<float> label (labelX, top + 6.0f, width, 18.0f);
    g.setColour (snapped ? markerColour : juce::Colour (0xee2d3340));
    g.fillRoundedRectangle (label, 4.0f);
    g.setColour (juce::Colours::white);
    g.drawText (text, label, juce::Justification::centred);
}

void WaveformView::paintRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xffe4e1da));
    g.fillRect (area);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.drawHorizontalLine (area.getBottom() - 1, (float) area.getX(), (float) area.getRight());

    if (! hasAudio())
        return;

    // Pick a tick spacing of at least ~90 px from a 1-2-5 sequence.
    const auto secondsPerPixel = samplesPerPixel / sampleRate;
    const double steps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
    auto step = steps[std::size (steps) - 1];

    for (auto s : steps)
        if (s / secondsPerPixel >= 90.0) { step = s; break; }

    const auto startSec = viewStart / sampleRate;
    const auto endSec = startSec + (double) area.getWidth() * secondsPerPixel;

    g.setFont (juce::FontOptions (11.0f));

    for (auto t = std::ceil (startSec / step) * step; t <= endSec; t += step)
    {
        const auto x = juce::roundToInt (sampleToX (t * sampleRate));
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawVerticalLine (x, (float) area.getBottom() - 7.0f, (float) area.getBottom());

        auto label = formatTime (t);
        if (step >= 1.0) label = label.dropLastCharacters (4);   // hide milliseconds
        if (label.startsWith ("00:")) label = label.substring (3);

        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawText (label, x + 3, area.getY() + 2, 90, area.getHeight() - 8, juce::Justification::centredLeft);
    }

    for (auto t = std::ceil (startSec / (step / 5.0)) * (step / 5.0); t <= endSec; t += step / 5.0)
    {
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.drawVerticalLine (juce::roundToInt (sampleToX (t * sampleRate)), (float) area.getBottom() - 4.0f, (float) area.getBottom());
    }
}

void WaveformView::levelZoomIn()
{
    levelZoom = juce::jmin (64.0f, levelZoom * 2.0f);
    repaint();
}

void WaveformView::levelZoomOut()
{
    levelZoom = juce::jmax (1.0f, levelZoom * 0.5f);
    repaint();
}
