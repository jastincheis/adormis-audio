#include "PluginHost.h"
#include "DarkTheme.h"
#include "SpaceKey.h"
#include "StartupLog.h"
#include <juce_audio_processors/format/juce_AudioPluginFormatManagerHelpers.h>
#include <juce_cryptography/juce_cryptography.h>

namespace wf::plugins
{
namespace
{
using namespace dark;

constexpr int scanTimeoutMs = 60000;
constexpr int renderBlock = 4096;
constexpr double longestTailSeconds = 30.0;

juce::File libraryFile()   { return startup::settingsFolder().getChildFile ("plugins.xml"); }

juce::File stateFile (const juce::PluginDescription& d)
{
    const auto id = d.createIdentifierString();
    return startup::settingsFolder().getChildFile ("plugin-state")
        .getChildFile (juce::SHA256 (id.toUTF8()).toHexString().substring (0, 32) + ".bin");
}

//==============================================================================
// How the file's channels meet the plug-in's. Sidechains and other extra buses
// stay off. A mono file through a stereo-only plug-in goes in on both sides and
// comes back as the average.
int setUpChannels (juce::AudioPluginInstance& plugin, int fileChannels)
{
    auto attempt = [&plugin] (int channels)
    {
        auto layout = plugin.getBusesLayout();

        if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
            return false;

        const auto set = juce::AudioChannelSet::canonicalChannelSet (channels);

        for (auto& bus : layout.inputBuses)   bus = juce::AudioChannelSet::disabled();
        for (auto& bus : layout.outputBuses)  bus = juce::AudioChannelSet::disabled();

        layout.inputBuses.getReference (0) = set;
        layout.outputBuses.getReference (0) = set;
        return plugin.setBusesLayout (layout);
    };

    if (attempt (fileChannels))
        return fileChannels;

    return fileChannels == 1 && attempt (2) ? 2 : 0;
}

// One block through the plug-in. `work` has the plug-in's channels; the block is
// copied in from `in` at inPos (silence past its end) and left in `work`.
void fillWork (juce::AudioBuffer<float>& work, const juce::AudioBuffer<float>& in, juce::int64 inPos, int n, bool loop)
{
    const auto length = in.getNumSamples();

    for (int ch = 0; ch < work.getNumChannels(); ++ch)
    {
        const auto source = std::min (ch, in.getNumChannels() - 1);
        auto* out = work.getWritePointer (ch);
        const auto* from = in.getReadPointer (source);

        for (int i = 0; i < n; ++i)
        {
            const auto p = inPos + i;
            out[i] = loop ? from[p % length] : (p < length ? from[p] : 0.0f);
        }
    }
}

void runPlugin (juce::AudioPluginInstance& plugin, juce::AudioBuffer<float>& work, int n, juce::MidiBuffer& midi)
{
    juce::AudioBuffer<float> block (work.getArrayOfWritePointers(), work.getNumChannels(), n);
    midi.clear();
    const juce::ScopedLock sl (plugin.getCallbackLock());

    if (plugin.isSuspended())
        block.clear();
    else
        plugin.processBlock (block, midi);
}

// From the plug-in's channels back to the file's.
void readOut (const juce::AudioBuffer<float>& work, juce::AudioBuffer<float>& out, int outChannels, int outStart, int workStart, int count)
{
    if (outChannels == 1 && work.getNumChannels() == 2)
    {
        out.clear (0, outStart, count);
        out.addFrom (0, outStart, work, 0, workStart, count, 0.5f);
        out.addFrom (0, outStart, work, 1, workStart, count, 0.5f);
        return;
    }

    for (int ch = 0; ch < outChannels; ++ch)
        out.copyFrom (ch, outStart, work, ch, workStart, count);
}

//==============================================================================
// The selection in a loop through the plug-in.
class PreviewSource final : public juce::PositionableAudioSource
{
public:
    PreviewSource (std::shared_ptr<const juce::AudioBuffer<float>> a, std::shared_ptr<juce::AudioPluginInstance> p,
                   int pluginChannelsIn, std::shared_ptr<std::atomic<bool>> b)
        : audio (std::move (a)), plugin (std::move (p)), bypass (std::move (b)), pluginChannels (pluginChannelsIn) {}

    void prepareToPlay (int block, double rate) override
    {
        const auto n = std::max (block, renderBlock);
        work.setSize (pluginChannels, n);
        plugin->setNonRealtime (false);
        plugin->setPlayConfigDetails (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels(), rate, n);
        plugin->prepareToPlay (rate, n);
        plugin->reset();
    }

    void releaseResources() override   { plugin->releaseResources(); }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const auto length = audio->getNumSamples();

        if (length == 0)
        {
            info.clearActiveBufferRegion();
            return;
        }

        const auto fileChannels = audio->getNumChannels();

        for (int done = 0; done < info.numSamples;)
        {
            const auto n = std::min (info.numSamples - done, work.getNumSamples());
            const auto pos = position.load();
            fillWork (work, *audio, pos, n, true);
            position = pos + n;

            if (! bypass->load())
                runPlugin (*plugin, work, n, midi);

            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            {
                const auto from = fileChannels == 1 ? 0 : std::min (ch, pluginChannels - 1);

                if (fileChannels == 1 && pluginChannels == 2)
                {
                    info.buffer->clear (ch, info.startSample + done, n);
                    info.buffer->addFrom (ch, info.startSample + done, work, 0, 0, n, 0.5f);
                    info.buffer->addFrom (ch, info.startSample + done, work, 1, 0, n, 0.5f);
                }
                else
                {
                    info.buffer->copyFrom (ch, info.startSample + done, work, from, 0, n);
                }
            }

            done += n;
        }
    }

    void setNextReadPosition (juce::int64 p) override   { position = p; }
    juce::int64 getNextReadPosition() const override    { return position; }
    juce::int64 getTotalLength() const override         { return std::numeric_limits<juce::int64>::max(); }
    bool isLooping() const override                     { return true; }

private:
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    std::shared_ptr<juce::AudioPluginInstance> plugin;
    std::shared_ptr<std::atomic<bool>> bypass;
    int pluginChannels;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;
    std::atomic<juce::int64> position { 0 };
};

//==============================================================================
// The whole audio through the plug-in, offline. The plug-in's delay is taken
// out, so the result stays in time with the file; its tail rings on after the
// end, without the silence that follows it.
std::optional<juce::AudioBuffer<float>> render (juce::AudioPluginInstance& plugin, const juce::AudioBuffer<float>& audio,
                                                int pluginChannels, double rate, const std::function<bool (float)>& progress)
{
    plugin.releaseResources();
    plugin.setNonRealtime (true);
    plugin.setPlayConfigDetails (plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels(), rate, renderBlock);
    plugin.prepareToPlay (rate, renderBlock);
    plugin.reset();

    const auto latency = std::max (0, plugin.getLatencySamples());
    const auto tailSeconds = plugin.getTailLengthSeconds();
    const auto tail = (int) std::lround (juce::jlimit (0.0, longestTailSeconds, std::isfinite (tailSeconds) ? tailSeconds : longestTailSeconds) * rate);
    const auto length = audio.getNumSamples();
    const auto wanted = (juce::int64) length + tail;

    if (wanted > (juce::int64) std::numeric_limits<int>::max() - latency)
        return {};

    juce::AudioBuffer<float> result (audio.getNumChannels(), (int) wanted);
    result.clear();
    juce::AudioBuffer<float> work (pluginChannels, renderBlock);
    juce::MidiBuffer midi;
    const auto total = wanted + latency;
    auto finish = [&plugin]
    {
        plugin.setNonRealtime (false);
        plugin.releaseResources();
    };

    for (juce::int64 pos = 0; pos < total; pos += renderBlock)
    {
        const auto n = (int) std::min ((juce::int64) renderBlock, total - pos);
        fillWork (work, audio, pos, n, false);
        runPlugin (plugin, work, n, midi);

        // What comes out `latency` samples late lands where it belongs.
        const auto outPos = pos - latency;
        const auto skip = (int) std::max ((juce::int64) 0, -outPos);

        if (skip < n)
            readOut (work, result, result.getNumChannels(), (int) std::max ((juce::int64) 0, outPos), skip, n - skip);

        if (progress != nullptr && ! progress ((float) ((double) (pos + n) / (double) total)))
        {
            finish();
            return {};
        }
    }

    finish();

    // The tail ends where it goes quiet.
    constexpr float silent = 1.0e-5f;   // -100 dB
    auto end = (int) wanted;

    while (end > length)
    {
        auto loud = false;

        for (int ch = 0; ch < result.getNumChannels() && ! loud; ++ch)
            loud = std::abs (result.getSample (ch, end - 1)) > silent;

        if (loud)
            break;

        --end;
    }

    result.setSize (result.getNumChannels(), end, true);
    return result;
}

//==============================================================================
void styleLabel (juce::Label& l, float size, bool bold, juce::Colour c = colours::ink)
{
    l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    l.setColour (juce::Label::textColourId, c);
    l.setJustificationType (juce::Justification::centredLeft);
    l.setInterceptsMouseClicks (false, false);
}

//==============================================================================
// The plug-in's own window above a bar with Preview, Bypass, Apply and Close.
class PluginPanel final : public juce::Component,
                          public SpaceTarget,
                          private juce::Timer
{
public:
    PluginPanel (std::shared_ptr<juce::AudioPluginInstance> instance, juce::PluginDescription descriptionIn, int channels,
                 BassFinderSource sourceIn, BassFinderHost hostIn, std::function<void()> onCloseIn)
        : plugin (std::move (instance)), description (std::move (descriptionIn)), pluginChannels (channels),
          source (std::move (sourceIn)), host (std::move (hostIn)), onClose (std::move (onCloseIn))
    {
        audio = std::make_shared<juce::AudioBuffer<float>> (source.audio);
        plugin->getStateInformation (defaults);

        juce::MemoryBlock saved;

        if (stateFile (description).loadFileAsData (saved) && saved.getSize() > 0)
            plugin->setStateInformation (saved.getData(), (int) saved.getSize());

        styleLabel (title, 13.0f, true);
        title.setText (source.title, juce::dontSendNotification);
        addAndMakeVisible (title);
        styleLabel (status, 12.0f, false, colours::muted);
        status.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (status);

        preview.onClick = [this] { togglePreview(); };
        bypassButton.setClickingTogglesState (true);
        bypassButton.onClick = [this] { bypass->store (bypassButton.getToggleState()); };
        defaultsButton.setTooltip ("Back to the plug-in's original settings");
        defaultsButton.onClick = [this] { plugin->setStateInformation (defaults.getData(), (int) defaults.getSize()); };
        apply.onClick = [this] { applyToFile(); };
        cancel.onClick = [this] { close(); };

        for (auto* b : { &preview, &bypassButton, &defaultsButton, &apply, &cancel })
            addAndMakeVisible (b);

        editor.reset (plugin->createEditorAndMakeActive());

        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (*plugin);

        addAndMakeVisible (*editor);
        fitToEditor();
        startTimerHz (20);
    }

    ~PluginPanel() override
    {
        stopTimer();
        stopPreview();
        editor.reset();   // before the plug-in goes
    }

    bool spacePressed() override   { togglePreview(); return true; }

    void close()
    {
        stopPreview();
        saveState();

        if (onClose != nullptr)
            onClose();
    }

    void paint (juce::Graphics& g) override   { g.fillAll (colours::bg); }

    void childBoundsChanged (juce::Component* child) override
    {
        if (child == editor.get())
            fitToEditor();
    }

    void resized() override
    {
        if (editor == nullptr)
            return;

        editor->setTopLeftPosition (0, 0);
        auto area = getLocalBounds().withTop (editor->getHeight()).reduced (10, 6);
        auto top = area.removeFromTop (22);
        status.setBounds (top.removeFromRight (top.getWidth() / 2));
        title.setBounds (top);
        area.removeFromTop (6);
        auto row = area.removeFromTop (30);
        cancel.setBounds (row.removeFromRight (70));
        row.removeFromRight (6);
        apply.setBounds (row.removeFromRight (70));
        preview.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (6);
        bypassButton.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (6);
        defaultsButton.setBounds (row.removeFromLeft (80));
    }

private:
    void fitToEditor()
    {
        if (editor != nullptr)
            setSize (std::max (editor->getWidth(), minWidth), editor->getHeight() + barHeight);
    }

    void saveState()
    {
        juce::MemoryBlock state;
        plugin->getStateInformation (state);
        const auto file = stateFile (description);
        file.getParentDirectory().createDirectory();
        file.replaceWithData (state.getData(), state.getSize());
    }

    void togglePreview()
    {
        if (previewing)
        {
            stopPreview();
            return;
        }

        if (host.play == nullptr)
            return;

        host.play (std::make_unique<PreviewSource> (audio, plugin, pluginChannels, bypass), source.sampleRate,
                   std::max (1, audio->getNumChannels()));
        previewing = true;
        preview.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xa0  Stop preview (Space)"));
        status.setText ("Previewing in a loop.", juce::dontSendNotification);
    }

    void stopPreview()
    {
        if (! previewing)
            return;

        if (host.stop != nullptr)
            host.stop();

        previewing = false;
        preview.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb6  Preview (Space)"));

        if (source.showPlayhead != nullptr)
            source.showPlayhead (-1.0);

        status.setText ({}, juce::dontSendNotification);
    }

    void timerCallback() override
    {
        if (! previewing)
            return;

        const auto p = host.playPosition != nullptr ? host.playPosition() : -1;

        if (p < 0)
            stopPreview();
        else if (source.showPlayhead != nullptr)
            source.showPlayhead ((double) (p % std::max (1, audio->getNumSamples())) / source.sampleRate);
    }

    void applyToFile()
    {
        auto write = source.applyWithTail;

        if (write == nullptr || host.runJob == nullptr)
            return;

        stopPreview();
        saveState();
        auto instance = plugin;
        auto input = audio;
        const auto rate = source.sampleRate;
        const auto channels = pluginChannels;
        auto result = std::make_shared<std::optional<juce::AudioBuffer<float>>>();
        juce::Component::SafePointer<PluginPanel> safeThis (this);

        host.runJob ("Applying " + description.name + "...",
                     [instance, input, rate, channels, result] (std::function<bool (float)> progress)
                     {
                         *result = render (*instance, *input, channels, rate, progress);
                         return result->has_value();
                     },
                     [safeThis, write, result] (bool finished)
                     {
                         auto* self = safeThis.getComponent();

                         if (! finished || ! result->has_value())
                         {
                             if (self != nullptr)
                                 self->status.setText ("Cancelled.", juce::dontSendNotification);

                             return;
                         }

                         if (! write (**result))
                         {
                             if (self != nullptr)
                                 self->status.setText ("The file is closed: can't apply.", juce::dontSendNotification);

                             return;
                         }

                         if (self != nullptr)
                             self->close();
                     });
    }

    static constexpr int barHeight = 76, minWidth = 520;

    std::shared_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription description;
    int pluginChannels;
    BassFinderSource source;
    BassFinderHost host;
    std::function<void()> onClose;
    std::shared_ptr<juce::AudioBuffer<float>> audio;
    std::shared_ptr<std::atomic<bool>> bypass = std::make_shared<std::atomic<bool>> (false);
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::MemoryBlock defaults;
    bool previewing = false;
    juce::Label title, status;
    juce::TextButton preview { juce::CharPointer_UTF8 ("\xe2\x96\xb6  Preview (Space)") }, bypassButton { "Bypass" },
                     defaultsButton { "Defaults" }, apply { "Apply" }, cancel { "Close" };
};

class PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow (const juce::String& name, PluginPanel* panel)
        : juce::DocumentWindow (name, colours::bg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (false);
        setLookAndFeel (&look);
        setContentOwned (panel, true);
        setResizable (false, false);
        // Stays in front of the main window when you click there.
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    ~PluginWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    void closeButtonPressed() override
    {
        if (auto* p = dynamic_cast<PluginPanel*> (getContentComponent()))
            p->close();
    }

private:
    DarkLook look;
};

//==============================================================================
class ManagerPanel final : public juce::Component,
                           private juce::ListBoxModel,
                           private juce::ChangeListener
{
public:
    ManagerPanel (Library& libraryIn, ManagerCallbacks callbacksIn)
        : library (libraryIn), callbacks (std::move (callbacksIn))
    {
        styleLabel (pluginsLabel, 13.0f, true);
        pluginsLabel.setText ("VST3 plug-ins", juce::dontSendNotification);
        styleLabel (foldersLabel, 13.0f, true);
        foldersLabel.setText ("Folders searched", juce::dontSendNotification);
        styleLabel (status, 12.0f, false, colours::muted);
        addAndMakeVisible (pluginsLabel);
        addAndMakeVisible (foldersLabel);
        addAndMakeVisible (status);

        plugins.setRowHeight (36);
        plugins.setModel (this);
        plugins.setColour (juce::ListBox::backgroundColourId, colours::surface);
        addAndMakeVisible (plugins);

        folderModel.owner = this;
        folders.setRowHeight (22);
        folders.setModel (&folderModel);
        folders.setColour (juce::ListBox::backgroundColourId, colours::surface);
        addAndMakeVisible (folders);

        use.setButtonText ("Use on the file...");
        use.onClick = [this] { useSelected(); };
        rescan.setButtonText ("Rescan");
        rescan.onClick = [this] { library.startScan(); };
        addFolder.setButtonText ("Add folder...");
        addFolder.onClick = [this]
        {
            if (callbacks.chooseFolder == nullptr)
                return;

            juce::Component::SafePointer<ManagerPanel> safeThis (this);
            callbacks.chooseFolder ([safeThis] (juce::File folder)
            {
                if (auto* self = safeThis.getComponent(); self != nullptr && folder.isDirectory())
                {
                    self->library.addFolder (folder);
                    self->library.startScan();
                }
            });
        };
        removeFolder.setButtonText ("Remove folder");
        removeFolder.onClick = [this]
        {
            const auto row = folders.getSelectedRow();
            const auto own = library.folders();

            if (juce::isPositiveAndBelow (row, own.size()))
            {
                library.removeFolder (own[row]);
                library.startScan();
            }
        };
        close.setButtonText ("Close");
        close.onClick = [this] { if (callbacks.onClose != nullptr) callbacks.onClose(); };

        for (auto* b : { &use, &rescan, &addFolder, &removeFolder, &close })
            addAndMakeVisible (b);

        library.addChangeListener (this);
        refresh();
        setSize (720, 560);

        if (! library.everScanned())
            library.startScan();
    }

    ~ManagerPanel() override
    {
        library.removeChangeListener (this);
        plugins.setModel (nullptr);
        folders.setModel (nullptr);
    }

    void paint (juce::Graphics& g) override   { g.fillAll (colours::bg); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto buttons = area.removeFromBottom (32);
        close.setBounds (buttons.removeFromRight (70));
        buttons.removeFromRight (6);
        use.setBounds (buttons.removeFromRight (130));
        buttons.removeFromRight (6);
        rescan.setBounds (buttons.removeFromLeft (80));
        buttons.removeFromLeft (6);
        addFolder.setBounds (buttons.removeFromLeft (100));
        buttons.removeFromLeft (6);
        removeFolder.setBounds (buttons.removeFromLeft (110));
        area.removeFromBottom (8);
        status.setBounds (area.removeFromBottom (22));
        area.removeFromBottom (4);
        folders.setBounds (area.removeFromBottom (100));
        foldersLabel.setBounds (area.removeFromBottom (24));
        area.removeFromBottom (4);
        pluginsLabel.setBounds (area.removeFromTop (24));
        this->plugins.setBounds (area);
    }

private:
    struct FolderModel final : juce::ListBoxModel
    {
        ManagerPanel* owner = nullptr;

        int getNumRows() override   { return owner->allFolders.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            if (selected)
                g.fillAll (colours::accent.withAlpha (0.25f));

            const auto isDefault = row >= owner->library.folders().size();
            g.setColour (isDefault ? colours::muted : colours::ink);
            g.setFont (12.5f);
            g.drawText (owner->allFolders[row] + (isDefault ? "  (standard)" : ""), 8, 0, width - 12, height, juce::Justification::centredLeft, true);
        }
    };

    bool isPluginRow (int row) const   { return juce::isPositiveAndBelow (row, descriptions.size()); }

    int getNumRows() override   { return descriptions.size() + failures.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (selected)
            g.fillAll (colours::accent.withAlpha (0.25f));

        const auto text = width - 16;

        if (isPluginRow (row))
        {
            const auto& d = descriptions.getReference (row);
            g.setColour (d.isInstrument ? colours::muted : colours::ink);
            g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
            g.drawText (d.name + (d.isInstrument ? "  (instrument: makes sound, can't process audio)" : ""), 8, 2, text, height / 2, juce::Justification::centredLeft, true);
            g.setColour (colours::muted);
            g.setFont (11.5f);
            g.drawText (d.manufacturerName + "  " + d.version + "  " + d.fileOrIdentifier, 8, height / 2, text, height / 2 - 2, juce::Justification::centredLeft, true);
            return;
        }

        g.setColour (colours::bad);
        g.setFont (12.0f);
        g.drawText (failures[row - descriptions.size()], 8, 0, text, height, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int) override   { updateButtons(); }
    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override   { useSelected(); }

    void changeListenerCallback (juce::ChangeBroadcaster*) override   { refresh(); }

    void refresh()
    {
        descriptions = library.plugins();
        failures = library.failed();
        allFolders = library.folders();
        allFolders.addArray (library.defaultFolders());
        allFolders.removeDuplicates (false);
        plugins.updateContent();
        plugins.repaint();
        folders.updateContent();
        folders.repaint();

        const auto scanning = library.isScanning();
        status.setText (scanning ? library.progressText()
                                 : juce::String (descriptions.size()) + " plug-ins found"
                                       + (failures.isEmpty() ? juce::String() : ", " + juce::String (failures.size()) + " could not be opened")
                                       + ". Install a plug-in with its installer or copy it to one of these folders, then Rescan.",
                        juce::dontSendNotification);
        rescan.setEnabled (! scanning);
        updateButtons();
    }

    void updateButtons()
    {
        const auto row = plugins.getSelectedRow();
        use.setEnabled (isPluginRow (row) && ! descriptions.getReference (row).isInstrument
                        && callbacks.hasFile != nullptr && callbacks.hasFile());
        removeFolder.setEnabled (juce::isPositiveAndBelow (folders.getSelectedRow(), library.folders().size()));
    }

    void useSelected()
    {
        const auto row = plugins.getSelectedRow();

        if (isPluginRow (row) && use.isEnabled() && callbacks.use != nullptr)
            callbacks.use (descriptions.getReference (row));
    }

    Library& library;
    ManagerCallbacks callbacks;
    FolderModel folderModel;
    juce::ListBox plugins, folders;
    juce::Label pluginsLabel, foldersLabel, status;
    juce::TextButton use, rescan, addFolder, removeFolder, close;
    juce::Array<juce::PluginDescription> descriptions;
    juce::StringArray failures, allFolders;
};

class ManagerWindow final : public juce::DocumentWindow
{
public:
    ManagerWindow (Library& library, ManagerCallbacks callbacks)
        : juce::DocumentWindow ("Plug-In Manager", colours::bg, juce::DocumentWindow::closeButton),
          onClose (callbacks.onClose)
    {
        setUsingNativeTitleBar (false);
        setLookAndFeel (&look);
        setContentOwned (new ManagerPanel (library, std::move (callbacks)), true);
        adoptLook (*getContentComponent());
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    ~ManagerWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    void closeButtonPressed() override
    {
        if (onClose != nullptr)
            onClose();
    }

private:
    DarkLook look;
    std::function<void()> onClose;
};
} // namespace

//==============================================================================
bool runScanChild (const juce::StringArray& args)
{
    if (args.size() != 3 || args[0] != "--scan-plugin")
        return false;

    juce::AudioPluginFormatManager manager;
    juce::addDefaultFormatsToManager (manager);
    juce::XmlElement report ("PLUGINS");

    for (auto* format : manager.getFormats())
    {
        if (! format->fileMightContainThisPluginType (args[1].unquoted()))
            continue;

        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, args[1].unquoted());

        for (auto* d : found)
        {
            // The file's description alone doesn't load a modern VST3, so open
            // it here: if it crashes or hangs, it does so in this process.
            if (! d->isInstrument)
            {
                juce::String error;

                if (manager.createPluginInstance (*d, 44100.0, renderBlock, error) == nullptr)
                    continue;
            }

            report.addChildElement (d->createXml().release());
        }
    }

    report.writeTo (juce::File (args[2].unquoted()));
    return true;
}

//==============================================================================
Library::Library() : juce::Thread ("Plug-in scan")
{
    juce::addDefaultFormatsToManager (formatManager);
    load();
}

Library::~Library()
{
    alive->store (false);
    stopThread (scanTimeoutMs + 2000);
}

void Library::load()
{
    const auto xml = juce::parseXML (libraryFile());

    if (xml == nullptr || ! xml->hasTagName ("ADORMIS_PLUGINS"))
        return;

    scannedBefore = xml->getBoolAttribute ("scanned");

    if (auto* list = xml->getChildByName ("KNOWNPLUGINS"))
        known.recreateFromXml (*list);

    for (auto* e : xml->getChildWithTagNameIterator ("FOLDER"))
        userFolders.add (e->getStringAttribute ("path"));

    for (auto* e : xml->getChildWithTagNameIterator ("FAILED"))
        failures.add (e->getStringAttribute ("text"));
}

void Library::save()
{
    juce::XmlElement xml ("ADORMIS_PLUGINS");
    const juce::ScopedLock sl (lock);
    xml.setAttribute ("scanned", scannedBefore);

    if (auto list = known.createXml())
        xml.addChildElement (list.release());

    for (const auto& f : userFolders)
        xml.createNewChildElement ("FOLDER")->setAttribute ("path", f);

    for (const auto& f : failures)
        xml.createNewChildElement ("FAILED")->setAttribute ("text", f);

    libraryFile().getParentDirectory().createDirectory();
    xml.writeTo (libraryFile());
}

juce::Array<juce::PluginDescription> Library::plugins() const
{
    auto types = known.getTypes();
    std::sort (types.begin(), types.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return types;
}

juce::StringArray Library::failed() const
{
    const juce::ScopedLock sl (lock);
    return failures;
}

juce::StringArray Library::folders() const
{
    const juce::ScopedLock sl (lock);
    return userFolders;
}

juce::StringArray Library::defaultFolders() const
{
    juce::StringArray result;

    for (auto* format : formatManager.getFormats())
        for (int i = 0; i < format->getDefaultLocationsToSearch().getNumPaths(); ++i)
            result.add (format->getDefaultLocationsToSearch()[i].getFullPathName());

    return result;
}

void Library::addFolder (const juce::File& folder)
{
    {
        const juce::ScopedLock sl (lock);
        userFolders.addIfNotAlreadyThere (folder.getFullPathName());
    }

    save();
    announce();
}

void Library::removeFolder (const juce::String& folder)
{
    {
        const juce::ScopedLock sl (lock);
        userFolders.removeString (folder);
    }

    save();
    announce();
}

bool Library::everScanned() const
{
    const juce::ScopedLock sl (lock);
    return scannedBefore;
}

juce::String Library::progressText() const
{
    const juce::ScopedLock sl (lock);
    return progress;
}

void Library::startScan()
{
    if (isThreadRunning())
        return;

    {
        const juce::ScopedLock sl (lock);
        progress = "Looking for plug-ins...";
    }

    startThread();
    announce();
}

void Library::announce()
{
    juce::MessageManager::callAsync ([this, stillAlive = alive]
    {
        if (stillAlive->load())
            sendChangeMessage();
    });
}

void Library::run()
{
    juce::StringArray nowFailing;
    juce::StringArray stillThere;
    const auto program = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();

    for (auto* format : formatManager.getFormats())
    {
        juce::FileSearchPath search = format->getDefaultLocationsToSearch();

        for (const auto& folder : folders())
            search.addIfNotAlreadyThere (juce::File (folder));

        const auto files = format->searchPathsForPlugins (search, true, true);

        for (int i = 0; i < files.size() && ! threadShouldExit(); ++i)
        {
            const auto& file = files[i];
            stillThere.add (file);

            {
                const juce::ScopedLock sl (lock);
                progress = "Checking " + juce::String (i + 1) + " of " + juce::String (files.size()) + ": "
                         + format->getNameOfPluginFromIdentifier (file);
            }

            announce();

            if (known.isListingUpToDate (file, *format))
                continue;

            // A plug-in that crashes or hangs here takes only this child with it.
            const auto report = juce::File::createTempFile (".xml");
            juce::ChildProcess child;
            auto problem = juce::String();

            if (! child.start (juce::StringArray { program, "--scan-plugin", file, report.getFullPathName() }, 0))
            {
                problem = "could not start the check";
            }
            else
            {
                const auto started = juce::Time::getMillisecondCounter();

                while (child.isRunning() && ! threadShouldExit()
                       && juce::Time::getMillisecondCounter() - started < (juce::uint32) scanTimeoutMs)
                    wait (50);

                if (child.isRunning())
                {
                    child.kill();
                    problem = threadShouldExit() ? "scan stopped" : "it did not answer for 60 seconds";
                }
                else if (const auto xml = juce::parseXML (report); xml == nullptr)
                {
                    problem = "it crashed while being opened";
                }
                else
                {
                    auto any = false;

                    for (auto* e : xml->getChildIterator())
                    {
                        juce::PluginDescription d;

                        if (d.loadFromXml (*e))
                        {
                            d.lastFileModTime = juce::File (file).getLastModificationTime();
                            d.lastInfoUpdateTime = juce::Time::getCurrentTime();
                            known.addType (d);
                            any = true;
                        }
                    }

                    if (! any)
                        problem = "no plug-in could be opened in it";
                }
            }

            report.deleteFile();

            if (problem.isNotEmpty())
                nowFailing.add (file + ": " + problem);
        }

        if (threadShouldExit())
            break;
    }

    if (! threadShouldExit())
    {
        for (const auto& d : known.getTypes())
            if (! stillThere.contains (d.fileOrIdentifier))
                known.removeType (d);

        const juce::ScopedLock sl (lock);
        failures = nowFailing;
        scannedBefore = true;
        progress = {};
    }

    save();
    announce();
}

//==============================================================================
std::unique_ptr<juce::DocumentWindow> createManagerWindow (Library& library, ManagerCallbacks callbacks)
{
    return std::make_unique<ManagerWindow> (library, std::move (callbacks));
}

std::unique_ptr<juce::DocumentWindow> createPluginWindow (Library& library, const juce::PluginDescription& description,
                                                          BassFinderSource source, BassFinderHost host,
                                                          std::function<void()> onClose, juce::String& error)
{
    if (description.isInstrument)
    {
        error = description.name + " is an instrument: it makes sound of its own and can't process a file.";
        return {};
    }

    auto instance = library.formats().createPluginInstance (description, source.sampleRate, renderBlock, error);

    if (instance == nullptr)
    {
        error = "Could not open " + description.name + ": " + error;
        return {};
    }

    const auto fileChannels = source.audio.getNumChannels();
    const auto channels = setUpChannels (*instance, fileChannels);

    if (channels == 0)
    {
        error = description.name + " can't work on " + juce::String (fileChannels)
              + (fileChannels == 1 ? " channel." : " channels.");
        return {};
    }

    std::shared_ptr<juce::AudioPluginInstance> shared (std::move (instance));
    auto* panel = new PluginPanel (shared, description, channels, std::move (source), std::move (host), std::move (onClose));
    return std::make_unique<PluginWindow> (description.name, panel);
}
} // namespace wf::plugins
