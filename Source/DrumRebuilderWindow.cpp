#include "DrumRebuilderWindow.h"
#include "StartupLog.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <map>
#include "plugin/PluginProcessor.h"

namespace wf
{
namespace
{
class DrumRebuilderWindow final : public juce::DocumentWindow
{
public:
    explicit DrumRebuilderWindow (DrumRebuilderHost h)
        : juce::DocumentWindow ("Drum Rebuilder", juce::Colours::black, juce::DocumentWindow::closeButton), host (std::move (h))
    {
        processor.hostName = "Adormis Audio";
        processor.sendToHost = [this] (juce::AudioBuffer<float> beat, double rate)
        {
            if (host.openAsNewFile != nullptr)
                host.openAsNewFile (nameOfLoaded() + " rebuilt", std::move (beat), rate);
        };
        if (host.openBeats != nullptr)
            processor.hostBeats = [this]
            {
                std::vector<DrumRebuilderProcessor::HostBeat> list;
                for (auto& b : host.openBeats())
                    list.push_back ({ b.name, [this, name = b.name, write = b.write]
                    {
                        const auto f = write();
                        if (f != juce::File())
                            names[f.getFullPathName()] = name;   // what comes back is "<name> rebuilt"
                        return f;
                    } });
                return list;
            };

        player.setProcessor (&processor);
        if (host.devices != nullptr)
            host.devices->addAudioCallback (&player);

        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setContentOwned (processor.createEditor(), true);

        // The window has a fixed size, so the editor's resize corner goes.
        if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
            editor->setResizable (false, false);
        // Fixed size so tiling window managers (Hyprland) float it instead of
        // tiling it next to the main window.
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
    }

    ~DrumRebuilderWindow() override
    {
        clearContentComponent();   // the editor uses the processor

        if (host.devices != nullptr)
            host.devices->removeAudioCallback (&player);
        player.setProcessor (nullptr);

        // Asking for the state writes the beat's fixes to Drum Rebuilder's
        // memory now, not 0.4 s later when the window is gone.
        juce::MemoryBlock state;
        processor.getStateInformation (state);
    }

    void load (const juce::File& beat, const juce::String& name)
    {
        names[beat.getFullPathName()] = name;
        processor.loadFile (beat);
    }

    void closeButtonPressed() override
    {
        if (host.onClose != nullptr)
            host.onClose();
    }

private:
    DrumRebuilderHost host;
    DrumRebuilderProcessor processor;
    juce::AudioProcessorPlayer player;
    std::map<juce::String, juce::String> names;   // beat file -> the name it had in Adormis Audio

    juce::String nameOfLoaded() const
    {
        if (const auto* l = processor.getLoaded())
        {
            if (const auto it = names.find (l->file.getFullPathName()); it != names.end())
                return it->second;
            return l->file.getFileNameWithoutExtension();   // opened from a folder inside Drum Rebuilder
        }
        return "Drum Rebuilder";
    }
};

juce::String contentTag (const juce::AudioBuffer<float>& audio)
{
    // FNV-1a over a sample in 16 of each channel: cheap, and different for
    // any real edit.
    juce::uint64 h = 1469598103934665603ull;

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int i = 0; i < audio.getNumSamples(); i += 16)
        {
            const auto v = audio.getSample (ch, i);
            juce::uint32 bits;
            std::memcpy (&bits, &v, sizeof (bits));
            h = (h ^ bits) * 1099511628211ull;
        }

    return juce::String::toHexString ((juce::int64) (h ^ (juce::uint64) audio.getNumSamples())).substring (0, 6);
}
} // namespace

std::unique_ptr<juce::DocumentWindow> createDrumRebuilderWindow (DrumRebuilderHost host)
{
    return std::make_unique<DrumRebuilderWindow> (std::move (host));
}

void loadDrumRebuilderBeat (juce::DocumentWindow& window, const juce::File& beat, const juce::String& name)
{
    if (auto* w = dynamic_cast<DrumRebuilderWindow*> (&window))
        w->load (beat, name);
}

juce::File drumRebuilderBeatFile (const juce::String& title, const juce::File& original, bool wholeAndUnchanged,
                                  const juce::AudioBuffer<float>& audio, double sampleRate, double startSeconds)
{
    if (wholeAndUnchanged && original.existsAsFile() && original.hasFileExtension ("wav;aif;aiff;flac"))
        return original;

    auto name = juce::File::createLegalFileName (juce::File (title).getFileNameWithoutExtension());

    if (! wholeAndUnchanged)
        name << " " << juce::String (startSeconds, 2) << "-" << juce::String (startSeconds + audio.getNumSamples() / sampleRate, 2) << " s";

    name << " [" << contentTag (audio) << "].wav";
    const auto folder = startup::settingsFolder().getChildFile ("Drum Rebuilder beats");
    const auto file = folder.getChildFile (name);

    if (file.existsAsFile())
        return file;   // the same audio, written before

    if (! folder.createDirectory())
        return {};

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());

    if (out == nullptr)
        return {};

    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
        out, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (audio.getNumChannels()).withBitsPerSample (32)));

    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
    {
        writer.reset();
        file.deleteFile();
        return {};
    }

    return file;
}
} // namespace wf
