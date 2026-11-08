#pragma once

#include "BassFinder.h"
#include <juce_audio_processors/juce_audio_processors.h>

// VST3 plug-ins: finding them (each one is opened in a separate process, so a
// broken plug-in can't take the program down), the Plug-In Manager window, and
// the window that runs one plug-in on the selection or the whole file, with a
// live preview and an undoable Apply.
namespace wf::plugins
{
// Adormis Audio --scan-plugin <plug-in> <report.xml>: the child process of a
// scan. Writes what it finds and returns true when these are its arguments.
bool runScanChild (const juce::StringArray& args);

// The known plug-ins and the folders searched for them, remembered between starts.
class Library final : public juce::ChangeBroadcaster,
                      private juce::Thread
{
public:
    Library();
    ~Library() override;

    juce::AudioPluginFormatManager& formats()   { return formatManager; }

    juce::Array<juce::PluginDescription> plugins() const;
    juce::StringArray failed() const;    // "path: reason"
    juce::StringArray folders() const;   // only those the user added
    juce::StringArray defaultFolders() const;

    void addFolder (const juce::File&);
    void removeFolder (const juce::String&);

    void startScan();
    bool isScanning() const   { return isThreadRunning(); }
    juce::String progressText() const;
    bool everScanned() const;

private:
    void run() override;
    void load();
    void save();
    void announce();

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList known;
    mutable juce::CriticalSection lock;
    juce::StringArray userFolders, failures;
    juce::String progress;
    bool scannedBefore = false;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

// Tools > the Plug-In Manager: what was found, the folders, Rescan.
struct ManagerCallbacks
{
    std::function<void (const juce::PluginDescription&)> use;   // run it on the active file
    std::function<bool()> hasFile;
    std::function<void (std::function<void (juce::File)>)> chooseFolder;
    std::function<void()> onClose;
};

std::unique_ptr<juce::DocumentWindow> createManagerWindow (Library&, ManagerCallbacks);

// Opens the plug-in; empty and `error` set when it can't run on this audio.
std::unique_ptr<juce::DocumentWindow> createPluginWindow (Library&, const juce::PluginDescription&, BassFinderSource,
                                                          BassFinderHost, std::function<void()> onClose,
                                                          juce::String& error);
} // namespace wf::plugins
