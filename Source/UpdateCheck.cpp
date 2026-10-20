#include "UpdateCheck.h"

namespace wf::updates
{
namespace
{
juce::Array<int> versionNumbers (juce::String v)
{
    v = v.trim();

    if (v.startsWithIgnoreCase ("v"))
        v = v.substring (1);

    juce::Array<int> numbers;

    for (auto& part : juce::StringArray::fromTokens (v, ".", ""))
    {
        if (part.isEmpty() || ! part.containsOnly ("0123456789"))
            return {};

        numbers.add (part.getIntValue());
    }

    return numbers;
}
} // namespace

bool isNewer (const juce::String& candidate, const juce::String& current)
{
    const auto a = versionNumbers (candidate), b = versionNumbers (current);

    if (a.isEmpty() || b.isEmpty())
        return false;

    for (int i = 0; i < std::max (a.size(), b.size()); ++i)
    {
        const auto x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;

        if (x != y)
            return x > y;
    }

    return false;
}

std::optional<Release> newestIn (const juce::String& json)
{
    const auto parsed = juce::JSON::parse (json);

    if (! parsed.isArray())
        return {};

    std::optional<Release> best;

    for (auto& r : *parsed.getArray())
    {
        if ((bool) r["draft"])
            continue;

        auto tag = r["tag_name"].toString();

        if (versionNumbers (tag).isEmpty())
            continue;

        if (tag.startsWithIgnoreCase ("v"))
            tag = tag.substring (1);

        if (! best.has_value() || isNewer (tag, best->version))
            best = Release { tag, r["html_url"].toString() };
    }

    return best;
}

std::optional<Release> fetchNewest (int timeoutMs)
{
    const juce::URL url ("https://api.github.com/repos/jastincheis/adormis-audio/releases?per_page=20");
    int status = 0;
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withExtraHeaders ("Accept: application/vnd.github+json\r\n"
                                                                "User-Agent: Adormis-Audio-update-check")
                                             .withConnectionTimeoutMs (timeoutMs)
                                             .withStatusCode (&status));

    if (stream == nullptr || status != 200)
        return {};

    return newestIn (stream->readEntireStreamAsString());
}
} // namespace wf::updates
