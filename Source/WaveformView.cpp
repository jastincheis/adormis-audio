#include "WaveformView.h"
#include "BackgroundPattern.h"

namespace
{
constexpr int rulerHeight     = 22;
constexpr int scrollBarHeight = 14;
constexpr int laneGap         = 4;

const juce::Colour waveColour      (0xffd8ecff);
const juce::Colour waveSelected    (0xffffffff);
const juce::Colour selectionShade  = juce::Colours::black.withAlpha (0.38f);
const juce::Colour cursorColour    (0xffffffff);
const juce::Colour playheadColour  (0xffffc23a);
const juce::Colour markerColour    (0xffff8a3d);
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
    zoomToFit();
    notifyChanged();
}

void WaveformView::audioChanged()
{
    cursor = juce::jmin (cursor, totalSamples());
    selection = selection.getIntersectionWith ({ 0, totalSamples() });
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
    return getLocalBounds().withTrimmedTop (rulerHeight).withTrimmedBottom (scrollBarHeight);
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

void WaveformView::setSelection (juce::Range<juce::int64> range)
{
    selection = range.getIntersectionWith ({ 0, totalSamples() });
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
}

void WaveformView::notifyChanged()
{
    if (onCursorOrSelectionChanged != nullptr)
        onCursorOrSelectionChanged();
}

//==============================================================================
int WaveformView::markerNear (float x) const
{
    if (markers == nullptr)
        return -1;

    for (int i = 0; i < (int) markers->size(); ++i)
        if (std::abs (sampleToX ((double) (*markers)[(size_t) i]) - x) <= 4.0f)
            return i;

    return -1;
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (! hasAudio())
        return;

    // Right-click on a marker removes it.
    if (e.mods.isPopupMenu())
    {
        if (const auto index = markerNear ((float) e.x); index >= 0 && onRemoveMarker != nullptr)
            onRemoveMarker (index);

        return;
    }

    if (! waveArea().contains (e.getPosition()))
        return;

    const auto sample = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));

    // Pressing inside the selection may start dragging it out (decided on drag).
    pressedInSelection = ! e.mods.isShiftDown() && selection.contains (sample);
    draggingOut = false;

    if (pressedInSelection)
        return;

    if (e.mods.isShiftDown())
    {
        // Shift+click extends the existing selection or cursor to the click.
        setSelection (juce::Range<juce::int64>::between (dragAnchor, sample));
        return;
    }

    dragAnchor = sample;
    cursor = sample;
    setSelection ({});
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (! hasAudio())
        return;

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

    const auto sample = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));
    cursor = sample;
    setSelection (juce::Range<juce::int64>::between (dragAnchor, sample));
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    // A plain click inside the selection (no drag) just places the cursor.
    if (pressedInSelection && ! draggingOut && hasAudio())
    {
        const auto sample = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));
        dragAnchor = sample;
        cursor = sample;
        setSelection ({});
    }

    pressedInSelection = false;
    draggingOut = false;
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
        g.setColour (waveColour);

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
    g.drawText ("Selection", 76, 6, 70, 16, juce::Justification::centredLeft);
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

    if (markers == nullptr || markers->empty())
    {
        selectAll();
        return;
    }

    // Sound Forge: double-click selects the span between the surrounding
    // markers (or the file start/end).
    const auto sample = juce::jlimit ((juce::int64) 0, totalSamples(), (juce::int64) xToSample ((float) e.x));
    juce::int64 start = 0, end = totalSamples();

    for (auto m : *markers)
    {
        if (m <= sample) start = juce::jmax (start, m);
        else             end = juce::jmin (end, m);
    }

    dragAnchor = start;
    cursor = start;
    setSelection ({ start, end });
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! hasAudio())
        return;

    // Horizontal wheel/trackpad scrolls; vertical wheel zooms around the mouse.
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
    const auto scale = (float) g.getInternalContext().getPhysicalPixelScaleFactor();

    if (backgroundTile.isNull() || ! juce::approximatelyEqual (backgroundScale, scale))
    {
        backgroundTile = wf::makeBackgroundTile (scale);
        backgroundScale = scale;
    }

    g.setFillType (juce::FillType (backgroundTile, juce::AffineTransform::scale (1.0f / scale)));
    g.fillRect (getLocalBounds());

    const auto area = waveArea();
    paintRuler (g, getLocalBounds().removeFromTop (rulerHeight));

    if (! hasAudio())
    {
        g.setFont (juce::FontOptions (16.0f));
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawText ("No file open", area.translated (1, 1), juce::Justification::centred);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawText ("No file open", area, juce::Justification::centred);
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
            g.setColour (selectionShade);
            g.fillRect (juce::Rectangle<float> (x1, (float) area.getY(), x2 - x1, (float) area.getHeight()));
        }
    }

    const auto channels = buffer->getNumChannels();
    const auto laneHeight = (area.getHeight() - laneGap * (channels - 1)) / juce::jmax (1, channels);

    for (int ch = 0; ch < channels; ++ch)
    {
        const juce::Rectangle<int> lane (area.getX(), area.getY() + ch * (laneHeight + laneGap), area.getWidth(), laneHeight);

        if (ch > 0)
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (area.getX(), lane.getY() - laneGap, area.getWidth(), laneGap);
        }

        paintChannel (g, ch, lane, clip.getIntersection (lane));
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
    const auto half = (float) lane.getHeight() * 0.5f - 1.0f;

    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawHorizontalLine (lane.getCentreY(), (float) clip.getX(), (float) clip.getRight());

    if (samplesPerPixel <= 1.0)
    {
        // Zoomed in past one sample per pixel: join individual samples.
        const auto first = juce::jmax ((juce::int64) 0, (juce::int64) xToSample ((float) clip.getX()) - 1);
        const auto last  = juce::jmin (total - 1, (juce::int64) xToSample ((float) clip.getRight()) + 1);

        juce::Path path;

        for (auto s = first; s <= last; ++s)
        {
            const juce::Point<float> p (sampleToX ((double) s), mid - data[s] * half);
            if (s == first) path.startNewSubPath (p); else path.lineTo (p);
        }

        g.setColour (waveColour);
        g.strokePath (path, juce::PathStrokeType (1.2f));

        if (samplesPerPixel <= 0.25)
            for (auto s = first; s <= last; ++s)
                g.fillEllipse (sampleToX ((double) s) - 2.0f, mid - data[s] * half - 2.0f, 4.0f, 4.0f);

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
        const auto selected = selection.contains (s0);
        g.setColour (selected ? waveSelected : waveColour);

        const auto top = mid - juce::jlimit (-1.0f, 1.0f, range.getEnd()) * half;
        const auto bottom = mid - juce::jlimit (-1.0f, 1.0f, range.getStart()) * half;
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
        const auto x = sampleToX ((double) (*markers)[(size_t) i]);

        if (x < (float) wave.getX() - 20.0f || x > (float) wave.getRight())
            continue;

        // Dashed line through the waveform.
        const float dashes[] = { 5.0f, 3.0f };
        g.setColour (markerColour);
        g.drawDashedLine ({ x, (float) wave.getY(), x, (float) wave.getBottom() }, dashes, 2, 1.2f);

        // Numbered tag hanging from the ruler, like a little flag.
        // Flags hang to the right of the line, or to the left at the far edge.
        const auto flipped = x + 16.0f > (float) wave.getRight();
        const juce::Rectangle<float> tag (flipped ? x - 16.0f : x, (float) rulerArea.getY() + 2.0f, 16.0f, 13.0f);
        juce::Path flag;
        flag.addRoundedRectangle (tag.getX(), tag.getY(), tag.getWidth(), tag.getHeight(), 3.0f, 3.0f,
                                  flipped, ! flipped, flipped, ! flipped);
        g.fillPath (flag);
        g.setColour (juce::Colours::white);
        g.drawText (juce::String (i + 1), tag, juce::Justification::centred);
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
