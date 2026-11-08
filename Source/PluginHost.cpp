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

int scanSilenceLimitMs = 60000;                  // no plug-in checked for this long: it hangs
constexpr int scanTotalLimitMs = 30 * 60 * 1000;   // one file (a shell can hold hundreds)
constexpr int renderBlock = 4096;
constexpr double longestTailSeconds = 30.0;

juce::File libraryFile()   { return startup::settingsFolder().getChildFile ("plugins.xml"); }

// What a scan's child process has found so far. Rewritten after every plug-in,
// so the parent sees it moving and keeps what came before a crash or a hang.
class ScanReport
{
public:
    explicit ScanReport (juce::File fileIn) : file (std::move (fileIn)) {}

    void found (const juce::PluginDescription& d)    { xml.addChildElement (d.createXml().release()); write(); }
    void failed (const juce::String& name)           { xml.createNewChildElement ("FAILED")->setAttribute ("name", name); write(); }
    void finish()                                    { xml.setAttribute ("complete", true); write(); }

private:
    void write()
    {
        const auto next = file.getSiblingFile (file.getFileName() + ".next");
        xml.writeTo (next);
        next.moveFileTo (file);
    }

    juce::File file;
    juce::XmlElement xml { "PLUGINS" };
};

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

// One plug-in in a chain. A chain of one is the single plug-in window.
struct Slot
{
    juce::PluginDescription description;
    std::shared_ptr<juce::AudioPluginInstance> plugin;
    int channels = 0;
    std::shared_ptr<std::atomic<bool>> on = std::make_shared<std::atomic<bool>> (true);
};

using Slots = std::vector<Slot>;

std::optional<Slot> openSlot (Library& library, const juce::PluginDescription& description, double rate, int fileChannels, juce::String& error)
{
    if (description.isInstrument)
    {
        error = description.name + " is an instrument: it makes sound of its own and can't process a file.";
        return {};
    }

    auto instance = library.formats().createPluginInstance (description, rate, renderBlock, error);

    if (instance == nullptr)
    {
        error = "Could not open " + description.name + ": " + error;
        return {};
    }

    const auto channels = setUpChannels (*instance, fileChannels);

    if (channels == 0)
    {
        error = description.name + " can't work on " + juce::String (fileChannels) + (fileChannels == 1 ? " channel." : " channels.");
        return {};
    }

    Slot slot;
    slot.description = description;
    slot.plugin = std::shared_ptr<juce::AudioPluginInstance> (std::move (instance));
    slot.channels = channels;
    return slot;
}

void prepareSlot (const Slot& slot, double rate, int block, bool offline)
{
    auto& plugin = *slot.plugin;
    plugin.releaseResources();
    plugin.setNonRealtime (offline);
    plugin.setPlayConfigDetails (plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels(), rate, block);
    plugin.prepareToPlay (rate, block);
    plugin.reset();
}

// A block of the audio into `signal` (one channel per file channel); silence past its end.
void fillSignal (juce::AudioBuffer<float>& signal, const juce::AudioBuffer<float>& in, juce::int64 inPos, int n, bool loop)
{
    const auto length = in.getNumSamples();

    for (int ch = 0; ch < signal.getNumChannels(); ++ch)
    {
        auto* out = signal.getWritePointer (ch);
        const auto* from = in.getReadPointer (ch);

        for (int i = 0; i < n; ++i)
        {
            const auto p = inPos + i;
            out[i] = loop ? from[p % length] : (p < length ? from[p] : 0.0f);
        }
    }
}

// `signal` through one plug-in and back. A mono file through a stereo-only
// plug-in goes in on both sides and comes back as the average.
void runSlot (const Slot& slot, juce::AudioBuffer<float>& signal, juce::AudioBuffer<float>& work, int n, juce::MidiBuffer& midi)
{
    const auto fileChannels = signal.getNumChannels();

    for (int ch = 0; ch < slot.channels; ++ch)
        work.copyFrom (ch, 0, signal, std::min (ch, fileChannels - 1), 0, n);

    juce::AudioBuffer<float> block (work.getArrayOfWritePointers(), slot.channels, n);
    midi.clear();

    {
        const juce::ScopedLock sl (slot.plugin->getCallbackLock());

        if (slot.plugin->isSuspended())
            block.clear();
        else
            slot.plugin->processBlock (block, midi);
    }

    if (fileChannels == 1 && slot.channels == 2)
    {
        signal.clear (0, 0, n);
        signal.addFrom (0, 0, work, 0, 0, n, 0.5f);
        signal.addFrom (0, 0, work, 1, 0, n, 0.5f);
        return;
    }

    for (int ch = 0; ch < fileChannels; ++ch)
        signal.copyFrom (ch, 0, work, ch, 0, n);
}

//==============================================================================
// The selection in a loop through the plug-ins that are on.
class PreviewSource final : public juce::PositionableAudioSource
{
public:
    PreviewSource (std::shared_ptr<const juce::AudioBuffer<float>> a, Slots s, std::shared_ptr<std::atomic<bool>> b)
        : audio (std::move (a)), slots (std::move (s)), bypass (std::move (b)) {}

    void prepareToPlay (int block, double rate) override
    {
        const auto n = std::max (block, renderBlock);
        signal.setSize (audio->getNumChannels(), n);
        work.clear();

        for (const auto& slot : slots)
        {
            work.emplace_back (slot.channels, n);
            prepareSlot (slot, rate, n, false);
        }
    }

    void releaseResources() override
    {
        for (const auto& slot : slots)
            slot.plugin->releaseResources();
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        if (audio->getNumSamples() == 0)
        {
            info.clearActiveBufferRegion();
            return;
        }

        for (int done = 0; done < info.numSamples;)
        {
            const auto n = std::min (info.numSamples - done, signal.getNumSamples());
            const auto pos = position.load();
            fillSignal (signal, *audio, pos, n, true);
            position = pos + n;

            if (! bypass->load())
                for (size_t i = 0; i < slots.size(); ++i)
                    if (slots[i].on->load())
                        runSlot (slots[i], signal, work[i], n, midi);

            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->copyFrom (ch, info.startSample + done, signal, std::min (ch, signal.getNumChannels() - 1), 0, n);

            done += n;
        }
    }

    void setNextReadPosition (juce::int64 p) override   { position = p; }
    juce::int64 getNextReadPosition() const override    { return position; }
    juce::int64 getTotalLength() const override         { return std::numeric_limits<juce::int64>::max(); }
    bool isLooping() const override                     { return true; }

private:
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    Slots slots;
    std::shared_ptr<std::atomic<bool>> bypass;
    juce::AudioBuffer<float> signal;
    std::vector<juce::AudioBuffer<float>> work;
    juce::MidiBuffer midi;
    std::atomic<juce::int64> position { 0 };
};

//==============================================================================
// The whole audio through the plug-ins that are on, offline. Their delays add up
// and are taken out, so the result stays in time with the file; the tails ring
// on after the end, without the silence that follows them.
std::optional<juce::AudioBuffer<float>> render (const Slots& allSlots, const juce::AudioBuffer<float>& audio, double rate,
                                                const std::function<bool (float)>& progress)
{
    Slots slots;

    for (const auto& slot : allSlots)
        if (slot.on->load())
            slots.push_back (slot);

    juce::int64 latency = 0;
    auto tailSeconds = 0.0;
    std::vector<juce::AudioBuffer<float>> work;

    for (const auto& slot : slots)
    {
        prepareSlot (slot, rate, renderBlock, true);
        latency += std::max (0, slot.plugin->getLatencySamples());
        const auto t = slot.plugin->getTailLengthSeconds();
        tailSeconds += std::isfinite (t) ? std::max (0.0, t) : longestTailSeconds;
        work.emplace_back (slot.channels, renderBlock);
    }

    auto finish = [&slots]
    {
        for (const auto& slot : slots)
        {
            slot.plugin->setNonRealtime (false);
            slot.plugin->releaseResources();
        }
    };

    const auto tail = (int) std::lround (std::min (tailSeconds, longestTailSeconds) * rate);
    const auto length = audio.getNumSamples();
    const auto wanted = (juce::int64) length + tail;

    if (wanted + latency > (juce::int64) std::numeric_limits<int>::max())
    {
        finish();
        return {};
    }

    juce::AudioBuffer<float> result (audio.getNumChannels(), (int) wanted);
    result.clear();
    juce::AudioBuffer<float> signal (audio.getNumChannels(), renderBlock);
    juce::MidiBuffer midi;
    const auto total = wanted + latency;

    for (juce::int64 pos = 0; pos < total; pos += renderBlock)
    {
        const auto n = (int) std::min ((juce::int64) renderBlock, total - pos);
        fillSignal (signal, audio, pos, n, false);

        for (size_t i = 0; i < slots.size(); ++i)
            runSlot (slots[i], signal, work[i], n, midi);

        // What comes out `latency` samples late lands where it belongs.
        const auto outPos = pos - latency;
        const auto skip = (int) std::max ((juce::int64) 0, -outPos);

        if (skip < n)
            for (int ch = 0; ch < result.getNumChannels(); ++ch)
                result.copyFrom (ch, (int) std::max ((juce::int64) 0, outPos), signal, ch, skip, n - skip);

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
    PluginPanel (Slot slotIn, BassFinderSource sourceIn, BassFinderHost hostIn, std::function<void()> onCloseIn)
        : slot (std::move (slotIn)), plugin (slot.plugin), description (slot.description),
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

        host.play (std::make_unique<PreviewSource> (audio, Slots { slot }, bypass), source.sampleRate,
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
        auto slots = Slots { slot };
        auto input = audio;
        const auto rate = source.sampleRate;
        auto result = std::make_shared<std::optional<juce::AudioBuffer<float>>>();
        juce::Component::SafePointer<PluginPanel> safeThis (this);

        host.runJob ("Applying " + description.name + "...",
                     [slots, input, rate, result] (std::function<bool (float)> progress)
                     {
                         *result = render (slots, *input, rate, progress);
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

    Slot slot;
    std::shared_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription description;
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
// Plug-In Chain: several plug-ins one after the other. The list on the left
// (click the box to turn one off), the selected plug-in's own window on the
// right. The chain and every plug-in's settings are remembered.
class ChainPanel final : public juce::Component,
                         public SpaceTarget,
                         private juce::ListBoxModel,
                         private juce::Timer
{
public:
    ChainPanel (Library& libraryIn, BassFinderSource sourceIn, BassFinderHost hostIn,
                std::function<void()> openManagerIn, std::function<void()> onCloseIn)
        : library (libraryIn), source (std::move (sourceIn)), host (std::move (hostIn)),
          openManager (std::move (openManagerIn)), onClose (std::move (onCloseIn))
    {
        audio = std::make_shared<juce::AudioBuffer<float>> (source.audio);

        styleLabel (title, 13.0f, true);
        title.setText (source.title, juce::dontSendNotification);
        addAndMakeVisible (title);
        styleLabel (status, 12.0f, false, colours::muted);
        status.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (status);
        styleLabel (empty, 13.0f, false, colours::muted);
        empty.setJustificationType (juce::Justification::centred);
        empty.setText ("Add a plug-in to start the chain.", juce::dontSendNotification);
        addAndMakeVisible (empty);

        list.setRowHeight (30);
        list.setModel (this);
        list.setColour (juce::ListBox::backgroundColourId, colours::surface);
        addAndMakeVisible (list);

        add.onClick = [this] { showAddMenu(); };
        remove.onClick = [this] { removeSelected(); };
        up.onClick = [this] { moveSelected (-1); };
        down.onClick = [this] { moveSelected (1); };
        preview.onClick = [this] { togglePreview(); };
        bypassAll.setClickingTogglesState (true);
        bypassAll.onClick = [this] { bypass->store (bypassAll.getToggleState()); };
        apply.onClick = [this] { applyToFile(); };
        cancel.onClick = [this] { close(); };

        for (auto* b : { &add, &remove, &up, &down, &preview, &bypassAll, &apply, &cancel })
            addAndMakeVisible (b);

        restore();
        list.updateContent();
        list.selectRow (0);
        showEditorOfSelected();
        update();
        startTimerHz (20);
    }

    ~ChainPanel() override
    {
        stopTimer();
        stopPreview();
        editor.reset();
        list.setModel (nullptr);
    }

    bool spacePressed() override   { togglePreview(); return true; }

    void close()
    {
        stopPreview();
        save();

        if (onClose != nullptr)
            onClose();
    }

    // Puts the plug-in at the end of the chain, with the settings it last had.
    bool addPlugin (const juce::PluginDescription& description, juce::String& error)
    {
        auto slot = openSlot (library, description, source.sampleRate, audio->getNumChannels(), error);

        if (! slot.has_value())
            return false;

        juce::MemoryBlock saved;

        if (stateFile (description).loadFileAsData (saved) && saved.getSize() > 0)
            slot->plugin->setStateInformation (saved.getData(), (int) saved.getSize());

        restructure ([this, &slot]
        {
            slots.push_back (std::move (*slot));
            selected = (int) slots.size() - 1;
        });
        return true;
    }

    void removeSelected()
    {
        if (! juce::isPositiveAndBelow (selected, (int) slots.size()))
            return;

        restructure ([this]
        {
            slots.erase (slots.begin() + selected);
            selected = std::min (selected, (int) slots.size() - 1);
        });
    }

    void moveSelected (int delta)
    {
        const auto to = selected + delta;

        if (! juce::isPositiveAndBelow (selected, (int) slots.size()) || ! juce::isPositiveAndBelow (to, (int) slots.size()))
            return;

        restructure ([this, to]
        {
            std::swap (slots[(size_t) selected], slots[(size_t) to]);
            selected = to;
        });
    }

    void paint (juce::Graphics& g) override   { g.fillAll (colours::bg); }

    void childBoundsChanged (juce::Component* child) override
    {
        if (child == editor.get())
            fit();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto bar = area.removeFromBottom (barHeight).reduced (10, 6);
        auto left = area.removeFromLeft (listWidth).reduced (10);
        auto buttons = left.removeFromBottom (64);
        left.removeFromBottom (6);
        list.setBounds (left);
        auto first = buttons.removeFromTop (29);
        add.setBounds (first.removeFromLeft (first.getWidth() / 2 - 2));
        first.removeFromLeft (4);
        remove.setBounds (first);
        buttons.removeFromTop (6);
        up.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
        buttons.removeFromLeft (4);
        down.setBounds (buttons);

        if (editor != nullptr)
            editor->setTopLeftPosition (listWidth, 0);

        empty.setBounds (area);

        auto top = bar.removeFromTop (22);
        status.setBounds (top.removeFromRight (top.getWidth() / 2));
        title.setBounds (top);
        bar.removeFromTop (6);
        auto row = bar.removeFromTop (30);
        cancel.setBounds (row.removeFromRight (70));
        row.removeFromRight (6);
        apply.setBounds (row.removeFromRight (70));
        preview.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (6);
        bypassAll.setBounds (row.removeFromLeft (90));
    }

private:
    int getNumRows() override   { return (int) slots.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool isSelected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) slots.size()))
            return;

        if (isSelected)
            g.fillAll (colours::accent.withAlpha (0.25f));

        const auto& slot = slots[(size_t) row];
        const auto on = slot.on->load();
        const juce::Rectangle<float> box (8.0f, (float) height / 2.0f - 7.0f, 14.0f, 14.0f);
        g.setColour (on ? colours::ok : colours::muted);
        g.drawRoundedRectangle (box, 3.0f, 1.5f);

        if (on)
            g.fillRoundedRectangle (box.reduced (3.5f), 1.5f);

        g.setColour (on ? colours::ink : colours::muted);
        g.setFont (13.0f);
        g.drawText (juce::String (row + 1) + ".  " + slot.description.name, 30, 0, width - 34, height, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.x < 28 && juce::isPositiveAndBelow (row, (int) slots.size()))
        {
            const auto& on = slots[(size_t) row].on;
            on->store (! on->load());
            list.repaint();
            update();
        }
    }

    void selectedRowsChanged (int row) override
    {
        if (row >= 0 && row != selected)
        {
            selected = row;
            showEditorOfSelected();
            update();
        }
    }

    // Changes the chain with the preview stopped; it goes on afterwards.
    void restructure (const std::function<void()>& change)
    {
        const auto wasPlaying = previewing;
        const auto position = host.playPosition != nullptr ? host.playPosition() : -1;
        stopPreview();
        editor.reset();
        change();
        list.updateContent();
        list.selectRow (selected, true, true);
        list.repaint();
        showEditorOfSelected();
        update();
        save();

        if (wasPlaying && anyOn())
            startPreview (std::max ((juce::int64) 0, position));
    }

    void showEditorOfSelected()
    {
        editor.reset();

        if (juce::isPositiveAndBelow (selected, (int) slots.size()))
        {
            auto& plugin = *slots[(size_t) selected].plugin;
            editor.reset (plugin.createEditorAndMakeActive());

            if (editor == nullptr)
                editor = std::make_unique<juce::GenericAudioProcessorEditor> (plugin);

            addAndMakeVisible (*editor);
        }

        empty.setVisible (editor == nullptr);
        fit();
    }

    void fit()
    {
        const auto editorWidth = editor != nullptr ? editor->getWidth() : 0;
        const auto editorHeight = editor != nullptr ? editor->getHeight() : 0;
        setSize (listWidth + std::max (editorWidth, minEditorWidth), std::max (editorHeight, minEditorHeight) + barHeight);

        if (editor != nullptr)
            editor->setTopLeftPosition (listWidth, 0);
    }

    bool anyOn() const
    {
        return std::any_of (slots.begin(), slots.end(), [] (const Slot& s) { return s.on->load(); });
    }

    void update()
    {
        const auto has = juce::isPositiveAndBelow (selected, (int) slots.size());
        remove.setEnabled (has);
        up.setEnabled (has && selected > 0);
        down.setEnabled (has && selected < (int) slots.size() - 1);
        preview.setEnabled (previewing || anyOn());
        apply.setEnabled (anyOn());
    }

    void showAddMenu()
    {
        juce::PopupMenu menu;
        const auto plugins = library.plugins();
        auto id = 1;

        for (const auto& d : plugins)
        {
            if (! d.isInstrument)
                menu.addItem (id, d.name + "  (" + d.manufacturerName + ")");

            ++id;
        }

        if (menu.getNumItems() == 0)
            menu.addItem (-2, library.isScanning() ? "Still looking for plug-ins..." : "No plug-ins found", false);

        menu.addSeparator();
        menu.addItem (-1, "Plug-In Manager...");
        juce::Component::SafePointer<ChainPanel> safeThis (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (add).withParentComponent (getTopLevelComponent()),
                            [safeThis, plugins] (int chosen)
                            {
                                auto* self = safeThis.getComponent();

                                if (self == nullptr || chosen == 0)
                                    return;

                                if (chosen == -1)
                                {
                                    if (self->openManager != nullptr)
                                        self->openManager();

                                    return;
                                }

                                juce::String error;

                                if (chosen > 0 && ! self->addPlugin (plugins[chosen - 1], error))
                                    self->status.setText (error, juce::dontSendNotification);
                            });
    }

    void togglePreview()
    {
        if (previewing)
            stopPreview();
        else if (anyOn())
            startPreview (0);
    }

    void startPreview (juce::int64 position)
    {
        if (host.play == nullptr)
            return;

        auto player = std::make_unique<PreviewSource> (audio, slots, bypass);
        player->setNextReadPosition (position);
        host.play (std::move (player), source.sampleRate, std::max (1, audio->getNumChannels()));
        previewing = true;
        preview.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xa0  Stop preview (Space)"));
        status.setText ("Previewing the chain in a loop.", juce::dontSendNotification);
        update();
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
        update();
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

        if (write == nullptr || host.runJob == nullptr || ! anyOn())
            return;

        stopPreview();
        save();
        auto chain = slots;
        auto input = audio;
        const auto rate = source.sampleRate;
        auto result = std::make_shared<std::optional<juce::AudioBuffer<float>>>();
        juce::Component::SafePointer<ChainPanel> safeThis (this);

        host.runJob ("Applying the plug-in chain...",
                     [chain, input, rate, result] (std::function<bool (float)> progress)
                     {
                         *result = render (chain, *input, rate, progress);
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

    static juce::File chainFile()   { return startup::settingsFolder().getChildFile ("plugin-chain.xml"); }

    void save()
    {
        juce::XmlElement xml ("ADORMIS_CHAIN");

        for (const auto& slot : slots)
        {
            juce::MemoryBlock state;
            slot.plugin->getStateInformation (state);
            auto* e = xml.createNewChildElement ("SLOT");
            e->setAttribute ("on", slot.on->load());
            e->setAttribute ("state", state.toBase64Encoding());
            e->addChildElement (slot.description.createXml().release());
        }

        chainFile().getParentDirectory().createDirectory();
        xml.writeTo (chainFile());
    }

    // The last chain; a plug-in that can't be opened any more is left out and named.
    void restore()
    {
        const auto xml = juce::parseXML (chainFile());

        if (xml == nullptr || ! xml->hasTagName ("ADORMIS_CHAIN"))
            return;

        juce::StringArray missing;

        for (auto* e : xml->getChildWithTagNameIterator ("SLOT"))
        {
            juce::PluginDescription description;
            auto* d = e->getChildByName ("PLUGIN");

            if (d == nullptr || ! description.loadFromXml (*d))
                continue;

            juce::String error;
            auto slot = openSlot (library, description, source.sampleRate, audio->getNumChannels(), error);

            if (! slot.has_value())
            {
                missing.add (description.name);
                continue;
            }

            juce::MemoryBlock state;

            if (state.fromBase64Encoding (e->getStringAttribute ("state")) && state.getSize() > 0)
                slot->plugin->setStateInformation (state.getData(), (int) state.getSize());

            slot->on->store (e->getBoolAttribute ("on", true));
            slots.push_back (std::move (*slot));
        }

        selected = slots.empty() ? -1 : 0;

        if (! missing.isEmpty())
            status.setText ("Left out: " + missing.joinIntoString (", "), juce::dontSendNotification);
    }

    static constexpr int barHeight = 76, listWidth = 240, minEditorWidth = 420, minEditorHeight = 300;

    Library& library;
    BassFinderSource source;
    BassFinderHost host;
    std::function<void()> openManager, onClose;
    std::shared_ptr<juce::AudioBuffer<float>> audio;
    std::shared_ptr<std::atomic<bool>> bypass = std::make_shared<std::atomic<bool>> (false);
    Slots slots;
    int selected = -1;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    bool previewing = false;
    juce::ListBox list;
    juce::Label title, status, empty;
    juce::TextButton add { "Add..." }, remove { "Remove" }, up { "Move up" }, down { "Move down" },
                     preview { juce::CharPointer_UTF8 ("\xe2\x96\xb6  Preview (Space)") }, bypassAll { "Bypass all" },
                     apply { "Apply" }, cancel { "Close" };
};

class ChainWindow final : public juce::DocumentWindow
{
public:
    explicit ChainWindow (ChainPanel* panel)
        : juce::DocumentWindow ("Plug-In Chain", colours::bg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (false);
        setLookAndFeel (&look);
        setContentOwned (panel, true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    ~ChainWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    void closeButtonPressed() override
    {
        if (auto* p = dynamic_cast<ChainPanel*> (getContentComponent()))
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
    ScanReport report { juce::File (args[2].unquoted()) };

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
                {
                    report.failed (d->name);
                    continue;
                }
            }

            report.found (*d);
        }
    }

    report.finish();
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
    stopThread (5000);
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
                auto lastChange = started;
                auto lastSize = (juce::int64) -1;
                auto tooLong = false;

                while (child.isRunning() && ! threadShouldExit())
                {
                    const auto now = juce::Time::getMillisecondCounter();

                    if (const auto size = report.getSize(); size != lastSize)
                    {
                        lastSize = size;
                        lastChange = now;
                    }

                    if (now - lastChange > (juce::uint32) scanSilenceLimitMs || now - started > (juce::uint32) scanTotalLimitMs)
                    {
                        tooLong = true;
                        break;
                    }

                    wait (50);
                }

                const auto stopped = threadShouldExit();

                if (child.isRunning())
                    child.kill();

                // What it reported before anything went wrong still counts.
                const auto xml = juce::parseXML (report);
                const auto complete = xml != nullptr && xml->getBoolAttribute ("complete");
                juce::StringArray notOpened;
                auto kept = 0;

                if (xml != nullptr)
                {
                    for (auto* e : xml->getChildWithTagNameIterator ("FAILED"))
                        notOpened.add (e->getStringAttribute ("name"));

                    for (auto* e : xml->getChildIterator())
                    {
                        juce::PluginDescription d;

                        if (! e->hasTagName ("FAILED") && d.loadFromXml (*e))
                        {
                            // An unfinished file is checked again at the next Rescan.
                            d.lastFileModTime = complete && notOpened.isEmpty() ? juce::File (file).getLastModificationTime() : juce::Time();
                            d.lastInfoUpdateTime = juce::Time::getCurrentTime();
                            known.addType (d);
                            ++kept;
                        }
                    }
                }

                if (stopped)
                    problem = "scan stopped";
                else if (! complete)
                    problem = juce::String (tooLong ? "it stopped answering" : "it crashed while being opened")
                            + (kept > 0 ? " (" + juce::String (kept) + " of its plug-ins were kept)" : juce::String());
                else if (kept == 0 && notOpened.isEmpty())
                    problem = "no plug-in could be opened in it";
                else if (! notOpened.isEmpty())
                    problem = "could not open " + notOpened.joinIntoString (", ");
            }

            report.deleteFile();
            report.getSiblingFile (report.getFileName() + ".next").deleteFile();

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
    auto slot = openSlot (library, description, source.sampleRate, source.audio.getNumChannels(), error);

    if (! slot.has_value())
        return {};

    auto* panel = new PluginPanel (std::move (*slot), std::move (source), std::move (host), std::move (onClose));
    return std::make_unique<PluginWindow> (description.name, panel);
}

std::unique_ptr<juce::DocumentWindow> createChainWindow (Library& library, BassFinderSource source, BassFinderHost host,
                                                         std::function<void()> openManager, std::function<void()> onClose)
{
    auto* panel = new ChainPanel (library, std::move (source), std::move (host), std::move (openManager), std::move (onClose));
    return std::make_unique<ChainWindow> (panel);
}
} // namespace wf::plugins
