#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "SpaceKey.h"

namespace wf
{
// The listening panel of the Open window: what the picked file is (format,
// length, rate, channels), its waveform, and Play / Stop. With Auto play on,
// a click on a file plays it right away; a click on the waveform plays from
// there.
struct FilePreviewHost
{
    std::function<void (const juce::File& file, double fromSeconds)> play;
    std::function<void()> stop;
    std::function<double()> position;   // seconds into the file being played, or < 0 when nothing plays
};

class AudioFilePreview final : public juce::FilePreviewComponent,
                               public SpaceTarget,
                               private juce::ChangeListener,
                               private juce::Timer
{
public:
    AudioFilePreview (juce::AudioFormatManager& formats, FilePreviewHost host);
    ~AudioFilePreview() override;

    void selectedFileChanged (const juce::File& newSelectedFile) override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool spacePressed() override;   // play / stop the file

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override   { repaint(); }
    void timerCallback() override;
    void playFrom (double seconds);
    void stop();
    juce::Rectangle<int> waveArea() const;

    juce::AudioFormatManager& formats;
    FilePreviewHost host;
    juce::AudioThumbnailCache cache { 8 };
    juce::AudioThumbnail thumbnail;
    juce::File file;
    juce::String info;
    double length = 0.0;
    bool playing = false;

    juce::Label name, details;
    juce::TextButton playButton, stopButton { "Stop" };
    juce::ToggleButton autoPlay { "Auto play (a click on a file plays it)" };
};
} // namespace wf
