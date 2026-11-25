#include "CdjExportWindow.h"

#include <ExportComponent.h>
#include <SharedAudio.h>

namespace wf
{
namespace
{
class CdjExportWindow final : public juce::DocumentWindow
{
public:
    explicit CdjExportWindow (CdjExportHost h)
        : juce::DocumentWindow ("CDJ Export", juce::Colour (0xff1d1f24), juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          host (std::move (h))
    {
        cdj::sharedAudioDevices() = host.devices;

        // The standalone app's look (JUCE's dark default), not Adormis Audio's
        // light one, so its own colours stay readable.
        setLookAndFeel (&look);

        content = new cdj::ExportComponent();
        content->setSize (1000, 720);

        // Like the Drum Rebuilder window: own title bar, fixed size, above the
        // main window (a tiling window manager would otherwise tile it).
        setUsingNativeTitleBar (false);
        setContentOwned (content, true);
        setResizable (false, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    ~CdjExportWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
        cdj::sharedAudioDevices() = nullptr;
    }

    cdj::ExportComponent& exporter() { return *content; }

    void closeButtonPressed() override
    {
        if (! content->hasUnsavedMarkers())
        {
            close();
            return;
        }

        juce::Component::SafePointer<CdjExportWindow> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Save the markers?")
                                          .withMessage ("Some markers were changed in CDJ Export and not saved into their files.")
                                          .withButton ("Save")
                                          .withButton ("Don't save")
                                          .withButton ("Cancel")
                                          .withAssociatedComponent (this),
                                      [safe] (int result)
                                      {
                                          if (safe == nullptr || result == 0)
                                              return;

                                          if (result == 1)
                                          {
                                              juce::String errors;

                                              if (! safe->content->saveAllMarkers (errors))
                                              {
                                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                                          "Markers not saved", errors);
                                                  return;
                                              }
                                          }

                                          safe->close();
                                      });
    }

private:
    void close()
    {
        setVisible (false);

        if (host.onClose != nullptr)
            host.onClose();
    }

    juce::LookAndFeel_V4 look;   // declared before the content: outlives it
    CdjExportHost host;
    cdj::ExportComponent* content = nullptr;   // owned by the window
};
}

std::unique_ptr<juce::DocumentWindow> createCdjExportWindow (CdjExportHost host)
{
    return std::make_unique<CdjExportWindow> (std::move (host));
}

void addToCdjExport (juce::DocumentWindow& window, const juce::StringArray& files)
{
    if (auto* w = dynamic_cast<CdjExportWindow*> (&window); w != nullptr && ! files.isEmpty())
        w->exporter().addFiles (files);
}

bool cdjExportHasUnsavedMarkers (juce::DocumentWindow& window)
{
    auto* w = dynamic_cast<CdjExportWindow*> (&window);
    return w != nullptr && w->exporter().hasUnsavedMarkers();
}
}
