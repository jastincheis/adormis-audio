#include "AudioFilePreview.h"
#include "Settings.h"

namespace wf
{
AudioFilePreview::AudioFilePreview (juce::AudioFormatManager& f, FilePreviewHost h)
    : formats (f), host (std::move (h)), thumbnail (512, formats, cache)
{
    thumbnail.addChangeListener (this);

    name.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    details.setFont (juce::FontOptions (13.0f));
    details.setJustificationType (juce::Justification::topLeft);

    for (auto* l : { &name, &details })
    {
        l->setColour (juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible (l);
    }

    autoPlay.setColour (juce::ToggleButton::textColourId, juce::Colours::black);
    autoPlay.setColour (juce::ToggleButton::tickColourId, juce::Colours::black);
    autoPlay.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff666666));

    playButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb6  Play"));
    playButton.onClick = [this] { playFrom (0.0); };
    stopButton.onClick = [this] { stop(); };
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);

    const auto saved = settings::get ("open.autoPlay");
    autoPlay.setToggleState (saved.isVoid() || (bool) saved, juce::dontSendNotification);
    autoPlay.onClick = [this] { settings::set ("open.autoPlay", autoPlay.getToggleState()); };
    addAndMakeVisible (autoPlay);

    selectedFileChanged ({});
    startTimerHz (30);
}

AudioFilePreview::~AudioFilePreview()
{
    stop();
    thumbnail.removeChangeListener (this);
}

void AudioFilePreview::selectedFileChanged (const juce::File& newFile)
{
    if (newFile == file && newFile.existsAsFile())
        return;

    stop();
    file = newFile;
    thumbnail.clear();
    length = 0.0;
    info.clear();

    std::unique_ptr<juce::AudioFormatReader> reader;

    if (file.existsAsFile())
        reader.reset (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        name.setText (file.existsAsFile() ? file.getFileName() : juce::String ("Pick a sound to hear it"), juce::dontSendNotification);
        details.setText (file.existsAsFile() ? "Not an audio file this program can read." : juce::String(), juce::dontSendNotification);
        playButton.setEnabled (false);
        stopButton.setEnabled (false);
        repaint();
        return;
    }

    length = (double) reader->lengthInSamples / reader->sampleRate;
    const auto seconds = (int) std::floor (length);
    const auto bits = reader->usesFloatingPointData ? juce::String ("32-bit float") : juce::String ((int) reader->bitsPerSample) + "-bit";
    info = reader->getFormatName() + "\n"
         + juce::String (reader->sampleRate / 1000.0, 1) + " kHz, " + (reader->numChannels == 1 ? "mono" : reader->numChannels == 2 ? "stereo" : juce::String ((int) reader->numChannels) + " channels")
         + ", " + bits + "\n"
         + juce::String::formatted ("%d:%02d.%03d", seconds / 60, seconds % 60, (int) ((length - seconds) * 1000.0))
         + "   (" + juce::File::descriptionOfSizeInBytes (file.getSize()) + ")";
    name.setText (file.getFileName(), juce::dontSendNotification);
    details.setText (info, juce::dontSendNotification);
    thumbnail.setSource (new juce::FileInputSource (file));
    playButton.setEnabled (true);
    stopButton.setEnabled (true);

    if (autoPlay.getToggleState())
        playFrom (0.0);

    repaint();
}

juce::Rectangle<int> AudioFilePreview::waveArea() const
{
    auto r = getLocalBounds().reduced (10);
    r.removeFromTop (24 + 64 + 8);
    return r.removeFromTop (std::max (60, std::min (160, r.getHeight() - 90)));
}

void AudioFilePreview::resized()
{
    auto r = getLocalBounds().reduced (10);
    name.setBounds (r.removeFromTop (24));
    details.setBounds (r.removeFromTop (64));
    r.removeFromTop (8);
    r.removeFromTop (waveArea().getHeight());
    r.removeFromTop (8);
    auto row = r.removeFromTop (30);
    playButton.setBounds (row.removeFromLeft (100).reduced (2));
    stopButton.setBounds (row.removeFromLeft (80).reduced (2));
    r.removeFromTop (6);
    autoPlay.setBounds (r.removeFromTop (24));
}

void AudioFilePreview::paint (juce::Graphics& g)
{
    // its own light panel (the window around it may be dark or light)
    g.setColour (juce::Colour (0xffd4d0c8));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    const auto area = waveArea();
    g.setColour (juce::Colour (0xff1c1f26));
    g.fillRoundedRectangle (area.toFloat(), 6.0f);

    if (length <= 0.0)
        return;

    g.setColour (juce::Colour (0xff8fb4ff));
    thumbnail.drawChannels (g, area.reduced (4), 0.0, length, 1.0f);

    if (const auto at = playing && host.position != nullptr ? host.position() : -1.0; at >= 0.0)
    {
        const auto x = (float) area.getX() + 4.0f + (float) (area.getWidth() - 8) * (float) (at / length);
        g.setColour (juce::Colour (0xffffc23a));
        g.fillRect (x, (float) area.getY(), 1.5f, (float) area.getHeight());
    }
}

void AudioFilePreview::mouseDown (const juce::MouseEvent& e)
{
    const auto area = waveArea().reduced (4);

    if (length > 0.0 && area.contains (e.getPosition()))
        playFrom (length * (e.x - area.getX()) / std::max (1, area.getWidth()));
}

void AudioFilePreview::playFrom (double seconds)
{
    if (! file.existsAsFile() || length <= 0.0 || host.play == nullptr)
        return;

    host.play (file, seconds);
    playing = true;
}

bool AudioFilePreview::spacePressed()
{
    if (playing)
        stop();
    else
        playFrom (0.0);

    return true;
}

void AudioFilePreview::stop()
{
    if (playing && host.stop != nullptr)
        host.stop();

    playing = false;
    repaint();
}

void AudioFilePreview::timerCallback()
{
    if (! playing)
        return;

    // the end of the file: done
    if (host.position != nullptr && host.position() < 0.0)
        playing = false;

    repaint (waveArea());
}
} // namespace wf
