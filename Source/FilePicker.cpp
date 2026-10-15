#include "FilePicker.h"

namespace wf
{
void FilePicker::launch (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                         std::function<void (const juce::Array<juce::File>&)> onResult)
{
    if (juce::FileChooser::isPlatformDialogAvailable())
    {
        chooser = std::make_unique<juce::FileChooser> (title, start, patterns);
        chooser->launchAsync (flags, [onResult] (const juce::FileChooser& c) { onResult (c.getResults()); });
        return;
    }

    if (box != nullptr)
        box->exitModalState (0);

    box.reset();
    browser.reset();

    filter = std::make_unique<juce::WildcardFileFilter> (patterns, juce::String(), juce::String());
    browser = std::make_unique<juce::FileBrowserComponent> (flags, start, filter.get(), nullptr);
    box = std::make_unique<juce::FileChooserDialogBox> (title, juce::String(), *browser,
                                                        (flags & juce::FileBrowserComponent::warnAboutOverwriting) != 0,
                                                        browser->findColour (juce::AlertWindow::backgroundColourId).withAlpha (1.0f));
    box->setResizable (false, false);
    box->centreWithSize (1000, 700);

    auto* active = box.get();
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, active, onResult] (int result)
    {
        if (active != box.get())   // replaced by a newer launch
            return;

        juce::Array<juce::File> files;

        if (result != 0)
            for (int i = 0; i < browser->getNumSelectedFiles(); ++i)
                files.add (browser->getSelectedFile (i));

        // The box can't be deleted from inside its own modal callback.
        juce::MessageManager::callAsync ([this, active]
        {
            if (active == box.get())
            {
                box.reset();
                browser.reset();
            }
        });

        onResult (files);
    }), false);
}
} // namespace wf
