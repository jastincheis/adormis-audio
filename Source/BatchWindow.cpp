#include "BatchWindow.h"
#include "DataWindow.h"
#include "FormDialog.h"
#include "Mp3Encoder.h"
#include "MoreEffects.h"

namespace wf
{
namespace
{
const juce::Colour panelColour (0xffd4d0c8);

struct OutputChoice
{
    juce::String name, extension;
    int bits = 16;
    bool floatingPoint = false;
    int kbps = 0;
};

std::vector<OutputChoice> outputChoices()
{
    std::vector<OutputChoice> c {
        { "WAV 16-bit", ".wav", 16 }, { "WAV 24-bit", ".wav", 24 }, { "WAV 32-bit float", ".wav", 32, true },
        { "FLAC 16-bit", ".flac", 16 }, { "FLAC 24-bit", ".flac", 24 },
        { "AIFF 16-bit", ".aiff", 16 }, { "AIFF 24-bit", ".aiff", 24 },
    };

    if (mp3EncodingAvailable())
        for (int k : { 320, 256, 192, 128 })
            c.push_back ({ "MP3 " + juce::String (k) + " kbps", ".mp3", 16, false, k });

    for (int k : { 256, 192, 128 })
        c.push_back ({ "OGG " + juce::String (k) + " kbps", ".ogg", 16, false, k });

    return c;
}

// What the window remembers between uses (this session).
struct Settings
{
    bool normalize = true, trim = false, fadeIn = false, fadeOut = false, channels = false, rate = false;
    int normalizeMode = 2;   // 0 peak, 1 loudness, 2 loudness with limiter
    juce::String target = "-14", ceiling = "-1", trimDb = "-60", fadeInMs = "10", fadeOutMs = "500";
    int channelMode = 0, rateIndex = 1, outputIndex = 1;
    juce::String suffix, folder;
};

Settings& remembered()
{
    static Settings s;
    return s;
}

const juce::StringArray rates { "32000", "44100", "48000", "88200", "96000" };

class BatchPanel final : public juce::Component,
                         private juce::ListBoxModel
{
public:
    BatchPanel (juce::AudioFormatManager& f, FilePicker& p, juce::Array<juce::File> initial, std::function<void()> close)
        : formats (f), picker (p), onClose (std::move (close)), choices (outputChoices())
    {
        files.addArray (initial);
        list.setModel (this);
        list.setRowHeight (20);
        list.setMultipleSelectionEnabled (true);
        list.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff808080));
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        for (auto* b : { &addFiles, &addFolder, &removeFiles, &clearFiles, &chooseFolder, &start, &closeButton })
            addAndMakeVisible (b);

        addFiles.onClick = [this] { addFromPicker (false); };
        addFolder.onClick = [this] { addFromPicker (true); };
        removeFiles.onClick = [this]
        {
            for (int i = files.size(); --i >= 0;)
                if (list.isRowSelected (i))
                    files.remove (i);

            refreshList();
        };
        clearFiles.onClick = [this] { files.clear(); refreshList(); };
        chooseFolder.onClick = [this] { pickOutputFolder(); };
        start.onClick = [this] { running ? cancel() : begin(); };
        closeButton.onClick = [this] { if (! running && onClose) onClose(); };

        const auto& s = remembered();
        setupToggle (normalize, "Normalize", s.normalize);
        setupToggle (trim, "Trim silence at the ends, below", s.trim);
        setupToggle (fadeIn, "Fade in", s.fadeIn);
        setupToggle (fadeOut, "Fade out", s.fadeOut);
        setupToggle (channels, "Channels", s.channels);
        setupToggle (rate, "Sample rate", s.rate);

        normalizeMode.addItemList ({ "Peak level (dB)", "Loudness (LUFS)", "Loudness, limiting peaks if needed" }, 1);
        normalizeMode.setSelectedItemIndex (s.normalizeMode, juce::dontSendNotification);
        normalizeMode.onChange = [this] { updateUnits(); };
        channelMode.addItemList ({ "Mono (mix both)", "Stereo (copy mono to both)" }, 1);
        channelMode.setSelectedItemIndex (s.channelMode, juce::dontSendNotification);
        rateBox.addItemList (rates, 1);
        rateBox.setSelectedItemIndex (s.rateIndex, juce::dontSendNotification);

        for (const auto& c : choices)
            output.addItem (c.name, output.getNumItems() + 1);

        output.setSelectedItemIndex (juce::jlimit (0, (int) choices.size() - 1, s.outputIndex), juce::dontSendNotification);

        setupText (target, s.target);
        setupText (ceiling, s.ceiling);
        setupText (trimDb, s.trimDb);
        setupText (fadeInMs, s.fadeInMs);
        setupText (fadeOutMs, s.fadeOutMs);
        setupText (suffix, s.suffix);
        suffix.setJustification (juce::Justification::centredLeft);
        suffix.setTextToShowWhenEmpty ("(nothing)", juce::Colours::grey);

        for (auto* c : { &normalizeMode, &channelMode, &rateBox, &output })
            addAndMakeVisible (c);

        for (auto* l : { &filesLabel, &stepsLabel, &outputLabel, &targetUnit, &ceilingLabel, &ceilingUnit, &dbUnit,
                         &msUnit1, &msUnit2, &formatLabel, &folderLabel, &suffixLabel, &folderPath, &status })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::black);
            addAndMakeVisible (l);
        }

        for (auto* l : { &filesLabel, &stepsLabel, &outputLabel })
            l->setFont (juce::FontOptions (14.0f, juce::Font::bold));

        folder = s.folder.isNotEmpty() ? juce::File (s.folder) : juce::File();

        if (folder == juce::File() && ! files.isEmpty())
            folder = files[0].getParentDirectory().getChildFile ("Converted");

        updateUnits();
        refreshList();
        setSize (760, 600);
    }

    ~BatchPanel() override
    {
        // Kept for the next time the window is opened (this session).
        auto& s = remembered();
        s.normalize = normalize.getToggleState();
        s.trim = trim.getToggleState();
        s.fadeIn = fadeIn.getToggleState();
        s.fadeOut = fadeOut.getToggleState();
        s.channels = channels.getToggleState();
        s.rate = rate.getToggleState();
        s.normalizeMode = normalizeMode.getSelectedItemIndex();
        s.channelMode = channelMode.getSelectedItemIndex();
        s.rateIndex = rateBox.getSelectedItemIndex();
        s.outputIndex = output.getSelectedItemIndex();
        s.target = target.getText();
        s.ceiling = ceiling.getText();
        s.trimDb = trimDb.getText();
        s.fadeInMs = fadeInMs.getText();
        s.fadeOutMs = fadeOutMs.getText();
        s.suffix = suffix.getText();
        s.folder = folder.getFullPathName();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        filesLabel.setBounds (r.removeFromTop (20));
        auto filesArea = r.removeFromTop (170);
        auto buttons = filesArea.removeFromRight (120);
        filesArea.removeFromRight (8);
        list.setBounds (filesArea);

        for (auto* b : { &addFiles, &addFolder, &removeFiles, &clearFiles })
        {
            b->setBounds (buttons.removeFromTop (28));
            buttons.removeFromTop (6);
        }

        r.removeFromTop (10);
        stepsLabel.setBounds (r.removeFromTop (20));

        auto row = [&r] { auto x = r.removeFromTop (28); r.removeFromTop (4); return x; };

        auto line = row();
        normalize.setBounds (line.removeFromLeft (160));
        normalizeMode.setBounds (line.removeFromLeft (250).reduced (0, 1));
        line.removeFromLeft (8);
        target.setBounds (line.removeFromLeft (60));
        targetUnit.setBounds (line.removeFromLeft (50));
        ceilingLabel.setBounds (line.removeFromLeft (90));
        ceiling.setBounds (line.removeFromLeft (50));
        ceilingUnit.setBounds (line.removeFromLeft (50));

        line = row();
        trim.setBounds (line.removeFromLeft (250));
        trimDb.setBounds (line.removeFromLeft (60));
        dbUnit.setBounds (line.removeFromLeft (40));

        line = row();
        fadeIn.setBounds (line.removeFromLeft (100));
        fadeInMs.setBounds (line.removeFromLeft (60));
        msUnit1.setBounds (line.removeFromLeft (60));
        fadeOut.setBounds (line.removeFromLeft (100));
        fadeOutMs.setBounds (line.removeFromLeft (60));
        msUnit2.setBounds (line.removeFromLeft (40));

        line = row();
        channels.setBounds (line.removeFromLeft (160));
        channelMode.setBounds (line.removeFromLeft (250).reduced (0, 1));

        line = row();
        rate.setBounds (line.removeFromLeft (160));
        rateBox.setBounds (line.removeFromLeft (120).reduced (0, 1));

        r.removeFromTop (8);
        outputLabel.setBounds (r.removeFromTop (20));
        line = row();
        formatLabel.setBounds (line.removeFromLeft (80));
        output.setBounds (line.removeFromLeft (200).reduced (0, 1));
        line.removeFromLeft (20);
        suffixLabel.setBounds (line.removeFromLeft (130));
        suffix.setBounds (line.removeFromLeft (140));

        line = row();
        folderLabel.setBounds (line.removeFromLeft (80));
        chooseFolder.setBounds (line.removeFromRight (100));
        line.removeFromRight (8);
        folderPath.setBounds (line);

        auto bottom = r.removeFromBottom (30);
        closeButton.setBounds (bottom.removeFromRight (100));
        bottom.removeFromRight (8);
        start.setBounds (bottom.removeFromRight (140));
        r.removeFromBottom (6);
        status.setBounds (r);
    }

    void close()   { if (onClose) onClose(); }
    bool isRunning() const   { return running; }

private:
    //==========================================================================
    int getNumRows() override   { return files.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, files.size()))
            return;

        g.fillAll (selected ? juce::Colour (0xff0a246a) : (row % 2 == 0 ? juce::Colours::white : juce::Colour (0xfff0f0f0)));
        g.setColour (selected ? juce::Colours::white : juce::Colours::black);
        g.setFont (juce::FontOptions (13.0f));
        const auto state = juce::isPositiveAndBelow (row, (int) results.size()) ? "   " + results[(size_t) row] : juce::String();
        g.drawText (files[row].getFileName() + state, 6, 0, width - 10, height, juce::Justification::centredLeft, true);
    }

    void deleteKeyPressed (int) override   { removeFiles.triggerClick(); }

    void setupToggle (juce::ToggleButton& t, const juce::String& text, bool on)
    {
        t.setButtonText (text);
        t.setToggleState (on, juce::dontSendNotification);
        t.setColour (juce::ToggleButton::textColourId, juce::Colours::black);
        t.setColour (juce::ToggleButton::tickColourId, juce::Colours::black);
        t.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff606060));
        addAndMakeVisible (t);
    }

    void setupText (juce::TextEditor& t, const juce::String& value)
    {
        t.setText (value, false);
        t.setJustification (juce::Justification::centredRight);
        addAndMakeVisible (t);
    }

    void updateUnits()
    {
        const auto peak = normalizeMode.getSelectedItemIndex() == 0;
        targetUnit.setText (peak ? "dB" : "LUFS", juce::dontSendNotification);
        ceiling.setEnabled (! peak);
        ceilingLabel.setText ("true peak max", juce::dontSendNotification);
    }

    void refreshList()
    {
        results.clear();
        list.updateContent();
        list.repaint();
        folderPath.setText (folder == juce::File() ? juce::String ("(choose a folder)") : folder.getFullPathName(),
                            juce::dontSendNotification);
        status.setText (juce::String (files.size()) + (files.size() == 1 ? " file" : " files")
                            + ". The originals are not changed.", juce::dontSendNotification);
        start.setEnabled (! files.isEmpty());
    }

    void addFromPicker (bool directory)
    {
        const auto startDir = files.isEmpty() ? juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                              : files.getLast().getParentDirectory();
        const auto browseFlags = juce::FileBrowserComponent::openMode
                         | (directory ? juce::FileBrowserComponent::canSelectDirectories
                                      : juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems);
        const juce::Component::SafePointer<BatchPanel> safeThis (this);

        picker.launch (directory ? "Add every audio file in a folder" : "Add audio files", startDir,
                       formats.getWildcardForAllFormats(), browseFlags,
                       [safeThis, directory] (const juce::Array<juce::File>& chosen)
                       {
                           auto* self = safeThis.getComponent();

                           if (self == nullptr)
                               return;

                           for (const auto& f : chosen)
                           {
                               if (directory && f.isDirectory())
                               {
                                   for (const auto& entry : juce::RangedDirectoryIterator (f, false, self->formats.getWildcardForAllFormats(),
                                                                                           juce::File::findFiles))
                                       self->files.addIfNotAlreadyThere (entry.getFile());
                               }
                               else if (f.existsAsFile())
                               {
                                   self->files.addIfNotAlreadyThere (f);
                               }
                           }

                           if (self->folder == juce::File() && ! self->files.isEmpty())
                               self->folder = self->files[0].getParentDirectory().getChildFile ("Converted");

                           self->refreshList();
                       });
    }

    void pickOutputFolder()
    {
        const juce::Component::SafePointer<BatchPanel> safeThis (this);
        picker.launch ("Folder for the converted files", folder.exists() ? folder : folder.getParentDirectory(), "*",
                       juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                       [safeThis] (const juce::Array<juce::File>& chosen)
                       {
                           if (auto* self = safeThis.getComponent(); self != nullptr && ! chosen.isEmpty())
                           {
                               self->folder = chosen[0];
                               self->refreshList();
                           }
                       });
    }

    //==========================================================================
    void begin()
    {
        // Check the numbers first.
        auto number = [] (juce::TextEditor& t, double lo, double hi) -> std::optional<double>
        {
            const auto v = parseNumber (t.getText());
            return v.has_value() && *v >= lo && *v <= hi ? v : std::nullopt;
        };

        juce::StringArray problems;
        const auto targetValue = number (target, -70.0, 0.0), ceilingValue = number (ceiling, -12.0, 0.0);
        const auto trimValue = number (trimDb, -120.0, -6.0);
        const auto inMs = number (fadeInMs, 0.0, 60000.0), outMs = number (fadeOutMs, 0.0, 60000.0);

        if (normalize.getToggleState() && ! targetValue) problems.add ("the normalize target (-70 .. 0)");
        if (normalize.getToggleState() && normalizeMode.getSelectedItemIndex() > 0 && ! ceilingValue) problems.add ("the true peak max (-12 .. 0)");
        if (trim.getToggleState() && ! trimValue) problems.add ("the silence level (-120 .. -6 dB)");
        if (fadeIn.getToggleState() && ! inMs) problems.add ("the fade in length");
        if (fadeOut.getToggleState() && ! outMs) problems.add ("the fade out length");
        if (folder == juce::File()) problems.add ("the output folder");

        if (! problems.isEmpty())
        {
            status.setText ("Please check " + problems.joinIntoString (", ") + ".", juce::dontSendNotification);
            return;
        }

        if (! folder.isDirectory() && ! folder.createDirectory())
        {
            status.setText ("Can't create the folder " + folder.getFullPathName(), juce::dontSendNotification);
            return;
        }

        plan = {};
        plan.normalize = normalize.getToggleState();
        plan.normalizeMode = normalizeMode.getSelectedItemIndex();
        plan.target = targetValue.value_or (-14.0);
        plan.ceiling = ceilingValue.value_or (-1.0);
        plan.trim = trim.getToggleState();
        plan.trimDb = trimValue.value_or (-60.0);
        plan.fadeInMs = fadeIn.getToggleState() ? inMs.value_or (0.0) : 0.0;
        plan.fadeOutMs = fadeOut.getToggleState() ? outMs.value_or (0.0) : 0.0;
        plan.channels = channels.getToggleState() ? channelMode.getSelectedItemIndex() + 1 : 0;
        plan.rate = rate.getToggleState() ? rates[rateBox.getSelectedItemIndex()].getDoubleValue() : 0.0;
        plan.output = choices[(size_t) juce::jlimit (0, (int) choices.size() - 1, output.getSelectedItemIndex())];
        plan.suffix = suffix.getText().trim();

        results.assign ((size_t) files.size(), {});
        failures.clear();
        next = 0;
        running = true;
        cancelled = false;
        start.setButtonText ("Cancel");
        closeButton.setEnabled (false);
        processNext();
    }

    void cancel()
    {
        cancelled = true;
        status.setText ("Stopping after this file...", juce::dontSendNotification);
    }

    // One file, then let the window repaint before the next.
    void processNext()
    {
        if (cancelled || next >= files.size())
        {
            finish();
            return;
        }

        const auto file = files[next];
        status.setText ("Processing " + juce::String (next + 1) + " of " + juce::String (files.size()) + ": " + file.getFileName(),
                        juce::dontSendNotification);
        list.repaint();

        juce::Timer::callAfterDelay (30, [safeThis = juce::Component::SafePointer<BatchPanel> (this), file]
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            juce::String error;
            const auto ok = self->processFile (file, error);
            self->results[(size_t) self->next] = ok ? juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93 done")) : "failed: " + error;

            if (! ok)
                self->failures.add (file.getFileName() + ": " + error);

            ++self->next;
            self->list.repaint();
            self->processNext();
        });
    }

    bool processFile (const juce::File& file, juce::String& error)
    {
        auto doc = DataWindow::load (file, formats, error);

        if (doc == nullptr)
            return false;

        auto& view = doc->getView();

        if (plan.channels == 1 && doc->getBuffer().getNumChannels() == 2)
            doc->convertChannels (DataWindow::ChannelConversion::stereoToMono);
        else if (plan.channels == 2 && doc->getBuffer().getNumChannels() == 1)
            doc->convertChannels (DataWindow::ChannelConversion::monoToStereo);

        // The rate first, so the loudness and peaks are measured as they'll be saved.
        if (plan.rate > 0.0 && ! juce::approximatelyEqual (plan.rate, doc->getSampleRate()))
            doc->resample (plan.rate);

        if (plan.trim)
            doc->autoTrim ((float) plan.trimDb);

        view.setSelection ({});

        const auto rateNow = doc->getSampleRate();

        if (plan.normalize)
        {
            if (plan.normalizeMode == 2)
            {
                // Loudness with the limiter: the gain that reaches the target
                // with the peaks held under the ceiling.
                fx::LimiterParams lp;
                lp.ceilingDb = plan.ceiling;
                const auto& audio = doc->getBuffer();

                if (const auto gain = fx::findLimiterGain (audio, rateNow, lp, plan.target, {}))
                {
                    lp.inputDb = *gain;
                    auto limiter = fx::makeLimiter();
                    fx::setParams (*limiter, lp);

                    if (const auto rendered = fx::render (*limiter, audio, rateNow, 0, {}))
                        doc->applyEffect ({ 0, audio.getNumSamples() }, *rendered);
                }
                else
                {
                    doc->normalize (true, plan.target, plan.ceiling);
                }
            }
            else
            {
                doc->normalize (plan.normalizeMode == 1, plan.target, plan.ceiling);
            }
        }

        const auto total = (juce::int64) doc->getBuffer().getNumSamples();

        if (plan.fadeInMs > 0.0)
        {
            view.setSelection ({ 0, std::min (total, (juce::int64) (plan.fadeInMs * 0.001 * rateNow)) });
            doc->applyProcess (Process::fadeIn);
        }

        if (plan.fadeOutMs > 0.0)
        {
            view.setSelection ({ std::max ((juce::int64) 0, total - (juce::int64) (plan.fadeOutMs * 0.001 * rateNow)), total });
            doc->applyProcess (Process::fadeOut);
        }

        view.setSelection ({});

        // Down to 16 bits (or fewer): with dither, like the Bit-Depth Converter.
        if (! plan.output.floatingPoint && plan.output.bits < 24 && plan.output.kbps == 0)
            doc->convertBitDepth (plan.output.bits, true);

        SaveFormat format;
        format.extension = plan.output.extension;
        format.bitsPerSample = plan.output.bits;
        format.floatingPoint = plan.output.floatingPoint;
        format.kbps = plan.output.kbps;

        const auto name = juce::File::createLegalFileName (file.getFileNameWithoutExtension() + plan.suffix);
        auto out = folder.getChildFile (name + format.extension);

        for (int n = 2; out.exists(); ++n)
            out = folder.getChildFile (name + " (" + juce::String (n) + ")" + format.extension);

        return doc->saveTo (out, formats, error, &format);
    }

    void finish()
    {
        running = false;
        start.setButtonText ("Start");
        closeButton.setEnabled (true);
        const auto done = next - failures.size();
        status.setText ((cancelled ? "Stopped. " : "Finished. ") + juce::String (done) + " of " + juce::String (files.size())
                            + " saved in " + folder.getFullPathName()
                            + (failures.isEmpty() ? juce::String() : "  (" + juce::String (failures.size()) + " failed)"),
                        juce::dontSendNotification);

        if (! failures.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Batch Converter",
                                                    "These files could not be converted:\n\n" + failures.joinIntoString ("\n"));
    }

    struct Plan
    {
        bool normalize = false, trim = false;
        int normalizeMode = 0, channels = 0;
        double target = -14.0, ceiling = -1.0, trimDb = -60.0, fadeInMs = 0.0, fadeOutMs = 0.0, rate = 0.0;
        OutputChoice output;
        juce::String suffix;
    };

    juce::AudioFormatManager& formats;
    FilePicker& picker;
    std::function<void()> onClose;
    std::vector<OutputChoice> choices;
    juce::Array<juce::File> files;
    std::vector<juce::String> results;
    juce::StringArray failures;
    juce::File folder;
    Plan plan;
    int next = 0;
    bool running = false, cancelled = false;

    juce::ListBox list;
    juce::TextButton addFiles { "Add Files..." }, addFolder { "Add Folder..." }, removeFiles { "Remove" }, clearFiles { "Clear" },
                     chooseFolder { "Choose..." }, start { "Start" }, closeButton { "Close" };
    juce::ToggleButton normalize, trim, fadeIn, fadeOut, channels, rate;
    juce::ComboBox normalizeMode, channelMode, rateBox, output;
    juce::TextEditor target, ceiling, trimDb, fadeInMs, fadeOutMs, suffix;
    juce::Label filesLabel { {}, "Files" }, stepsLabel { {}, "Steps" }, outputLabel { {}, "Output" },
                targetUnit, ceilingLabel, ceilingUnit { {}, "dBTP" }, dbUnit { {}, "dB" }, msUnit1 { {}, "ms" }, msUnit2 { {}, "ms" },
                formatLabel { {}, "Format:" }, folderLabel { {}, "Folder:" }, suffixLabel { {}, "Add to the names:" }, folderPath, status;
};

class BatchWindow final : public juce::DocumentWindow
{
public:
    explicit BatchWindow (std::unique_ptr<BatchPanel> panel)
        : juce::DocumentWindow ("Batch Converter", panelColour, juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setContentOwned (panel.release(), true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<BatchPanel*> (getContentComponent()); panel != nullptr && ! panel->isRunning())
            panel->close();
    }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createBatchWindow (juce::AudioFormatManager& formats, FilePicker& picker,
                                                         juce::Array<juce::File> initialFiles, std::function<void()> onClose)
{
    return std::make_unique<BatchWindow> (std::make_unique<BatchPanel> (formats, picker, std::move (initialFiles), std::move (onClose)));
}
} // namespace wf
