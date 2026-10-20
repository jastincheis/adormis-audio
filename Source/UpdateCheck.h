#pragma once

#include <juce_core/juce_core.h>

#include <optional>

// Help > Check for Updates and the check at start: asks GitHub which is the
// newest release of Adormis Audio (pre-releases count). Nothing about the
// user or their files is sent; GitHub only gets the usual request.
namespace wf::updates
{
struct Release
{
    juce::String version;   // "0.1.15", without the "v"
    juce::String pageUrl;   // the release page, where the downloads are
};

// The newest release among GitHub's answer (a JSON list of releases);
// drafts and tags that aren't versions are skipped.
std::optional<Release> newestIn (const juce::String& json);

// Asks GitHub; blocks for up to `timeoutMs`, so call it off the message thread.
// Nothing when there is no connection or the answer makes no sense.
std::optional<Release> fetchNewest (int timeoutMs = 8000);

// "0.1.15" > "0.1.9": compares the numbers, not the text.
bool isNewer (const juce::String& candidate, const juce::String& current);
} // namespace wf::updates
