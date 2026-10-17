#include "StartupLog.h"

namespace wf::startup
{
namespace
{
std::unique_ptr<juce::FileLogger>& logger()
{
    static std::unique_ptr<juce::FileLogger> instance;
    return instance;
}

juce::File audioMarker()   { return settingsFolder().getChildFile ("audio-starting.flag"); }

void crashHandler (void*)
{
    log ("CRASH. Stack:\n" + juce::SystemStats::getStackBacktrace());
}
} // namespace

juce::File settingsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Adormis Audio");
}

void beginLog()
{
    const auto folder = settingsFolder();

    // Settings from before the rename (WaveForge) carry over on the first start.
    const auto oldFolder = folder.getSiblingFile ("WaveForge");

    if (! folder.exists() && oldFolder.isDirectory())
    {
        oldFolder.copyDirectoryTo (folder);
        folder.getChildFile ("waveforge.log").deleteFile();
        folder.getChildFile ("waveforge-previous.log").deleteFile();
    }

    folder.createDirectory();
    const auto file = folder.getChildFile ("adormis-audio.log");

    if (file.existsAsFile())
        file.copyFileTo (folder.getChildFile ("adormis-audio-previous.log"));

    file.deleteFile();
    logger() = std::make_unique<juce::FileLogger> (file, "Adormis Audio " JUCE_APPLICATION_VERSION_STRING " - "
                                                         + juce::SystemStats::getOperatingSystemName()
                                                         + (juce::SystemStats::isOperatingSystem64Bit() ? " 64-bit" : ""));
    juce::SystemStats::setApplicationCrashHandler (crashHandler);
}

void log (const juce::String& line)
{
    if (auto& l = logger())
        l->logMessage (juce::Time::getCurrentTime().formatted ("%H:%M:%S  ") + line);
}

void endLog()
{
    log ("quit");
    logger().reset();
}

bool lastStartFailedAtAudio()   { return audioMarker().existsAsFile(); }
void markAudioStarting()        { audioMarker().replaceWithText ("audio device opening"); }
void markAudioStarted()         { audioMarker().deleteFile(); }
} // namespace wf::startup
