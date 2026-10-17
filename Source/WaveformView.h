#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

// A named stretch of a file (Insert > Region, R).
struct AudioRegion
{
    juce::int64 start = 0, end = 0;
    juce::String name;
};

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
    void setMarkers (const std::vector<juce::int64>* newMarkers, const std::vector<juce::int64>* bars = nullptr)
    {
        markers = newMarkers;
        barStarts = bars;
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
    std::function<void (int markerIndex)> onRemoveMarker;
    // A marker dragged to `position` (sent once, when the mouse is let go).
    std::function<void (int markerIndex, juce::int64 position)> onMoveMarker;

    // The drag strip (the band between the channels, or under a mono
    // waveform): dragging it slides the audio past the cursor line like tape,
    // with or without sound (right-click the strip for the options). The
    // cursor stays at the line; scrubbing reports where it is.
    enum class Scrub { start, move, end };
    std::function<void (Scrub, juce::int64 sample)> onScrub;

    // A plain click in the waveform (no drag): the cursor is now at `sample`.
    // While the file plays, playback goes on from there.
    std::function<void (juce::int64 sample)> onCursorClicked;

    // Left / Right arrows: the cursor moves by a few pixels of the screen, so
    // the step follows the zoom (fine when zoomed in, coarse when zoomed out);
    // `extend` grows the selection instead (Shift).
    void nudgeCursor (int direction, bool extend);
    juce::int64 arrowStep() const;   // the arrow-key step in samples at this zoom

    struct DragOptions
    {
        bool sound = true;          // hear the audio while dragging the strip
        bool audioFollowsHand = true;   // false: the view moves the way you drag
        int arrowPixels = 10;       // the arrow-key step, in screen pixels
    };
    static DragOptions dragOptions();

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
    int markerNear (float x) const;  // index of a marker within a few pixels, or -1
    int markerAt (juce::Point<int> position) const;  // its flag or line under the mouse, or -1
    int selectionEdgeAt (juce::Point<int> position) const;  // 0 = start, 1 = end of the selection under the mouse, or -1
    bool isMarkerSelected (juce::int64 position) const;
    juce::Image makeDragImage() const;
    void paintChannel (juce::Graphics&, int channel, juce::Rectangle<int> lane, juce::Rectangle<int> clip);

    const juce::AudioBuffer<float>* buffer = nullptr;
    const std::vector<juce::int64>* markers = nullptr;
    const std::vector<AudioRegion>* regions = nullptr;
    const std::vector<juce::int64>* barStarts = nullptr;
    juce::String emptyText { "No file open" };
    double sampleRate = 44100.0;
    double samplesPerPixel = 1.0;
    float levelZoom = 1.0f;
    double viewStart = 0.0;            // first visible sample

    juce::int64 cursor = 0;
    juce::int64 dragAnchor = 0;
    bool pressedInSelection = false;   // a drag from here moves the selection out
    bool pressedOnMarker = false;      // a click picked a marker; no drag-select
    int markerDragIndex = -1;          // the marker under the press (it can be dragged)
    juce::int64 markerDragPos = 0;
    bool markerDragged = false;
    std::vector<juce::int64> selectedMarkers;
    bool draggingOut = false;
    bool scrubbing = false;            // dragging the strip
    int grabX = 0;                     // where on the screen the strip was taken
    double grabViewStart = 0.0;
    juce::Rectangle<int> scrubStripAt (juce::Point<int> position) const;   // empty when not on a strip
    void showStripMenu();
    juce::Range<juce::int64> selection;
    int selectedChannel = -1;   // -1: all channels
    int dragChannel = -1;       // channel the current drag-select started in

    // Sound Forge rule for stereo: the top quarter of the first channel picks
    // only it, the bottom quarter of the last channel picks only it, anywhere
    // else picks all channels.
    int channelZoneAt (juce::Point<int> position) const;
    juce::Rectangle<int> laneBounds (int channel) const;
    juce::int64 playhead = -1;
    juce::Range<juce::int64> dropPreview;
    int dropSnappedTo = -1;

    juce::ScrollBar scrollBar { false };
    juce::Image backgroundTile;
    float backgroundScale = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
