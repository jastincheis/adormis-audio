# Adormis Audio

A destructive audio editor in the spirit of classic Sound Forge: the same menu
layout and default keyboard shortcuts, running natively on **macOS** (Apple
Silicon and Intel), **Linux** and **Windows**, with VST3 plug-in support planned.

Adormis Audio (formerly WaveForge) is an independent project. It is not affiliated with, endorsed by or
derived from Sound Forge or its owners; it only follows the publicly documented
menu structure and default shortcuts so the workflow feels familiar.

## Features

**Files and editing**
- Open WAV / FLAC / AIFF / OGG / MP3; save WAV / FLAC / AIFF / OGG and MP3, with sample-rate conversion
- The Open window plays the file you click on (format, length, waveform, Play / Stop, Auto play)
- Several files in overlapping windows; drag audio files in from Explorer / Finder / a file manager
- Cut / copy / paste (also to a new file), trim / crop, Mix (Ctrl+M); undo / redo
- Selection on both channels or one (L / R); drag a selection's edges to change it; drag it out into a new file, or onto another file to mix it in (volumes, live preview, snapping to markers)
- Markers (drag them left / right to move them) and regions (R): a region strip under the ruler, View > Regions List, Tools > Auto Region (split at silences), Tools > Extract Regions (one file per region); WAV files keep both, as cue points with labels
- Markers and regions travel with the audio when you drag a selection out, mix it into another file, or copy and paste
- Set Selection, Go To, Mark In / Out, W / E to the selection's left / right edge, Repeat Last Process (Ctrl+Y), Level Zoom, Save All
- Auto Snap to Zero Crossings (Ctrl+B): selection edges land where the wave crosses zero, so edits don't click
- ; halves the selection, ' doubles it
- Undo/Redo History (Alt+8): every edit by name; click one to go back (or forward) to it
- Record (Ctrl+R) with input meter, mono / stereo and bit depth, or **record what the computer plays** (Ctrl+Shift+R: YouTube, music, any program); File > New from a tempo template with beat markers
- Right-click the workspace for Create New (an empty window to paste or drag audio into) and Open Recent (also in the File menu); right-click a file's waveform for play, the edits, New, Open and Open Recent
- **Minimize** a file window (the – button): it stays open as a small bar along the bottom of the workspace; click the bar to bring it back where it was, with its tabs and unsaved changes. **Maximized** (the [] button) the files become tabs over the whole workspace; the – [] × buttons at the top right minimize, go back to windows or close the active file (Window > Cascade / Tile also go back)
- **Space plays / stops everywhere**: in the file windows and in every other window (effects and Mix play their preview, the Open window the file you clicked, De-esser the S shown)

**Playback and meters**
- Loop button on the toolbar; Q loops the selection, and a new selection while looping becomes the loop
- A click in the waveform while playing goes on from there; the toolbar's Play starts again from the cursor
- Drag strip between the channels: slides the audio past the cursor like tape, with or without sound
- Smooth Playback Scrolling (Shift+F6): the playhead stays in the middle and the audio flows under it
- Left / Right arrows move the cursor by a step that follows the zoom (Shift grows the selection); while playing they jump back / ahead
- Channel Meters (Alt+5): peak, RMS, peak hold, clip lights
- Loudness Meters (Alt+6): momentary, short-term and integrated LUFS, loudness range, true peak, a minute of history, streaming / broadcast targets (ITU BS.1770 / EBU R128)
- Tempo on the toolbar: a selection's tempo from its length in beats (2 decimals, live while dragging), or the detected tempo of the whole file
- Spectrum Analysis (Alt+9): live while playing, or the selection / the moment at the cursor; the strongest frequency with its note; FFT size, smoothing, peak hold; a sonogram (time x frequency)

**Processes**
- Fade in / out, mute, normalize (to a peak or a LUFS target, with a true-peak ceiling), volume, reverse, invert, DC offset
- Volume Envelope / Graphic Fade (V): a volume line over the waveform, drawn like a path in a drawing program (points with Bezier handles: smooth curves, corners, bells)
- Insert Silence, Resample, Bit-Depth Converter (with dither), Channel Converter, Auto Trim / Crop
- Time Stretch and Pitch Shift (Rubber Band)

**Effects** (live preview, Space plays / stops it; Apply is undoable; your own presets in every effect)
- Graphic EQ (5, 10, 15, 20 or 31 sliders; the curve passes through every slider), Parametric EQ, Compressor, Limiter (true peak; finds the gain for -14 / -16 / -23 LUFS), Noise Gate
- Delay / Echo, Reverb, Chorus, Flanger, Vibrato, Distortion (4 types, oversampled)

**Get Stems** (Tools > Get Stems, or the Get Stems button on a file window)
- Splits a song into **Vocals, Drums, Bass and Other instruments**, on your own computer (nothing is uploaded), with the AI model Demucs by Meta, which comes with the program
- The parts open as **tabs in the same window**: Original | Drums | Bass | Vocals | Other. Each tab is a full file: edit, select L / R, markers, effects, undo, play, drag in and out
- **Save** saves the tab you are looking at (on the Bass tab, the bass); a saved part remembers it is a stem, so it doesn't offer Get Stems again
- How long it takes: a few minutes, on the processor (a 2½-minute song took about 4 minutes on an Apple M1 laptop with 16 GB); it uses more cores when there is enough memory, and you can cancel it
- Tip: send the Drums tab to Drum Rebuilder and the Bass tab to Bass Creator

**Get WAV from a link** (the box next to Help)
- Paste a YouTube, TikTok, Instagram, Facebook... link and press Enter: the best audio the page has is saved as a WAV (24-bit, at the site's own sample rate) in `Music/Adormis Audio Downloads` and opens right away
- It works through the free programs [yt-dlp](https://github.com/yt-dlp/yt-dlp), [Deno](https://github.com/denoland/deno) and [ffmpeg](https://ffmpeg.org): the first time, Adormis Audio asks, then downloads them from their GitHub releases into its settings folder and checks them against the published SHA-256 sums (ffmpeg only on Windows; on macOS and Linux install it yourself, e.g. `brew install ffmpeg`). yt-dlp keeps itself up to date
- The sites send compressed audio: the WAV keeps it as it is, it doesn't make it better. Some posts need an account and can't be fetched. Only download what you have the right to use

**Repair and tools**
- Noise Reduction (from a captured noise print, or learned from the quietest parts), Hum Removal (finds 50 / 60 Hz by itself), Click and Crackle Removal, Clipped Peak Restoration
- MP3 Restore: grows back the top end an MP3 cut off (by saturation or spectral copy), smooths the "swirl", adds warmth and punch
- De-esser (**Apply to all N S's** changes every S found in the file or the selection, not just the one shown; drag on its bar, which shows the whole file, to work on another stretch, or **Whole file**), Detect Clipping, Copy Other Channel, Statistics (peak, true peak, RMS, DC offset, LUFS, LRA)
- Bass Creator: suggests a bass line for a sample that has none (also with no drums): key, tempo (auto or by hand, Tap), the chords, bass lines in 10 styles to listen to with the sample and save as MIDI or audio; a note detector to check and correct an existing bass (in the song's own tuning; mains hum ignored; Up / Down zoom, Space plays from the cursor)
- **Drum Rebuilder** (Tools > Drum Rebuilder, or the drum button next to Bass Creator): the whole [Drum Rebuilder](https://github.com/jastincheis/drum-rebuilder) inside Adormis Audio. It takes the active file (or the selection), finds the kicks, snares and hats, lets you swap them for other sounds, and sends the rebuilt beat back as a new window. Its Open list also shows every file open in Adormis Audio (stem tabs too), so you can switch beats without leaving it; its Sample Browser can import a whole folder of sounds, sorted by name (else by how they sound) into Kick, Snare, Hi-hat…, Loops and Unsorted, as links to your files
- Batch Converter: many files at once through the same steps (channels, sample rate, trim silence, normalize to a peak or a LUFS target with the limiter if needed, fades) into WAV / FLAC / AIFF / MP3 / OGG; the originals are not changed

**Remembered settings:** the effect windows, Mix, Volume and the small settings windows start where you left them (also after a restart); each effect window has a **Defaults** button. Values measured from each file (hum frequency, clip level, MP3 cut-off, tempo) are measured again.

**Look:** right-click a waveform or the empty workspace for **Waveform Colour...** (one colour for all windows) and **Background...** (textures such as the teddy bears, stripes, dots or waves in any colour, a plain colour, or your own photo behind everything).

**Updates:** when a newer version is out, a green **New update** button appears at the top right (on macOS, next to the status bar); it opens a message with Download / Skip this version / Later. Adormis Audio asks GitHub each time it starts; turn it off in **Help > Check for Updates at Start**, or ask by hand with **Help > Check for Updates...**.

**Keyboard:** the classic default shortcuts (F1 lists them); change any of them in **Options > Customize Keyboard** (saved).

Planned: VST3 plug-ins (Plug-In Manager and Plug-In Chainer).

On macOS, `Ctrl` shortcuts use `Cmd` and `Alt` uses `Option`.

## Install

Download the file for your system from the
[latest release](https://github.com/jastincheis/adormis-audio/releases).
The apps are not signed yet, so each system asks you to confirm the first time.

### macOS (11 Big Sur or newer, Apple Silicon and Intel)

1. Download `AdormisAudio-<version>-macOS.zip` (for example `AdormisAudio-0.1.16-macOS.zip`) and double-click it to unzip (Safari may do this for you).
2. Drag `Adormis Audio.app` into **Applications**.
3. Open it. macOS says it cannot check the app for malware:
   - **macOS 15 Sequoia and newer:** click **Done**, open **System Settings → Privacy & Security**,
     scroll down to *"Adormis Audio" was blocked* and click **Open Anyway**, then enter your password.
   - **macOS 14 and older:** right-click `Adormis Audio.app` → **Open** → **Open**.
4. The first time you record (Ctrl+R / Cmd+R), allow access to the microphone.

You only do this once. Alternatively, in Terminal:
`xattr -dr com.apple.quarantine "/Applications/Adormis Audio.app"`

### Windows (10 / 11, 64-bit)

1. Download `AdormisAudio-<version>-Windows.zip`, right-click it → **Extract All…** → **Extract**.
2. Double-click `Adormis Audio.exe` (keep the `models` folder next to it: Get Stems needs it).
3. If **Windows protected your PC** appears, click **More info** → **Run anyway**.
4. Optional: right-click `Adormis Audio.exe` → **Show more options** → **Send to → Desktop (create shortcut)**.

Adormis Audio plays through Windows Audio. To use an audio interface's ASIO driver, tick
**Options > Audio Device > Use ASIO drivers** and restart Adormis Audio.

### Linux (x86_64 or ARM64)

1. Check your processor: `uname -m` prints `x86_64` or `aarch64`, then download
   `AdormisAudio-<version>-Linux-x86_64.tar.gz` or `AdormisAudio-<version>-Linux-arm64.tar.gz`.
2. Install libsamplerate and libcurl (most systems already have libcurl):
   - Debian / Ubuntu / Mint: `sudo apt install libsamplerate0 libcurl4`
   - Fedora: `sudo dnf install libsamplerate libcurl`
   - Arch: `sudo pacman -S libsamplerate curl`
3. Unpack and run (keep the `models` folder next to the program: Get Stems needs it):
   ```bash
   tar -xzf AdormisAudio-*-Linux-*.tar.gz
   ./"Adormis Audio"
   ```
4. Optional, to start it by name from anywhere: `mkdir -p ~/.local/bin && mv "Adormis Audio" ~/.local/bin/adormis-audio`

### If it doesn't start

Adormis Audio writes a start-up log that says how far it got (window, audio device,
timings, and the stack of a crash):

- Windows: type `%APPDATA%\Adormis Audio` in the File Explorer address bar → `adormis-audio.log`
- macOS: `~/Library/Application Support/Adormis Audio/adormis-audio.log`
- Linux: `~/.config/Adormis Audio/adormis-audio.log`

The previous start's log is kept next to it as `adormis-audio-previous.log`. Please attach
both to an [issue](https://github.com/jastincheis/adormis-audio/issues). If a start ever
gets stuck opening the audio device, the next one starts without ASIO and without
the saved device, and says so.

### Known limitations of the preview

- The apps are not signed (see the confirmation steps above).
- macOS and Windows builds cannot export MP3 yet (Linux can), and use a simpler sample-rate converter.
- ASIO on Windows is off by default (see above).
- Recording what the computer plays needs macOS 13 or newer and the *Screen & System Audio Recording* permission (macOS asks the first time; then open Adormis Audio again). Copy-protected streams may record as silence.

## Build

Requires CMake 3.22+ and a C++20 compiler. JUCE 9.0.4 is downloaded
automatically (pinned by commit and checksum).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Linux needs the usual JUCE development packages (ALSA, JACK, FreeType,
fontconfig, X11, Xcomposite, Xcursor, Xext, Xi, Xinerama, Xrandr, Xrender,
OpenGL). Optional: libmp3lame (MP3 export) and libsamplerate (best
sample-rate conversion). Rubber Band 4.0.0, [Drum Rebuilder](https://github.com/jastincheis/drum-rebuilder)
and [demucs.cpp](https://github.com/sevagh/demucs.cpp) (with Eigen) are downloaded
automatically, pinned by commit. Linux also needs libcurl (for the update check).
To use an existing JUCE checkout instead of downloading it, add
`-DADORMIS_JUCE_DIR=/path/to/JUCE`.

Get Stems needs its model next to the program. Download
`ggml-model-htdemucs-4s-f16.bin` from
[adormis-audio-models](https://github.com/jastincheis/adormis-audio-models/releases/tag/htdemucs-4s-f16)
and add `-DADORMIS_DEMUCS_MODEL=/path/to/ggml-model-htdemucs-4s-f16.bin`;
the build copies it into `models` (or the app bundle's Resources). Without it,
Get Stems says the model is missing.

## Code signing policy

Windows builds are going to be signed through the SignPath Foundation
(application pending; until then the Windows builds are unsigned):
free code signing provided by [SignPath.io](https://about.signpath.io),
certificate by [SignPath Foundation](https://signpath.org).

- Only binaries built by this repository's public GitHub Actions workflow
  ([build.yml](.github/workflows/build.yml)) from the `main` branch are signed,
  and only for releases published on the
  [Releases page](https://github.com/jastincheis/adormis-audio/releases).
- Roles — authors, reviewers and approvers: [@jastincheis](https://github.com/jastincheis)
  (sole maintainer). Contributions from anyone else are reviewed by the
  maintainer before they are merged. Every signing request is approved manually.
- Privacy: Adormis Audio does not collect or send any data. It connects to the
  internet for the update check: each time it starts it asks GitHub for the
  list of Adormis Audio releases (nothing about you or your files is sent; turn
  it off in Help > Check for Updates at Start). Get WAV contacts the site of the
  link you paste (through yt-dlp) and, the first time, GitHub for the tools,
  only when you use it. Get Stems runs on your own
  computer. It only reads and writes the audio files you open or save, and
  keeps its settings and a start-up log in your user profile folder
  (`%APPDATA%\Adormis Audio` on Windows).

## License

GNU Affero General Public License v3.0 — see [LICENSE](LICENSE). Built with
[JUCE](https://juce.com), used under the AGPLv3. Includes
[Drum Rebuilder](https://github.com/jastincheis/drum-rebuilder) (AGPLv3, same author),
[demucs.cpp](https://github.com/sevagh/demucs.cpp) (MIT) with
[Eigen](https://eigen.tuxfamily.org) (MPL 2.0), and the
[Demucs](https://github.com/facebookresearch/demucs) htdemucs model by Meta (MIT).
