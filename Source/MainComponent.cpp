#include "MainComponent.h"
#include "BackgroundPattern.h"
#include "Commands.h"
#include "MixDialog.h"
#include "ShortcutsWindow.h"

namespace
{
constexpr int menuBarHeight   = 24;
constexpr int statusBarHeight = 22;
constexpr int cascadeStep     = 26;

const juce::Colour panelColour (0xffd4d0c8);
}

//==============================================================================
class MainComponent::Workspace final : public juce::MultiDocumentPanel,
                                       public juce::DragAndDropTarget
{
public:
    explicit Workspace (MainComponent& ownerIn) : owner (ownerIn)
    {
        setLayoutMode (FloatingWindows);
        setBackgroundColour (juce::Colours::transparentBlack);
    }

    void paint (juce::Graphics& g) override
    {
        const auto scale = (float) g.getInternalContext().getPhysicalPixelScaleFactor();

        if (tile.isNull() || ! juce::approximatelyEqual (tileScale, scale))
        {
            tile = wf::makeBackgroundTile (scale);
            tileScale = scale;
        }

        g.setFillType (juce::FillType (tile, juce::AffineTransform::scale (1.0f / scale)));
        g.fillAll();

        // Slightly darker than the data windows so they stand out.
        g.fillAll (juce::Colours::black.withAlpha (0.18f));
    }

    void activeDocumentChanged() override  { owner.activeDocumentChanged(); }

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

        // Another window: mix into it at the previewed (possibly snapped) spot,
        // after asking for volumes.
        int snappedTo = -1;
        const auto position = planMix (*sourceDoc, *targetDoc, details.localPosition, snappedTo);
        auto audio = std::make_shared<juce::AudioBuffer<float>> (sourceDoc->copySelection());
        const auto sourceRate = sourceDoc->getSampleRate();

        const auto description = "Mix " + WaveformView::formatTime ((double) audio->getNumSamples() / sourceRate).substring (3)
                               + " from \"" + sourceDoc->getTitle() + "\"\ninto \"" + targetDoc->getTitle() + "\" at "
                               + WaveformView::formatTime ((double) position / targetDoc->getSampleRate()) + ".";

        wf::showMixDialog (description, [this, target = juce::Component::SafePointer<DataWindow> (targetDoc),
                                         audio, sourceRate, position] (wf::MixSettings settings)
        {
            if (auto* doc = target.getComponent())
            {
                owner.releasePlaybackSource();
                doc->mixIn (*audio, sourceRate, position, settings.sourceGain, settings.destGain);
                setActiveDocument (doc);

                if (auto* window = getWindowFor (doc))
                    window->toFront (true);
            }
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
                if (window->isVisible() && window->getBounds().contains (position))
                    return dynamic_cast<DataWindow*> (window->getContentComponent());

        return nullptr;
    }

    juce::MultiDocumentPanelWindow* getWindowFor (juce::Component* document) const
    {
        return document != nullptr ? document->findParentComponentOfClass<juce::MultiDocumentPanelWindow>() : nullptr;
    }

private:
    MainComponent& owner;
    juce::Component::SafePointer<DataWindow> previewDoc;
    juce::Image tile;
    float tileScale = 0.0f;
};

//==============================================================================
MainComponent::MainComponent()
{
    formatManager.registerBasicFormats();

    commandManager.registerAllCommandsForTarget (this);
    commandManager.setFirstCommandTarget (this);
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
    addAndMakeVisible (*toolbar);

    statusBar.setColour (juce::Label::backgroundColourId, panelColour);
    statusBar.setColour (juce::Label::textColourId, juce::Colours::black);
    addAndMakeVisible (statusBar);

    deviceManager.initialiseWithDefaultDevices (0, 2);
    sourcePlayer.setSource (&transport);
    deviceManager.addAudioCallback (&sourcePlayer);

    setWantsKeyboardFocus (true);
    setSize (1100, 700);
    updateStatus();
}

MainComponent::~MainComponent()
{
    stopTimer();
    transport.stop();
    transport.setSource (nullptr);
    deviceManager.removeAudioCallback (&sourcePlayer);
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
    toolbar->setBounds (area.removeFromBottom (ProcessToolbar::preferredHeight));
    workspace->setBounds (area);
}

//==============================================================================
DataWindow* MainComponent::activeDocument() const
{
    // Commands are registered (and queried) before the workspace exists.
    return workspace != nullptr ? dynamic_cast<DataWindow*> (workspace->getActiveDocument()) : nullptr;
}

bool MainComponent::loadFile (const juce::File& file)
{
    juce::String error;
    auto doc = DataWindow::load (file, formatManager, error);

    if (doc == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open", error);
        return false;
    }

    addDocument (std::move (doc));
    return true;
}

void MainComponent::addDocument (std::unique_ptr<DataWindow> doc, std::optional<juce::Point<int>> position)
{
    if (doc == nullptr)
        return;

    auto* raw = doc.get();
    raw->onChanged = [this, raw]
    {
        if (raw == activeDocument())
        {
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
    updateTitle();
    updateStatus();
    commandManager.commandStatusChanged();
}

void MainComponent::documentClosing (DataWindow* doc)
{
    if (doc != nullptr && doc == playingDocument.getComponent())
        releasePlaybackSource();
}

void MainComponent::openFileDialog()
{
    auto start = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    if (auto* doc = activeDocument(); doc != nullptr && doc->getFile().existsAsFile())
        start = doc->getFile().getParentDirectory();

    fileChooser = std::make_unique<juce::FileChooser> ("Open", start, formatManager.getWildcardForAllFormats());
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  for (const auto& file : chooser.getResults())
                                      if (file.existsAsFile())
                                          loadFile (file);
                              });
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

    // Suggest the same folder and name, as WAV when the original can't be written.
    auto folder = doc->getFile().existsAsFile() ? doc->getFile().getParentDirectory()
                                                : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    auto name = juce::File::createLegalFileName (doc->getTitle());

    if (! DataWindow::isSaveExtension (juce::File (name).getFileExtension()))
        name = juce::File (name).getFileNameWithoutExtension() + ".wav";

    fileChooser = std::make_unique<juce::FileChooser> ("Save As", folder.getChildFile (name), DataWindow::saveWildcard);
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, doc = juce::Component::SafePointer<DataWindow> (doc), finish] (const juce::FileChooser& chooser)
                              {
                                  auto file = chooser.getResult();

                                  if (file == juce::File() || doc == nullptr)
                                  {
                                      finish (false, {});
                                      return;
                                  }

                                  if (file.getFileExtension().isEmpty())
                                      file = file.withFileExtension (".wav");

                                  if (doc.getComponent() == playingDocument.getComponent())
                                      stopPlayback (false);

                                  juce::String error;
                                  finish (doc->saveTo (file, formatManager, error), error);
                              });
}

void MainComponent::askToSaveBeforeClosing (DataWindow* doc, std::function<void (bool)> done)
{
    if (doc == nullptr || ! doc->isModified())
    {
        done (true);
        return;
    }

    auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                            .withTitle ("WaveForge")
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
        if (auto* doc = dynamic_cast<DataWindow*> (workspace->getDocument (i)); doc != nullptr && doc->isModified())
            ++unsaved;

    if (unsaved == 0)
    {
        quit();
        return;
    }

    auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                            .withTitle ("WaveForge")
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
    }
}

void MainComponent::releasePlaybackSource()
{
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

    const auto area = workspace->getLocalBounds();

    for (int i = 0; i < count; ++i)
    {
        auto* window = workspace->getWindowFor (workspace->getDocument (i));

        if (window == nullptr)
            continue;

        if (cascade)
        {
            const auto offset = (i % 10) * cascadeStep;
            window->setBounds (area.getX() + 8 + offset, area.getY() + 8 + offset,
                               juce::jmax (420, area.getWidth() * 2 / 3), juce::jmax (220, area.getHeight() / 2));
        }
        else
        {
            // Stack windows top to bottom, each one full width.
            const auto rowHeight = area.getHeight() / count;
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

    if (auto* window = workspace->getWindowFor (next))
        window->toFront (true);
}

//==============================================================================
juce::int64 MainComponent::playPosition() const
{
    if (auto* doc = playingDocument.getComponent())
        return (juce::int64) (transport.getCurrentPosition() * doc->getSampleRate());

    return 0;
}

void MainComponent::startPlayback (juce::Range<juce::int64> range)
{
    auto* doc = activeDocument();

    if (doc == nullptr || range.isEmpty())
        return;

    stopPlayback (false);

    transport.setSource (nullptr);
    memorySource = std::make_unique<juce::MemoryAudioSource> (doc->getBuffer(), false, false);
    transport.setSource (memorySource.get(), 0, nullptr, doc->getSampleRate(), doc->getBuffer().getNumChannels());

    playingDocument = doc;
    playRange = range;
    playStartCursor = doc->getView().getCursor();
    transport.setPosition ((double) range.getStart() / doc->getSampleRate());
    transport.start();
    startTimerHz (30);
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
    auto* doc = playingDocument.getComponent();
    const auto position = playPosition();

    if (doc == nullptr || ! transport.isPlaying() || position >= playRange.getEnd())
    {
        stopPlayback (true);
        return;
    }

    doc->getView().setPlayhead (position);
    doc->getView().ensureVisible (position);
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
        window->setName (doc != nullptr ? doc->getName() + " - WaveForge" : juce::String ("WaveForge"));
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
        case wf::fileOpen: case wf::fileExit: case wf::optAudioDevice:
        case wf::helpShortcuts: case wf::helpAbout:
            break;

        case wf::editUndo:  available = available && doc != nullptr && doc->canUndo(); break;
        case wf::editRedo:  available = available && doc != nullptr && doc->canRedo(); break;
        case wf::editDelete: available = available && doc != nullptr && ! doc->getView().getSelection().isEmpty(); break;
        case wf::optStop:   available = available && isPlaying(); break;

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

    info.setActive (available);

    for (const auto& key : def->keys)
        info.addDefaultKeypress (key.getKeyCode(), key.getModifiers());
}

bool MainComponent::perform (const InvocationInfo& info)
{
    auto* doc = activeDocument();
    const auto total = doc != nullptr ? (juce::int64) doc->getBuffer().getNumSamples() : 0;

    switch (info.commandID)
    {
        case wf::fileOpen:          openFileDialog(); return true;
        case wf::fileClose:         closeActiveDocument(); return true;
        case wf::fileSave:          saveDocument (doc, false); return true;
        case wf::fileSaveAs:        saveDocument (doc, true); return true;
        case wf::fileExit:          juce::JUCEApplication::getInstance()->systemRequestedQuit(); return true;
        case wf::optAudioDevice:    showAudioSettings(); return true;
        case wf::helpShortcuts:     showShortcuts(); return true;
        case wf::helpAbout:         showAbout(); return true;
        case wf::winCascade:        arrangeWindows (true); return true;
        case wf::winTile:           arrangeWindows (false); return true;
        case wf::winNext:           cycleWindows (1); return true;
        case wf::winPrev:           cycleWindows (-1); return true;
        case wf::optStop:           stopPlayback (true); return true;
        default: break;
    }

    if (doc == nullptr)
        return false;

    auto& view = doc->getView();

    switch (info.commandID)
    {
        case wf::editUndo:          releasePlaybackSource(); doc->undo(); return true;
        case wf::editRedo:          releasePlaybackSource(); doc->redo(); return true;
        case wf::editDelete:        releasePlaybackSource(); doc->deleteSelection(); return true;
        case wf::editSelectAll:     view.selectAll(); return true;
        case wf::insertMarker:      doc->addMarker (view.getCursor()); return true;

        case wf::viewZoomIn:        view.zoomIn(); return true;
        case wf::viewZoomOut:       view.zoomOut(); return true;
        case wf::viewZoomSelection: view.zoomToSelection(); return true;
        case wf::viewZoomNormal:    view.zoomToFit(); return true;

        case wf::procFadeIn:        process (wf::Process::fadeIn); return true;
        case wf::procFadeOut:       process (wf::Process::fadeOut); return true;
        case wf::procMute:          process (wf::Process::mute); return true;
        case wf::procNormalize:     process (wf::Process::normalize); return true;
        case wf::procReverse:       process (wf::Process::reverse); return true;
        case wf::procInvert:        process (wf::Process::invert); return true;
        case wf::procDcOffset:      process (wf::Process::dcOffset); return true;
        case wf::procGainUp:        process (wf::Process::gainUp); return true;
        case wf::procGainDown:      process (wf::Process::gainDown); return true;

        case wf::optPlay:
            if (isPlaying())
                stopPlayback (true);
            else
                startPlayback (view.getSelection().isEmpty() ? juce::Range<juce::int64> (view.getCursor(), total)
                                                             : view.getSelection());
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
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (deviceManager, 0, 0, 1, 2, false, false, true, false);
    selector->setSize (520, 360);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = "Audio Device";
    options.dialogBackgroundColour = panelColour;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::showAbout()
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                            "About WaveForge",
                                            "WaveForge " JUCE_APPLICATION_VERSION_STRING "\n\n"
                                            "A destructive audio editor with a classic layout.\n"
                                            "Licensed under the GNU AGPL v3.\n\n"
                                            "Built with JUCE " + juce::SystemStats::getJUCEVersion().fromLastOccurrenceOf ("v", false, false));
}

void MainComponent::showShortcuts()
{
    if (shortcutsWindow == nullptr)
        shortcutsWindow = std::make_unique<ShortcutsWindow>();

    shortcutsWindow->setVisible (true);
    shortcutsWindow->toFront (true);
}
