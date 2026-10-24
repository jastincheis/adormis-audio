#include "LinkDownload.h"
#include "StartupLog.h"

#include <juce_cryptography/juce_cryptography.h>

namespace wf::links
{
namespace
{
const juce::String ytDlpReleases = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";
const juce::String ffmpegReleases = "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/";
const juce::String denoReleases = "https://github.com/denoland/deno/releases/latest/download/";

juce::File toolsFolder() { return startup::settingsFolder().getChildFile ("tools"); }

juce::String exeName (const juce::String& tool)
{
   #if JUCE_WINDOWS
    return tool + ".exe";
   #else
    return tool;
   #endif
}

// Ours first (in the settings folder), then the PATH; apps started from the
// macOS Dock don't get Homebrew's folders in their PATH, so those too.
// yt-dlp only ours: the sites change often, and a system copy can be months
// old (YouTube then answers 403); ours updates itself.
juce::File findTool (const juce::String& tool)
{
    const auto name = exeName (tool);

    for (auto& f : { toolsFolder().getChildFile (name), toolsFolder().getChildFile ("ffmpeg").getChildFile (name) })
        if (f.existsAsFile())
            return f;

    if (tool == "yt-dlp")
        return {};

   #if JUCE_WINDOWS
    const auto pathSeparator = ";";
   #else
    const auto pathSeparator = ":";
   #endif
    auto folders = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), pathSeparator, "");
    folders.addArray ({ "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin" });

    for (auto& folder : folders)
        if (folder.isNotEmpty() && juce::File::isAbsolutePath (folder))
            if (auto f = juce::File (folder).getChildFile (name); f.existsAsFile())
                return f;

    return {};
}

// The release file yt-dlp publishes for this system.
juce::String ytDlpAsset()
{
   #if JUCE_WINDOWS
    #if defined (_M_ARM64)
     return "yt-dlp_arm64.exe";
    #else
     return "yt-dlp.exe";
    #endif
   #elif JUCE_MAC
    return "yt-dlp_macos";
   #elif JUCE_ARM
    return "yt-dlp_linux_aarch64";
   #else
    return "yt-dlp_linux";
   #endif
}

// Deno runs YouTube's player code for yt-dlp (without it, YouTube gives
// fewer formats, and in time none).
juce::String denoAsset()
{
   #if JUCE_WINDOWS && defined (_M_ARM64)
    return "deno-aarch64-pc-windows-msvc.zip";
   #elif JUCE_WINDOWS
    return "deno-x86_64-pc-windows-msvc.zip";
   #elif JUCE_MAC && JUCE_ARM
    return "deno-aarch64-apple-darwin.zip";
   #elif JUCE_MAC
    return "deno-x86_64-apple-darwin.zip";
   #elif JUCE_ARM
    return "deno-aarch64-unknown-linux-gnu.zip";
   #else
    return "deno-x86_64-unknown-linux-gnu.zip";
   #endif
}

juce::String ffmpegAsset()
{
   #if JUCE_WINDOWS && defined (_M_ARM64)
    return "ffmpeg-master-latest-winarm64-gpl-shared.zip";
   #elif JUCE_WINDOWS
    return "ffmpeg-master-latest-win64-gpl-shared.zip";
   #else
    return {};
   #endif
}

// Into a file, with the progress between `from` and `to`.
bool fetch (const juce::String& address, const juce::File& target, float from, float to,
            const std::function<bool (float)>& progress, juce::String& error)
{
    int status = 0;
    auto stream = juce::URL (address).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                             .withConnectionTimeoutMs (15000)
                                                             .withStatusCode (&status)
                                                             .withNumRedirectsToFollow (10));

    if (stream == nullptr || status >= 400)
    {
        error = "Could not download " + address + (status >= 400 ? " (HTTP " + juce::String (status) + ")" : juce::String());
        return false;
    }

    target.getParentDirectory().createDirectory();
    target.deleteFile();
    juce::FileOutputStream out (target);

    if (! out.openedOk())
    {
        error = "Could not write " + target.getFullPathName();
        return false;
    }

    const auto total = stream->getTotalLength();
    juce::HeapBlock<char> block (1 << 16);
    juce::int64 done = 0;

    for (;;)
    {
        const auto n = stream->read (block, 1 << 16);

        if (n <= 0)
            break;

        if (! out.write (block, (size_t) n))
        {
            error = "Could not write " + target.getFullPathName();
            return false;
        }

        done += n;

        if (! progress (total > 0 ? from + (to - from) * (float) done / (float) total : from))
            return false;
    }

    out.flush();

    if (total > 0 && done != total)
    {
        error = "The download of " + address + " stopped before the end.";
        return false;
    }

    return true;
}

// The file must match its line in the release's list of SHA-256 sums (or,
// for a sum file of its own, the one sum in it, in whatever layout).
bool matchesSum (const juce::File& file, const juce::String& sums, const juce::String& assetName, juce::String& error)
{
    juce::String expected;
    juce::StringArray hashes;

    for (auto& line : juce::StringArray::fromLines (sums))
    {
        auto parts = juce::StringArray::fromTokens (line, " *:", "");
        parts.removeEmptyStrings();

        if (parts.size() == 2 && parts[1] == assetName)
            expected = parts[0].toLowerCase();

        for (auto& part : parts)
            if (part.length() == 64 && part.containsOnly ("0123456789abcdefABCDEF"))
                hashes.add (part.toLowerCase());
    }

    if (expected.isEmpty() && hashes.size() == 1)
        expected = hashes[0];

    juce::FileInputStream in (file);
    const auto actual = in.openedOk() ? juce::SHA256 (in).toHexString().toLowerCase() : juce::String();

    if (expected.isEmpty() || actual != expected)
    {
        error = assetName + " doesn't match the SHA-256 sum published with it; it was not used.";
        return false;
    }

    return true;
}

juce::String fetchText (const juce::String& address)
{
    return juce::URL (address).readEntireTextStream();
}

// The files of `zip` whose folder ends with `inFolder` ("" = the top), flat into `target`.
bool unzipInto (const juce::File& zip, const juce::String& inFolder, const juce::File& target)
{
    juce::ZipFile archive (zip);
    target.createDirectory();

    for (int i = 0; i < archive.getNumEntries(); ++i)
    {
        const juce::String path = archive.getEntry (i)->filename.replaceCharacter ('\\', '/');
        const auto folder = path.containsChar ('/') ? path.upToLastOccurrenceOf ("/", false, false) : juce::String();

        if (path.endsWith ("/") || (inFolder.isEmpty() ? folder.isNotEmpty() : ! folder.endsWith (inFolder)))
            continue;

        std::unique_ptr<juce::InputStream> in (archive.createStreamForEntry (i));
        const auto out = target.getChildFile (path.fromLastOccurrenceOf ("/", false, false));
        out.deleteFile();
        juce::FileOutputStream stream (out);

        if (in == nullptr || ! stream.openedOk() || stream.writeFromInputStream (*in, -1) < 0)
            return false;

        stream.flush();
        out.setExecutePermission (true);
    }

    return true;
}

// Downloads a release zip, checks it, unpacks it; the zip goes either way.
bool fetchZip (const juce::String& address, const juce::String& sums, const juce::String& asset, const juce::String& inFolder,
               const juce::File& target, float from, float to, const std::function<bool (float)>& progress, juce::String& error)
{
    const auto zip = toolsFolder().getChildFile (asset);
    auto ok = fetch (address, zip, from, to, progress, error) && matchesSum (zip, sums, asset, error);

    if (ok && ! unzipInto (zip, inFolder, target))
    {
        error = "Could not unpack " + asset;
        ok = false;
    }

    zip.deleteFile();
    return ok;
}

// Our yt-dlp brings itself up to date (from its GitHub page, checked by
// itself) at most once a week, or when asked after a failure.
void updateYtDlp (const juce::File& ytDlp, bool evenIfRecent)
{
    if (! evenIfRecent && juce::Time::getCurrentTime() - ytDlp.getLastModificationTime() < juce::RelativeTime::days (7))
        return;

    juce::ChildProcess update;

    if (update.start (juce::StringArray { ytDlp.getFullPathName(), "-U" }))
    {
        startup::log ("yt-dlp update: " + update.readAllProcessOutput().trim().replaceCharacters ("\r\n", "  "));
        ytDlp.setLastModificationTime (juce::Time::getCurrentTime());
    }
}
} // namespace

juce::File downloadsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Adormis Audio Downloads");
}

juce::String missingTools()
{
    juce::StringArray missing;

    for (auto tool : { "yt-dlp", "deno", "ffmpeg" })
        if (findTool (tool) == juce::File())
            missing.add (tool);

    // "yt-dlp, deno and ffmpeg"
    const auto last = missing.size() > 1 ? " and " + missing[missing.size() - 1] : juce::String();

    if (last.isNotEmpty())
        missing.remove (missing.size() - 1);

    return missing.joinIntoString (", ") + last;
}

bool canFetchMissingTools()
{
    return findTool ("ffmpeg") != juce::File() || ffmpegAsset().isNotEmpty();
}

juce::String installHint()
{
   #if JUCE_MAC
    return "Install ffmpeg with Homebrew (brew install ffmpeg), then try again.";
   #else
    return "Install ffmpeg from your system's packages (e.g. sudo apt install ffmpeg, sudo pacman -S ffmpeg), then try again.";
   #endif
}

bool fetchMissingTools (const std::function<bool (float)>& progress, juce::String& error)
{
    const auto folder = toolsFolder();
    folder.createDirectory();

    if (findTool ("yt-dlp") == juce::File())
    {
        const auto asset = ytDlpAsset();
        const auto part = folder.getChildFile (asset + ".part");

        if (! fetch (ytDlpReleases + asset, part, 0.0f, 0.25f, progress, error)
            || ! matchesSum (part, fetchText (ytDlpReleases + "SHA2-256SUMS"), asset, error))
        {
            part.deleteFile();
            return false;
        }

        const auto target = folder.getChildFile (exeName ("yt-dlp"));
        target.deleteFile();
        part.moveFileTo (target);
        target.setExecutePermission (true);
    }

    if (findTool ("deno") == juce::File())
    {
        const auto asset = denoAsset();

        if (! fetchZip (denoReleases + asset, fetchText (denoReleases + asset + ".sha256sum"), asset, {}, folder,
                        0.25f, 0.5f, progress, error))
            return false;
    }

    if (findTool ("ffmpeg") == juce::File())
    {
        const auto asset = ffmpegAsset();

        if (asset.isEmpty())
        {
            error = "ffmpeg is missing. " + installHint();
            return false;
        }

        // Only the programs and their DLLs (bin/), flat in tools/ffmpeg.
        const auto target = folder.getChildFile ("ffmpeg");
        target.deleteRecursively();

        if (! fetchZip (ffmpegReleases + asset, fetchText (ffmpegReleases + "checksums.sha256"), asset, "/bin", target,
                        0.5f, 0.95f, progress, error))
            return false;

        if (! target.getChildFile (exeName ("ffmpeg")).existsAsFile())
        {
            error = asset + " had no ffmpeg inside.";
            return false;
        }
    }

    progress (1.0f);
    return true;
}

namespace
{
juce::File attempt (const juce::File& ytDlp, const juce::File& ffmpeg, const juce::File& deno, const juce::String& link,
                    const std::function<bool (float)>& progress, bool& stopped, juce::String& error)
{
    const auto folder = downloadsFolder();
    folder.createDirectory();

    // The best audio-only stream (or the best with video, for sites without
    // one), decoded by ffmpeg into 24-bit PCM at the stream's own rate.
    juce::ChildProcess process;
    // UTF-8 output: on Windows yt-dlp otherwise prints in the console's code
    // page, and a title with diacritics gave a path that doesn't exist.
    const juce::StringArray args { ytDlp.getFullPathName(), "--ignore-config", "--encoding", "utf-8", "--no-playlist", "--newline",
                                   "--no-simulate", "--progress", "--windows-filenames",
                                   "--progress-template", "download:ADORMIS-PROGRESS %(progress._percent_str)s",
                                   "--print", "after_move:ADORMIS-FILE %(filepath)s",
                                   "--print", "after_move:ADORMIS-ID %(id)s",
                                   "-f", "bestaudio/best", "-x", "--audio-format", "wav",
                                   "--postprocessor-args", "ExtractAudio:-c:a pcm_s24le",
                                   "--ffmpeg-location", ffmpeg.getFullPathName(),
                                   "--js-runtimes", "deno:" + deno.getFullPathName(),
                                   "-o", folder.getChildFile ("%(title).120B [%(id)s].%(ext)s").getFullPathName(),
                                   "--", link.trim() };

    if (! process.start (args))
    {
        error = "Could not start " + ytDlp.getFullPathName();
        return {};
    }

    juce::String pending, result, id;
    float shown = 0.0f;
    juce::StringArray errors, lastLines;
    char buffer[4096];

    for (;;)
    {
        const auto n = process.readProcessOutput (buffer, sizeof (buffer));

        if (n <= 0)
            break;

        pending += juce::String::fromUTF8 (buffer, n);

        while (pending.containsChar ('\n'))
        {
            const auto line = pending.upToFirstOccurrenceOf ("\n", false, false).trim();
            pending = pending.fromFirstOccurrenceOf ("\n", false, false);

            if (line.startsWith ("ADORMIS-PROGRESS"))
            {
                // The download is most of the time; the conversion is the rest.
                shown = 0.9f * juce::jlimit (0.0f, 1.0f, line.fromFirstOccurrenceOf (" ", false, false).trim().getFloatValue() / 100.0f);
            }
            else if (line.startsWith ("ADORMIS-FILE "))
            {
                result = line.fromFirstOccurrenceOf (" ", false, false);
            }
            else if (line.startsWith ("ADORMIS-ID "))
            {
                id = line.fromFirstOccurrenceOf (" ", false, false).trim();
            }
            else if (line.isNotEmpty())
            {
                lastLines.add (line);

                if (lastLines.size() > 6)
                    lastLines.remove (0);
            }
            else if (line.startsWith ("ERROR:"))
            {
                errors.add (line.fromFirstOccurrenceOf ("ERROR:", false, false).trim());
            }

            if (! progress (shown))
            {
                process.kill();
                stopped = true;
                return {};
            }
        }
    }

    process.waitForProcessToFinish (10000);
    juce::File file (juce::File::isAbsolutePath (result) ? result : juce::String());

    // The path printed may not match the name on disk (an odd character in
    // the title): the newest WAV ending in "[id]" is the one.
    if ((file == juce::File() || ! file.existsAsFile()) && id.isNotEmpty())
    {
        file = juce::File();

        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.wav"))
            if (f.getFileNameWithoutExtension().endsWith ("[" + id + "]")
                && (file == juce::File() || f.getLastModificationTime() > file.getLastModificationTime()))
                file = f;
    }

    if (file == juce::File() || ! file.existsAsFile())
    {
        error = errors.isEmpty() ? "yt-dlp found nothing to save at this link." : errors.joinIntoString ("\n");
        startup::log ("Link download failed: " + link + " / " + error + " / file: " + result + " / id: " + id
                      + " / last output: " + lastLines.joinIntoString (" | "));
        return {};
    }

    return file;
}
} // namespace

juce::File download (const juce::String& link, const std::function<bool (float)>& progress, juce::String& error)
{
    const auto ytDlp = findTool ("yt-dlp"), ffmpeg = findTool ("ffmpeg"), deno = findTool ("deno");

    if (ytDlp == juce::File() || ffmpeg == juce::File() || deno == juce::File())
    {
        error = missingTools() + " missing.";
        return {};
    }

    updateYtDlp (ytDlp, false);
    bool stopped = false;
    auto file = attempt (ytDlp, ffmpeg, deno, link, progress, stopped, error);

    // A site that changed since the last update: once more, with the newest yt-dlp.
    if (file == juce::File() && ! stopped)
    {
        updateYtDlp (ytDlp, true);
        juce::String again;
        file = attempt (ytDlp, ffmpeg, deno, link, progress, stopped, again);

        if (file == juce::File())
            error = again.isNotEmpty() || stopped ? again : error;
    }

    if (stopped)
        error.clear();

    if (file != juce::File())
        progress (1.0f);

    return file;
}

bool looksLikeLink (const juce::String& text)
{
    const auto t = text.trim();
    return t.isNotEmpty() && ! t.containsAnyOf (" \t\r\n")
           && (t.startsWithIgnoreCase ("http://") || t.startsWithIgnoreCase ("https://") || t.containsChar ('.'));
}
} // namespace wf::links
