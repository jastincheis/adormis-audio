#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "DataWindow.h"
#include "ProcessToolbar.h"

class ShortcutsWindow;

class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel,
                            public juce::DragAndDropContainer,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool loadFile (const juce::File& file);

    // Opens `doc` in a new workspace window; `position` is its top-left corner
    // in workspace coordinates (or cascaded when not given).
    void addDocument (std::unique_ptr<DataWindow> doc, std::optional<juce::Point<int>> position = {});

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override  { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform (const InvocationInfo& info) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected (int, int) override {}

    juce::ApplicationCommandManager& getCommandManager()  { return commandManager; }

    // Asks about unsaved files, then calls `quit` if it's fine to exit.
    void requestQuit (std::function<void()> quit);

private:
    // The grey area holding the floating data windows.
    class Workspace;

    void timerCallback() override;

    DataWindow* activeDocument() const;
    void activeDocumentChanged();
    void documentClosing (DataWindow*);

    void openFileDialog();
    void closeActiveDocument();
    void saveDocument (DataWindow* doc, bool forceSaveAs, std::function<void (bool saved)> done = {});
    void askToSaveBeforeClosing (DataWindow* doc, std::function<void (bool okToClose)> done);
    void process (wf::Process process);
    void arrangeWindows (bool cascade);
    void cycleWindows (int direction);

    void startPlayback (juce::Range<juce::int64> range);
    void stopPlayback (bool returnToStart);
    void releasePlaybackSource();   // before a document's buffer is changed
    bool isPlaying() const  { return transport.isPlaying(); }
    juce::int64 playPosition() const;

    void showAudioSettings();
    void showAbout();
    void showShortcuts();
    void updateStatus();
    void updateTitle();

    juce::ApplicationCommandManager commandManager;
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<Workspace> workspace;
    std::unique_ptr<ProcessToolbar> toolbar;
    std::unique_ptr<ShortcutsWindow> shortcutsWindow;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Label statusBar;

    // Playback (always of one document at a time)
    juce::AudioFormatManager formatManager;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer sourcePlayer;
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::MemoryAudioSource> memorySource;
    juce::Component::SafePointer<DataWindow> playingDocument;
    juce::Range<juce::int64> playRange;
    juce::int64 playStartCursor = 0;

    int untitledCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
