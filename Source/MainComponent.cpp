#include "MainComponent.h"
#include "Analysis.h"
#include "Tempo.h"
#include "Restoration.h"
#include "BatchWindow.h"
#include "StartupLog.h"
#include "Settings.h"
#include "Commands.h"
#include "MixDialog.h"
#include "ShortcutsWindow.h"
#include "DrumRebuilderWindow.h"
#include "StemSeparator.h"
#include "WaveColourPicker.h"
#include "Appearance.h"
#include "LinkDownload.h"

namespace
{
constexpr int menuBarHeight   = 24;
constexpr int statusBarHeight = 22;
constexpr int cascadeStep     = 26;

const juce::Colour panelColour (0xffd4d0c8);

// A file's window in the workspace. Minimize (Sound Forge style): the window
// stays open but shrinks to a small title bar along the bottom of the
// workspace; a click on the bar brings it back as it was.
class DocumentFrame final : public juce::MultiDocumentPanelWindow
{
public:
    static constexpr int barWidth = 220;

    explicit DocumentFrame (juce::Colour colour) : MultiDocumentPanelWindow (colour)
    {
        setTitleBarButtonsRequired (allButtons, false);
    }

    ~DocumentFrame() override
    {
        // The document may live on in another window (or a tab).
        if (auto* content = getContentComponent())
            content->setVisible (true);

        if (minimised && onBarsChanged)
            juce::MessageManager::callAsync (onBarsChanged);
    }

    bool isMinimisedToBar() const   { return minimised; }
    int barHeight() const           { return barHeightPx; }

    void setMinimisedToBar (bool shouldBe)
    {
        if (shouldBe == minimised)
            return;

        minimised = shouldBe;

        if (minimised)
        {
            restoredBounds = getBounds();   // before the border changes
            barHeightPx = getTitleBarHeight() + 6;   // measured while the window is tall (it's capped by the height)
        }

        if (auto* content = getContentComponent())
            content->setVisible (! minimised);

        // The bar keeps only Restore (the minimize button) and Close.
        setTitleBarButtonsRequired (minimised ? (minimiseButton | closeButton) : allButtons, false);
        setResizable (! minimised, false);
        setDraggable (! minimised);

        if (minimised)
            toBack();
        else
        {
            setBounds (restoredBounds);
            toFront (true);
        }

        if (onBarsChanged)
            onBarsChanged();
    }

    void minimiseButtonPressed() override   { setMinimisedToBar (! minimised); }

    void maximiseButtonPressed() override
    {
        // Tabs replace the windows: every document comes back first.
        if (beforeMaximise)
            beforeMaximise();

        // This window is deleted as the files turn into tabs: keep a copy.
        const auto after = afterMaximise;
        MultiDocumentPanelWindow::maximiseButtonPressed();

        if (after)
            after();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        MultiDocumentPanelWindow::mouseUp (e);

        if (minimised && e.mouseWasClicked())
            setMinimisedToBar (false);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! minimised)
            MultiDocumentPanelWindow::mouseDoubleClick (e);
    }

    std::function<void()> onBarsChanged, beforeMaximise, afterMaximise;

private:
    static constexpr int allButtons = minimiseButton | maximiseButton | closeButton;
    bool minimised = false;
    int barHeightPx = 32;
    juce::Rectangle<int> restoredBounds;
};
}

//==============================================================================
class MainComponent::Workspace final : public juce::MultiDocumentPanel,
                                       public juce::DragAndDropTarget,
                                       public juce::FileDragAndDropTarget
{
public:
    explicit Workspace (MainComponent& ownerIn) : owner (ownerIn)
    {
        setLayoutMode (FloatingWindows);
        setBackgroundColour (juce::Colours::transparentBlack);

        // Drawn like the ones on the file windows' title bars.
        auto& lf = getLookAndFeel();
        minimiseActive.reset (lf.createDocumentWindowButton (juce::DocumentWindow::minimiseButton));
        restoreActive.reset (lf.createDocumentWindowButton (juce::DocumentWindow::maximiseButton));
        closeActive.reset (lf.createDocumentWindowButton (juce::DocumentWindow::closeButton));

        minimiseActive->setTooltip ("Minimize");
        restoreActive->setTooltip ("Restore down (back to windows)");
        closeActive->setTooltip ("Close");

        for (auto* b : { minimiseActive.get(), restoreActive.get(), closeActive.get() })
        {
            b->setWantsKeyboardFocus (false);
            addChildComponent (*b);
        }

        restoreActive->onClick = [this] { restoreWindows(); };

        minimiseActive->onClick = [this]
        {
            auto* doc = getActiveDocument();
            restoreWindows();

            if (auto* frame = frameOf (getWindowFor (doc)))
                frame->setMinimisedToBar (true);
        };

        closeActive->onClick = [this]
        {
            if (auto* doc = getActiveDocument())
                closeDocumentAsync (doc, true, [this] (bool) { updateMaximisedButtons(); });
        };
    }

    void paint (juce::Graphics& g) override
    {
        wf::appearance::paintBackground (g, *this, getLocalBounds());

        // Slightly darker than the data windows so they stand out.
        g.fillAll (juce::Colours::black.withAlpha (0.18f));
    }

    void activeDocumentChanged() override
    {
        updateMaximisedButtons();
        owner.activeDocumentChanged();
    }

    juce::MultiDocumentPanelWindow* createNewDocumentWindow() override
    {
        auto* frame = new DocumentFrame (getBackgroundColour());
        const juce::Component::SafePointer<Workspace> safeThis (this);

        frame->onBarsChanged = [safeThis]
        {
            if (auto* w = safeThis.getComponent())
                w->barsChanged();
        };

        frame->beforeMaximise = [safeThis]
        {
            if (auto* w = safeThis.getComponent())
                w->restoreAllBars();
        };

        frame->afterMaximise = [safeThis]
        {
            if (auto* w = safeThis.getComponent())
                w->updateMaximisedButtons();
        };

        return frame;
    }

    void resized() override
    {
        MultiDocumentPanel::resized();
        layoutBars();
        updateMaximisedButtons();
    }

    bool isMaximised() const   { return getLayoutMode() == MaximisedWindowsWithTabs; }

    // Maximized, the files are tabs and their windows (with _ [] x) are
    // gone, so the same three buttons sit at the top right of the tabs.
    void restoreWindows()
    {
        setLayoutMode (FloatingWindows);
        updateMaximisedButtons();
    }

    void updateMaximisedButtons()
    {
        if (closeActive == nullptr)
            return;   // still being built

        const auto show = isMaximised() && getNumDocuments() > 0;
        const auto* tabs = getCurrentTabbedComponent();
        const auto size = (tabs != nullptr ? tabs->getTabBarDepth() : 30) - 4;
        auto area = getLocalBounds().removeFromTop (size + 4).removeFromRight (3 * size + 4).reduced (2);

        for (auto* b : { minimiseActive.get(), restoreActive.get(), closeActive.get() })
        {
            b->setVisible (show);
            b->setBounds (area.removeFromLeft (size).reduced (1));
            b->toFront (false);
        }
    }

    static DocumentFrame* frameOf (juce::Component* c)   { return dynamic_cast<DocumentFrame*> (c); }

    std::vector<DocumentFrame*> minimisedBars() const
    {
        std::vector<DocumentFrame*> bars;

        for (int i = 0; i < getNumDocuments(); ++i)
            if (auto* frame = frameOf (getWindowFor (getDocument (i))); frame != nullptr && frame->isMinimisedToBar())
                bars.push_back (frame);

        return bars;
    }

    // Minimized windows: side by side along the bottom, more rows upwards
    // when they don't fit in one.
    void layoutBars()
    {
        const auto bars = minimisedBars();
        const auto perRow = juce::jmax (1, getWidth() / DocumentFrame::barWidth);

        for (int i = 0; i < (int) bars.size(); ++i)
        {
            const auto h = bars[(size_t) i]->barHeight();
            bars[(size_t) i]->setBounds ((i % perRow) * DocumentFrame::barWidth, getHeight() - (i / perRow + 1) * h,
                                         DocumentFrame::barWidth, h);
        }
    }

    // What's left for the windows above the bars (Cascade / Tile).
    juce::Rectangle<int> areaAboveBars() const
    {
        auto area = getLocalBounds();
        const auto bars = minimisedBars();

        if (! bars.empty())
        {
            const auto perRow = juce::jmax (1, getWidth() / DocumentFrame::barWidth);
            const auto rows = ((int) bars.size() + perRow - 1) / perRow;
            area.removeFromBottom (rows * bars.front()->barHeight());
        }

        return area;
    }

    void restoreAllBars()
    {
        for (auto* frame : minimisedBars())
            frame->setMinimisedToBar (false);
    }

    void barsChanged()
    {
        layoutBars();

        // A minimized file isn't the one to work on: the top open window is.
        if (auto* frame = frameOf (getWindowFor (getActiveDocument())); frame != nullptr && frame->isMinimisedToBar())
        {
            for (int i = getNumChildComponents(); --i >= 0;)
                if (auto* other = frameOf (getChildComponent (i)); other != nullptr && ! other->isMinimisedToBar())
                {
                    other->toFront (true);
                    break;
                }
        }
    }

    // Right-click on the empty workspace (not on a window).
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && e.eventComponent == this)
            owner.showWorkspaceMenu (e.getPosition());
    }

    // Audio files dragged in from Explorer / Finder / a file manager open
    // where they are dropped, one window per file.
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (const auto& path : files)
            if (owner.formatManager.findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
                return true;

        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override  { setFileDragHighlight (true); }
    void fileDragExit (const juce::StringArray&) override             { setFileDragHighlight (false); }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        setFileDragHighlight (false);
        int opened = 0;

        for (const auto& path : files)
        {
            const juce::File file (path);

            if (file.existsAsFile() && owner.formatManager.findFormatForFileExtension (file.getFileExtension()) != nullptr)
            {
                owner.loadFile (file, juce::Point<int> (x, y) + juce::Point<int> (cascadeStep, cascadeStep) * opened);
                ++opened;
            }
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (fileDragHighlight)
        {
            g.setColour (juce::Colour (0xff3478f6));
            g.drawRect (getLocalBounds(), 4);
        }
    }

    void tryToCloseDocumentAsync (juce::Component* component, std::function<void (bool)> callback) override
    {
        auto* doc = dynamic_cast<DataWindow*> (component);

        owner.askToSaveBeforeClosing (doc, [this, doc = juce::Component::SafePointer<DataWindow> (doc),
                                            callback = std::move (callback)] (bool okToClose)
        {
            if (okToClose)
                owner.documentClosing (doc.getComponent());

            callback (okToClose);
        });
    }

    // A selection dragged out of its window and dropped anywhere else in the
    // workspace (empty space or another window) becomes a new document.
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return details.description == juce::var (WaveformView::selectionDragId);
    }

    void itemDragEnter (const SourceDetails& details) override  { updatePreview (details); }
    void itemDragMove (const SourceDetails& details) override   { updatePreview (details); }
    void itemDragExit (const SourceDetails&) override           { showPreviewOn (nullptr, {}, -1); }

    void itemDropped (const SourceDetails& details) override
    {
        showPreviewOn (nullptr, {}, -1);

        auto* sourceDoc = details.sourceComponent != nullptr
                              ? details.sourceComponent->findParentComponentOfClass<DataWindow>() : nullptr;

        if (sourceDoc == nullptr)
            return;

        auto* targetDoc = documentAt (details.localPosition);

        if (targetDoc == sourceDoc)
            return;   // dropped back onto its own window

        if (targetDoc == nullptr)
        {
            // Empty workspace: the selection becomes a new file.
            owner.addDocument (DataWindow::fromSelection (*sourceDoc, "Untitled " + juce::String (++owner.untitledCount)),
                               details.localPosition);
            return;
        }

        // An empty window (Create New): the audio simply goes in.
        if (targetDoc->isEmpty())
        {
            owner.releasePlaybackSource();
            const auto marks = sourceDoc->selectionMarks();
            targetDoc->mixIn (sourceDoc->copySelection(), sourceDoc->getSampleRate(), 0, 1.0f, 1.0f, &marks);
            setActiveDocument (targetDoc);
            return;
        }

        // Another window: mix into it at the previewed (possibly snapped) spot,
        // after asking for volumes.
        int snappedTo = -1;
        const auto position = planMix (*sourceDoc, *targetDoc, details.localPosition, snappedTo);
        auto audio = std::make_shared<juce::AudioBuffer<float>> (sourceDoc->copySelection());
        const auto sourceRate = sourceDoc->getSampleRate();
        const auto marks = std::make_shared<DataWindow::Marks> (sourceDoc->selectionMarks());

        const auto description = "Mix " + WaveformView::formatTime ((double) audio->getNumSamples() / sourceRate).substring (3)
                               + " from \"" + sourceDoc->getTitle() + "\"\ninto \"" + targetDoc->getTitle() + "\" at "
                               + WaveformView::formatTime ((double) position / targetDoc->getSampleRate()) + ".";

        auto gains = std::make_shared<wf::MixGains>();
        const juce::Component::SafePointer<DataWindow> target (targetDoc);

        wf::MixDialogCallbacks callbacks;
        callbacks.onOk = [this, target, audio, sourceRate, position, gains, marks]
        {
            if (auto* doc = target.getComponent())
            {
                owner.releasePlaybackSource();
                doc->mixIn (*audio, sourceRate, position, gains->source, gains->dest, marks.get());
                setActiveDocument (doc);

                if (auto* window = getWindowFor (doc))
                    window->toFront (true);
            }
        };
        callbacks.onPreview = [this, target, audio, sourceRate, position, gains] (bool play)
        {
            if (play)
                owner.startMixPreview (target.getComponent(), *audio, sourceRate, position, gains);
            else
                owner.stopMixPreview();
        };
        callbacks.isPreviewing = [this] { return owner.isMixPreviewing(); };

        // Open it once the drag has fully finished: opening it from inside the
        // drop handler can leave the drag's mouse capture stuck on the window.
        juce::MessageManager::callAsync ([safeOwner = juce::Component::SafePointer<MainComponent> (&owner),
                                          description, gains, callbacks = std::move (callbacks)]() mutable
        {
            if (auto* main = safeOwner.getComponent())
                main->openMixWindow (description, gains, std::move (callbacks));
        });
    }

    // Mix position in `target` for a drop at `point` (workspace coordinates).
    juce::int64 planMix (DataWindow& source, DataWindow& target, juce::Point<int> point, int& snappedTo) const
    {
        auto& view = target.getView();
        const auto start = view.sampleAt (view.getLocalPoint (this, point));
        return view.snapDrop (start, lengthIn (source, target), snappedTo);
    }

    // Length of the source selection once converted to the target's sample rate.
    static juce::int64 lengthIn (DataWindow& source, DataWindow& target)
    {
        return (juce::int64) std::ceil ((double) source.getView().getSelection().getLength()
                                        * target.getSampleRate() / source.getSampleRate());
    }

    void updatePreview (const SourceDetails& details)
    {
        auto* sourceDoc = details.sourceComponent != nullptr
                              ? details.sourceComponent->findParentComponentOfClass<DataWindow>() : nullptr;
        auto* targetDoc = documentAt (details.localPosition);

        if (sourceDoc == nullptr || targetDoc == nullptr || targetDoc == sourceDoc)
        {
            showPreviewOn (nullptr, {}, -1);
            return;
        }

        int snappedTo = -1;
        const auto start = planMix (*sourceDoc, *targetDoc, details.localPosition, snappedTo);
        showPreviewOn (targetDoc, { start, start + lengthIn (*sourceDoc, *targetDoc) }, snappedTo);
    }

    void showPreviewOn (DataWindow* doc, juce::Range<juce::int64> range, int snappedTo)
    {
        if (auto* previous = previewDoc.getComponent(); previous != nullptr && previous != doc)
            previous->getView().setDropPreview ({}, -1);

        previewDoc = doc;

        if (doc != nullptr)
            doc->getView().setDropPreview (range, snappedTo);
    }

    // Topmost document window under a point in workspace coordinates.
    DataWindow* documentAt (juce::Point<int> position) const
    {
        for (int i = getNumChildComponents(); --i >= 0;)
            if (auto* window = dynamic_cast<juce::MultiDocumentPanelWindow*> (getChildComponent (i)))
                if (window->isVisible() && window->getBounds().contains (position)
                    && ! (frameOf (window) != nullptr && frameOf (window)->isMinimisedToBar()))
                    return dynamic_cast<DataWindow*> (window->getContentComponent());

        return nullptr;
    }

    juce::MultiDocumentPanelWindow* getWindowFor (juce::Component* document) const
    {
        return document != nullptr ? document->findParentComponentOfClass<juce::MultiDocumentPanelWindow>() : nullptr;
    }

private:
    void setFileDragHighlight (bool on)
    {
        fileDragHighlight = on;
        repaint();
    }

    MainComponent& owner;
    std::unique_ptr<juce::Button> minimiseActive, restoreActive, closeActive;
    bool fileDragHighlight = false;
    juce::Component::SafePointer<DataWindow> previewDoc;
};

//==============================================================================
MainComponent::MainComponent()
{
    formatManager.registerBasicFormats();

    commandManager.registerAllCommandsForTarget (this);
    commandManager.setFirstCommandTarget (this);
    loadKeyMappings();
    setApplicationCommandManagerToWatch (&commandManager);

   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (this);
   #else
    menuBar = std::make_unique<juce::MenuBarComponent> (this);
    addAndMakeVisible (*menuBar);
   #endif

    workspace = std::make_unique<Workspace> (*this);
    addAndMakeVisible (*workspace);

    toolbar = std::make_unique<ProcessToolbar> (commandManager);
    toolbar->onQuickVolume = [this] (float decibels)
    {
        if (auto* doc = activeDocument())
        {
            releasePlaybackSource();
            doc->applyVolume (decibels);
        }
    };
    addAndMakeVisible (*toolbar);

    statusBar.setColour (juce::Label::backgroundColourId, panelColour);
    statusBar.setColour (juce::Label::textColourId, juce::Colours::black);
    addAndMakeVisible (statusBar);

    sourcePlayer.setSource (&transport);
    meterTap = std::make_unique<wf::MeterTap> (sourcePlayer);
    deviceManager.addAudioCallback (meterTap.get());

    tempoTimer.tick = [this] { updateTempo(); };

    // Remembered from last time.
    WaveformView::snapToZeroCrossings = (bool) wf::settings::get ("edit.snapZero");

    if (const auto v = wf::settings::get ("view.beatLines"); ! v.isVoid())
        WaveformView::showBeatLines = (bool) v;

    if (const auto v = wf::settings::get ("view.followPlayback"); ! v.isVoid())
        followPlayback = (bool) v;

    smoothScrolling = (bool) wf::settings::get ("view.smoothScrolling");

    if (const auto v = wf::settings::get ("volume.db"); ! v.isVoid())
        lastVolumeDecibels = (float) (double) v;
    toolbar->onTempoClicked = [this] { showTempoMenu(); };
    wf::setDynamicSubmenus ([this] (const juce::String& title)
    {
        return title == "Open Recent" ? recentFilesMenu() : juce::PopupMenu();
    });

    meters = std::make_unique<wf::MeterPanel> (*meterTap);
    addAndMakeVisible (*meters);

    spaceEverywhere = std::make_unique<wf::SpaceEverywhere> (*this, [this] (bool all)
    {
        commandManager.invokeDirectly (all ? wf::optPlayAll : wf::optPlay, false);
    });

    setWantsKeyboardFocus (true);
    setSize (1100, 700);
    updateStatus();
    scheduleTempo();

    updateButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2e7d32));
    updateButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    updateButton.setWantsKeyboardFocus (false);
    updateButton.onClick = [this] { showUpdateMessage(); };
    addChildComponent (updateButton);

    linkBox.setTextToShowWhenEmpty ("Paste a link (YouTube, TikTok, Instagram, Facebook...)", juce::Colours::grey);
    linkBox.setTooltip ("The sound of the page is saved as a WAV in Music/Adormis Audio Downloads and opened here.");
    linkBox.setSelectAllWhenFocused (true);
    linkBox.setIndents (6, 3);
    linkBox.onReturnKey = [this] { downloadLink(); };
    linkBox.onEscapeKey = [this] { linkBox.clear(); grabKeyboardFocus(); };
    addAndMakeVisible (linkBox);
    linkButton.setWantsKeyboardFocus (false);
    linkButton.onClick = [this] { downloadLink(); };
    addAndMakeVisible (linkButton);

    // A few seconds after the start (not in the --list-commands self-test).
    juce::Timer::callAfterDelay (3000, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (auto* main = safeThis.getComponent(); main != nullptr && main->isShowing())
            main->checkForUpdates (false);
    });

    // The audio device opens once the window is on screen.
    juce::Timer::callAfterDelay (150, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (auto* main = safeThis.getComponent())
            main->startAudio();
    });
}

juce::File MainComponent::asioFlagFile()    { return wf::startup::settingsFolder().getChildFile ("use-asio"); }
juce::File MainComponent::audioStateFile()  { return wf::startup::settingsFolder().getChildFile ("audio-device.xml"); }

void MainComponent::startAudio()
{
    // If the last start never got past this point (a driver hung or crashed),
    // start this time without ASIO and without the saved device.
    const auto safeMode = wf::startup::lastStartFailedAtAudio();

    if (safeMode)
        asioFlagFile().deleteFile();

    deviceManager.allowAsio = asioFlagFile().existsAsFile();
    wf::startup::log ("audio: opening the device (ASIO " + juce::String (deviceManager.allowAsio ? "on" : "off")
                      + (safeMode ? ", safe start after a failed one)" : ")"));
    wf::startup::markAudioStarting();

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto saved = safeMode ? nullptr : juce::parseXML (audioStateFile());
    const auto error = deviceManager.initialise (0, 2, saved.get(), true);

    wf::startup::markAudioStarted();
    auto* device = deviceManager.getCurrentAudioDevice();
    wf::startup::log ("audio: " + (device != nullptr ? device->getTypeName() + " / " + device->getName()
                                                         + " at " + juce::String (device->getCurrentSampleRate(), 0) + " Hz"
                                                     : juce::String ("no device"))
                      + (error.isNotEmpty() ? " - error: " + error : juce::String())
                      + " (" + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started)) + " ms)");

    // Remember the device chosen in Options > Audio Device for the next start.
    audioSaver.onChange = [this]
    {
        if (auto xml = deviceManager.createStateXml())
            xml->writeTo (audioStateFile());
    };
    deviceManager.addChangeListener (&audioSaver);

    if (safeMode)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Audio device",
            "Last time Adormis Audio stopped while opening the audio device (often an ASIO driver).\n"
            "This time it uses the system's default audio output and ASIO is off.\n\n"
            "Options > Audio Device lets you choose the device, or turn ASIO on again.");

    if (device == nullptr)
        statusBar.setText ("  No audio output could be opened: Options > Audio Device to choose one.", juce::dontSendNotification);
}

MainComponent::~MainComponent()
{
    spaceEverywhere.reset();
    wf::setDynamicSubmenus (nullptr);
    wf::settings::flush();
    tempoTimer.stopTimer();
    tempoPool.removeAllJobs (true, 10000);
    commandManager.getKeyMappings()->removeChangeListener (&keySaver);
    keyboardWindow.reset();
    mixWindow.reset();
    saveAsWindow.reset();
    volumeWindow.reset();
    templateWindow.reset();
    closeRecorder();
    stretchWindow.reset();
    bassFinderWindow.reset();
    deEsserWindow.reset();
    drumRebuilderWindow.reset();   // takes its player off the audio device
    effectWindow.reset();
    job.reset();
    stopToolPreview();
    stopTimer();
    transport.stop();
    transport.setSource (nullptr);
    statisticsWindow.reset();
    regionsWindow.reset();
    batchWindow.reset();
    meters.reset();
    deviceManager.removeChangeListener (&audioSaver);
    deviceManager.removeAudioCallback (meterTap.get());
    sourcePlayer.setSource (nullptr);
    workspace->closeAllDocumentsAsync (false, nullptr);

   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (panelColour);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    if (menuBar != nullptr)
        menuBar->setBounds (area.removeFromTop (menuBarHeight));

    statusBar.setBounds (area.removeFromBottom (statusBarHeight));

    // Top right, on the menu bar (on macOS, where the menu is at the top of
    // the screen, on the right of the status bar).
    const auto updateWidth = updateButton.getBestWidthForHeight (menuBarHeight) + 24;
    updateButton.setBounds (menuBar != nullptr ? getLocalBounds().removeFromTop (menuBarHeight).removeFromRight (updateWidth).reduced (1)
                                               : statusBar.getBounds().removeFromRight (updateWidth).reduced (1));
    updateButton.toFront (false);

    // Right after the last menu (Help); on macOS, at the left of the status bar.
    {
        auto row = statusBar.getBounds();

        if (menuBar != nullptr)
        {
            row = getLocalBounds().removeFromTop (menuBarHeight);
            const auto names = getMenuBarNames();
            int menusWidth = 0;

            for (int i = 0; i < names.size(); ++i)
                menusWidth += getLookAndFeel().getMenuBarItemWidth (*menuBar, i, names[i]);

            row.removeFromLeft (menusWidth + 12);
            row.removeFromRight (updateButton.isVisible() ? updateButton.getWidth() + 8 : 4);
        }

        row = row.withWidth (juce::jmin (row.getWidth(), 460)).reduced (0, 2);
        linkButton.setBounds (row.removeFromRight (linkButton.getBestWidthForHeight (row.getHeight()) + 16));
        row.removeFromRight (4);
        linkBox.setBounds (row);
        linkBox.toFront (false);
        linkButton.toFront (false);
    }
    toolbar->setBounds (area.removeFromBottom (ProcessToolbar::preferredHeight));

    if (meters != nullptr)
    {
        meters->setVisible (meters->showsAnything());

        if (meters->showsAnything())
            meters->setBounds (area.removeFromRight (wf::MeterPanel::preferredWidth));
    }

    workspace->setBounds (area);
}

//==============================================================================
juce::String MainComponent::commandReport (bool selectPart)
{
    auto* doc = activeDocument();

    if (doc != nullptr && selectPart)
    {
        const auto length = (juce::int64) doc->getBuffer().getNumSamples();
        doc->getView().setSelection ({ length / 4, length / 2 });
    }

    juce::String report;
    report << "Open documents: " << (workspace != nullptr ? workspace->getNumDocuments() : 0)
           << ", active: " << (doc != nullptr ? doc->getTitle() : juce::String ("(none)"))
           << ", selection: " << (doc != nullptr && ! doc->getView().getSelection().isEmpty() ? "yes" : "no") << "\n";

    int usable = 0, total = 0;

    for (const auto& def : wf::allCommands())
    {
        juce::ApplicationCommandInfo info (def.id);
        getCommandInfo (def.id, info);
        const auto on = (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
        usable += on ? 1 : 0;
        ++total;
        report << (on ? "+ " : "- ") << def.category << " > " << def.name << (def.implemented ? "" : "   (not written yet)") << "\n";
    }

    report << usable << " of " << total << " usable\n";
    return report;
}

DataWindow* MainComponent::activeDocument() const
{
    // Commands are registered (and queried) before the workspace exists.
    return workspace != nullptr ? dynamic_cast<DataWindow*> (workspace->getActiveDocument()) : nullptr;
}

bool MainComponent::loadFile (const juce::File& file, std::optional<juce::Point<int>> position)
{
    juce::String error;
    auto doc = DataWindow::load (file, formatManager, error);

    if (doc == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open", error);
        return false;
    }

    addDocument (std::move (doc), position);
    addRecentFile (file);
    return true;
}

void MainComponent::addDocument (std::unique_ptr<DataWindow> doc, std::optional<juce::Point<int>> position)
{
    if (doc == nullptr)
        return;

    auto* raw = doc.get();
    raw->getView().onCursorClicked = [this, raw] (juce::int64 sample) { jumpPlayback (raw, sample); };
    raw->getView().onContextMenu = [this, raw] { showWaveMenu (raw); };
    raw->onGetStems = [this, raw] { getStems (raw); };

    // A tab switch while playing goes on at the same place in the new part.
    raw->beforePageChange = [this, raw]
    {
        resumeAfterPageChange.reset();

        if (isPlaying() && playingDocument.getComponent() == raw)
            resumeAfterPageChange = std::make_pair (playPosition(), playSoloChannel);

        releasePlaybackSource();
    };
    raw->afterPageChange = [this, raw]
    {
        if (! resumeAfterPageChange.has_value() || raw != activeDocument())
            return;

        const auto [at, solo] = *resumeAfterPageChange;
        resumeAfterPageChange.reset();
        const auto length = (juce::int64) raw->getBuffer().getNumSamples();

        if (at < length)
            startPlayback ({ at, length }, solo);
    };

    raw->onRegionAction = [this, raw] (int index, bool menu)
    {
        if (menu) regionMenu (raw, index);
        else      renameRegion (raw, index);
    };

    raw->onMarkerAction = [this, raw] (int index) { markerMenu (raw, index); };

    raw->onChanged = [this, raw]
    {
        followSelectionLoop (raw);

        if (raw == activeDocument())
            refreshRegionsList();

        if (raw == activeDocument())
        {
            scheduleTempo();
            updateStatus();
            updateTitle();
            commandManager.commandStatusChanged();
        }
    };

    // New windows open cascaded, about two thirds of the workspace in size.
    const auto index = workspace->getNumDocuments();
    raw->setSize (juce::jmax (420, workspace->getWidth() * 2 / 3), juce::jmax (220, workspace->getHeight() / 2));

    if (! workspace->addDocument (doc.release(), panelColour, true))
        return;

    if (auto* window = workspace->getWindowFor (raw))
    {
        const auto cascaded = juce::Point<int> (8 + (index % 8) * cascadeStep, 8 + (index % 8) * cascadeStep);
        auto topLeft = position.value_or (cascaded);

        // Keep the whole window inside the workspace when it fits.
        topLeft.x = juce::jlimit (0, juce::jmax (0, workspace->getWidth() - window->getWidth()), topLeft.x);
        topLeft.y = juce::jlimit (0, juce::jmax (0, workspace->getHeight() - window->getHeight()), topLeft.y);
        window->setTopLeftPosition (topLeft);
    }

    activeDocumentChanged();
}

void MainComponent::activeDocumentChanged()
{
    scheduleTempo();
    refreshRegionsList();
    updateTitle();
    updateStatus();
    commandManager.commandStatusChanged();
}

void MainComponent::documentClosing (DataWindow* doc)
{
    tempoCache.erase (std::remove_if (tempoCache.begin(), tempoCache.end(), [doc] (auto& e) { return e.first.doc == doc; }),
                      tempoCache.end());
    shownTempo = {};
    scheduleTempo();

    if (doc != nullptr && doc == recordDocument.getComponent())
        closeRecorder();

    if (doc != nullptr && doc == playingDocument.getComponent())
        releasePlaybackSource();
}

void MainComponent::scheduleTempo()
{
    refreshTempoDisplay();          // the selection's tempo (from its length) at once
    tempoTimer.startTimer (300);    // the detection once the selection settles
}

MainComponent::TempoKey MainComponent::currentTempoKey() const
{
    TempoKey key;
    auto* doc = activeDocument();

    if (doc == nullptr || doc->isEmpty())
        return key;

    const auto selection = doc->getView().getSelection();
    const auto whole = selection.isEmpty();
    key.doc = doc;
    key.range = whole ? juce::Range<juce::int64> (0, doc->getBuffer().getNumSamples()) : selection;
    key.channel = whole ? -1 : doc->getView().getSelectedChannel();
    key.edits = doc->getEditCount();
    return key;
}

std::optional<double> MainComponent::cachedTempo (const TempoKey& key) const
{
    for (const auto& [cached, bpm] : tempoCache)
        if (cached == key)
            return bpm;

    return {};
}

void MainComponent::refreshTempoDisplay()
{
    auto* doc = activeDocument();
    const auto key = currentTempoKey();

    if (key.doc == nullptr)
    {
        toolbar->setTempo ("-", doc == nullptr ? "no file" : "empty file", "Open a file to see its tempo");
        return;
    }

    const auto rate = doc->getSampleRate();
    const auto whole = doc->getView().getSelection().isEmpty();
    const auto factor = tempoMode == 1 ? 2.0 : tempoMode == 2 ? 0.5 : 1.0;

    // What the detection says (or will say) about this audio.
    juce::String detected;

    if (whole && doc->getKnownTempo() > 0.0)
        detected = juce::String (doc->getKnownTempo(), 1);
    else if ((double) key.range.getLength() < rate * 2.0)
        detected = "too short";
    else if (const auto bpm = cachedTempo (key))
        detected = *bpm > 0.0 ? juce::String (*bpm * factor, 1) : juce::String ("no beat");
    else
        detected = "...";

    const auto factorText = tempoMode == 1 ? juce::String (" x2") : tempoMode == 2 ? juce::String (" x1/2") : juce::String();
    const auto channelText = key.channel == 0 ? juce::String (", left") : key.channel == 1 ? juce::String (", right") : juce::String();

    if (whole)
    {
        toolbar->setTempo (detected == "..." || detected == "no beat" || detected == "too short" ? detected : detected + " BPM",
                           "whole file, detected" + factorText,
                           "Tempo of the whole file, detected from the beat.\n"
                           "Select a part to get its tempo from its length.\n"
                           "Click to choose how many beats a selection is, or double / half time.");
        return;
    }

    // Sound Forge's Edit Tempo: the selection is `selectionBeats` beats long.
    const auto seconds = (double) key.range.getLength() / rate;
    const auto bpm = selectionBeats * 60.0 / seconds;
    const auto beatsText = selectionBeats == 4 ? juce::String ("1 bar") : juce::String (selectionBeats) + " beats";

    toolbar->setTempo (juce::String (bpm, 2) + " BPM",
                       "selection = " + beatsText + channelText + "  |  audio " + detected + factorText,
                       "The selection taken as " + beatsText + " (" + juce::String (selectionBeats) + " beats): "
                       + juce::String (seconds, 3) + " s -> " + juce::String (bpm, 3) + " BPM.\n"
                       "Drag its edges to fit a bar exactly; \"audio\" is the tempo detected in the sound.\n"
                       "Click to choose how many beats the selection is.");
}

void MainComponent::updateTempo()
{
    tempoTimer.stopTimer();
    auto* doc = activeDocument();
    const auto key = currentTempoKey();

    if (key.doc == nullptr || key == shownTempo)
        return;

    shownTempo = key;
    const auto rate = doc->getSampleRate();

    if ((doc->getView().getSelection().isEmpty() && doc->getKnownTempo() > 0.0)
        || (double) key.range.getLength() < rate * 2.0 || cachedTempo (key).has_value())
    {
        refreshTempoDisplay();
        return;
    }

    // At most a minute from the middle: enough for the beat, quick, and the
    // copy stays small however long the file is.
    const auto length = (int) juce::jmin (key.range.getLength(), (juce::int64) (rate * 60.0));
    const auto start = (int) (key.range.getStart() + (key.range.getLength() - length) / 2);
    const auto& buffer = doc->getBuffer();
    const auto channels = key.channel >= 0 ? 1 : buffer.getNumChannels();
    auto excerpt = std::make_shared<juce::AudioBuffer<float>> (channels, length);

    for (int ch = 0; ch < channels; ++ch)
        excerpt->copyFrom (ch, 0, buffer, key.channel >= 0 ? key.channel : ch, start, length);

    tempoPool.addJob ([excerpt, rate, key, safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        const auto bpm = wf::measureTempo (*excerpt, { 0, excerpt->getNumSamples() }, rate).value_or (0.0);

        juce::MessageManager::callAsync ([safeThis, key, bpm]
        {
            auto* main = safeThis.getComponent();

            if (main == nullptr)
                return;

            main->tempoCache.push_back ({ key, bpm });

            if (main->tempoCache.size() > 32)
                main->tempoCache.erase (main->tempoCache.begin());

            if (main->currentTempoKey() == key)
                main->refreshTempoDisplay();
        });
    });
}

void MainComponent::showTempoMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("A selection is");

    for (int beats : { 1, 2, 3, 4, 6, 8, 12, 16, 32 })
    {
        const auto name = beats == 4 ? juce::String ("4 beats (1 bar)")
                        : beats % 4 == 0 ? juce::String (beats) + " beats (" + juce::String (beats / 4) + " bars)"
                                         : juce::String (beats) + (beats == 1 ? " beat" : " beats");
        menu.addItem (beats, name, true, beats == selectionBeats);
    }

    menu.addSectionHeader ("Detected tempo");
    menu.addItem (101, "As detected", true, tempoMode == 0);
    menu.addItem (102, "Double time (x2)", true, tempoMode == 1);
    menu.addItem (103, "Half time (x1/2)", true, tempoMode == 2);
    menu.addSeparator();
    menu.addItem (201, "Beat lines in the selection", true, WaveformView::showBeatLines);

    const juce::Component::SafePointer<MainComponent> safeThis (this);
    // Inside the main window: Hyprland moves focus onto separate menu windows
    // and JUCE then closes them as soon as the mouse slips off.
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (toolbar->getTempoDisplay())
                                                  .withParentComponent (getTopLevelComponent()),
                        [safeThis] (int result)
                        {
                            auto* main = safeThis.getComponent();

                            if (main == nullptr || result == 0)
                                return;

                            if (result == 201)
                            {
                                WaveformView::showBeatLines = ! WaveformView::showBeatLines;
                                wf::settings::set ("view.beatLines", WaveformView::showBeatLines);
                            }
                            else if (result > 100)
                                main->tempoMode = result - 101;
                            else
                            {
                                main->selectionBeats = result;
                                WaveformView::selectionBeats = result;
                            }

                            for (int i = 0; i < main->workspace->getNumDocuments(); ++i)
                                main->workspace->getDocument (i)->repaint();   // the beat lines

                            main->refreshTempoDisplay();
                        });
}

void MainComponent::openFileDialog()
{
    auto start = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    if (auto* doc = activeDocument(); doc != nullptr && doc->getFile().existsAsFile())
        start = doc->getFile().getParentDirectory();

    // The Open window plays the picked file (read ahead on a thread, so a
    // long MP3 doesn't stutter).
    wf::FilePreviewHost host;
    host.play = [this] (const juce::File& file, double from)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

        if (reader == nullptr)
            return;

        const auto rate = reader->sampleRate;
        const auto channels = (int) reader->numChannels;
        auto source = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
        source->setNextReadPosition ((juce::int64) (from * rate));

        releasePlaybackSource();

        if (! previewReadThread.isThreadRunning())
            previewReadThread.startThread();

        toolPreview = std::move (source);
        transport.setSource (toolPreview.get(), 65536, &previewReadThread, rate, juce::jmax (2, channels));
        transport.setPosition (from);
        transport.start();
    };
    host.stop = [this] { stopToolPreview(); };
    host.position = [this] { return toolPreview != nullptr && transport.isPlaying() ? transport.getCurrentPosition() : -1.0; };

    filePicker.launch ("Open", start, formatManager.getWildcardForAllFormats(),
                       juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                           | juce::FileBrowserComponent::canSelectMultipleItems,
                       [this] (const juce::Array<juce::File>& files)
                       {
                           stopToolPreview();

                           for (const auto& file : files)
                               if (file.existsAsFile())
                                   loadFile (file);
                       },
                       std::make_unique<wf::AudioFilePreview> (formatManager, std::move (host)));
}

void MainComponent::closeActiveDocument()
{
    if (auto* doc = activeDocument())
        workspace->closeDocumentAsync (doc, true, [this] (bool) { activeDocumentChanged(); });
}

void MainComponent::saveDocument (DataWindow* doc, bool forceSaveAs, std::function<void (bool)> done)
{
    if (doc == nullptr)
    {
        if (done) done (false);
        return;
    }

    auto finish = [this, done] (bool saved, const juce::String& error)
    {
        if (! saved && error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save", error);

        updateTitle();
        updateStatus();
        commandManager.commandStatusChanged();

        if (done) done (saved);
    };

    if (! forceSaveAs && doc->canSaveInPlace())
    {
        juce::String error;
        finish (doc->saveTo (doc->getFile(), formatManager, error), error);
        return;
    }

    // Save As: first the format/quality window, then the location.
    const auto suggested = DataWindow::isSaveExtension (doc->getFile().getFileExtension())
                               ? doc->getFile().getFileExtension() : juce::String (".wav");

    auto onChosen = [this, doc = juce::Component::SafePointer<DataWindow> (doc), finish] (wf::SaveFormat format)
    {
        if (doc == nullptr)
        {
            finish (false, {});
            return;
        }

        auto folder = doc->getSaveFolder();
        auto name = juce::File (juce::File::createLegalFileName (doc->getTitle())).getFileNameWithoutExtension()
                  + format.extension;

        filePicker.launch ("Save As", folder.getChildFile (name), "*" + format.extension,
                           juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                               | juce::FileBrowserComponent::warnAboutOverwriting,
                           [this, doc, finish, format] (const juce::Array<juce::File>& files)
                           {
                               auto file = files.isEmpty() ? juce::File() : files.getFirst();

                               if (file == juce::File() || doc == nullptr)
                               {
                                   finish (false, {});
                                   return;
                               }

                               // The chosen format decides the extension.
                               if (! file.hasFileExtension (format.extension))
                                   file = file.withFileExtension (format.extension);

                               if (doc.getComponent() == playingDocument.getComponent())
                                   stopPlayback (false);

                               juce::String error;
                               const auto saved = doc->saveTo (file, formatManager, error, &format);

                               if (saved)
                                   addRecentFile (file);

                               finish (saved, error);
                           });
    };

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->saveAsWindow.reset();
        });
    };

    saveAsWindow = wf::createSaveAsWindow (doc->getTitle(), suggested, doc->getSampleRate(), std::move (onChosen), std::move (onClose));
    saveAsWindow->setVisible (true);
    saveAsWindow->toFront (true);
}

void MainComponent::askToSaveBeforeClosing (DataWindow* doc, std::function<void (bool)> done)
{
    if (doc == nullptr || doc->unsavedPages().isEmpty())
    {
        done (true);
        return;
    }

    // With stem tabs, Save would only save the tab shown: say which tabs go.
    if (doc->hasStemPages())
    {
        const auto unsaved = doc->unsavedPages();
        auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                .withTitle ("Adormis Audio")
                                                .withMessage ("These tabs of \"" + doc->getTitle() + "\" are not saved: "
                                                              + unsaved.joinIntoString (", ") + ".\n\nClose and lose them? "
                                                              "(Cancel, then Save on each tab to keep them.)")
                                                .withButton ("Close")
                                                .withButton ("Cancel");

        juce::AlertWindow::showAsync (options, [done] (int result) { done (result == 1); });
        return;
    }

    auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                            .withTitle ("Adormis Audio")
                                            .withMessage ("Save changes to \"" + doc->getTitle() + "\" before closing?")
                                            .withButton ("Save")
                                            .withButton ("Don't Save")
                                            .withButton ("Cancel");

    juce::AlertWindow::showAsync (options, [this, doc = juce::Component::SafePointer<DataWindow> (doc), done] (int result)
    {
        if (result == 1)        saveDocument (doc.getComponent(), false, done);   // closes only if saved
        else if (result == 2)   done (true);
        else                    done (false);
    });
}

void MainComponent::requestQuit (std::function<void()> quit)
{
    int unsaved = 0;

    for (int i = 0; i < workspace->getNumDocuments(); ++i)
        if (auto* doc = dynamic_cast<DataWindow*> (workspace->getDocument (i)); doc != nullptr && ! doc->unsavedPages().isEmpty())
            ++unsaved;

    if (unsaved == 0)
    {
        quit();
        return;
    }

    auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                            .withTitle ("Adormis Audio")
                                            .withMessage (juce::String (unsaved) + (unsaved == 1 ? " file has" : " files have")
                                                          + " unsaved changes. Quit and lose them?")
                                            .withButton ("Quit")
                                            .withButton ("Cancel");

    juce::AlertWindow::showAsync (options, [quit] (int result) { if (result == 1) quit(); });
}

void MainComponent::process (wf::Process process)
{
    if (auto* doc = activeDocument())
    {
        releasePlaybackSource();
        doc->applyProcess (process);
        rememberAction ("process", [process] (DataWindow& d) { d.applyProcess (process); });
    }
}

void MainComponent::rememberAction (const juce::String& name, std::function<void (DataWindow&)> action)
{
    lastActionName = name;
    lastAction = std::move (action);
    commandManager.commandStatusChanged();
}

void MainComponent::showNewFileDialog()
{
    auto onCreate = [this] (wf::TemplateSettings settings)
    {
        lastTemplate = settings;
        const auto bpm = juce::String (settings.bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
        const auto title = "Untitled " + juce::String (++untitledCount) + " (" + bpm + " BPM, 1/"
                         + juce::String (settings.division) + ")";

        auto doc = DataWindow::createSilent (title, settings.sampleRate, settings.channels,
                                               settings.lengthInSamples(), settings.markerPositions(),
                                               settings.barStartPositions(), settings.bits);
        doc->setKnownTempo (settings.bpm);
        addDocument (std::move (doc));
    };

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->templateWindow.reset();
        });
    };

    templateWindow = wf::createTemplateWindow (lastTemplate, std::move (onCreate), std::move (onClose));
    templateWindow->setVisible (true);
    templateWindow->toFront (true);
}

void MainComponent::showVolumeDialog()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    auto onApply = [this, safeDoc = juce::Component::SafePointer<DataWindow> (doc)] (float decibels)
    {
        lastVolumeDecibels = decibels;
        wf::settings::set ("volume.db", decibels);

        if (auto* target = safeDoc.getComponent())
        {
            releasePlaybackSource();
            target->applyVolume (decibels);
            rememberAction ("Volume", [decibels] (DataWindow& d) { d.applyVolume (decibels); });
        }
    };

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->volumeWindow.reset();
        });
    };

    // Mute: Process > Mute on the same file (named so in the undo history).
    auto onMute = [this, safeDoc = juce::Component::SafePointer<DataWindow> (doc)]
    {
        if (safeDoc == nullptr)
            return;
        if (safeDoc.getComponent() == activeDocument())
            commandManager.invokeDirectly (wf::procMute, false);
        else
        {
            releasePlaybackSource();
            safeDoc->applyProcess (wf::Process::mute);
        }
    };

    volumeWindow = wf::createVolumeWindow (doc->getTitle() + " (" + doc->describeProcessTarget() + ")",
                                           lastVolumeDecibels, std::move (onApply), std::move (onMute), std::move (onClose));
    volumeWindow->setVisible (true);
    volumeWindow->toFront (true);
}

void MainComponent::showForm (const juce::String& title, const juce::String& info, std::vector<wf::FormField> fields,
                              const juce::String& hint, std::function<void (DataWindow&, const juce::StringArray&)> onOk)
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    std::function<void (const juce::StringArray&)> apply = [this, safeDoc = juce::Component::SafePointer<DataWindow> (doc), onOk = std::move (onOk)] (const juce::StringArray& values)
    {
        if (auto* target = safeDoc.getComponent())
        {
            releasePlaybackSource();
            onOk (*target, values);
            updateStatus();
            updateTitle();
            commandManager.commandStatusChanged();
        }
    };

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->formWindow.reset();
        });
    };

    // These windows start from the values used last time (the others start
    // from the file: its rate, its selection, the cursor).
    static const juce::StringArray remembered { "Insert Silence", "Bit-Depth Converter", "Channel Converter", "Auto Trim/Crop",
                                                "Detect Clipping", "Normalize", "Pitch Shift", "Auto Region" };
    const auto key = "form." + title;

    if (remembered.contains (title))
    {
        const auto stored = wf::settings::get (key);   // alive while `saved` points into it

        if (const auto* saved = stored.getArray(); saved != nullptr && saved->size() == (int) fields.size())
        {
            for (size_t i = 0; i < fields.size(); ++i)
            {
                const auto value = (*saved)[(int) i].toString();
                auto& f = fields[i];

                if (f.choices.isEmpty() ? (f.isValid == nullptr || f.isValid (value)) : f.choices.contains (value))
                    f.value = value;
            }
        }

        apply = [inner = std::move (apply), key] (const juce::StringArray& values)
        {
            juce::Array<juce::var> list;

            for (const auto& v : values)
                list.add (v);

            wf::settings::set (key, list);
            inner (values);
        };
    }

    // what the form does is named after it in the undo history
    apply = [this, inner = std::move (apply), title] (const juce::StringArray& values)
    {
        const juce::Component::SafePointer<DataWindow> d (activeDocument());

        if (auto* w = d.getComponent())
            w->setUndoName (title);

        inner (values);

        if (auto* w = d.getComponent())
            w->setUndoName ({});
    };

    formWindow = wf::createFormWindow (title, info, std::move (fields), hint, std::move (apply), std::move (onClose));
    formWindow->setVisible (true);
    formWindow->toFront (true);
}

namespace
{
bool isSeconds (const juce::String& t)
{
    const auto v = wf::parseTime (t);
    return v.has_value() && *v > 0.0 && *v <= 3600.0;
}

bool isTime (const juce::String& t)   { return wf::parseTime (t).has_value(); }

juce::String secondsText (double seconds)
{
    return WaveformView::formatTime (seconds);
}
} // namespace

void MainComponent::showInsertSilence()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    showForm ("Insert Silence", "Insert silence into: " + doc->getTitle(),
              { { "Length:", "1", {}, "seconds", isSeconds },
                { "Where:", "At the cursor", { "At the cursor", "At the start", "At the end" }, {}, {} } },
              "Length in seconds (1.5) or as minutes:seconds (1:30).",
              [] (DataWindow& d, const juce::StringArray& v)
              {
                  const auto where = v[1] == "At the start" ? (juce::int64) 0
                                   : v[1] == "At the end"   ? (juce::int64) d.getBuffer().getNumSamples()
                                                            : d.getView().getCursor();
                  d.insertSilence (where, *wf::parseTime (v[0]));
              });
}

void MainComponent::showResample()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    const juce::StringArray rates { "8000", "11025", "16000", "22050", "32000", "44100", "48000", "88200", "96000", "176400", "192000" };
    const auto now = juce::String (juce::roundToInt (doc->getSampleRate()));

    showForm ("Resample", doc->getTitle() + " is now " + now + " Hz",
              { { "New sample rate:", now == "44100" ? "48000" : "44100", rates, {}, {} } },
              "Converts the whole file (best-quality sinc). The pitch and length stay the same.",
              [this] (DataWindow& d, const juce::StringArray& v)
              {
                  const auto rate = v[0].getDoubleValue();
                  d.resample (rate);
                  rememberAction ("Resample", [rate] (DataWindow& x) { x.resample (rate); });
              });
}

void MainComponent::showBitDepth()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    const auto now = doc->isFloatingPoint() ? juce::String ("32-bit float") : juce::String (doc->getBitDepth()) + "-bit";

    showForm ("Bit-Depth Converter", doc->getTitle() + " is now " + now,
              { { "New bit depth:", "16-bit", { "8-bit", "16-bit", "24-bit", "32-bit float" }, {}, {} },
                { "Dither:", "Yes (TPDF)", { "Yes (TPDF)", "No" }, {}, {} } },
              "Dither adds a very faint noise that hides the rounding\n(use it when going down to 16 or 8 bits).",
              [this] (DataWindow& d, const juce::StringArray& v)
              {
                  const auto bits = v[0].getIntValue();
                  const auto dither = v[1].startsWith ("Yes");
                  d.convertBitDepth (bits, dither);
                  rememberAction ("Bit-Depth Converter", [bits, dither] (DataWindow& x) { x.convertBitDepth (bits, dither); });
              });
}

void MainComponent::showChannelConverter()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    using CC = DataWindow::ChannelConversion;
    const auto stereo = doc->getBuffer().getNumChannels() == 2;
    const juce::StringArray choices = stereo
        ? juce::StringArray { "Stereo to mono (mix L + R)", "Left channel only (mono)", "Right channel only (mono)", "Swap left and right" }
        : juce::StringArray { "Mono to stereo (same sound in both)" };

    showForm ("Channel Converter", doc->getTitle() + " is now " + (stereo ? "stereo" : "mono"),
              { { "Convert:", choices[0], choices, {}, {} } }, {},
              [this] (DataWindow& d, const juce::StringArray& v)
              {
                  const auto c = v[0].startsWith ("Stereo") ? CC::stereoToMono : v[0].startsWith ("Left") ? CC::leftOnly
                               : v[0].startsWith ("Right") ? CC::rightOnly : v[0].startsWith ("Swap") ? CC::swap : CC::monoToStereo;
                  d.convertChannels (c);
              });
}

void MainComponent::showAutoTrim()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    showForm ("Auto Trim/Crop", "Remove the quiet start and end of: " + doc->getTitle(),
              { { "Quieter than:", "-60", {}, "dB", [] (const juce::String& t)
                  { const auto v = wf::parseNumber (t); return v.has_value() && *v <= 0.0 && *v >= -120.0; } } },
              "Everything before the first and after the last sound louder\nthan this is cut off (undo with Ctrl+Z).",
              [this] (DataWindow& d, const juce::StringArray& v)
              {
                  if (! d.autoTrim ((float) *wf::parseNumber (v[0])))
                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Auto Trim/Crop",
                                                              "Nothing to trim: the file starts and ends above " + v[0] + " dB\n(or is quieter than that everywhere).");
              });
}

void MainComponent::showSetSelection()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    const auto rate = doc->getSampleRate();
    const auto sel = doc->getView().getSelection();
    const auto start = sel.isEmpty() ? doc->getView().getCursor() : sel.getStart();
    const auto end = sel.isEmpty() ? (juce::int64) doc->getBuffer().getNumSamples() : sel.getEnd();

    showForm ("Set Selection", "Select a stretch of: " + doc->getTitle()
                                   + "  (" + secondsText ((double) doc->getBuffer().getNumSamples() / rate) + " long)",
              { { "Start:", secondsText ((double) start / rate), {}, {}, isTime },
                { "End:", secondsText ((double) end / rate), {}, {}, isTime } },
              "Times as hh:mm:ss.ms, m:ss or seconds (12.5).",
              [] (DataWindow& d, const juce::StringArray& v)
              {
                  const auto r = d.getSampleRate();
                  const auto total = (juce::int64) d.getBuffer().getNumSamples();
                  auto a = juce::jlimit ((juce::int64) 0, total, (juce::int64) std::llround (*wf::parseTime (v[0]) * r));
                  auto b = juce::jlimit ((juce::int64) 0, total, (juce::int64) std::llround (*wf::parseTime (v[1]) * r));

                  if (b < a)
                      std::swap (a, b);

                  d.getView().setSelection ({ a, b });
                  d.getView().zoomToSelection();
              });
}

void MainComponent::showGoTo()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    showForm ("Go To", "Move the cursor in: " + doc->getTitle(),
              { { "Position:", secondsText ((double) doc->getView().getCursor() / doc->getSampleRate()), {}, {}, isTime } },
              "Time as hh:mm:ss.ms, m:ss or seconds (12.5).",
              [] (DataWindow& d, const juce::StringArray& v)
              {
                  const auto total = (juce::int64) d.getBuffer().getNumSamples();
                  const auto at = juce::jlimit ((juce::int64) 0, total, (juce::int64) std::llround (*wf::parseTime (v[0]) * d.getSampleRate()));
                  d.getView().setSelection ({});
                  d.getView().setCursor (at);
                  d.getView().ensureVisible (at);
              });
}

void MainComponent::showDetectClipping()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    showForm ("Detect Clipping", "Find the distorted (clipped) places in: " + doc->getTitle(),
              { { "Level at or above:", "-0.1", {}, "dB", [] (const juce::String& t)
                  { const auto v = wf::parseNumber (t); return v.has_value() && *v <= 0.0 && *v >= -20.0; } },
                { "For at least:", "3", {}, "samples in a row", [] (const juce::String& t)
                  { const auto v = wf::parseNumber (t); return v.has_value() && *v >= 1.0 && *v <= 1000.0; } } },
              "Each clipped place gets a marker (M markers; double-click\nbetween two to select).",
              [] (DataWindow& d, const juce::StringArray& v)
              {
                  const auto found = d.markClipping ((float) *wf::parseNumber (v[0]), (int) *wf::parseNumber (v[1]));
                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Detect Clipping",
                                                          found == 0 ? juce::String ("No clipping found.")
                                                                     : juce::String (found) + (found == 1 ? " clipped place, marked." : " clipped places, marked."));
              });
}

namespace
{
const juce::StringArray normalizeTargets {
    "Peak -0.1 dB", "Peak -1 dB", "Peak -3 dB", "Peak (custom level)",
    "-14 LUFS: Spotify, YouTube, Tidal", "-16 LUFS: Apple Music, podcasts",
    "-23 LUFS: EBU R128 broadcast", "-24 LUFS: US TV (ATSC A/85)", "Loudness (custom level)"
};
} // namespace

void MainComponent::showNormalize()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    if (lastNormalize.size() != 3)
        lastNormalize = { normalizeTargets[0], "-0.1", "-1.0" };

    auto inRange = [] (double low, double high)
    {
        return [low, high] (const juce::String& t) { const auto v = wf::parseNumber (t); return v.has_value() && *v >= low && *v <= high; };
    };

    showForm ("Normalize", "Normalize: " + doc->getTitle() + " (" + doc->describeProcessTarget() + ")",
              { { "Normalize to:", lastNormalize[0], normalizeTargets, {}, {} },
                { "Custom level:", lastNormalize[1], {}, "dB / LUFS", inRange (-70.0, 0.0) },
                { "True peak at most:", lastNormalize[2], {}, "dBTP", inRange (-12.0, 0.0) } },
              "Peak: the loudest sample reaches the level. Loudness: the\n"
              "integrated loudness (LUFS, what streaming services measure)\n"
              "reaches it, with less gain if the true peak would go over\n"
              "the limit. Custom level is used by the two \"custom\" choices.",
              [this] (DataWindow& d, const juce::StringArray& v)
              {
                  lastNormalize = v;
                  const auto loudness = ! v[0].startsWith ("Peak");
                  const auto custom = v[0].contains ("custom");
                  const auto level = custom ? *wf::parseNumber (v[1])
                                            : loudness ? v[0].upToFirstOccurrenceOf (" ", false, false).getDoubleValue()
                                                       : v[0].fromFirstOccurrenceOf ("Peak ", false, false).getDoubleValue();
                  const auto ceiling = *wf::parseNumber (v[2]);

                  const auto result = d.normalize (loudness, level, ceiling);
                  rememberAction ("Normalize", [loudness, level, ceiling] (DataWindow& w) { w.normalize (loudness, level, ceiling); });

                  if (result.silent)
                  {
                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Normalize",
                                                              "Nothing to normalize: it is silent.");
                  }
                  else if (result.limited)
                  {
                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Normalize",
                          "Reached " + wf::formatLufs (result.reached) + " instead of " + wf::formatLufs (level) + ":\n"
                          "more gain would push the true peak over " + juce::String (ceiling, 1) + " dBTP.\n"
                          "Gain used: " + juce::String (result.gainDb, 2) + " dB.\n\n"
                          "To get louder, the peaks have to be limited first\n"
                          "(a Limiter is planned for a later version).");
                  }
              });
}

void MainComponent::showPitchShift()
{
    auto* doc = activeDocument();

    if (doc == nullptr || doc->isEmpty())
        return;

    if (lastPitch.size() != 2)
        lastPitch = { "+2", "Keep (voices stay natural)" };

    showForm ("Pitch Shift", "Change the pitch of: " + doc->getTitle() + " (" + doc->describeProcessTarget() + ")",
              { { "Shift by:", lastPitch[0], {}, "semitones", [] (const juce::String& t)
                  { const auto v = wf::parseNumber (t); return v.has_value() && std::abs (*v) <= wf::maxPitchSemitones && std::abs (*v) > 0.0; } },
                { "Formants:", lastPitch[1], { "Keep (voices stay natural)", "Shift with the pitch" }, {}, {} } },
              "The length stays the same (Time Stretch changes both).\n"
              "+12 = an octave up, -12 down; 0.5 = a quarter tone (50 cents).",
              [this] (DataWindow& d, const juce::StringArray& v)
              {
                  lastPitch = v;
                  wf::StretchSettings settings;
                  settings.semitones = *wf::parseNumber (v[0]);
                  settings.preserveFormants = v[1].startsWith ("Keep");
                  runStretch (&d, d.processRange(), settings);
                  rememberAction ("Pitch Shift", [this, settings] (DataWindow& w) { runStretch (&w, w.processRange(), settings); });
              });
}

void MainComponent::captureNoisePrint()
{
    auto source = bassFinderSource();   // the selection (one channel if picked)

    if (! source.has_value() || source->audio.getNumSamples() < wf::fx::noiseFftSize (source->sampleRate))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Capture Noise Print",
                                                "Select at least 0.1 s with only the noise in it (no music or voice).");
        return;
    }

    if (auto profile = wf::fx::makeNoiseProfile (source->audio, source->sampleRate, false))
    {
        profile->source = "captured from " + juce::String ((double) source->audio.getNumSamples() / source->sampleRate, 2)
                        + " s of " + source->title;
        wf::fx::setCapturedNoiseProfile (profile);
        statusBar.setText ("  Noise print " + profile->source + ". Now select what to clean and open Noise Reduction.",
                           juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::insertRegion()
{
    auto* doc = activeDocument();

    if (doc == nullptr || doc->getView().getSelection().isEmpty())
    {
        statusBar.setText ("  Select the audio the region should cover, then press R.", juce::dontSendNotification);
        return;
    }

    doc->addRegion (doc->getView().getSelection(), doc->nextRegionName());
}

void MainComponent::renameRegion (DataWindow* doc, int index)
{
    if (doc == nullptr || ! juce::isPositiveAndBelow (index, (int) doc->getRegions().size()))
        return;

    const juce::Component::SafePointer<DataWindow> safeDoc (doc);
    auto apply = [safeDoc, index] (const juce::StringArray& v)
    {
        if (auto* d = safeDoc.getComponent())
            d->renameRegion (index, v[0]);
    };

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->formWindow.reset();
        });
    };

    formWindow = wf::createFormWindow ("Rename Region", "Region " + juce::String (index + 1) + " of " + doc->getTitle(),
                                       { { "Name:", doc->getRegions()[(size_t) index].name, {}, {},
                                           [] (const juce::String& t) { return t.trim().isNotEmpty() && t.length() <= 200; } } },
                                       {}, std::move (apply), std::move (onClose));
    formWindow->setVisible (true);
    formWindow->toFront (true);
}

void MainComponent::renameMarker (DataWindow* doc, int index)
{
    if (doc == nullptr || ! juce::isPositiveAndBelow (index, (int) doc->getMarkers().size()))
        return;

    // The marker is found again by its position: the list may change while
    // the box is open.
    const auto position = doc->getMarkers()[(size_t) index];
    const juce::Component::SafePointer<DataWindow> safeDoc (doc);
    auto apply = [safeDoc, position] (const juce::StringArray& v)
    {
        if (auto* d = safeDoc.getComponent())
        {
            const auto& m = d->getMarkers();

            if (const auto it = std::lower_bound (m.begin(), m.end(), position); it != m.end() && *it == position)
                d->renameMarker ((int) (it - m.begin()), v[0]);
        }
    };

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->formWindow.reset();
        });
    };

    // An empty name takes the name away (only the number shows).
    formWindow = wf::createFormWindow ("Rename Marker", "Marker " + juce::String (index + 1) + " of " + doc->getTitle(),
                                       { { "Name:", doc->getMarkerName (index), {}, {},
                                           [] (const juce::String& t) { return t.length() <= 200; } } },
                                       {}, std::move (apply), std::move (onClose));
    formWindow->setVisible (true);
    formWindow->toFront (true);
}

void MainComponent::markerMenu (DataWindow* doc, int index)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Rename...");
    menu.addItem (2, "Delete marker");

    const juce::Component::SafePointer<DataWindow> safeDoc (doc);
    const juce::Component::SafePointer<MainComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition().withParentComponent (getTopLevelComponent()),
                        [safeThis, safeDoc, index] (int result)
                        {
                            auto* main = safeThis.getComponent();
                            auto* d = safeDoc.getComponent();

                            if (main == nullptr || d == nullptr)
                                return;

                            if (result == 1) main->renameMarker (d, index);
                            if (result == 2) d->removeMarker (index);
                        });
}

void MainComponent::playRegion (DataWindow* doc, int index)
{
    if (doc == nullptr || ! juce::isPositiveAndBelow (index, (int) doc->getRegions().size()))
        return;

    const auto& r = doc->getRegions()[(size_t) index];
    doc->getView().setSelection ({ r.start, r.end });

    if (doc == activeDocument())
        startPlayback ({ r.start, r.end });
}

void MainComponent::regionMenu (DataWindow* doc, int index)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Play");
    menu.addItem (2, "Rename...");
    menu.addItem (3, "Delete region");

    const juce::Component::SafePointer<DataWindow> safeDoc (doc);
    const juce::Component::SafePointer<MainComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition().withParentComponent (getTopLevelComponent()),
                        [safeThis, safeDoc, index] (int result)
                        {
                            auto* main = safeThis.getComponent();
                            auto* d = safeDoc.getComponent();

                            if (main == nullptr || d == nullptr)
                                return;

                            if (result == 1) main->playRegion (d, index);
                            if (result == 2) main->renameRegion (d, index);
                            if (result == 3) d->removeRegion (index);
                        });
}

void MainComponent::toggleRegionsList()
{
    if (regionsWindow != nullptr)
    {
        regionsWindow.reset();
        commandManager.commandStatusChanged();
        return;
    }

    wf::RegionsCallbacks callbacks;

    // A marker row puts the cursor on the marker, a region row selects the region.
    callbacks.select = [this] (wf::ListEntry e)
    {
        auto* d = activeDocument();

        if (d == nullptr)
            return;

        auto& view = d->getView();

        if (e.region && juce::isPositiveAndBelow (e.index, (int) d->getRegions().size()))
        {
            const auto& r = d->getRegions()[(size_t) e.index];
            view.setSelection ({ r.start, r.end });
            view.ensureVisible (r.start);
        }
        else if (! e.region && juce::isPositiveAndBelow (e.index, (int) d->getMarkers().size()))
        {
            const auto at = d->getMarkers()[(size_t) e.index];
            view.setSelection ({});
            view.setCursor (at);
            view.centreOn ((double) at);
        }
    };
    callbacks.play = [this] (wf::ListEntry e)
    {
        auto* d = activeDocument();

        if (e.region)
            playRegion (d, e.index);
        else if (d != nullptr && juce::isPositiveAndBelow (e.index, (int) d->getMarkers().size()))
            startPlayback ({ d->getMarkers()[(size_t) e.index], (juce::int64) d->getBuffer().getNumSamples() });
    };
    callbacks.rename = [this] (wf::ListEntry e)
    {
        if (e.region) renameRegion (activeDocument(), e.index);
        else          renameMarker (activeDocument(), e.index);
    };
    callbacks.remove = [this] (wf::ListEntry e)
    {
        if (auto* d = activeDocument())
        {
            if (e.region) d->removeRegion (e.index);
            else          d->removeMarker (e.index);
        }
    };
    callbacks.onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->regionsWindow.reset();
                main->commandManager.commandStatusChanged();
            }
        });
    };

    regionsWindow = std::make_unique<wf::RegionsWindow> (std::move (callbacks));
    refreshRegionsList();
    regionsWindow->setVisible (true);
    regionsWindow->toFront (false);
    commandManager.commandStatusChanged();
}

// View > Spectrum Analysis: follows the active file, its selection and cursor,
// and whatever plays.
void MainComponent::toggleSpectrum()
{
    if (spectrumWindow != nullptr)
    {
        spectrumWindow.reset();
        commandManager.commandStatusChanged();
        return;
    }

    if (meterTap == nullptr)
        return;

    wf::SpectrumSource source;
    source.audio = [this]() -> const juce::AudioBuffer<float>* { auto* d = activeDocument(); return d != nullptr ? &d->getBuffer() : nullptr; };
    source.sampleRate = [this] { auto* d = activeDocument(); return d != nullptr ? d->getSampleRate() : 44100.0; };
    source.selection = [this] { auto* d = activeDocument(); return d != nullptr ? d->getView().getSelection() : juce::Range<juce::int64>(); };
    source.cursor = [this] { auto* d = activeDocument(); return d != nullptr ? d->getView().getCursor() : (juce::int64) 0; };
    source.playing = [this] { return isPlaying(); };
    source.title = [this] { auto* d = activeDocument(); return d != nullptr ? d->getTitle() : juce::String(); };
    source.onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->spectrumWindow.reset();
                main->commandManager.commandStatusChanged();
            }
        });
    };

    spectrumWindow = std::make_unique<wf::SpectrumWindow> (std::move (source), *meterTap);
    spectrumWindow->setVisible (true);
    spectrumWindow->toFront (false);
    commandManager.commandStatusChanged();
}

// View > Undo/Redo History: the active file's steps; a click goes there.
void MainComponent::toggleUndoHistory()
{
    if (undoHistoryWindow != nullptr)
    {
        undoHistoryWindow.reset();
        commandManager.commandStatusChanged();
        return;
    }

    wf::UndoHistoryCallbacks callbacks;
    callbacks.goTo = [this] (int position)
    {
        if (auto* d = activeDocument(); d != nullptr && ! isRecording() && job == nullptr)
        {
            releasePlaybackSource();
            d->goToHistory (position);
            refreshUndoHistory();
            commandManager.commandStatusChanged();
        }
    };
    callbacks.onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->undoHistoryWindow.reset();
                main->commandManager.commandStatusChanged();
            }
        });
    };

    undoHistoryWindow = std::make_unique<wf::UndoHistoryWindow> (std::move (callbacks));
    refreshUndoHistory();
    undoHistoryWindow->setVisible (true);
    undoHistoryWindow->toFront (false);
    commandManager.commandStatusChanged();
}

void MainComponent::refreshUndoHistory()
{
    if (undoHistoryWindow == nullptr)
        return;

    if (auto* doc = activeDocument())
        undoHistoryWindow->refresh (doc->getTitle(), doc->undoNames(), doc->redoNames());
    else
        undoHistoryWindow->refresh ({}, {}, {});
}

void MainComponent::refreshRegionsList()
{
    if (spectrumWindow != nullptr)
        spectrumWindow->sourceChanged();

    refreshUndoHistory();

    if (regionsWindow == nullptr)
        return;

    if (auto* doc = activeDocument())
        regionsWindow->refresh (doc->getTitle(), doc->getMarkers(), doc->getMarkerNames(), doc->getRegions(), doc->getSampleRate());
    else
        regionsWindow->refresh ({}, {}, {}, {}, 44100.0);
}

namespace
{
// The stretches of sound between silences of at least `minSilence` seconds
// (quieter than `thresholdDb`, measured in 10 ms steps).
std::vector<AudioRegion> findSoundRegions (const juce::AudioBuffer<float>& audio, double rate, double thresholdDb,
                                           double minSilence, double minRegion)
{
    const auto step = std::max (1, (int) (rate * 0.01));
    const auto threshold = juce::Decibels::decibelsToGain ((float) thresholdDb);
    const auto steps = audio.getNumSamples() / step + 1;
    std::vector<bool> loud ((size_t) steps, false);

    for (int k = 0; k < steps; ++k)
    {
        const auto start = k * step, n = std::min (step, audio.getNumSamples() - start);

        if (n <= 0)
            break;

        float level = 0.0f;

        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            level = std::max (level, audio.getRMSLevel (ch, start, n));

        loud[(size_t) k] = level >= threshold;
    }

    const auto minQuietSteps = std::max (1, (int) std::lround (minSilence / 0.01));
    std::vector<AudioRegion> found;
    int k = 0;

    while (k < steps)
    {
        while (k < steps && ! loud[(size_t) k]) ++k;   // skip silence

        if (k >= steps)
            break;

        const auto first = k;
        int lastLoud = k;

        // Runs on until a silence long enough.
        while (k < steps)
        {
            if (loud[(size_t) k]) { lastLoud = k; ++k; continue; }

            int quiet = 0;
            while (k < steps && ! loud[(size_t) k]) { ++quiet; ++k; }

            if (quiet >= minQuietSteps || k >= steps)
                break;
        }

        const auto start = (juce::int64) first * step;
        const auto end = std::min ((juce::int64) audio.getNumSamples(), (juce::int64) (lastLoud + 1) * step);

        if ((double) (end - start) / rate >= minRegion)
            found.push_back ({ start, end, {} });
    }

    return found;
}
} // namespace

void MainComponent::showAutoRegion()
{
    auto* doc = activeDocument();

    if (doc == nullptr || doc->isEmpty())
        return;

    auto number = [] (double lo, double hi)
    {
        return [lo, hi] (const juce::String& t) { const auto v = wf::parseNumber (t); return v.has_value() && *v >= lo && *v <= hi; };
    };

    showForm ("Auto Region", "Make a region of every stretch of sound in " + doc->getTitle(),
              { { "Silence is below:", "-45", {}, "dB", number (-100.0, -6.0) },
                { "Silence lasts at least:", "0.5", {}, "seconds", number (0.05, 30.0) },
                { "Shortest region:", "1", {}, "seconds", number (0.0, 600.0) },
                { "Existing regions:", "Keep them", { "Keep them", "Replace them" }, {}, {} } },
              "Good for splitting a recording of several takes,\nsongs or samples separated by silence.",
              [this] (DataWindow& d, const juce::StringArray& v)
              {
                  auto found = findSoundRegions (d.getBuffer(), d.getSampleRate(), *wf::parseNumber (v[0]),
                                                 *wf::parseNumber (v[1]), *wf::parseNumber (v[2]));
                  auto all = v[3] == "Keep them" ? d.getRegions() : std::vector<AudioRegion>();
                  int n = 1;

                  for (auto& r : found)
                  {
                      r.name = "Region " + juce::String (n++);
                      all.push_back (r);
                  }

                  if (found.empty())
                  {
                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Auto Region",
                                                              "No stretch of sound found with these settings.");
                      return;
                  }

                  d.setRegions (std::move (all));
                  statusBar.setText ("  " + juce::String ((int) found.size()) + " regions made.", juce::dontSendNotification);
              });
}

void MainComponent::extractRegions()
{
    auto* doc = activeDocument();

    if (doc == nullptr || doc->getRegions().empty())
        return;

    const auto start = doc->getFile().existsAsFile() ? doc->getFile().getParentDirectory()
                                                     : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const juce::Component::SafePointer<DataWindow> safeDoc (doc);

    filePicker.launch ("Folder to save the regions in", start, "*",
                       juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                       [this, safeDoc] (const juce::Array<juce::File>& chosen)
                       {
                           auto* d = safeDoc.getComponent();

                           if (d == nullptr || chosen.isEmpty() || ! chosen[0].isDirectory())
                               return;

                           const auto folder = chosen[0];
                           const auto base = d->getFile().existsAsFile() ? d->getFile().getFileNameWithoutExtension() : d->getTitle();
                           const auto keep = d->getFile().existsAsFile() && DataWindow::isSaveExtension (d->getFile().getFileExtension())
                                             && ! d->getFile().hasFileExtension (".mp3");
                           wf::SaveFormat format;
                           format.extension = keep ? d->getFile().getFileExtension().toLowerCase() : ".wav";
                           format.bitsPerSample = d->getBitDepth() > 0 ? d->getBitDepth() : 24;
                           format.floatingPoint = d->isFloatingPoint();
                           int saved = 0;
                           juce::StringArray failed;

                           for (const auto& r : d->getRegions())
                           {
                               const auto length = (int) (r.end - r.start);
                               juce::AudioBuffer<float> audio (d->getBuffer().getNumChannels(), length);

                               for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                                   audio.copyFrom (ch, 0, d->getBuffer(), ch, (int) r.start, length);

                               auto name = juce::File::createLegalFileName (base + " - " + r.name);
                               auto file = folder.getChildFile (name + format.extension);

                               for (int n = 2; file.exists(); ++n)
                                   file = folder.getChildFile (name + " (" + juce::String (n) + ")" + format.extension);

                               auto part = DataWindow::fromAudio (name, d->getSampleRate(), std::move (audio), {});
                               juce::String error;

                               if (part != nullptr && part->saveTo (file, formatManager, error, &format))
                                   ++saved;
                               else
                                   failed.add (r.name + (error.isNotEmpty() ? ": " + error : juce::String()));
                           }

                           juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Extract Regions",
                               juce::String (saved) + (saved == 1 ? " file" : " files") + " saved in\n" + folder.getFullPathName()
                               + (failed.isEmpty() ? juce::String() : "\n\nNot saved:\n" + failed.joinIntoString ("\n")));
                       });
}

void MainComponent::showBatch()
{
    if (batchWindow != nullptr)
    {
        batchWindow->toFront (true);
        return;
    }

    // Starts with the open files that are on disk.
    juce::Array<juce::File> files;

    for (int i = 0; i < workspace->getNumDocuments(); ++i)
        if (auto* d = dynamic_cast<DataWindow*> (workspace->getDocument (i)); d != nullptr && d->getFile().existsAsFile())
            files.addIfNotAlreadyThere (d->getFile());

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->batchWindow.reset();
        });
    };

    batchWindow = wf::createBatchWindow (formatManager, filePicker, files, std::move (onClose));
    batchWindow->setVisible (true);
    batchWindow->toFront (true);
}

void MainComponent::addRecentFile (const juce::File& file)
{
    juce::StringArray paths;

    // keep the parsed list alive while it is read (get() returns a fresh copy)
    const auto recent = wf::settings::get ("recentFiles");

    if (const auto* list = recent.getArray())
        for (const auto& v : *list)
            paths.add (v.toString());

    paths.removeString (file.getFullPathName());
    paths.insert (0, file.getFullPathName());

    while (paths.size() > 10)
        paths.remove (paths.size() - 1);

    juce::Array<juce::var> list;

    for (const auto& p : paths)
        list.add (p);

    wf::settings::set ("recentFiles", list);
}

juce::PopupMenu MainComponent::recentFilesMenu()
{
    juce::PopupMenu menu;
    const juce::Component::SafePointer<MainComponent> safeThis (this);

    // keep the parsed list alive while it is read (get() returns a fresh copy)
    const auto recent = wf::settings::get ("recentFiles");

    if (const auto* list = recent.getArray())
    {
        for (const auto& v : *list)
        {
            const juce::File file (v.toString());
            juce::PopupMenu::Item item (file.getFileName() + "   (" + file.getParentDirectory().getFullPathName() + ")");
            item.isEnabled = file.existsAsFile();
            item.action = [safeThis, file]
            {
                if (auto* main = safeThis.getComponent())
                    main->loadFile (file);
            };
            menu.addItem (item);
        }
    }

    if (menu.getNumItems() > 0)
    {
        menu.addSeparator();
        menu.addItem ("Clear the list", [] { wf::settings::set ("recentFiles", juce::Array<juce::var>()); });
    }

    return menu;
}

void MainComponent::createEmptyDocument (std::optional<juce::Point<int>> position)
{
    auto doc = DataWindow::createEmpty ("Untitled " + juce::String (++untitledCount), 44100.0, 2);
    doc->getView().setEmptyText ("Empty: paste (Ctrl+V) or drag audio in here");
    addDocument (std::move (doc), position);
}

void MainComponent::showWorkspaceMenu (juce::Point<int> position)
{
    juce::PopupMenu menu;
    const juce::Component::SafePointer<MainComponent> safeThis (this);
    menu.addItem ("Create New (empty)", [safeThis, position]
    {
        if (auto* main = safeThis.getComponent())
            main->createEmptyDocument (position);
    });
    menu.addCommandItem (&commandManager, wf::fileNew, "New from a Template...");
    menu.addSeparator();
    menu.addCommandItem (&commandManager, wf::fileOpen);

    const auto recent = recentFilesMenu();
    menu.addSubMenu ("Open Recent", recent, recent.getNumItems() > 0);

    if (hasClipboard())
    {
        menu.addSeparator();
        menu.addCommandItem (&commandManager, wf::editPasteToNew);
    }

    menu.addSeparator();
    addLookItems (menu);

    // Inside the main window: Hyprland moves focus onto separate menu windows
    // and JUCE then closes them as soon as the mouse slips off.
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition().withParentComponent (getTopLevelComponent()));
}

// Waveform Colour... and Background...: the look of every window at once,
// in the right-click menus of the waveforms and of the empty workspace.
void MainComponent::addLookItems (juce::PopupMenu& menu)
{
    const juce::Component::SafePointer<MainComponent> safeThis (this);
    const auto at = getLocalArea (nullptr, juce::Rectangle<int> (1, 1) + juce::Desktop::getMousePosition());
    const auto repaintAll = [safeThis] { if (auto* main = safeThis.getComponent()) main->repaint(); };

    menu.addItem ("Waveform Colour...", [safeThis, at, repaintAll]
    {
        if (auto* main = safeThis.getComponent())
            wf::showWaveColourPicker (*main, at, repaintAll);
    });
    menu.addItem ("Background...", [safeThis, at, repaintAll]
    {
        if (auto* main = safeThis.getComponent())
            wf::showBackgroundPicker (*main, at, repaintAll, [safeThis, repaintAll]
            {
                if (auto* m = safeThis.getComponent())
                    m->chooseBackgroundPhoto (repaintAll);
            });
    });
}

// Right-click in a file's waveform: the usual edits, a few tools, and files.
void MainComponent::showWaveMenu (DataWindow* doc)
{
    if (doc != nullptr && workspace != nullptr && workspace->getActiveDocument() != doc)
        workspace->setActiveDocument (doc);

    commandManager.commandStatusChanged();
    juce::PopupMenu menu;
    const juce::Component::SafePointer<MainComponent> safeThis (this);

    menu.addCommandItem (&commandManager, wf::optPlay);
    menu.addCommandItem (&commandManager, wf::optPlayFromCursor);
    menu.addSeparator();

    for (auto id : { wf::editCut, wf::editCopy, wf::editPaste, wf::editOverwrite, wf::editPasteToNew, wf::editDelete, wf::editTrim, wf::editSelectAll })
        menu.addCommandItem (&commandManager, id);

    menu.addSeparator();

    for (auto id : { wf::insertMarker, wf::insertRegion, wf::insertVolumeEnv })
        menu.addCommandItem (&commandManager, id);

    menu.addSeparator();

    addLookItems (menu);
    menu.addSeparator();

    menu.addItem ("New (empty)", [safeThis]
    {
        if (auto* main = safeThis.getComponent())
            main->createEmptyDocument ({});
    });
    menu.addCommandItem (&commandManager, wf::fileNew, "New from a Template...");
    menu.addCommandItem (&commandManager, wf::fileOpen);

    const auto recent = recentFilesMenu();
    menu.addSubMenu ("Open Recent", recent, recent.getNumItems() > 0);

    // Inside the main window: Hyprland moves focus onto separate menu windows
    // and JUCE then closes them as soon as the mouse slips off.
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition().withParentComponent (getTopLevelComponent()));
}

void MainComponent::chooseBackgroundPhoto (std::function<void()> onChanged)
{
    const auto current = wf::appearance::background().photo;
    const auto start = current.existsAsFile() ? current : juce::File::getSpecialLocation (juce::File::userPicturesDirectory);

    filePicker.launch ("Background photo", start, "*.jpg;*.jpeg;*.png;*.gif",
                       juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                       [onChanged = std::move (onChanged)] (const juce::Array<juce::File>& files)
    {
        if (files.isEmpty())
            return;

        if (! wf::appearance::canUsePhoto (files[0]))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Background",
                                                    "\"" + files[0].getFileName() + "\" isn't a picture Adormis Audio can read (JPG, PNG or GIF).");
            return;
        }

        auto b = wf::appearance::background();
        b.photo = files[0];
        wf::appearance::setBackground (b);

        if (onChanged)
            onChanged();
    });
}

void MainComponent::showStatistics()
{
    auto* doc = activeDocument();

    if (doc == nullptr || doc->isEmpty() || job != nullptr)
        return;

    // Measured on a copy, so the file can't change under the job.
    const auto range = doc->processRange();
    const auto picked = doc->getView().getSelectedChannel();
    const auto& source = doc->getBuffer();
    const auto channels = picked >= 0 ? 1 : source.getNumChannels();
    auto audio = std::make_shared<juce::AudioBuffer<float>> (channels, range.getLength());

    for (int ch = 0; ch < channels; ++ch)
        audio->copyFrom (ch, 0, source, picked >= 0 ? picked : ch, range.getStart(), range.getLength());

    const auto rate = doc->getSampleRate();
    const auto what = doc->getTitle() + " (" + doc->describeProcessTarget() + ")";
    auto result = std::make_shared<std::optional<wf::AudioStatistics>>();
    const juce::Component::SafePointer<DataWindow> safeDoc (doc);

    runJob ("Measuring " + doc->getTitle() + "...",
            [audio, rate, result] (std::function<bool (float)> progress)
            {
                *result = wf::measure (*audio, rate, { 0, audio->getNumSamples() }, progress);
                return result->has_value();
            },
            [this, result, rate, what, safeDoc, range] (bool finished)
            {
                if (! finished || ! result->has_value())
                    return;

                auto stats = **result;

                for (auto& c : stats.channels)
                    c.peakPosition += range.getStart();

                auto goTo = [safeDoc] (juce::int64 position)
                {
                    if (auto* d = safeDoc.getComponent())
                    {
                        d->getView().setSelection ({});
                        d->getView().setCursor (position);
                        d->getView().ensureVisible (position);
                    }
                };

                auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
                {
                    juce::MessageManager::callAsync ([safeThis]
                    {
                        if (auto* main = safeThis.getComponent())
                            main->statisticsWindow.reset();
                    });
                };

                statisticsWindow = wf::createStatisticsWindow (what, stats, rate, std::move (goTo), std::move (onClose));
                statisticsWindow->setVisible (true);
                statisticsWindow->toFront (true);
            });
}

void MainComponent::setMeterSections (bool channels, bool loudness)
{
    meters->setSections (channels, loudness);
    resized();
    commandManager.commandStatusChanged();
}

void MainComponent::mixClipboard()
{
    auto* doc = activeDocument();

    if (doc == nullptr || ! hasClipboard())
        return;

    const juce::Component::SafePointer<DataWindow> target (doc);
    const auto position = doc->getView().getSelection().isEmpty() ? doc->getView().getCursor() : doc->getView().getSelection().getStart();
    auto audio = std::make_shared<juce::AudioBuffer<float>> (clipboard.audio);
    const auto sourceRate = clipboard.sampleRate;
    auto gains = std::make_shared<wf::MixGains>();
    const auto marks = std::make_shared<DataWindow::Marks> (clipboard.marks);

    const auto description = "Mix " + WaveformView::formatTime ((double) audio->getNumSamples() / sourceRate).substring (3)
                           + " of copied audio\ninto \"" + doc->getTitle() + "\" at "
                           + WaveformView::formatTime ((double) position / doc->getSampleRate()) + ".";

    wf::MixDialogCallbacks callbacks;
    callbacks.onOk = [this, target, audio, sourceRate, position, gains, marks]
    {
        if (auto* d = target.getComponent())
        {
            releasePlaybackSource();
            d->mixIn (*audio, sourceRate, position, gains->source, gains->dest, marks.get());
        }
    };
    callbacks.onPreview = [this, target, audio, sourceRate, position, gains] (bool play)
    {
        if (play)
            startMixPreview (target.getComponent(), *audio, sourceRate, position, gains);
        else
            stopMixPreview();
    };
    callbacks.isPreviewing = [this] { return isMixPreviewing(); };

    openMixWindow (description, gains, std::move (callbacks));
}

void MainComponent::saveAll (std::vector<juce::Component::SafePointer<DataWindow>> remaining)
{
    // One after the other: a file never saved asks where to go first.
    while (! remaining.empty() && (remaining.front().getComponent() == nullptr || ! remaining.front()->isModified()))
        remaining.erase (remaining.begin());

    if (remaining.empty())
        return;

    auto* doc = remaining.front().getComponent();
    remaining.erase (remaining.begin());

    saveDocument (doc, false, [safeThis = juce::Component::SafePointer<MainComponent> (this), remaining] (bool saved)
    {
        if (auto* main = safeThis.getComponent(); main != nullptr && saved)
            main->saveAll (remaining);
    });
}

void MainComponent::markInOut (bool in)
{
    auto* doc = activeDocument();

    if (doc == nullptr)
        return;

    // While playing, I / O mark where the sound is (Sound Forge); else the cursor.
    auto& view = doc->getView();
    const auto at = isPlaying() && doc == playingDocument.getComponent() ? playPosition() : view.getCursor();
    const auto sel = view.getSelection();

    if (in)
    {
        markInPosition = at;

        if (! sel.isEmpty() && at < sel.getEnd())
            view.setSelection ({ at, sel.getEnd() });
        else if (! isPlaying())
            view.setCursor (at);
    }
    else
    {
        const auto start = ! sel.isEmpty() ? sel.getStart() : juce::jmax ((juce::int64) 0, markInPosition);

        if (at > start)
            view.setSelection ({ start, at });
    }

    updateStatus();
    commandManager.commandStatusChanged();
}

namespace
{
// Runs work off the message thread behind a progress bar with Cancel.
class ProgressJob final : public juce::ThreadWithProgressWindow
{
public:
    ProgressJob (const juce::String& title, std::function<bool (std::function<bool (float)>)> workIn,
                 std::function<void (bool)> doneIn)
        : juce::ThreadWithProgressWindow (title, true, true), work (std::move (workIn)), done (std::move (doneIn))
    {
    }

    void run() override
    {
        finished = work ([this] (float fraction)
        {
            setProgress (fraction);
            return ! threadShouldExit();
        });
    }

    void threadComplete (bool userPressedCancel) override
    {
        if (done != nullptr)
            done (finished && ! userPressedCancel);
    }

private:
    std::function<bool (std::function<bool (float)>)> work;
    std::function<void (bool)> done;
    bool finished = false;
};
} // namespace

void MainComponent::showStretchDialog()
{
    auto* doc = activeDocument();

    if (doc == nullptr || job != nullptr || toolWindowOpen())
        return;

    const auto range = doc->processRange();
    const juce::Component::SafePointer<DataWindow> safeDoc (doc);

    // The tempo and key belong to this audio, the rest is remembered.
    auto initial = lastStretch;
    initial.fromBpm.reset();
    initial.fromKey = -1;

    wf::StretchCallbacks callbacks;

    callbacks.detectTempo = [safeDoc, range]() -> std::optional<wf::TempoEstimate>
    {
        if (auto* d = safeDoc.getComponent())
            return wf::detectTempo (d->getBuffer(), range, d->getSampleRate());

        return {};
    };

    callbacks.detectKey = [safeDoc, range]() -> std::optional<wf::KeyEstimate>
    {
        if (auto* d = safeDoc.getComponent())
            return wf::detectKey (d->getBuffer(), range, d->getSampleRate());

        return {};
    };

    callbacks.onApply = [this, safeDoc, range] (wf::StretchRequest request)
    {
        lastStretch = request;

        if (auto* target = safeDoc.getComponent())
            runStretch (target, range, request.settings());
    };

    callbacks.onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->stretchWindow.reset();
                main->commandManager.commandStatusChanged();
            }
        });
    };

    // Changing the length works on all channels together.
    const auto target = doc->getTitle() + " ("
                      + (doc->getView().getSelection().isEmpty() ? "whole file" : "selection, all channels") + ")";

    stretchWindow = wf::createStretchWindow (target, range.getLength() / doc->getSampleRate(), initial, std::move (callbacks));
    stretchWindow->setVisible (true);
    stretchWindow->toFront (true);
    commandManager.commandStatusChanged();
}

void MainComponent::runStretch (DataWindow* doc, juce::Range<int> range, wf::StretchSettings settings)
{
    if (doc == nullptr || job != nullptr)
        return;

    range = range.getIntersectionWith ({ 0, doc->getBuffer().getNumSamples() });

    if (range.isEmpty())
        return;

    auto audio = std::make_shared<juce::AudioBuffer<float>> (doc->getBuffer().getNumChannels(), range.getLength());

    for (int ch = 0; ch < audio->getNumChannels(); ++ch)
        audio->copyFrom (ch, 0, doc->getBuffer(), ch, range.getStart(), range.getLength());

    auto result = std::make_shared<std::optional<juce::AudioBuffer<float>>>();
    const auto rate = doc->getSampleRate();

    runJob ("Time stretch...",
            [audio, result, rate, settings] (std::function<bool (float)> progress)
            {
                *result = wf::stretchAudio (*audio, { 0, audio->getNumSamples() }, rate, settings, progress);
                return result->has_value();
            },
            [this, safeDoc = juce::Component::SafePointer<DataWindow> (doc), range, result] (bool finished)
            {
                if (auto* target = safeDoc.getComponent(); target != nullptr && finished && result->has_value())
                {
                    releasePlaybackSource();
                    target->replaceRange (range, **result, (double) (*result)->getNumSamples() / (double) range.getLength());
                }
            });
}

void MainComponent::runJob (const juce::String& title, std::function<bool (std::function<bool (float)>)> work,
                            std::function<void (bool finished)> done)
{
    if (job != nullptr)
        return;

    // what the job writes into the file is named after the command that started it
    auto finish = [safeThis = juce::Component::SafePointer<MainComponent> (this), done = std::move (done),
                   name = lastCommandName, target = juce::Component::SafePointer<DataWindow> (activeDocument())] (bool finished)
    {
        if (auto* d = target.getComponent())
            d->setUndoName (name);

        if (done != nullptr)
            done (finished);

        if (auto* d = target.getComponent())
            d->setUndoName ({});

        // The job is still on the stack here; let it go afterwards.
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->job.reset();
                main->commandManager.commandStatusChanged();
            }
        });
    };

    job = std::make_unique<ProgressJob> (title, std::move (work), std::move (finish));
    job->launchThread();
    commandManager.commandStatusChanged();
}

std::optional<wf::BassFinderSource> MainComponent::bassFinderSource()
{
    auto* doc = activeDocument();

    if (doc == nullptr || doc->getBuffer().getNumSamples() == 0)
        return {};

    return bassFinderSourceFor (*doc, doc->processRange(), ! doc->getView().getSelection().isEmpty());
}

wf::BassFinderSource MainComponent::bassFinderSourceFor (DataWindow& doc, juce::Range<int> range, bool isSelection)
{
    wf::BassFinderSource source;
    source.isSelection = isSelection;
    source.title = doc.getTitle() + (source.isSelection ? " (selection)" : " (whole file)");
    source.sampleRate = doc.getSampleRate();
    source.startInFile = range.getStart() / source.sampleRate;

    if (doc.getKnownTempo() > 0.0)
    {
        source.fileBpm = doc.getKnownTempo();
    }
    else
    {
        TempoKey whole;
        whole.doc = &doc;
        whole.range = { 0, doc.getBuffer().getNumSamples() };
        whole.edits = doc.getEditCount();

        if (const auto bpm = cachedTempo (whole); bpm.has_value() && *bpm > 0.0)
            source.fileBpm = *bpm * (tempoMode == 1 ? 2.0 : tempoMode == 2 ? 0.5 : 1.0);
    }

    source.fileDuration = doc.getBuffer().getNumSamples() / source.sampleRate;
    source.audio.setSize (doc.getBuffer().getNumChannels(), range.getLength());

    for (int ch = 0; ch < source.audio.getNumChannels(); ++ch)
        source.audio.copyFrom (ch, 0, doc.getBuffer(), ch, range.getStart(), range.getLength());

    const juce::Component::SafePointer<DataWindow> safeDoc (&doc);

    source.filePeaks = [safeDoc] (int columns)
    {
        std::vector<float> peaks ((size_t) std::max (0, columns), 0.0f);
        auto* target = safeDoc.getComponent();

        if (target == nullptr)
            return peaks;

        const auto& a = target->getBuffer();
        const auto total = (juce::int64) a.getNumSamples();

        for (size_t c = 0; c < peaks.size() && total > 0; ++c)
        {
            const auto from = (int) (total * (juce::int64) c / (juce::int64) peaks.size());
            const auto to = (int) (total * (juce::int64) (c + 1) / (juce::int64) peaks.size());

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                peaks[c] = std::max (peaks[c], a.getMagnitude (ch, from, std::max (1, to - from)));
        }

        return peaks;
    };

    source.otherRange = [this, safeDoc] (double start, double end) -> std::optional<wf::BassFinderSource>
    {
        auto* target = safeDoc.getComponent();

        if (target == nullptr || target->getBuffer().getNumSamples() == 0)
            return {};

        const auto total = target->getBuffer().getNumSamples();
        const auto rate = target->getSampleRate();
        const auto whole = end <= start;
        const auto from = whole ? 0 : juce::jlimit (0, total - 1, (int) std::llround (start * rate));
        const auto to = whole ? total : juce::jlimit (from + 1, total, (int) std::llround (end * rate));

        if (whole)
            target->getView().setSelection ({});
        else
            target->getView().setSelection ({ (juce::int64) from, (juce::int64) to });

        target->getView().ensureVisible (from);
        return bassFinderSourceFor (*target, { from, to }, ! whole);
    };

    source.applyToFile = [this, safeDoc, range] (const juce::AudioBuffer<float>& corrected)
    {
        auto* target = safeDoc.getComponent();

        if (target == nullptr || range.getEnd() > target->getBuffer().getNumSamples()
            || corrected.getNumChannels() != target->getBuffer().getNumChannels())
            return false;

        releasePlaybackSource();
        target->setUndoName (toolUndoName);
        target->replaceRange (range, corrected, 1.0);
        return true;
    };

    source.applyWithTail = [this, safeDoc, range] (const juce::AudioBuffer<float>& processed)
    {
        auto* target = safeDoc.getComponent();

        if (target == nullptr || range.getEnd() > target->getBuffer().getNumSamples()
            || processed.getNumChannels() != target->getBuffer().getNumChannels())
            return false;

        releasePlaybackSource();
        target->setUndoName (toolUndoName);
        target->applyEffect (range, processed);
        return true;
    };

    source.showRange = [safeDoc, range] (double start, double end)
    {
        if (auto* target = safeDoc.getComponent())
        {
            const auto rate = target->getSampleRate();
            const auto from = range.getStart() + (juce::int64) std::llround (start * rate);
            const auto to = range.getStart() + (juce::int64) std::llround (end * rate);
            target->getView().setSelection ({ from, to });
            target->getView().ensureVisible (from);
        }
    };

    source.showPlayhead = [safeDoc, range] (double seconds)
    {
        if (auto* target = safeDoc.getComponent())
        {
            auto& view = target->getView();

            if (seconds < 0.0)
            {
                view.setPlayhead (-1);
                return;
            }

            const auto at = range.getStart() + (juce::int64) std::llround (seconds * target->getSampleRate());
            view.setPlayhead (at);
            view.ensureVisible (at);
        }
    };

    source.cursorSeconds = [safeDoc, range]
    {
        auto* target = safeDoc.getComponent();

        if (target == nullptr)
            return 0.0;

        const auto& view = target->getView();
        const auto at = view.getPlayhead() >= 0 ? view.getPlayhead() : view.getCursor();
        return (double) (at - range.getStart()) / target->getSampleRate();
    };

    return source;
}

void MainComponent::showDeEsser()
{
    if (deEsserWindow != nullptr)
    {
        deEsserWindow->toFront (true);
        return;
    }

    if (toolWindowOpen())
        return;

    auto source = bassFinderSource();

    if (! source.has_value())
        return;

    // The selection is about to follow the sibilants, so the source keeps
    // the range it was opened on (bassFinderSource captured it).
    auto host = toolHost();

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->stopToolPreview();
                main->deEsserWindow.reset();
                main->commandManager.commandStatusChanged();
            }
        });
    };

    toolUndoName = lastCommandName;
    deEsserWindow = wf::createDeEsserWindow (std::move (*source), std::move (host), std::move (onClose));
    deEsserWindow->setVisible (true);
    deEsserWindow->toFront (true);
    commandManager.commandStatusChanged();
}

void MainComponent::getStems (DataWindow* doc)
{
    if (doc == nullptr || doc->isEmpty() || doc->hasStemPages() || doc->isStem() || job != nullptr)
        return;

    if (! wf::stems::modelFile().existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Get Stems",
                                                "The AI model for stems is missing from this copy of Adormis Audio.");
        return;
    }

    // The whole song, stereo at the model's rate; the parts come back in the file's format.
    auto input = std::make_shared<juce::AudioBuffer<float>> (
        DataWindow::prepareForMix (doc->getBuffer(), doc->getSampleRate(), wf::stems::modelRate, 2));
    auto parts = std::make_shared<std::optional<std::array<juce::AudioBuffer<float>, wf::stems::numParts>>>();
    auto error = std::make_shared<juce::String>();
    const auto rate = doc->getSampleRate();
    const auto channels = doc->getBuffer().getNumChannels();
    const auto minutes = juce::roundToInt (input->getNumSamples() / wf::stems::modelRate / 60.0 * 2.0);

    gettingStems = doc;
    doc->setGettingStems (true);

    runJob ("Getting stems with AI (about " + juce::String (juce::jmax (1, minutes)) + " min on a laptop)...",
            [input, parts, error] (std::function<bool (float)> progress)
            {
                *parts = wf::stems::separate (*input, progress, *error);
                return parts->has_value();
            },
            [this, parts, error, rate, channels] (bool finished)
            {
                auto* target = gettingStems.getComponent();
                gettingStems = nullptr;

                if (target == nullptr)
                    return;

                target->setGettingStems (false);

                if (! finished || ! parts->has_value())
                {
                    if (error->isNotEmpty())
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Get Stems", *error);

                    return;
                }

                // The tab order: drums, bass, vocals, then the rest.
                std::vector<std::pair<juce::String, juce::AudioBuffer<float>>> pages;
                const std::pair<wf::stems::Part, const char*> order[] { { wf::stems::drums, "Drums" }, { wf::stems::bass, "Bass" },
                                                                        { wf::stems::vocals, "Vocals" }, { wf::stems::other, "Other instruments" } };

                for (auto [part, name] : order)
                    pages.emplace_back (name, DataWindow::prepareForMix ((**parts)[(size_t) part], wf::stems::modelRate, rate, channels));

                parts->reset();
                target->addStemPages (std::move (pages));
                commandManager.commandStatusChanged();
            });
}

// The link box: first the tools (once, asked), then the download as a job;
// the WAV opens in a new window.
void MainComponent::downloadLink()
{
    const auto link = linkBox.getText().trim();

    if (job != nullptr)
        return;

    if (! wf::links::looksLikeLink (link))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Get WAV",
                                                "Paste the address of a video or song page, e.g. https://www.youtube.com/watch?v=...");
        return;
    }

    if (const auto missing = wf::links::missingTools(); missing.isNotEmpty())
    {
        if (! wf::links::canFetchMissingTools())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Get WAV",
                                                    missing + " is needed for links. " + wf::links::installHint());
            return;
        }

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Get WAV",
            "Getting the sound from links needs " + missing + " (free programs, from their GitHub pages; "
            "about 130 MB, more on Windows with ffmpeg).\n\n"
            "Download " + (missing.contains (" and ") ? "them" : "it") + " now into the Adormis Audio settings folder?",
            "Download", "Cancel", this,
            juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<MainComponent> (this)] (int answer)
            {
                auto* main = safeThis.getComponent();

                if (main == nullptr || answer == 0 || main->job != nullptr)
                    return;

                auto error = std::make_shared<juce::String>();
                main->runJob ("Downloading the tools for links...",
                              [error] (std::function<bool (float)> progress) { return wf::links::fetchMissingTools (progress, *error); },
                              [safeThis, error] (bool finished)
                              {
                                  if (safeThis == nullptr)
                                      return;

                                  // Then the link itself, once this job has let go.
                                  if (finished)
                                      juce::Timer::callAfterDelay (200, [safeThis] { if (auto* m = safeThis.getComponent()) m->downloadLink(); });
                                  else if (error->isNotEmpty())
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Get WAV", *error);
                              });
            }));
        return;
    }

    auto error = std::make_shared<juce::String>();
    auto file = std::make_shared<juce::File>();

    runJob ("Getting the sound from the link...",
            [link, error, file] (std::function<bool (float)> progress)
            {
                *file = wf::links::download (link, progress, *error);
                return file->existsAsFile();
            },
            [safeThis = juce::Component::SafePointer<MainComponent> (this), error, file] (bool finished)
            {
                auto* main = safeThis.getComponent();

                if (main == nullptr)
                    return;

                if (finished && file->existsAsFile())
                {
                    main->linkBox.clear();
                    main->loadFile (*file);
                }
                else if (error->isNotEmpty())
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Get WAV",
                                                            "Could not get the sound from this link:\n\n" + *error);
                }
            });
}

// Opens the selection (or the whole file) in Drum Rebuilder; with the window
// already open, the new stretch replaces the beat in it.
void MainComponent::showDrumRebuilder()
{
    auto* doc = activeDocument();

    if (doc == nullptr || doc->getBuffer().getNumSamples() == 0)
        return;

    const auto range = doc->processRange();
    const auto whole = doc->getView().getSelection().isEmpty();
    juce::AudioBuffer<float> audio (doc->getBuffer().getNumChannels(), range.getLength());

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        audio.copyFrom (ch, 0, doc->getBuffer(), ch, range.getStart(), range.getLength());

    const auto beat = wf::drumRebuilderBeatFile (doc->getTitle(), doc->getFile(), whole && ! doc->isModified(), audio,
                                                 doc->getSampleRate(), range.getStart() / doc->getSampleRate());

    if (beat == juce::File())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Drum Rebuilder",
                                                "Could not write the audio for Drum Rebuilder into " + wf::startup::settingsFolder().getFullPathName());
        return;
    }

    if (drumRebuilderWindow == nullptr)
    {
        wf::DrumRebuilderHost host;
        host.devices = &deviceManager;
        host.openAsNewFile = [this] (const juce::String& title, juce::AudioBuffer<float> rebuilt, double rate,
                                     std::vector<juce::int64> markers)
        {
            addDocument (DataWindow::fromAudio (title, rate, std::move (rebuilt), std::move (markers)));
        };
        host.onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            juce::MessageManager::callAsync ([safeThis]
            {
                if (auto* main = safeThis.getComponent())
                {
                    main->drumRebuilderWindow.reset();
                    main->commandManager.commandStatusChanged();
                }
            });
        };
        host.openBeats = [this]
        {
            std::vector<wf::DrumRebuilderHost::OpenBeat> list;

            for (int i = 0; i < workspace->getNumDocuments(); ++i)
                if (auto* d = dynamic_cast<DataWindow*> (workspace->getDocument (i)))
                    for (int page = 0; page < d->getNumPages(); ++page)
                    {
                        const auto a = d->getPageAudio (page);
                        if (a.buffer == nullptr || a.buffer->getNumSamples() == 0)
                            continue;

                        const auto name = juce::File (a.title).getFileNameWithoutExtension();   // a stem tab's: "Song - Drums"

                        list.push_back ({ name, [doc = juce::Component::SafePointer<DataWindow> (d), page]
                        {
                            if (doc == nullptr)
                                return juce::File();
                            const auto p = doc->getPageAudio (page);
                            return wf::drumRebuilderBeatFile (p.title, p.file, ! p.modified, *p.buffer, p.sampleRate, 0.0);
                        } });
                    }

            return list;
        };
        drumRebuilderWindow = wf::createDrumRebuilderWindow (std::move (host));
    }

    stopPlayback (false);
    wf::loadDrumRebuilderBeat (*drumRebuilderWindow, beat,
                               juce::File (doc->getTitle()).getFileNameWithoutExtension() + (whole ? "" : " (selection)"));
    drumRebuilderWindow->setVisible (true);
    drumRebuilderWindow->toFront (true);
}

// At every start GitHub is asked; when it can't be reached, the last answer
// (kept in the settings) still shows the button and the message.
void MainComponent::checkForUpdates (bool askedByUser)
{
    if (! askedByUser)
    {
        const auto atStart = wf::settings::get ("update.atStart");

        if (atStart.isBool() && ! (bool) atStart)
            return;
    }

    juce::Thread::launch ([safeThis = juce::Component::SafePointer<MainComponent> (this), askedByUser]
    {
        auto newest = wf::updates::fetchNewest();

        juce::MessageManager::callAsync ([safeThis, newest, askedByUser]() mutable
        {
            if (auto* main = safeThis.getComponent())
            {
                if (newest.has_value())
                {
                    wf::settings::set ("update.lastCheck", juce::Time::currentTimeMillis());
                    wf::settings::set ("update.version", newest->version);
                    wf::settings::set ("update.page", newest->pageUrl);
                }
                else if (! askedByUser)
                {
                    // Offline at start: what GitHub said last time.
                    const auto version = wf::settings::get ("update.version").toString();

                    if (version.isNotEmpty())
                        newest = wf::updates::Release { version, wf::settings::get ("update.page").toString() };
                }

                main->updateFound (newest, askedByUser);
            }
        });
    });
}

void MainComponent::updateFound (std::optional<wf::updates::Release> newest, bool askedByUser)
{
    const juce::String current (JUCE_APPLICATION_VERSION_STRING);

    if (newest.has_value() && wf::updates::isNewer (newest->version, current))
    {
        availableUpdate = newest;
        updateButton.setButtonText ("New update: " + newest->version);
        updateButton.setVisible (true);
        resized();

        if (askedByUser || wf::settings::get ("update.skip").toString() != newest->version)
            showUpdateMessage();

        return;
    }

    availableUpdate.reset();
    updateButton.setVisible (false);

    if (askedByUser)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Check for Updates",
                                                newest.has_value() ? "You have the newest version (" + current + ")."
                                                                   : juce::String ("Could not reach GitHub. Check the internet connection and try again."));
}

void MainComponent::showUpdateMessage()
{
    if (! availableUpdate.has_value())
        return;

    const auto release = *availableUpdate;
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::InfoIcon)
                                      .withTitle ("New update")
                                      .withMessage ("Adormis Audio " + release.version + " is out (you have " JUCE_APPLICATION_VERSION_STRING ").\n\n"
                                                    "Download opens its page on GitHub, with the files for Windows, macOS and Linux.")
                                      .withButton ("Download")
                                      .withButton ("Skip this version")
                                      .withButton ("Later"),
                                  [release] (int result)
                                  {
                                      // JUCE: the buttons give 1, 2 and 0 (the last one, also Escape).
                                      if (result == 1)
                                          juce::URL (release.pageUrl).launchInDefaultBrowser();
                                      else if (result == 2)
                                          wf::settings::set ("update.skip", release.version);
                                  });
}

void MainComponent::showEffect (wf::EffectKind kind)
{
    if (effectWindow != nullptr)
    {
        effectWindow->toFront (true);
        return;
    }

    if (toolWindowOpen())
        return;

    auto source = bassFinderSource();

    if (! source.has_value())
        return;

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->stopToolPreview();
                main->effectWindow.reset();
                main->commandManager.commandStatusChanged();
            }
        });
    };

    toolUndoName = lastCommandName;
    effectWindow = wf::createEffectWindow (kind, std::move (*source), toolHost(), std::move (onClose));
    effectWindow->setVisible (true);
    effectWindow->toFront (true);
    commandManager.commandStatusChanged();
}

wf::BassFinderHost MainComponent::toolHost()
{
    wf::BassFinderHost host;
    host.currentSource = [this] { return bassFinderSource(); };

    host.play = [this] (std::unique_ptr<juce::PositionableAudioSource> audio, double rate, int channels)
    {
        startToolPreview (std::move (audio), rate, channels);
    };

    host.stop = [this] { stopToolPreview(); };
    host.playPosition = [this]() -> juce::int64
    {
        return toolPreview != nullptr && transport.isPlaying() ? toolPreview->getNextReadPosition() : -1;
    };

    host.openAsNewFile = [this] (const juce::String& title, juce::AudioBuffer<float> audio, double rate,
                                 std::vector<juce::int64> markers)
    {
        // The Bass Finder's renders have a marker on every bar.
        auto bars = markers;
        addDocument (DataWindow::fromAudio (title, rate, std::move (audio), std::move (markers), std::move (bars)));
    };

    host.chooseSaveFile = [this] (const juce::String& title, const juce::File& suggested, const juce::String& pattern,
                                  std::function<void (juce::File)> onChosen)
    {
        filePicker.launch (title, suggested, pattern,
                           juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                               | juce::FileBrowserComponent::warnAboutOverwriting,
                           [onChosen] (const juce::Array<juce::File>& files)
                           {
                               onChosen (files.isEmpty() ? juce::File() : files.getFirst());
                           });
    };

    host.chooseOpenFile = [this] (const juce::String& title, const juce::File& suggested, const juce::String& pattern,
                                  std::function<void (juce::File)> onChosen)
    {
        filePicker.launch (title, suggested, pattern,
                           juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                           [onChosen] (const juce::Array<juce::File>& files)
                           {
                               onChosen (files.isEmpty() ? juce::File() : files.getFirst());
                           });
    };

    host.runJob = [this] (const juce::String& title, std::function<bool (std::function<bool (float)>)> work,
                          std::function<void (bool)> done)
    {
        runJob (title, std::move (work), std::move (done));
    };

    return host;
}

void MainComponent::showBassFinder()
{
    if (bassFinderWindow != nullptr)
    {
        bassFinderWindow->toFront (true);
        return;
    }

    if (toolWindowOpen())
        return;

    auto source = bassFinderSource();

    if (! source.has_value())
        return;

    auto host = toolHost();

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->stopToolPreview();
                main->bassFinderWindow.reset();
                main->commandManager.commandStatusChanged();
            }
        });
    };

    toolUndoName = lastCommandName;
    bassFinderWindow = wf::createBassFinderWindow (std::move (*source), std::move (host), std::move (onClose));
    bassFinderWindow->setVisible (true);
    bassFinderWindow->toFront (true);
    commandManager.commandStatusChanged();
}

void MainComponent::setInputEnabled (bool enabled)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();

    if (enabled)
    {
        // The app starts with no input open (no microphone prompt / light until
        // recording is wanted), so pick the system's default input now.
        if (setup.inputDeviceName.isEmpty())
            if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            {
                const auto names = type->getDeviceNames (true);
                const auto index = type->getDefaultDeviceIndex (true);

                if (juce::isPositiveAndBelow (index, names.size()))
                    setup.inputDeviceName = names[index];
            }

        setup.inputChannels.setRange (0, 2, true);
    }

    deviceManager.setAudioDeviceSetup (setup, true);
}

void MainComponent::record()
{
    if (recordWindow == nullptr)
        openRecorder();
    else if (isRecording())
        stopTake();
    else
        startTake();
}

// Opens the Record window set to what the computer plays (or switches it there).
void MainComponent::recordComputer()
{
    if (isRecording())
        return;

    if (recordWindow == nullptr)
        openRecorder();

    if (const auto error = setRecordSource (true); error.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Record What the Computer Plays", error);

    if (recordWindow != nullptr)
        recordWindow->toFront (true);
}

void MainComponent::openRecorder()
{
    releasePlaybackSource();
    setInputEnabled (true);
    deviceManager.addAudioCallback (&recorder);

    const auto rate = recorder.getSampleRate() > 0.0 ? recorder.getSampleRate() : 44100.0;
    // Mono by default: built-in and most USB microphones are mono (a stereo
    // take would have a silent right channel). Stereo is one click away.
    auto doc = DataWindow::createEmpty ("Record " + juce::String (++recordCount), rate, 1);
    doc->setResolution (lastRecordBits);
    recordDocument = doc.get();
    addDocument (std::move (doc));

    wf::RecordCallbacks callbacks;
    callbacks.toggleRecording = [this] { isRecording() ? stopTake() : startTake(); };
    callbacks.openAudioSettings = [this] { showAudioSettings(); };
    callbacks.setChannels = [this] (int count)
    {
        if (auto* d = recordDocument.getComponent())
            d->setEmptyFormat (recorder.getSampleRate(), count);
    };
    callbacks.setBits = [this] (int bits)
    {
        if (auto* d = recordDocument.getComponent(); d != nullptr && d->isEmpty())
        {
            d->setResolution (bits);
            lastRecordBits = bits;
        }
    };
    callbacks.fileBits = [this] { auto* d = recordDocument.getComponent(); return d != nullptr ? d->getResolution() : 24; };
    callbacks.takeSeconds = [this]
    {
        auto* d = recordDocument.getComponent();
        return d != nullptr ? (double) d->getTakeLength() / d->getSampleRate() : 0.0;
    };
    callbacks.fileSeconds = [this]
    {
        auto* d = recordDocument.getComponent();
        return d != nullptr ? (double) d->getBuffer().getNumSamples() / d->getSampleRate() : 0.0;
    };
    callbacks.fileIsEmpty = [this] { auto* d = recordDocument.getComponent(); return d == nullptr || d->isEmpty(); };
    callbacks.fileChannels = [this] { auto* d = recordDocument.getComponent(); return d != nullptr ? d->getBuffer().getNumChannels() : 2; };
    callbacks.fileTitle = [this] { auto* d = recordDocument.getComponent(); return d != nullptr ? d->getTitle() : juce::String ("(closed)"); };
    callbacks.systemAudioAvailable = []
    {
#if JUCE_WINDOWS || JUCE_LINUX || JUCE_MAC
        return true;
#else
        return false;
#endif
    };
    callbacks.recordingSystemAudio = [this] { return recordingSystemAudio; };
    callbacks.setSource = [this] (bool system) { return setRecordSource (system); };
    callbacks.onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->closeRecorder();
        });
    };

    recordWindow = wf::createRecordWindow (recorder, std::move (callbacks));
    recordWindow->setVisible (true);
    recordWindow->toFront (false);
    recordPump.tick = [this] { pumpRecording(); };
    commandManager.commandStatusChanged();
}

// Record > what the computer plays: the system's own capture of the default
// output feeds the recorder instead of the audio device's input.
juce::String MainComponent::setRecordSource (bool system)
{
    if (system == recordingSystemAudio || isRecording())
        return {};

    if (system)
    {
        if (systemAudio == nullptr)
            systemAudio = wf::createSystemAudioBackend();

        if (systemAudio == nullptr)
            return "This system can't record what the computer plays.";

        deviceManager.removeAudioCallback (&recorder);
        setInputEnabled (false);
        recorder.externalSourceStarted (systemAudio->sampleRate(), 2, juce::String (systemAudio->name()));
        std::string error;

        if (! systemAudio->start ([this] (const float* const* channels, int n) { recorder.push (channels, n); }, error))
        {
            recorder.externalSourceStopped();
            setInputEnabled (true);
            deviceManager.addAudioCallback (&recorder);
            return juce::String (error);
        }

        // the backend may only know its real rate once it runs
        recorder.externalSourceStarted (systemAudio->sampleRate(), 2, juce::String (systemAudio->name()));
        recordingSystemAudio = true;

        if (auto* d = recordDocument.getComponent(); d != nullptr && d->isEmpty())
            d->setEmptyFormat (systemAudio->sampleRate(), 2);
    }
    else
    {
        systemAudio->stop();
        recorder.externalSourceStopped();
        recordingSystemAudio = false;
        setInputEnabled (true);
        deviceManager.addAudioCallback (&recorder);

        if (auto* d = recordDocument.getComponent(); d != nullptr && d->isEmpty())
            d->setEmptyFormat (recorder.getSampleRate(), d->getBuffer().getNumChannels());
    }

    commandManager.commandStatusChanged();
    return {};
}

void MainComponent::startTake()
{
    auto* doc = recordDocument.getComponent();

    if (doc == nullptr || recorder.getInputChannels() <= 0 || isRecording())
        return;

    // The file takes the input's rate while it is still empty; later takes must match it.
    if (doc->isEmpty())
        doc->setEmptyFormat (recorder.getSampleRate(), doc->getBuffer().getNumChannels());
    else if (! juce::approximatelyEqual (doc->getSampleRate(), recorder.getSampleRate()))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Record",
                                                "The input runs at " + juce::String (juce::roundToInt (recorder.getSampleRate()))
                                                    + " Hz but this file is " + juce::String (juce::roundToInt (doc->getSampleRate()))
                                                    + " Hz. Press Ctrl+R in a new window or change the rate in Audio Device.");
        return;
    }

    releasePlaybackSource();
    doc->beginTake();
    recorder.setRecording (true);
    recordPump.startTimerHz (30);
    commandManager.commandStatusChanged();
}

void MainComponent::pumpRecording()
{
    auto* doc = recordDocument.getComponent();

    if (doc == nullptr)
        return;

    const auto count = recorder.drain (recordChunk);

    if (count <= 0)
        return;

    // Recording in mono from a stereo input: both sides mixed, nothing lost.
    // (A mono microphone arrives with the left copied right, so this keeps it as is.)
    if (doc->getBuffer().getNumChannels() == 1 && recordChunk.getNumChannels() >= 2)
    {
        recordChunk.addFrom (0, 0, recordChunk, 1, 0, count);
        recordChunk.applyGain (0, 0, count, 0.5f);
    }

    doc->appendAudio (recordChunk, count);
}

void MainComponent::stopTake()
{
    if (! isRecording())
        return;

    recorder.setRecording (false);
    recordPump.stopTimer();
    pumpRecording();   // what was still queued

    if (auto* doc = recordDocument.getComponent())
        doc->endTake();

    commandManager.commandStatusChanged();
}

void MainComponent::closeRecorder()
{
    stopTake();

    if (recordingSystemAudio)
    {
        systemAudio->stop();
        recorder.externalSourceStopped();
        recordingSystemAudio = false;
    }

    recordWindow.reset();
    recordDocument = nullptr;
    deviceManager.removeAudioCallback (&recorder);
    setInputEnabled (false);
    commandManager.commandStatusChanged();
}

void MainComponent::startToolPreview (std::unique_ptr<juce::PositionableAudioSource> source, double rate, int channels)
{
    releasePlaybackSource();
    toolPreview = std::move (source);
    transport.setSource (toolPreview.get(), 0, nullptr, rate, juce::jmax (2, channels));
    transport.start();
}

void MainComponent::stopToolPreview()
{
    if (toolPreview == nullptr)
        return;

    transport.stop();
    transport.setSource (nullptr);
    toolPreview.reset();
}

void MainComponent::startMixPreview (DataWindow* target, const juce::AudioBuffer<float>& audio, double sourceRate,
                                     juce::int64 position, std::shared_ptr<wf::MixGains> gains)
{
    if (target == nullptr)
        return;

    releasePlaybackSource();

    // Play from a little before the drop point to a little after the mix ends.
    const auto rate = target->getSampleRate();
    const auto& dest = target->getBuffer();
    auto prepared = DataWindow::prepareForMix (audio, sourceRate, rate, dest.getNumChannels());

    const auto regionStart = juce::jmax ((juce::int64) 0, position - (juce::int64) (1.5 * rate));
    const auto mixEnd = position + prepared.getNumSamples();
    const auto regionEnd = juce::jmin ((juce::int64) dest.getNumSamples(), mixEnd + (juce::int64) (0.5 * rate));

    juce::AudioBuffer<float> region (dest.getNumChannels(), (int) juce::jmax ((juce::int64) 0, regionEnd - regionStart));

    for (int ch = 0; ch < dest.getNumChannels(); ++ch)
        region.copyFrom (ch, 0, dest, ch, (int) regionStart, region.getNumSamples());

    mixPreview = std::make_unique<wf::MixPreviewSource> (std::move (region), std::move (prepared),
                                                          (int) (position - regionStart), std::move (gains));
    mixPreviewDocument = target;
    mixPreviewOffset = regionStart;

    transport.setSource (mixPreview.get(), 0, nullptr, rate, juce::jmax (2, dest.getNumChannels()));
    transport.setPosition (0.0);
    transport.start();
    startTimerHz (30);
}

void MainComponent::openMixWindow (const juce::String& description, std::shared_ptr<wf::MixGains> gains,
                                   wf::MixDialogCallbacks callbacks)
{
    // One Mix window at a time: a new drop replaces the previous one.
    stopMixPreview();
    mixWindow.reset();

    callbacks.onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        // Deleted asynchronously: we are still inside one of its button callbacks.
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
            {
                main->stopMixPreview();
                main->mixWindow.reset();
            }
        });
    };

    mixWindow = wf::createMixWindow (description, std::move (gains), std::move (callbacks));
    mixWindow->setVisible (true);
    mixWindow->toFront (true);
}

void MainComponent::stopMixPreview()
{
    if (mixPreview == nullptr)
        return;

    transport.stop();
    transport.setSource (nullptr);
    mixPreview.reset();

    if (auto* doc = mixPreviewDocument.getComponent())
        doc->getView().setPlayhead (-1);

    mixPreviewDocument = nullptr;
    stopTimer();
}

bool MainComponent::isMixPreviewing() const
{
    return mixPreview != nullptr && transport.isPlaying();
}

void MainComponent::releasePlaybackSource()
{
    stopMixPreview();
    stopToolPreview();

    // The memory source points straight at a document's samples, so detach it
    // (under the audio callback lock) before any buffer is modified or reallocated.
    stopPlayback (false);
    transport.setSource (nullptr);
    memorySource.reset();
    playingDocument = nullptr;
}

void MainComponent::arrangeWindows (bool cascade)
{
    const auto count = workspace->getNumDocuments();

    if (count == 0)
        return;

    if (workspace->isMaximised())
        workspace->restoreWindows();

    // Minimized windows stay in their bars.
    const auto area = workspace->areaAboveBars();
    std::vector<juce::MultiDocumentPanelWindow*> windows;

    for (int i = 0; i < count; ++i)
        if (auto* window = workspace->getWindowFor (workspace->getDocument (i)))
            if (auto* frame = Workspace::frameOf (window); frame == nullptr || ! frame->isMinimisedToBar())
                windows.push_back (window);

    if (windows.empty())
        return;

    for (int i = 0; i < (int) windows.size(); ++i)
    {
        auto* window = windows[(size_t) i];

        if (cascade)
        {
            const auto offset = (i % 10) * cascadeStep;
            window->setBounds (area.getX() + 8 + offset, area.getY() + 8 + offset,
                               juce::jmax (420, area.getWidth() * 2 / 3), juce::jmax (220, area.getHeight() / 2));
        }
        else
        {
            // Stack windows top to bottom, each one full width.
            const auto rowHeight = area.getHeight() / (int) windows.size();
            window->setBounds (area.getX(), area.getY() + i * rowHeight, area.getWidth(), rowHeight);
        }
    }

    if (auto* active = workspace->getWindowFor (activeDocument()))
        active->toFront (true);
}

void MainComponent::cycleWindows (int direction)
{
    const auto count = workspace->getNumDocuments();

    if (count < 2)
        return;

    int current = 0;
    for (int i = 0; i < count; ++i)
        if (workspace->getDocument (i) == workspace->getActiveDocument())
            current = i;

    auto* next = workspace->getDocument ((current + direction + count) % count);
    workspace->setActiveDocument (next);

    if (auto* frame = Workspace::frameOf (workspace->getWindowFor (next)); frame != nullptr && frame->isMinimisedToBar())
        frame->setMinimisedToBar (false);
    else if (auto* window = workspace->getWindowFor (next))
        window->toFront (true);
}

//==============================================================================
juce::int64 MainComponent::playPosition() const
{
    if (auto* doc = playingDocument.getComponent())
        return (juce::int64) (transport.getCurrentPosition() * doc->getSampleRate());

    return 0;
}

namespace
{
// Plays a document through MemoryAudioSource, optionally with only one
// channel audible (the others silenced), for single-channel selections.
class SoloPlaybackSource final : public juce::PositionableAudioSource
{
public:
    SoloPlaybackSource (juce::AudioBuffer<float>& audio, int soloChannelIn)
        : source (audio, false, false), soloChannel (soloChannelIn), mono (audio.getNumChannels() == 1) {}

    void prepareToPlay (int block, double rate) override  { source.prepareToPlay (block, rate); }
    void releaseResources() override                      { source.releaseResources(); }

    // Transport > Loop Playback: playback jumps back to the start of `range`
    // at its end (seamlessly, here on the audio thread). Empty = no loop.
    void setLoopRange (juce::Range<juce::int64> range)
    {
        loopStart = range.getStart();
        loopEnd = range.getEnd();
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const auto start = loopStart.load(), end = loopEnd.load();

        if (end > start)
        {
            // Fill the block piece by piece, wrapping at the loop end.
            int done = 0;

            while (done < info.numSamples)
            {
                auto position = source.getNextReadPosition();

                if (position >= end || position < start)
                {
                    source.setNextReadPosition (start);
                    position = start;
                }

                const auto n = (int) juce::jmin ((juce::int64) (info.numSamples - done), end - position);
                juce::AudioSourceChannelInfo part (info.buffer, info.startSample + done, n);
                source.getNextAudioBlock (part);
                done += n;
            }
        }
        else
        {
            source.getNextAudioBlock (info);
        }

        // A mono file is heard in the centre: the same signal in every speaker.
        if (mono)
            for (int ch = 1; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->copyFrom (ch, info.startSample, *info.buffer, 0, info.startSample, info.numSamples);

        if (soloChannel >= 0 && ! mono)
            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                if (ch != soloChannel)
                    info.buffer->clear (ch, info.startSample, info.numSamples);
    }

    void setNextReadPosition (juce::int64 position) override  { source.setNextReadPosition (position); }
    juce::int64 getNextReadPosition() const override          { return source.getNextReadPosition(); }
    juce::int64 getTotalLength() const override               { return source.getTotalLength(); }
    bool isLooping() const override                           { return source.isLooping(); }
    void setLooping (bool shouldLoop) override                { source.setLooping (shouldLoop); }

private:
    juce::MemoryAudioSource source;
    int soloChannel;
    bool mono;
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };
};
} // namespace

void MainComponent::toggleLoop()
{
    if (loopPlayback)
    {
        setLooping (false);
        return;
    }

    setLooping (true);

    // Q on a selection: that selection plays in a loop straight away
    // (unless it already is what's playing).
    auto* doc = activeDocument();

    if (doc == nullptr || doc->isEmpty() || isRecording())
        return;

    const auto selection = doc->getView().getSelection();

    if (selection.isEmpty())
        return;

    if (! isPlaying() || playingDocument.getComponent() != doc || playRange != selection
        || doc->getView().getSelectedChannel() != playSoloChannel)
        startPlayback (selection, doc->getView().getSelectedChannel());
}

void MainComponent::followSelectionLoop (DataWindow* doc)
{
    // While looping, a new selection in the file being played becomes the loop.
    if (! loopPlayback || ! isPlaying() || doc == nullptr || doc != playingDocument.getComponent() || mixPreview != nullptr)
        return;

    const auto selection = doc->getView().getSelection();

    if (selection.getLength() < 64 || selection == playRange)
        return;

    if (doc->getView().getSelectedChannel() != playSoloChannel)
    {
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<MainComponent> (this),
                                          safeDoc = juce::Component::SafePointer<DataWindow> (doc)]
        {
            auto* main = safeThis.getComponent();
            auto* d = safeDoc.getComponent();

            if (main != nullptr && d != nullptr && main->loopPlayback && main->isPlaying() && ! d->getView().getSelection().isEmpty())
                main->startPlayback (d->getView().getSelection(), d->getView().getSelectedChannel());
        });
        return;
    }

    playRange = selection;

    if (auto* solo = dynamic_cast<SoloPlaybackSource*> (memorySource.get()))
        solo->setLoopRange (playRange);   // playback outside it jumps to its start
}

juce::File MainComponent::keyMappingsFile()
{
    return wf::startup::settingsFolder().getChildFile ("keyboard.xml");
}

void MainComponent::loadKeyMappings()
{
    auto* mappings = commandManager.getKeyMappings();

    if (auto xml = juce::parseXML (keyMappingsFile()))
        mappings->restoreFromXml (*xml);

    // Every change made in Customize Keyboard is kept for the next start.
    keySaver.onChange = [this]
    {
        if (auto xml = commandManager.getKeyMappings()->createXml (true))
        {
            keyMappingsFile().getParentDirectory().createDirectory();
            xml->writeTo (keyMappingsFile());
        }

        shortcutsWindow.reset();   // rebuilt with the new keys next time
        commandManager.commandStatusChanged();
    };
    mappings->addChangeListener (&keySaver);
}

void MainComponent::showKeyboardEditor()
{
    if (keyboardWindow != nullptr)
    {
        keyboardWindow->toFront (true);
        return;
    }

    struct KeyboardWindow final : public juce::DocumentWindow
    {
        KeyboardWindow (juce::KeyPressMappingSet& mappings, std::function<void()> onCloseIn)
            : juce::DocumentWindow ("Customize Keyboard", juce::Colour (0xffd4d0c8), juce::DocumentWindow::closeButton),
              onClose (std::move (onCloseIn))
        {
            // Own title bar so it can be moved on window managers without decorations.
            setUsingNativeTitleBar (false);

            auto* content = new juce::Component();
            auto* hint = new juce::Label ({}, "Open a menu, click a command's \"+\" to add a key (press the key you want),\n"
                                              "click a key to change or remove it. Changes are saved right away.");
            hint->setColour (juce::Label::textColourId, juce::Colours::black);
            hint->setFont (juce::FontOptions (13.0f));
            auto* editor = new juce::KeyMappingEditorComponent (mappings, true);
            editor->setColours (juce::Colour (0xffd4d0c8), juce::Colours::black);
            content->addAndMakeVisible (hint);
            content->addAndMakeVisible (editor);
            owned.add (hint);
            owned.add (editor);
            content->setSize (620, 640);
            hint->setBounds (10, 6, 600, 38);
            editor->setBounds (0, 48, 620, 592);
            setContentOwned (content, true);
            setResizable (false, false);
            setAlwaysOnTop (true);
            centreWithSize (getWidth(), getHeight());
        }

        ~KeyboardWindow() override
        {
            if (auto* c = getContentComponent())
                c->removeAllChildren();
        }

        void closeButtonPressed() override  { if (onClose != nullptr) onClose(); }

        std::function<void()> onClose;
        juce::OwnedArray<juce::Component> owned;
    };

    auto onClose = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* main = safeThis.getComponent())
                main->keyboardWindow.reset();
        });
    };

    keyboardWindow = std::make_unique<KeyboardWindow> (*commandManager.getKeyMappings(), std::move (onClose));
    keyboardWindow->setVisible (true);
    keyboardWindow->toFront (true);
}

void MainComponent::setLooping (bool shouldLoop)
{
    loopPlayback = shouldLoop;

    if (auto* solo = dynamic_cast<SoloPlaybackSource*> (memorySource.get()))
        solo->setLoopRange (loopPlayback ? playRange : juce::Range<juce::int64>());

    commandManager.commandStatusChanged();
}

// A click while the file plays: playback goes on from there (to the end).
void MainComponent::jumpPlayback (DataWindow* doc, juce::int64 sample)
{
    if (! isPlaying() || mixPreview != nullptr || playingDocument.getComponent() != doc)
        return;

    auto* solo = dynamic_cast<SoloPlaybackSource*> (memorySource.get());

    if (solo == nullptr)
        return;

    // A one-channel selection was playing: start over with both channels.
    if (playSoloChannel >= 0)
    {
        startPlayback ({ sample, (juce::int64) doc->getBuffer().getNumSamples() });
        doc->getView().setCursor (sample);
        playStartCursor = sample;
        return;
    }

    playRange = { sample, (juce::int64) doc->getBuffer().getNumSamples() };
    playSoloChannel = -1;
    playStartCursor = sample;
    solo->setLoopRange (loopPlayback ? playRange : juce::Range<juce::int64>());
    transport.setPosition ((double) sample / doc->getSampleRate());
    doc->getView().setPlayhead (sample);
}

void MainComponent::startPlayback (juce::Range<juce::int64> range, int soloChannel)
{
    auto* doc = activeDocument();

    if (doc == nullptr || range.isEmpty())
        return;

    stopPlayback (false);
    stopToolPreview();

    transport.setSource (nullptr);
    auto solo = std::make_unique<SoloPlaybackSource> (doc->getBuffer(), soloChannel);

    if (loopPlayback)
        solo->setLoopRange (range);

    memorySource = std::move (solo);
    // At least two channels, so a mono file played at another rate than the
    // device's still reaches both speakers through the resampler.
    transport.setSource (memorySource.get(), 0, nullptr, doc->getSampleRate(), juce::jmax (2, doc->getBuffer().getNumChannels()));

    if (meters != nullptr)
        meters->resetLoudness();

    playingDocument = doc;
    playRange = range;
    playSoloChannel = soloChannel;
    playStartCursor = doc->getView().getCursor();
    transport.setPosition ((double) range.getStart() / doc->getSampleRate());
    transport.start();
    smoothPosition = -1.0;
    startTimerHz (playbackTimerHz());
    commandManager.commandStatusChanged();
}

void MainComponent::stopPlayback (bool returnToStart)
{
    if (! isPlaying() && ! isTimerRunning())
        return;

    const auto stoppedAt = playPosition();
    transport.stop();
    stopTimer();

    if (auto* doc = playingDocument.getComponent())
    {
        auto& view = doc->getView();
        view.setPlayhead (-1);

        // Space/Esc return the cursor to where playback began (Sound Forge);
        // Enter (pause) leaves it where playback stopped.
        if (returnToStart)
            view.setCursor (playStartCursor);
        else if (view.getSelection().isEmpty())
            view.setCursor (stoppedAt);
    }

    updateStatus();
    commandManager.commandStatusChanged();
}

void MainComponent::timerCallback()
{
    if (mixPreview != nullptr)
    {
        const auto position = mixPreview->getNextReadPosition();
        auto* target = mixPreviewDocument.getComponent();

        if (target == nullptr || ! transport.isPlaying() || position >= mixPreview->getTotalLength())
        {
            stopMixPreview();
            return;
        }

        target->getView().setPlayhead (juce::jmin (mixPreviewOffset + position, (juce::int64) target->getBuffer().getNumSamples()));
        return;
    }

    auto* doc = playingDocument.getComponent();
    const auto position = playPosition();

    if (doc == nullptr || ! transport.isPlaying() || (position >= playRange.getEnd() && ! loopPlayback))
    {
        stopPlayback (true);
        return;
    }

    doc->getView().setPlayhead (position);

    if (followPlayback && smoothScrolling)
    {
        // The device reports its position once per audio block; in between,
        // the time that passed moves it on, so the view glides.
        const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        const auto reported = (double) position;

        if (smoothPosition < 0.0 || std::abs (reported - smoothPosition) > 0.1 * doc->getSampleRate())
            smoothPosition = reported;   // a start or a jump
        else
            smoothPosition = std::max (smoothPosition + (now - smoothClock) * doc->getSampleRate() * 0.98
                                           + (reported - smoothPosition) * 0.1,
                                       smoothPosition);

        smoothClock = now;
        doc->getView().centreOn (smoothPosition);
    }
    else if (followPlayback)
    {
        doc->getView().ensureVisible (position);
    }

    statusBar.setText ("  Playing " + doc->getTitle() + "   "
                           + WaveformView::formatTime ((double) position / doc->getSampleRate()),
                       juce::dontSendNotification);
}

//==============================================================================
void MainComponent::updateTitle()
{
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
    {
        auto* doc = activeDocument();
        // the version in the title, so it's clear which build is running
        const auto app = juce::String ("Adormis Audio ") + JUCE_APPLICATION_VERSION_STRING;
        window->setName (doc != nullptr ? doc->getName() + " - " + app : app);
    }
}

void MainComponent::updateStatus()
{
    auto* doc = activeDocument();

    if (doc == nullptr)
    {
       #if JUCE_MAC
        statusBar.setText ("  Ready - File > Open (Cmd+O) to load audio", juce::dontSendNotification);
       #else
        statusBar.setText ("  Ready - File > Open (Ctrl+O) to load audio", juce::dontSendNotification);
       #endif
        return;
    }

    const auto rate = doc->getSampleRate();
    auto seconds = [rate] (juce::int64 s) { return WaveformView::formatTime ((double) s / rate); };

    juce::String text;
    text << "  " << doc->describeFormat() << "      ";

    const auto selection = doc->getView().getSelection();

    if (selection.isEmpty())
        text << "Cursor " << seconds (doc->getView().getCursor());
    else
        text << "Selection " << seconds (selection.getStart()) << " - " << seconds (selection.getEnd())
             << "  (" << seconds (selection.getLength()) << ")";

    if (const auto channel = doc->getView().getSelectedChannel(); channel >= 0)
        text << (channel == 0 ? "  Left channel" : "  Right channel");

    statusBar.setText (text, juce::dontSendNotification);
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& commands)
{
    for (const auto& def : wf::allCommands())
        commands.add (def.id);
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto* def = wf::findCommand (id);

    if (def == nullptr)
        return;

    info.setInfo (def->name, def->name, def->category, 0);

    auto* doc = activeDocument();
    const auto numDocuments = workspace != nullptr ? workspace->getNumDocuments() : 0;
    bool available = def->implemented;

    switch (id)
    {
        case wf::fileNew: case wf::fileOpen: case wf::fileExit: case wf::optAudioDevice: case wf::optKeyboard: case wf::toolsBatch:
        case wf::helpShortcuts: case wf::helpAbout: case wf::optRecord: case wf::optRecordComputer: case wf::helpCheckUpdates:
            break;

        case wf::helpUpdatesAtStart:
            info.setTicked (! wf::settings::get ("update.atStart").isBool() || (bool) wf::settings::get ("update.atStart"));
            break;

        case wf::editSnapZero:   // an option: always there
            info.setTicked (WaveformView::snapToZeroCrossings);
            break;

        case wf::viewUndoHistory:
            info.setTicked (undoHistoryWindow != nullptr);
            break;

        case wf::viewPlaybackScroll:
            info.setTicked (followPlayback);
            break;

        case wf::viewSmoothScroll:
            info.setTicked (followPlayback && smoothScrolling);
            break;

        case wf::editUndo:  available = available && doc != nullptr && doc->canUndo(); break;
        case wf::editRedo:  available = available && doc != nullptr && doc->canRedo(); break;
        case wf::editDelete: available = available && doc != nullptr
                                         && ((! doc->getView().getSelection().isEmpty() && doc->getView().getSelectedChannel() < 0)
                                             || ! doc->getView().getSelectedMarkers().empty()); break;
        case wf::optStop:   available = available && isPlaying(); break;

        case wf::editCopy: case wf::editHalveSelection: case wf::editDoubleSelection:
            available = available && doc != nullptr && ! doc->getView().getSelection().isEmpty();
            break;

        // Cut and Trim change the file's length, so not on one channel only.
        case wf::editCut: case wf::editTrim:
            available = available && doc != nullptr && ! doc->getView().getSelection().isEmpty()
                        && doc->getView().getSelectedChannel() < 0;
            break;

        case wf::editPaste:      available = available && doc != nullptr && hasClipboard(); break;
        case wf::editOverwrite:  available = available && doc != nullptr && hasClipboard(); break;
        case wf::editPasteToNew:
        {
            const bool selected = doc != nullptr && ! doc->getView().getSelection().isEmpty();
            available = available && (selected || hasClipboard());
            if (selected)
                info.shortName = "Selection to New File";
            break;
        }
        case wf::editMix:        available = available && doc != nullptr && hasClipboard() && mixWindow == nullptr; break;
        case wf::editRepeat:
            available = available && doc != nullptr && lastAction != nullptr;
            if (lastAction != nullptr)
                info.shortName = "Repeat " + lastActionName;
            break;

        case wf::fileNewNoDialog: break;
        case wf::fileSaveAll:     available = available && numDocuments > 0; break;

        case wf::optLoop:
            info.setTicked (loopPlayback);
            available = true;   // the switch works with or without a file
            break;

        case wf::viewChannelMeters:
            info.setTicked (meters != nullptr && meters->showsChannels());
            break;

        case wf::viewLoudnessMeters:
            info.setTicked (meters != nullptr && meters->showsLoudness());
            break;

        case wf::toolsStatistics:
            available = available && doc != nullptr && job == nullptr && ! doc->isEmpty();
            break;

        case wf::toolsCaptureNoise: case wf::insertRegion:
            available = available && doc != nullptr && ! doc->getView().getSelection().isEmpty();
            break;

        case wf::viewRegionsList:
            info.setTicked (regionsWindow != nullptr);
            break;

        case wf::viewSpectrum:
            info.setTicked (spectrumWindow != nullptr);
            break;

        case wf::toolsExtractRegions:
            available = available && doc != nullptr && ! doc->getRegions().empty() && job == nullptr;
            break;

        // Conversions change the whole file: not while a tool works on a copy of it.
        case wf::procResample: case wf::procBitDepth: case wf::procChannelConv: case wf::procAutoTrim:
        case wf::insertSilence:
            available = available && doc != nullptr && ! toolWindowOpen() && job == nullptr && ! doc->isEmpty();
            break;

        case wf::toolsRepairCopyChannel:
            available = available && doc != nullptr && doc->getBuffer().getNumChannels() == 2
                        && doc->getView().getSelectedChannel() >= 0;
            break;

        // Bass Finder works on a copy of the audio and can write it back, so
        // the file mustn't be stretched meanwhile: one or the other.
        case wf::procTimeStretch: case wf::fxPitchShift:
            available = available && doc != nullptr && ! toolWindowOpen() && job == nullptr && doc->getView().getSelectedChannel() < 0;
            break;
        case wf::toolsBassFinder: available = available && doc != nullptr && (bassFinderWindow != nullptr || ! toolWindowOpen()) && job == nullptr; break;
        case wf::toolsDrumRebuilder: available = available && doc != nullptr && job == nullptr; break;
        case wf::toolsGetStems:      available = available && doc != nullptr && ! doc->hasStemPages() && ! doc->isStem() && job == nullptr; break;
        case wf::toolsDeEsser:    available = available && doc != nullptr && (deEsserWindow != nullptr || ! toolWindowOpen()) && job == nullptr; break;

        case wf::procEqParametric: case wf::procEqGraphic: case wf::procFadeGraphic: case wf::insertVolumeEnv: case wf::fxCompressor: case wf::fxDelay: case wf::fxReverb: case wf::toolsMp3Restore:
        case wf::fxLimiter: case wf::fxGate: case wf::fxChorus: case wf::fxFlange: case wf::fxVibrato: case wf::fxDistortion:
        case wf::toolsNoiseReduction: case wf::toolsHumRemoval: case wf::toolsDeclick: case wf::toolsClipRestore:
            available = available && doc != nullptr && (effectWindow != nullptr || ! toolWindowOpen()) && job == nullptr;
            break;

        case wf::winCascade: case wf::winTile:
            available = available && numDocuments > 0;
            break;

        case wf::winNext: case wf::winPrev:
            available = available && numDocuments > 1;
            break;

        default:
            available = available && doc != nullptr;   // everything else works on the active file
            break;
    }

    // An empty file (waiting to be recorded into) has nothing to edit or play;
    // while recording, only Record (to stop), the windows and zoom work.
    const auto editsOrPlays = (id >= wf::editUndo && id < wf::favPluginManager) || (id >= wf::optPlay && id <= wf::optGoEnd);
    const auto stillWorksWhileRecording = id == wf::optRecord || id == wf::helpShortcuts || id == wf::helpAbout
                                       || id == wf::viewChannelMeters || id == wf::viewLoudnessMeters || id == wf::viewRegionsList || id == wf::viewSpectrum
                                       || id == wf::viewUndoHistory
                                       || (id >= wf::winCascade && id <= wf::winPrev) || (id >= wf::viewZoomIn && id <= wf::viewZoomNormal);

    if (editsOrPlays && id != wf::optRecord && id != wf::viewChannelMeters && id != wf::viewLoudnessMeters && id != wf::viewRegionsList
        && id != wf::viewSpectrum && id != wf::viewUndoHistory && id != wf::editSnapZero && id != wf::viewPlaybackScroll && id != wf::viewSmoothScroll
        && id != wf::editPaste && id != wf::editMix
        && doc != nullptr && doc->isEmpty())
        available = false;

    if (isRecording() && ! stillWorksWhileRecording)
        available = false;

    info.setActive (available);

    for (const auto& key : def->keys)
        info.addDefaultKeypress (key.getKeyCode(), key.getModifiers());
}

bool MainComponent::perform (const InvocationInfo& info)
{
    auto* doc = activeDocument();
    const auto total = doc != nullptr ? (juce::int64) doc->getBuffer().getNumSamples() : 0;

    // An edit the command makes right away is named after it in the undo
    // history (windows that edit later name their step themselves).
    lastCommandName = commandManager.getNameOfCommand (info.commandID).upToLastOccurrenceOf ("...", false, false).trim();

    struct NameUndo
    {
        juce::Component::SafePointer<DataWindow> d;
        NameUndo (DataWindow* w, const juce::String& n) : d (w)   { if (w != nullptr) w->setUndoName (n); }
        ~NameUndo()                                               { if (auto* w = d.getComponent()) w->setUndoName ({}); }
    } nameUndo (doc, lastCommandName);

    switch (info.commandID)
    {
        case wf::fileNew:           showNewFileDialog(); return true;
        case wf::fileOpen:          openFileDialog(); return true;
        case wf::fileClose:         closeActiveDocument(); return true;
        case wf::fileSave:          saveDocument (doc, false); return true;
        case wf::fileSaveAs:        saveDocument (doc, true); return true;
        case wf::fileExit:          juce::JUCEApplication::getInstance()->systemRequestedQuit(); return true;
        case wf::optAudioDevice:    showAudioSettings(); return true;
        case wf::optKeyboard:       showKeyboardEditor(); return true;
        case wf::optRecord:         record(); return true;
        case wf::optRecordComputer: recordComputer(); return true;
        case wf::helpShortcuts:     showShortcuts(); return true;
        case wf::helpAbout:         showAbout(); return true;
        case wf::helpCheckUpdates:  checkForUpdates (true); return true;
        case wf::helpUpdatesAtStart:
        {
            const auto v = wf::settings::get ("update.atStart");
            wf::settings::set ("update.atStart", v.isBool() && ! (bool) v);
            commandManager.commandStatusChanged();
            return true;
        }
        case wf::winCascade:        arrangeWindows (true); return true;
        case wf::winTile:           arrangeWindows (false); return true;
        case wf::winNext:           cycleWindows (1); return true;
        case wf::winPrev:           cycleWindows (-1); return true;
        case wf::optStop:           stopPlayback (true); return true;

        case wf::fileNewNoDialog:
        {
            const auto& t = lastTemplate;
            addDocument (DataWindow::createSilent ("Untitled " + juce::String (++untitledCount), t.sampleRate, t.channels,
                                                   t.lengthInSamples(), t.markerPositions(), t.barStartPositions(), t.bits));
            return true;
        }

        case wf::fileSaveAll:
        {
            std::vector<juce::Component::SafePointer<DataWindow>> docs;

            for (int i = 0; i < workspace->getNumDocuments(); ++i)
                if (auto* d = dynamic_cast<DataWindow*> (workspace->getDocument (i)))
                    docs.emplace_back (d);

            saveAll (std::move (docs));
            return true;
        }

        case wf::optLoop:           toggleLoop(); return true;

        // With a selection in the active file: a new file holding it (the
        // clipboard is left alone); without one, the clipboard as before.
        case wf::editPasteToNew:
            if (doc != nullptr && ! doc->getView().getSelection().isEmpty())
            {
                auto part = DataWindow::fromAudio ("Untitled " + juce::String (++untitledCount), doc->getSampleRate(),
                                                   doc->copySelection(), {});
                if (part != nullptr)
                    part->addMarks (doc->selectionMarks(), 0);
                addDocument (std::move (part));
            }
            else if (hasClipboard())
            {
                auto pasted = DataWindow::fromAudio ("Untitled " + juce::String (++untitledCount), clipboard.sampleRate,
                                                     clipboard.audio, {});

                if (pasted != nullptr)
                    pasted->addMarks (clipboard.marks, 0);

                addDocument (std::move (pasted));
            }
            return true;

        default: break;
    }

    if (doc == nullptr)
        return false;

    auto& view = doc->getView();

    switch (info.commandID)
    {
        case wf::editUndo:          releasePlaybackSource(); doc->undo(); return true;
        case wf::editRedo:          releasePlaybackSource(); doc->redo(); return true;
        case wf::editDelete:
            if (doc->deleteSelectedMarkers())   // picked markers go first; audio stays
                return true;

            releasePlaybackSource();
            doc->deleteSelection();
            return true;
        case wf::editCopy:
        case wf::editCut:
            if (view.getSelection().isEmpty())
                return true;

            clipboard = { doc->copySelection(), doc->getSampleRate(), doc->selectionMarks() };

            if (info.commandID == wf::editCut)
            {
                releasePlaybackSource();
                doc->deleteSelection();
            }

            commandManager.commandStatusChanged();
            return true;

        case wf::editPaste:
            if (hasClipboard())
            {
                releasePlaybackSource();
                doc->pasteAudio (clipboard.audio, clipboard.sampleRate, &clipboard.marks);
            }
            return true;

        case wf::editOverwrite:
            if (hasClipboard())
            {
                releasePlaybackSource();
                doc->overwriteAudio (clipboard.audio, clipboard.sampleRate, &clipboard.marks);
            }
            return true;

        case wf::editTrim:
            releasePlaybackSource();
            doc->trimToSelection();
            return true;

        case wf::editMix:           mixClipboard(); return true;
        case wf::editRepeat:
            if (lastAction != nullptr)
            {
                releasePlaybackSource();
                lastAction (*doc);
            }
            return true;
        case wf::editSetSelection:  showSetSelection(); return true;
        case wf::editGoTo:          showGoTo(); return true;
        case wf::editMarkIn:        markInOut (true); return true;
        case wf::editMarkOut:       markInOut (false); return true;
        case wf::viewLevelIn:       view.levelZoomIn(); return true;
        case wf::viewLevelOut:      view.levelZoomOut(); return true;
        case wf::insertSilence:     showInsertSilence(); return true;
        case wf::procResample:      showResample(); return true;
        case wf::procBitDepth:      showBitDepth(); return true;
        case wf::procChannelConv:   showChannelConverter(); return true;
        case wf::procAutoTrim:      showAutoTrim(); return true;
        case wf::toolsClipDetect:   showDetectClipping(); return true;
        case wf::toolsRepairCopyChannel:
            releasePlaybackSource();
            doc->copyOtherChannel();
            return true;

        case wf::editSelectAll:     view.selectAll(); return true;
        // With a selection the marker goes where it ends.
        case wf::insertMarker:      doc->addMarker (view.getSelection().isEmpty() ? view.getCursor() : view.getSelection().getEnd()); return true;

        case wf::viewZoomIn:        view.zoomIn(); return true;
        case wf::viewZoomOut:       view.zoomOut(); return true;
        case wf::viewZoomSelection: view.zoomToSelection(); return true;
        case wf::viewZoomNormal:    view.zoomToFit(); return true;

        case wf::procFadeIn:        process (wf::Process::fadeIn); return true;
        case wf::procFadeOut:       process (wf::Process::fadeOut); return true;
        case wf::procMute:          process (wf::Process::mute); return true;
        case wf::procNormalize:     showNormalize(); return true;
        case wf::toolsStatistics:   showStatistics(); return true;
        case wf::viewChannelMeters:  setMeterSections (! meters->showsChannels(), meters->showsLoudness()); return true;
        case wf::viewLoudnessMeters: setMeterSections (meters->showsChannels(), ! meters->showsLoudness()); return true;
        case wf::procReverse:       process (wf::Process::reverse); return true;
        case wf::procInvert:        process (wf::Process::invert); return true;
        case wf::procDcOffset:      process (wf::Process::dcOffset); return true;
        case wf::procVolume:        showVolumeDialog(); return true;
        case wf::procTimeStretch:   showStretchDialog(); return true;
        case wf::toolsBassFinder:   showBassFinder(); return true;
        case wf::toolsDrumRebuilder: showDrumRebuilder(); return true;
        case wf::toolsGetStems:     getStems (activeDocument()); return true;
        case wf::toolsDeEsser:      showDeEsser(); return true;
        case wf::procEqParametric:  showEffect (wf::EffectKind::eq); return true;
        case wf::procEqGraphic:     showEffect (wf::EffectKind::graphicEq); return true;
        case wf::procFadeGraphic: case wf::insertVolumeEnv: showEffect (wf::EffectKind::envelope); return true;
        case wf::fxCompressor:      showEffect (wf::EffectKind::compressor); return true;
        case wf::fxDelay:           showEffect (wf::EffectKind::delay); return true;
        case wf::fxReverb:          showEffect (wf::EffectKind::reverb); return true;
        case wf::toolsMp3Restore:   showEffect (wf::EffectKind::mp3Restore); return true;
        case wf::fxLimiter:         showEffect (wf::EffectKind::limiter); return true;
        case wf::fxGate:            showEffect (wf::EffectKind::gate); return true;
        case wf::fxChorus:          showEffect (wf::EffectKind::chorus); return true;
        case wf::fxFlange:          showEffect (wf::EffectKind::flanger); return true;
        case wf::fxVibrato:         showEffect (wf::EffectKind::vibrato); return true;
        case wf::fxDistortion:      showEffect (wf::EffectKind::distortion); return true;
        case wf::fxPitchShift:      showPitchShift(); return true;
        case wf::toolsNoiseReduction: showEffect (wf::EffectKind::noiseReduction); return true;
        case wf::toolsHumRemoval:   showEffect (wf::EffectKind::humRemoval); return true;
        case wf::toolsDeclick:      showEffect (wf::EffectKind::declick); return true;
        case wf::toolsClipRestore:  showEffect (wf::EffectKind::clipRestore); return true;
        case wf::toolsCaptureNoise: captureNoisePrint(); return true;
        case wf::insertRegion:      insertRegion(); return true;
        case wf::viewRegionsList:   toggleRegionsList(); return true;
        case wf::viewSpectrum:      toggleSpectrum(); return true;
        case wf::viewUndoHistory:   toggleUndoHistory(); return true;

        case wf::viewPlaybackScroll: case wf::viewSmoothScroll:
            if (info.commandID == wf::viewPlaybackScroll)
                followPlayback = ! followPlayback;
            else
            {
                smoothScrolling = ! (followPlayback && smoothScrolling);
                followPlayback = true;
            }

            wf::settings::set ("view.followPlayback", followPlayback);
            wf::settings::set ("view.smoothScrolling", smoothScrolling);
            smoothPosition = -1.0;

            if (isPlaying() && mixPreview == nullptr)
                startTimerHz (playbackTimerHz());

            commandManager.commandStatusChanged();
            return true;

        case wf::editSnapZero:
            WaveformView::snapToZeroCrossings = ! WaveformView::snapToZeroCrossings;
            wf::settings::set ("edit.snapZero", WaveformView::snapToZeroCrossings);
            updateStatus();
            commandManager.commandStatusChanged();
            return true;
        case wf::toolsAutoRegion:   showAutoRegion(); return true;
        case wf::toolsExtractRegions: extractRegions(); return true;
        case wf::toolsBatch:        showBatch(); return true;

        case wf::optPlay:
            if (isPlaying())
                stopPlayback (true);
            else
                startPlayback (view.getSelection().isEmpty() ? juce::Range<juce::int64> (view.getCursor(), total)
                                                             : view.getSelection(),
                               view.getSelectedChannel());
            return true;

        // The toolbar's Play: while playing it starts again from the cursor.
        case wf::optPlayFromCursor:
            startPlayback (view.getSelection().isEmpty() ? juce::Range<juce::int64> (view.getCursor(), total) : view.getSelection(),
                           view.getSelectedChannel());
            return true;

        case wf::optPlayAll:
            if (isPlaying())
                stopPlayback (true);
            else
                startPlayback ({ 0, total });
            return true;

        case wf::optPlayPause:
            if (isPlaying())
                stopPlayback (false);
            else
                startPlayback ({ view.getCursor(), total });
            return true;

        case wf::optGoStart:        view.setSelection ({}); view.setCursor (0); view.ensureVisible (0); return true;
        case wf::optGoEnd:          view.setSelection ({}); view.setCursor (total); view.ensureVisible (total); return true;

        // W / E: the left / right edge of the selection (it stays selected),
        // or the start / end of the file when nothing is selected.
        case wf::optGoLeftEdge: case wf::optGoRightEdge:
        {
            const auto selection = view.getSelection();
            const auto left = info.commandID == wf::optGoLeftEdge;
            const auto to = selection.isEmpty() ? (left ? (juce::int64) 0 : total)
                                                : (left ? selection.getStart() : selection.getEnd());
            view.setCursor (to);

            if (! selection.isEmpty())
            {
                view.setSelection (selection, view.getSelectedChannel());
                view.pickEdge (left ? -1 : 1);   // Shift + arrows now move this end
            }

            view.ensureVisible (to);
            return true;
        }

        // ; halves the selection, ' doubles it (from its start; the length in
        // beats on the toolbar halves / doubles with it).
        case wf::editHalveSelection: case wf::editDoubleSelection:
        {
            const auto selection = view.getSelection();

            if (selection.isEmpty())
                return true;

            const auto length = info.commandID == wf::editHalveSelection ? std::max ((juce::int64) 1, selection.getLength() / 2)
                                                                          : selection.getLength() * 2;
            auto end = std::min (total, selection.getStart() + length);

            if (WaveformView::snapToZeroCrossings)
                end = view.nearestZeroCrossing (end, view.getSelectedChannel());

            view.setSelection ({ selection.getStart(), std::max (selection.getStart() + 1, end) }, view.getSelectedChannel());
            view.setCursor (view.getSelection().getEnd());   // M then marks where it ends
            view.ensureVisible (end);
            return true;
        }

        // Arrows: a step of a few screen pixels, so it follows the zoom.
        case wf::optCursorLeft: case wf::optCursorRight:
        {
            const auto direction = info.commandID == wf::optCursorLeft ? -1 : 1;

            // While it plays: playback jumps back / ahead (held down, it winds
            // through the audio like a CD player), then plays on at normal speed.
            // A jump is the arrow step, but at least a fifth of a second.
            if (isPlaying() && playingDocument.getComponent() == doc && mixPreview == nullptr)
            {
                const auto step = std::max (view.arrowStep(), (juce::int64) (0.2 * doc->getSampleRate()));
                const auto to = juce::jlimit ((juce::int64) 0, total - 1, playPosition() + direction * step);
                view.setCursor (to);
                jumpPlayback (doc, to);
                view.ensureVisible (to);
                return true;
            }

            // With a selection the arrow goes to that end of it and picks it;
            // Shift + arrows then move that end.
            if (const auto selection = view.getSelection(); ! selection.isEmpty())
            {
                const auto edge = direction < 0 ? selection.getStart() : selection.getEnd();
                view.setCursor (edge);
                view.pickEdge (direction);
                view.ensureVisible (edge);
                return true;
            }

            view.nudgeCursor (direction, false);
            return true;
        }

        case wf::optExtendLeft:     view.nudgeCursor (-1, true);  return true;
        case wf::optExtendRight:    view.nudgeCursor (1, true);   return true;

        // Shift + arrows: the selection grows / shrinks by the arrow step at
        // its right end (or the end picked with W / E); with nothing selected
        // they start one, as Extend does.
        case wf::optMoveSelectionLeft: case wf::optMoveSelectionRight:
        {
            const auto direction = info.commandID == wf::optMoveSelectionLeft ? -1 : 1;
            if (view.getSelection().isEmpty())
                view.nudgeCursor (direction, true);
            else
                view.moveEdge (direction);   // the right end, or the one picked
            return true;
        }

        // Alt + Shift + arrows: the whole selection moves (same length).
        case wf::optSlideSelectionLeft: case wf::optSlideSelectionRight:
            view.moveSelection (info.commandID == wf::optSlideSelectionLeft ? -1 : 1);
            return true;
        default: break;
    }

    return false;
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return wf::menuBarNames();
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex, const juce::String&)
{
    return wf::buildMenu (topLevelMenuIndex, commandManager);
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (deviceManager, 0, 2, 1, 2, false, false, true, false);
    selector->setSize (520, 360);

   #if JUCE_WINDOWS
    // ASIO (audio interfaces) is off unless turned on here; it takes effect
    // at the next start, when the drivers are loaded.
    struct Panel final : public juce::Component
    {
        Panel (std::unique_ptr<juce::Component> sel, juce::File flag) : selector (std::move (sel)), flagFile (flag)
        {
            asio.setButtonText ("Use ASIO drivers (audio interfaces) - after a restart");
            asio.setColour (juce::ToggleButton::textColourId, juce::Colours::black);
            asio.setToggleState (flagFile.existsAsFile(), juce::dontSendNotification);
            asio.onClick = [this]
            {
                if (asio.getToggleState())
                    flagFile.replaceWithText ("on");
                else
                    flagFile.deleteFile();

                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "ASIO",
                    asio.getToggleState() ? "Restart Adormis Audio to see the ASIO drivers here."
                                          : "ASIO is off from the next start.");
            };
            addAndMakeVisible (asio);
            addAndMakeVisible (*selector);
            setSize (selector->getWidth(), selector->getHeight() + 40);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            asio.setBounds (r.removeFromTop (34).reduced (12, 4));
            selector->setBounds (r);
        }

        std::unique_ptr<juce::Component> selector;
        juce::File flagFile;
        juce::ToggleButton asio;
    };

    auto content = std::make_unique<Panel> (std::move (selector), asioFlagFile());
   #else
    auto content = std::move (selector);
   #endif

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content.release());
    options.dialogTitle = "Audio Device";
    options.dialogBackgroundColour = panelColour;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::showAbout()
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                            "About Adormis Audio",
                                            "Adormis Audio " JUCE_APPLICATION_VERSION_STRING "\n\n"
                                            "A destructive audio editor with a classic layout.\n"
                                            "Licensed under the GNU AGPL v3.\n\n"
                                            "Built with JUCE " + juce::SystemStats::getJUCEVersion().fromLastOccurrenceOf ("v", false, false));
}

void MainComponent::showShortcuts()
{
    if (shortcutsWindow == nullptr)
        shortcutsWindow = std::make_unique<ShortcutsWindow> (*commandManager.getKeyMappings());

    shortcutsWindow->setVisible (true);
    shortcutsWindow->toFront (true);
}
