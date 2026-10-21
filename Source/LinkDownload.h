#pragma once

#include <juce_core/juce_core.h>

#include <functional>

// The link box next to Help: the sound of a YouTube, TikTok, Instagram,
// Facebook... page saved as a WAV, through yt-dlp (it knows the sites) and
// ffmpeg (it decodes what the site sends). Both are separate programs: found
// on the computer, or downloaded once from their GitHub releases into the
// settings folder (Windows gets both; elsewhere ffmpeg comes from the system).
namespace wf::links
{
// Where the WAVs go: Music/Adormis Audio Downloads.
juce::File downloadsFolder();

// What is missing for a download, empty when nothing; e.g. "yt-dlp and ffmpeg".
juce::String missingTools();

// True when this system can fetch the missing tools itself (otherwise
// installHint() says how the user can).
bool canFetchMissingTools();
juce::String installHint();

// Downloads (and checks against the release's SHA-256 list) whatever tools
// are missing. Blocks; `progress` gets 0..1 and returns false to stop.
bool fetchMissingTools (const std::function<bool (float)>& progress, juce::String& error);

// The best audio the page has, as a WAV (24-bit, the site's sample rate).
// Blocks; `progress` gets 0..1 and returns false to stop. The saved file, or
// {} with `error` set (empty when stopped by the user).
juce::File download (const juce::String& link, const std::function<bool (float)>& progress, juce::String& error);

// "youtube.com/...", "https://..." and the like: worth handing to yt-dlp.
bool looksLikeLink (const juce::String& text);
} // namespace wf::links
