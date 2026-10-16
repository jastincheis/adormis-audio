#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The dark theme of the Bass Key Finder page, shared by the tool windows
// (Bass Finder, De-esser).
namespace wf::dark
{
// Colours of the Bass Key Finder page (its dark theme).
namespace colours
{
inline const juce::Colour bg (0xff0d0f15), surface (0xff161922), ink (0xffe8eaf2), muted (0xff8f95a9), line (0xff272b38),
                   accent (0xff8193ff), bass (0xffff9a3d), ok (0xff4fcf8c), warn (0xfff2b84b), bad (0xffff6b6b),
                   onAccent (0xff0d0f15);
}

// JUCE's own controls (buttons, lists, menus, sliders, tabs) in the same dark colours.
class DarkLook final : public juce::LookAndFeel_V4
{
public:
    DarkLook()
        : juce::LookAndFeel_V4 ({ colours::bg, colours::surface, colours::surface, colours::line, colours::ink,
                                  colours::accent, colours::onAccent, colours::accent, colours::ink })
    {
        using namespace colours;
        setColour (juce::TextButton::buttonColourId, surface.brighter (0.08f));
        setColour (juce::TextButton::textColourOffId, ink);
        setColour (juce::ComboBox::backgroundColourId, surface.brighter (0.08f));
        setColour (juce::ComboBox::outlineColourId, line);
        setColour (juce::ComboBox::arrowColourId, muted);
        setColour (juce::TextEditor::backgroundColourId, bg);
        setColour (juce::TextEditor::outlineColourId, line);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::backgroundColourId, line);
        setColour (juce::Slider::thumbColourId, ink);
        setColour (juce::Slider::textBoxBackgroundColourId, bg);
        setColour (juce::Slider::textBoxOutlineColourId, line);
        setColour (juce::Slider::textBoxTextColourId, ink);
        setColour (juce::ListBox::backgroundColourId, surface);
        setColour (juce::ScrollBar::thumbColourId, muted.withAlpha (0.5f));
        setColour (juce::TabbedButtonBar::tabTextColourId, muted);
        setColour (juce::TabbedButtonBar::frontTextColourId, accent);
        setColour (juce::TabbedButtonBar::tabOutlineColourId, line);
        setColour (juce::TabbedButtonBar::frontOutlineColourId, line);
        setColour (juce::TabbedComponent::outlineColourId, line);
        setColour (juce::PopupMenu::backgroundColourId, surface);
        setColour (juce::PopupMenu::textColourId, ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.25f));
        setColour (juce::PopupMenu::highlightedTextColourId, ink);
        setColour (juce::DocumentWindow::textColourId, ink);
        setColour (juce::ResizableWindow::backgroundColourId, bg);
    }
};
} // namespace wf::dark
