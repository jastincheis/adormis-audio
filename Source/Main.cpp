#include "PluginHost.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "StartupLog.h"
#include "Tempo.h"

class AdormisAudioApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override     { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override  { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override            { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // The child process of a plug-in scan: no window, no log, just the report.
        if (wf::plugins::runScanChild (getCommandLineParameterArray()))
        {
            quit();
            return;
        }

        // The child process of a plug-in preview: opens one plug-in, saves a
        // picture of its editor, then quits. A crash takes only this process.
        if (wf::plugins::runThumbnailChild (getCommandLineParameterArray()))
        {
            quit();
            return;
        }

        // Self-test: Adormis Audio --measure-tempo <report.txt> <audio files...>
        // writes the detected BPM of each file, then quits. No window.
        if (const auto a = getCommandLineParameterArray(); a.size() >= 2 && a[0] == "--measure-tempo")
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            juce::String out;

            for (int i = 2; i < a.size(); ++i)
            {
                const auto f = juce::File::getCurrentWorkingDirectory().getChildFile (a[i].unquoted());
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));

                if (r == nullptr)
                {
                    out << f.getFileName() << ": cannot read\n";
                    continue;
                }

                juce::AudioBuffer<float> buf ((int) r->numChannels, (int) r->lengthInSamples);
                r->read (&buf, 0, (int) r->lengthInSamples, 0, true, true);
                const auto bpm = wf::measureTempo (buf, { 0, (int) r->lengthInSamples }, r->sampleRate);
                out << f.getFileName() << ": " << (bpm ? juce::String (*bpm, 1) : juce::String ("none")) << "\n";
            }

            juce::File::getCurrentWorkingDirectory().getChildFile (a[1].unquoted()).replaceWithText (out);
            quit();
            return;
        }

        wf::startup::beginLog();
        wf::startup::log ("start: " + commandLine);
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        mainWindow = std::make_unique<MainWindow> (getApplicationName() + " " + getApplicationVersion());
        wf::startup::log ("window shown");

        auto args = getCommandLineParameterArray();

        // Self-test: Adormis Audio --list-commands report.txt [audio files...]
        // opens the files, writes which menu commands are usable, then quits.
        juce::File report;

        if (args.size() >= 2 && args[0] == "--list-commands")
        {
            report = juce::File::getCurrentWorkingDirectory().getChildFile (args[1].unquoted());
            args.removeRange (0, 2);
        }

        for (const auto& arg : args)
            openFile (juce::File::getCurrentWorkingDirectory().getChildFile (arg.unquoted()));

        if (report != juce::File())
        {
            juce::Timer::callAfterDelay (3000, [this, report]
            {
                if (auto* content = mainWindow != nullptr ? dynamic_cast<MainComponent*> (mainWindow->getContentComponent()) : nullptr)
                {
                    // Two statements: the order of calls inside one expression isn't fixed.
                    const auto unselected = content->commandReport (false);
                    const auto selected = content->commandReport (true);
                    report.replaceWithText ("== File open, nothing selected\n" + unselected
                                            + "\n== Part of the file selected\n" + selected, false, false, "\n");
                }

                quit();
            });
        }
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        openFile (juce::File::getCurrentWorkingDirectory().getChildFile (commandLine.unquoted()));
    }

    bool openFile (const juce::File& file)
    {
        if (mainWindow == nullptr || ! file.existsAsFile())
            return false;

        if (auto* content = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
        {
            wf::startup::log ("open: " + file.getFullPathName());
            return content->loadFile (file);
        }

        return false;
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        wf::startup::endLog();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            if (auto* content = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            {
                content->requestQuit ([] { juce::JUCEApplication::quit(); });
                return;
            }

        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colour (0xffd4d0c8), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            auto* content = new MainComponent();
            setContentOwned (content, true);

            // Listen on the window itself so shortcuts work wherever focus is.
            addKeyListener (content->getCommandManager().getKeyMappings());
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            getContentComponent()->grabKeyboardFocus();
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    // Light, classic desktop look (closer to Sound Forge than JUCE's dark default).
    juce::LookAndFeel_V4 lookAndFeel { juce::LookAndFeel_V4::getLightColourScheme() };
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (AdormisAudioApplication)
