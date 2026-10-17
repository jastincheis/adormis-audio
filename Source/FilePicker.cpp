#include "FilePicker.h"

namespace wf
{
void FilePicker::launch (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                         std::function<void (const juce::Array<juce::File>&)> onResult,
                         std::unique_ptr<juce::FilePreviewComponent> preview)
{
    if (juce::FileChooser::isPlatformDialogAvailable() && preview == nullptr)
    {
        chooser = std::make_unique<juce::FileChooser> (title, start, patterns);
        chooser->launchAsync (flags, [onResult] (const juce::FileChooser& c) { onResult (c.getResults()); });
        return;
    }

    if (box != nullptr)
        box->exitModalState (0);

    box.reset();
    browser.reset();
    previewPanel = std::move (preview);

    filter = std::make_unique<juce::WildcardFileFilter> (patterns, juce::String(), juce::String());
    browser = std::make_unique<juce::FileBrowserComponent> (flags, start, filter.get(), previewPanel.get());
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

        if (previewPanel != nullptr)
            previewPanel->selectedFileChanged ({});   // stops what it plays

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
                previewPanel.reset();
            }
        });

        onResult (files);
    }), false);
}
} // namespace wf
