#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "BassFinder.h"
#include "DeEsser.h"
#include "EffectWindow.h"
#include "DataWindow.h"
#include "FilePicker.h"
#include "AudioFilePreview.h"
#include "MixDialog.h"
#include "MixPreview.h"
#include "ProcessToolbar.h"
#include "Recorder.h"
#include "SystemAudioBackend.h"
#include "StretchDialog.h"
#include "TemplateDialog.h"
#include "VolumeDialog.h"
#include "FormDialog.h"
#include "MeterPanel.h"
#include "StatisticsWindow.h"
#include "RegionsWindow.h"
#include "SpectrumWindow.h"
#include "UndoHistoryWindow.h"
#include "SpaceKey.h"
#include "UpdateCheck.h"

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

    bool loadFile (const juce::File& file, std::optional<juce::Point<int>> position = {});

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

    // Self-test (--list-commands): one line per command, "+" usable or "-"
    // greyed out, as the menus would show it now. With `selectPart`, a stretch
    // of the active file is selected first (selection-only commands).
    juce::String commandReport (bool selectPart);

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
    void showVolumeDialog();

    // Package A: small commands with a settings window (FormDialog).
    void showForm (const juce::String& title, const juce::String& info, std::vector<wf::FormField> fields,
                   const juce::String& hint, std::function<void (DataWindow&, const juce::StringArray&)> onOk);
    void showInsertSilence();
    void showResample();
    void showBitDepth();
    void showChannelConverter();
    void showAutoTrim();
    void showSetSelection();
    void showGoTo();
    void showDetectClipping();
    void mixClipboard();

    // Package B: loudness.
    void showNormalize();
    void showPitchShift();

    // Package E: regions.
    void insertRegion();
    void renameRegion (DataWindow* doc, int index);
    void regionMenu (DataWindow* doc, int index);
    void playRegion (DataWindow* doc, int index);
    void toggleRegionsList();
    void refreshRegionsList();
    void showAutoRegion();
    void extractRegions();
    std::unique_ptr<wf::RegionsWindow> regionsWindow;
    std::unique_ptr<wf::SpectrumWindow> spectrumWindow;
    void toggleSpectrum();
    void showBatch();   // Tools > Batch Converter

    // Right-click on the workspace: Create New (an empty file) and Open Recent.
    void showWorkspaceMenu (juce::Point<int> position);
    void chooseBackgroundPhoto (std::function<void()> onChanged);   // Background > Photo...
    void addLookItems (juce::PopupMenu& menu);
    void createEmptyDocument (std::optional<juce::Point<int>> position);
    void addRecentFile (const juce::File& file);
    juce::PopupMenu recentFilesMenu();
    std::unique_ptr<juce::DocumentWindow> batchWindow;
    void captureNoisePrint();   // Tools > Noise Reduction > Capture Noise Print
    juce::StringArray lastPitch;
    void showStatistics();
    void setMeterSections (bool channels, bool loudness);
    juce::StringArray lastNormalize;   // the Normalize window starts from the last choices
    void saveAll (std::vector<juce::Component::SafePointer<DataWindow>> remaining);
    void markInOut (bool in);

    // Edit > Repeat Last Process (Ctrl+Y) runs this again on the active file.
    void rememberAction (const juce::String& name, std::function<void (DataWindow&)> action);
    std::function<void (DataWindow&)> lastAction;
    juce::String lastActionName;
    juce::int64 markInPosition = -1;
    void showStretchDialog();
    void runStretch (DataWindow* doc, juce::Range<int> range, wf::StretchSettings settings);
    void showBassFinder();
    void showDeEsser();
    void showDrumRebuilder();

    // Get Stems: the song in `doc` split by the AI into drums, bass, vocals
    // and the rest, added to its window as tabs.
    void getStems (DataWindow* doc);
    juce::Component::SafePointer<DataWindow> gettingStems;
    std::optional<std::pair<juce::int64, int>> resumeAfterPageChange;   // play position and solo channel
    wf::BassFinderHost toolHost();   // playback, jobs and files for the tool windows

    // Time Stretch, Bass Finder and De-esser work on the active file (the
    // last two on a copy they write back), so only one is open at a time.
    bool toolWindowOpen() const
    {
        return stretchWindow != nullptr || bassFinderWindow != nullptr || deEsserWindow != nullptr || effectWindow != nullptr;
    }

    void showEffect (wf::EffectKind kind);

    // Ctrl+R: the first press opens a new empty file and the Record window,
    // the next ones start and stop recording into it.
    void record();
    void openRecorder();
    void closeRecorder();
    void startTake();
    void stopTake();
    void pumpRecording();
    void setInputEnabled (bool enabled);
    bool isRecording() const  { return recorder.isRecording(); }
    std::optional<wf::BassFinderSource> bassFinderSource();
    wf::BassFinderSource bassFinderSourceFor (DataWindow& doc, juce::Range<int> range, bool isSelection);

    // Runs `work` behind a progress bar with Cancel, one job at a time.
    void runJob (const juce::String& title, std::function<bool (std::function<bool (float)>)> work,
                 std::function<void (bool finished)> done);

    // Plays audio that isn't a document (Bass Finder notes, tones and loops).
    void startToolPreview (std::unique_ptr<juce::PositionableAudioSource> source, double rate, int channels);
    void stopToolPreview();
    void showNewFileDialog();
    void arrangeWindows (bool cascade);
    void cycleWindows (int direction);

    // `soloChannel` >= 0 plays only that channel (a single-channel selection).
    void startPlayback (juce::Range<juce::int64> range, int soloChannel = -1);
    void stopPlayback (bool returnToStart);
    void releasePlaybackSource();   // before a document's buffer is changed
    void jumpPlayback (DataWindow* doc, juce::int64 sample);

    // View > Playback Scrolling (the view follows playback, a page at a time)
    // and Smooth Playback Scrolling (the playhead stays in the middle and the
    // audio flows under it).
    bool followPlayback = true, smoothScrolling = false;
    double smoothPosition = -1.0, smoothClock = 0.0;   // to glide between the audio device's position updates
    int playbackTimerHz() const   { return smoothScrolling && followPlayback ? 60 : 30; }
    void showWaveMenu (DataWindow* doc);

    // Undo step names: the command being done, and the tool window that is open.
    juce::String lastCommandName, toolUndoName;
    std::unique_ptr<wf::UndoHistoryWindow> undoHistoryWindow;
    std::unique_ptr<wf::SpaceEverywhere> spaceEverywhere;   // Space plays / stops in every window
    void toggleUndoHistory();
    void refreshUndoHistory();
    void setLooping (bool shouldLoop);
    void toggleLoop();   // Q: loop on (a selection starts looping) / off
    void followSelectionLoop (DataWindow* doc);
    int playSoloChannel = -1;

    // Options > Customize Keyboard; the changes live in keyboard.xml.
    static juce::File keyMappingsFile();
    void loadKeyMappings();
    void showKeyboardEditor();
    std::unique_ptr<juce::DocumentWindow> keyboardWindow;

    struct ChangeCall final : public juce::ChangeListener
    {
        std::function<void()> onChange;
        void changeListenerCallback (juce::ChangeBroadcaster*) override  { if (onChange != nullptr) onChange(); }
    };

    ChangeCall keySaver;
    bool loopPlayback = false;

    // Mix dialog preview: plays the mix result without touching the file.
    void startMixPreview (DataWindow* target, const juce::AudioBuffer<float>& audio, double sourceRate,
                          juce::int64 position, std::shared_ptr<wf::MixGains> gains);
    void stopMixPreview();
    bool isMixPreviewing() const;
    void openMixWindow (const juce::String& description, std::shared_ptr<wf::MixGains> gains,
                        wf::MixDialogCallbacks callbacks);
    bool isPlaying() const  { return transport.isPlaying(); }
    juce::int64 playPosition() const;

    void showAudioSettings();
    void showAbout();

    // A newer release on GitHub: a button in the top right corner, and a
    // message at start (until "Skip this version").
    void checkForUpdates (bool askedByUser);
    void updateFound (std::optional<wf::updates::Release>, bool askedByUser);
    void showUpdateMessage();
    std::optional<wf::updates::Release> availableUpdate;
    juce::TextButton updateButton;

    // The link box next to Help: the sound of a YouTube, TikTok... page,
    // saved as a WAV in Music/Adormis Audio Downloads and opened.
    juce::TextEditor linkBox;
    juce::TextButton linkButton { "Get WAV" };
    void downloadLink();
    void showShortcuts();
    void updateStatus();
    void updateTitle();

    juce::ApplicationCommandManager commandManager;
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<Workspace> workspace;
    std::unique_ptr<ProcessToolbar> toolbar;
    std::unique_ptr<ShortcutsWindow> shortcutsWindow;
    wf::FilePicker filePicker;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Label statusBar;

    // Playback (always of one document at a time)
    juce::AudioFormatManager formatManager;
    // The audio devices. ASIO drivers (audio interfaces) are only looked at
    // when turned on in Options > Audio Device: loading some of them can hang
    // or take long, and JUCE loads every installed one when it scans.
    class DeviceManager final : public juce::AudioDeviceManager
    {
    public:
        bool allowAsio = false;

        void createAudioDeviceTypes (juce::OwnedArray<juce::AudioIODeviceType>& types) override
        {
            juce::AudioDeviceManager::createAudioDeviceTypes (types);

            if (! allowAsio)
                for (int i = types.size(); --i >= 0;)
                    if (types[i]->getTypeName() == "ASIO")
                        types.remove (i);
        }
    };

    DeviceManager deviceManager;
    void startAudio();   // a moment after the window shows (a slow driver can't keep it hidden)
    static juce::File asioFlagFile();
    static juce::File audioStateFile();
    ChangeCall audioSaver;
    juce::AudioSourcePlayer sourcePlayer;
    juce::TimeSliceThread previewReadThread { "Adormis Audio file preview" };   // reads ahead for the Open window's preview
    juce::AudioTransportSource transport;
    std::unique_ptr<wf::MeterTap> meterTap;     // the device plays through this (for the meters)
    std::unique_ptr<wf::MeterPanel> meters;
    std::unique_ptr<juce::PositionableAudioSource> memorySource;
    juce::Component::SafePointer<DataWindow> playingDocument;
    juce::Range<juce::int64> playRange;
    juce::int64 playStartCursor = 0;

    int untitledCount = 0;

    // Edit > Copy / Cut keep the audio here for Paste (inside Adormis Audio).
    struct Clipboard
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        DataWindow::Marks marks;   // the markers and regions that were in it
    };

    Clipboard clipboard;
    bool hasClipboard() const   { return clipboard.audio.getNumSamples() > 0; }
    int recordCount = 0;
    int lastRecordBits = 24;   // a new recording starts with the last bit depth chosen

    std::unique_ptr<juce::DocumentWindow> mixWindow;
    std::unique_ptr<juce::DocumentWindow> saveAsWindow;
    std::unique_ptr<juce::DocumentWindow> volumeWindow;
    std::unique_ptr<juce::DocumentWindow> formWindow;
    std::unique_ptr<juce::DocumentWindow> statisticsWindow;
    std::unique_ptr<juce::DocumentWindow> templateWindow;
    std::unique_ptr<juce::DocumentWindow> stretchWindow;
    std::unique_ptr<juce::DocumentWindow> bassFinderWindow;
    std::unique_ptr<juce::DocumentWindow> deEsserWindow;
    std::unique_ptr<juce::DocumentWindow> drumRebuilderWindow;   // not a tool window: it works on its own copy
    std::unique_ptr<juce::DocumentWindow> effectWindow;   // EQ, compressor, delay or reverb
    std::unique_ptr<juce::ThreadWithProgressWindow> job;
    std::unique_ptr<juce::PositionableAudioSource> toolPreview;

    wf::InputRecorder recorder;
    std::unique_ptr<wf::SystemAudioBackend> systemAudio;   // after the recorder: stops before it goes
    bool recordingSystemAudio = false;
    juce::String setRecordSource (bool systemAudio);
    void recordComputer();
    std::unique_ptr<juce::DocumentWindow> recordWindow;
    juce::Component::SafePointer<DataWindow> recordDocument;
    juce::AudioBuffer<float> recordChunk;

    struct RecordPump final : public juce::Timer
    {
        std::function<void()> tick;
        void timerCallback() override  { if (tick != nullptr) tick(); }
    };

    RecordPump recordPump;

    // The tempo on the toolbar: of the selection, or of the whole file when
    // nothing is selected; detected in the background a moment after the
    // selection or the audio changes.
    void scheduleTempo();
    void updateTempo();           // starts the detection (background) when needed
    void refreshTempoDisplay();   // shows what is known now
    void showTempoMenu();

    struct TempoKey
    {
        const DataWindow* doc = nullptr;
        juce::Range<juce::int64> range;
        int channel = -1, edits = -1;

        bool operator== (const TempoKey& o) const
        {
            return doc == o.doc && range == o.range && channel == o.channel && edits == o.edits;
        }
    };

    RecordPump tempoTimer;
    TempoKey shownTempo;
    std::vector<std::pair<TempoKey, double>> tempoCache;   // the last few results
    int tempoGeneration = 0;
    int tempoMode = 0;        // detected tempo: 0 as detected, 1 double, 2 half
    int selectionBeats = 4;   // how many beats a selection is (its tempo comes from its length)
    TempoKey currentTempoKey() const;
    std::optional<double> cachedTempo (const TempoKey&) const;
    juce::ThreadPool tempoPool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("Tempo") };
    wf::StretchRequest lastStretch;   // the new tempo / pitch choices carry over
    wf::TemplateSettings lastTemplate;   // File > New starts from the last settings used
    float lastVolumeDecibels = 0.0f;   // the Volume window starts from the last value used
    std::unique_ptr<wf::MixPreviewSource> mixPreview;
    juce::Component::SafePointer<DataWindow> mixPreviewDocument;
    juce::int64 mixPreviewOffset = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
