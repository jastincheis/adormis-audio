#pragma once

#include <map>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

// A named stretch of a file (Insert > Region, R).
struct AudioRegion
{
    juce::int64 start = 0, end = 0;
    juce::String name;
};

// Marker names by marker position (a marker without one shows only its number).
using MarkerNames = std::map<juce::int64, juce::String>;

// The data window: time ruler, one lane per channel, cursor, selection and
// playhead, drawn over the textured background.
class WaveformView final : public juce::Component,
                           private juce::ScrollBar::Listener
{
public:
    WaveformView();

    // The buffer must outlive this view or be replaced with nullptr.
    void setAudio (const juce::AudioBuffer<float>* newBuffer, double newSampleRate);
    bool hasAudio() const  { return buffer != nullptr && buffer->getNumSamples() > 0; }

    // What an empty view says ("No file open" by default).
    void setEmptyText (const juce::String& text)   { emptyText = text; repaint(); }

    // Call after the buffer's samples (or length) changed in place.
    void audioChanged();

    juce::int64 getCursor() const                   { return cursor; }
    juce::Range<juce::int64> getSelection() const   { return selection; }
    void setCursor (juce::int64 sample);
    // `channel` limits the selection to one channel (-1 = all channels).
    void setSelection (juce::Range<juce::int64> range, int channel = -1);
    int getSelectedChannel() const                  { return selectedChannel; }
    void selectAll();

    // Sorted marker positions owned by the document (may be nullptr).
    // `bars` (sorted, may be null) are the markers that begin a bar: drawn in another colour.
    // `names` (may be null) are shown on the flags after the numbers.
    void setMarkers (const std::vector<juce::int64>* newMarkers, const std::vector<juce::int64>* bars = nullptr,
                     const MarkerNames* names = nullptr)
    {
        markers = newMarkers;
        barStarts = bars;
        markerNames = names;
        repaint();
    }

    // The document's regions (may be nullptr): a strip under the ruler shows
    // them; a click selects one, a double-click renames it, a right-click
    // offers rename / delete.
    void setRegions (const std::vector<AudioRegion>* newRegions)   { regions = newRegions; resized(); repaint(); }
    std::function<void (int regionIndex)> onRenameRegion;
    std::function<void (int regionIndex)> onRegionMenu;

    // Markers picked with a click on them (one) or a double-click (all),
    // by position; Delete removes them instead of audio.
    const std::vector<juce::int64>& getSelectedMarkers() const  { return selectedMarkers; }
    void clearMarkerSelection();

    // Sample under a point in this view's coordinates (clamped to the file).
    juce::int64 sampleAt (juce::Point<int> position) const;

    // Where audio of `length` samples dropped at `start` should land: its start
    // (or end) sticks to a marker or the file edges when within a few pixels.
    // `snappedTo` gets the marker index, -2 for a file edge, or -1 when free.
    juce::int64 snapDrop (juce::int64 start, juce::int64 length, int& snappedTo) const;

    // Shows where dragged audio will be mixed in (empty range hides it).
    void setDropPreview (juce::Range<juce::int64> range, int snappedTo);

    void setPlayhead (juce::int64 sample);  // -1 hides it
    juce::int64 getPlayhead() const                 { return playhead; }
    void ensureVisible (juce::int64 sample);
    void centreOn (double sample);   // smooth playback scrolling: the view moves so `sample` is in the middle

    void zoomIn();
    // View > Level Zoom: magnifies the waveform vertically (x1 .. x64) to see quiet parts.
    void levelZoomIn();
    void levelZoomOut();
    float getLevelZoom() const   { return levelZoom; }
    void zoomOut();
    void zoomToFit();
    void zoomToSelection();

    std::function<void()> onCursorOrSelectionChanged;
    std::function<void()> onPlayheadMoved;
    // Right-click on a marker's flag (rename / delete).
    std::function<void (int markerIndex)> onMarkerMenu;
    // A marker dragged to `position` (sent once, when the mouse is let go).
    std::function<void (int markerIndex, juce::int64 position)> onMoveMarker;

    // A plain click in the waveform (no drag): the cursor is now at `sample`.
    // While the file plays, playback goes on from there.
    std::function<void (juce::int64 sample)> onCursorClicked;

    // A right-click in the waveform (not on a marker): the window's menu.
    std::function<void()> onContextMenu;

    // Left / Right arrows: the cursor moves by a few pixels of the screen, so
    // the step follows the zoom (fine when zoomed in, coarse when zoomed out);
    // `extend` grows the selection instead (Shift).
    void nudgeCursor (int direction, bool extend);
    void moveSelection (int direction);   // by the arrow step, keeping its length
    // W / E pick the selection's left / right end; Shift + arrows then move
    // only that end. Any other change to the selection forgets the pick.
    void pickEdge (int side)   { pickedEdge = side; pickedFor = selection; repaint(); }
    // The end Shift + arrows move: the one picked last, else the right one
    // (0 with nothing selected).
    int getPickedEdge() const  { return selection.isEmpty() ? 0 : selection == pickedFor ? pickedEdge : 1; }
    void moveEdge (int direction);

    // Edit > Auto Snap to Zero Crossings (for every window): selection edges
    // made with the mouse move to the nearest point where the wave crosses
    // zero, so cuts and pastes don't click.
    static bool snapToZeroCrossings;

    // How many beats a selection is (the tempo box, shared by all windows):
    // that many equal parts, with a line at the start, between the beats
    // and at the end. No selection = no lines.
    static int selectionBeats;
    static bool showBeatLines;   // the tempo box's "Beat lines in the selection", remembered
    juce::int64 nearestZeroCrossing (juce::int64 sample, int channel) const;
    juce::int64 arrowStep() const;   // the arrow-key step in samples at this zoom

    struct DragOptions
    {
        int arrowPixels = 10;       // the arrow-key step, in screen pixels
    };
    static DragOptions dragOptions();

    // "Zoom on: Mouse | Cursor" (bottom left of every window, shared by all):
    // the mouse wheel zooms around the mouse, or around the cursor (put there
    // with a click, W / E...). True = cursor. Remembered between starts.
    static juce::Value& zoomOnCursor();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static juce::String formatTime (double seconds);

    // Drag-and-drop description used when a selection is dragged out.
    static constexpr const char* selectionDragId = "Adormis Audio selection";

private:
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;

    juce::int64 totalSamples() const  { return hasAudio() ? buffer->getNumSamples() : 0; }
    juce::Rectangle<int> waveArea() const;
    double xToSample (float x) const;
    float sampleToX (double sample) const;
    void setZoom (double newSamplesPerPixel, double anchorSample, float anchorX);
    void clampView();
    void updateScrollBar();
    void notifyChanged();

    void paintRuler (juce::Graphics&, juce::Rectangle<int> area);
    void paintRegions (juce::Graphics&, juce::Rectangle<int> area);
    juce::Rectangle<int> regionLane() const;   // empty when there are no regions
    int regionAt (juce::Point<int> position) const;
    void paintMarkers (juce::Graphics&, juce::Rectangle<int> rulerArea, juce::Rectangle<int> waveArea);
    void paintDropPreview (juce::Graphics&, juce::Rectangle<int> waveArea);
    int markerAt (juce::Point<int> position) const;  // the marker whose flag is under the mouse, or -1
    juce::Rectangle<float> markerFlag (int index, float x) const;  // its numbered flag in the ruler
    int selectionEdgeAt (juce::Point<int> position) const;  // 0 = start, 1 = end of the selection under the mouse, or -1
    bool isMarkerSelected (juce::int64 position) const;
    juce::Image makeDragImage() const;
    void paintChannel (juce::Graphics&, int channel, juce::Rectangle<int> lane, juce::Rectangle<int> clip);

    const juce::AudioBuffer<float>* buffer = nullptr;
    const std::vector<juce::int64>* markers = nullptr;
    const std::vector<AudioRegion>* regions = nullptr;
    const std::vector<juce::int64>* barStarts = nullptr;
    const MarkerNames* markerNames = nullptr;
    juce::int64 knownLength = 0;       // the length when the view last saw the audio
    juce::String emptyText { "No file open" };
    double sampleRate = 44100.0;
    double samplesPerPixel = 1.0;
    float levelZoom = 1.0f;
    double viewStart = 0.0;            // first visible sample

    juce::int64 cursor = 0;
    juce::int64 dragAnchor = 0;
    bool pressedInSelection = false;   // a drag from here moves the selection out
    bool pressedOnMarker = false;      // a click picked a marker; no drag-select
    bool pressedOnEdge = false;        // a click on an end of the selection picks that end
    int markerDragIndex = -1;          // the marker under the press (it can be dragged)
    juce::int64 markerDragPos = 0;
    bool markerDragged = false;
    std::vector<juce::int64> selectedMarkers;
    bool draggingOut = false;
    juce::Range<juce::int64> selection;
    int selectedChannel = -1;   // -1: all channels
    int pickedEdge = 0;          // -1 left end, 1 right end (see pickEdge)
    juce::Range<juce::int64> pickedFor;
    int dragChannel = -1;       // channel the current drag-select started in

    // Sound Forge rule for stereo: the top quarter of the first channel picks
    // only it, the bottom quarter of the last channel picks only it, anywhere
    // else picks all channels.
    int channelZoneAt (juce::Point<int> position) const;

    // Double-click between markers: the span between the markers round
    // `sample` (or the file start / end), on the channel lane clicked (-1: all).
    juce::Range<juce::int64> markerSpanAt (juce::int64 sample) const;
    int laneAt (juce::Point<int> position) const;
    bool spanDrag = false;                  // held after a double-click: the drag adds whole spans
    bool spanDragDone = false;              // the double-click itself has been handled on the press
    juce::Range<juce::int64> spanAnchor;    // the span double-clicked
    int spanLane = -1;
    juce::Rectangle<int> laneBounds (int channel) const;
    juce::int64 playhead = -1;
    juce::Range<juce::int64> dropPreview;
    int dropSnappedTo = -1;

    juce::ScrollBar scrollBar { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
