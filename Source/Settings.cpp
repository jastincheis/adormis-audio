#include "Settings.h"
#include "StartupLog.h"

namespace wf::settings
{
namespace
{
juce::PropertiesFile& file()
{
    static juce::PropertiesFile props (startup::settingsFolder().getChildFile ("settings.xml"), []
    {
        juce::PropertiesFile::Options o;
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        o.millisecondsBeforeSaving = 500;
        return o;
    }());
    return props;
}

const juce::Identifier skipId ("wfDontRemember");

bool skipped (juce::Component& c)
{
    for (auto* p = &c; p != nullptr; p = p->getParentComponent())
        if (p->getProperties().contains (skipId))
            return true;

    return false;
}

// Depth-first, in child order: the same order every time a window is built.
void collect (juce::Component& c, std::vector<juce::Component*>& out)
{
    for (auto* child : c.getChildren())
    {
        if (dynamic_cast<juce::Slider*> (child) != nullptr || dynamic_cast<juce::ComboBox*> (child) != nullptr)
        {
            out.push_back (child);
            continue;   // a slider's text box, a list's label are its own parts
        }

        if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && b->getClickingTogglesState())
        {
            out.push_back (child);
            continue;
        }

        collect (*child, out);
    }
}
} // namespace

juce::var get (const juce::String& key)
{
    const auto text = file().getValue (key);
    return text.isEmpty() ? juce::var() : juce::JSON::parse (text);
}

void set (const juce::String& key, const juce::var& value)
{
    file().setValue (key, juce::JSON::toString (value, true));   // written half a second later
}

void flush()
{
    file().saveIfNeeded();
}

void dontRemember (juce::Component& c)
{
    c.getProperties().set (skipId, true);
}

juce::var captureControls (juce::Component& root)
{
    std::vector<juce::Component*> controls;
    collect (root, controls);
    juce::Array<juce::var> values;

    for (auto* c : controls)
    {
        juce::DynamicObject::Ptr entry = new juce::DynamicObject();

        if (skipped (*c))
            entry->setProperty ("skip", true);
        else if (auto* s = dynamic_cast<juce::Slider*> (c))
            entry->setProperty ("slider", s->getValue());
        else if (auto* box = dynamic_cast<juce::ComboBox*> (c))
            entry->setProperty ("list", box->getSelectedId());
        else if (auto* b = dynamic_cast<juce::Button*> (c))
            entry->setProperty ("on", b->getToggleState());

        values.add (juce::var (entry.get()));
    }

    return values;
}

void restoreControls (juce::Component& root, const juce::var& state)
{
    std::vector<juce::Component*> controls;
    collect (root, controls);
    const auto* values = state.getArray();

    // Only when the window still has the same controls (a newer version may not).
    if (values == nullptr || values->size() != (int) controls.size())
        return;

    for (size_t i = 0; i < controls.size(); ++i)
    {
        const auto& v = (*values)[(int) i];
        auto* c = controls[i];

        if (skipped (*c) || v.hasProperty ("skip"))
            continue;

        if (auto* s = dynamic_cast<juce::Slider*> (c); s != nullptr && v.hasProperty ("slider"))
            s->setValue ((double) v["slider"], juce::sendNotificationSync);
        else if (auto* box = dynamic_cast<juce::ComboBox*> (c); box != nullptr && v.hasProperty ("list"))
        {
            if (const int id = v["list"]; box->indexOfItemId (id) >= 0)
                box->setSelectedId (id, juce::sendNotificationSync);
        }
        else if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && v.hasProperty ("on"))
        {
            // With a notification: the button's onClick runs, as for a click.
            if (const bool on = v["on"]; b->getToggleState() != on)
                b->setToggleState (on, juce::sendNotificationSync);
        }
    }
}
} // namespace wf::settings
