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
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("WaveForge");
}

void beginLog()
{
    const auto folder = settingsFolder();
    folder.createDirectory();
    const auto file = folder.getChildFile ("waveforge.log");

    if (file.existsAsFile())
        file.copyFileTo (folder.getChildFile ("waveforge-previous.log"));

    file.deleteFile();
    logger() = std::make_unique<juce::FileLogger> (file, "WaveForge " JUCE_APPLICATION_VERSION_STRING " - "
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
