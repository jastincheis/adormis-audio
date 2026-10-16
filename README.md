# WaveForge

A destructive audio editor in the spirit of classic Sound Forge: the same menu
layout and default keyboard shortcuts, running natively on **macOS** (Apple
Silicon and Intel), **Linux** and **Windows**, with VST3 plug-in support planned.

WaveForge is an independent project. It is not affiliated with, endorsed by or
derived from Sound Forge or its owners; it only follows the publicly documented
menu structure and default shortcuts so the workflow feels familiar.

## Features

**Files and editing**
- Open WAV / FLAC / AIFF / OGG / MP3; save WAV / FLAC / AIFF / OGG and MP3, with sample-rate conversion
- Several files in overlapping windows; drag audio files in from Explorer / Finder / a file manager
- Cut / copy / paste (also to a new file), trim / crop, Mix (Ctrl+M); undo / redo
- Selection on both channels or one (L / R); drag a selection's edges to change it; drag it out into a new file, or onto another file to mix it in (volumes, live preview, snapping to markers)
- Markers (drag them left / right to move them) and regions (R): a region strip under the ruler, View > Regions List, Tools > Auto Region (split at silences), Tools > Extract Regions (one file per region); WAV files keep both, as cue points with labels
- Markers and regions travel with the audio when you drag a selection out, mix it into another file, or copy and paste
- Set Selection, Go To, Mark In / Out, W / E to the selection's left / right edge, Repeat Last Process (Ctrl+Y), Level Zoom, Save All
- Record (Ctrl+R) with input meter, mono / stereo and bit depth; File > New from a tempo template with beat markers
- Right-click the workspace for Create New (an empty window to paste or drag audio into) and Open Recent (also in the File menu)

**Playback and meters**
- Loop button on the toolbar; Q loops the selection, and a new selection while looping becomes the loop
- Channel Meters (Alt+5): peak, RMS, peak hold, clip lights
- Loudness Meters (Alt+6): momentary, short-term and integrated LUFS, loudness range, true peak, a minute of history, streaming / broadcast targets (ITU BS.1770 / EBU R128)
- Tempo on the toolbar: a selection's tempo from its length in beats (2 decimals, live while dragging), or the detected tempo of the whole file

**Processes**
- Fade in / out, mute, normalize (to a peak or a LUFS target, with a true-peak ceiling), volume, reverse, invert, DC offset
- Insert Silence, Resample, Bit-Depth Converter (with dither), Channel Converter, Auto Trim / Crop
- Time Stretch and Pitch Shift (Rubber Band)

**Effects** (live preview; Apply is undoable)
- Parametric EQ, Compressor, Limiter (true peak; finds the gain for -14 / -16 / -23 LUFS), Noise Gate
- Delay / Echo, Reverb, Chorus, Flanger, Vibrato, Distortion (4 types, oversampled)

**Repair and tools**
- Noise Reduction (from a captured noise print, or learned from the quietest parts), Hum Removal (finds 50 / 60 Hz by itself), Click and Crackle Removal, Clipped Peak Restoration
- MP3 Restore: grows back the top end an MP3 cut off (by saturation or spectral copy), smooths the "swirl", adds warmth and punch
- De-esser, Detect Clipping, Copy Other Channel, Statistics (peak, true peak, RMS, DC offset, LUFS, LRA)
- Bass Finder: key, tempo, notes and a bass line
- Batch Converter: many files at once through the same steps (channels, sample rate, trim silence, normalize to a peak or a LUFS target with the limiter if needed, fades) into WAV / FLAC / AIFF / MP3 / OGG; the originals are not changed

**Remembered settings:** the effect windows, Mix, Volume and the small settings windows start where you left them (also after a restart); each effect window has a **Defaults** button. Values measured from each file (hum frequency, clip level, MP3 cut-off, tempo) are measured again.

**Keyboard:** the classic default shortcuts (F1 lists them); change any of them in **Options > Customize Keyboard** (saved).

Planned: VST3 plug-ins (Plug-In Manager and Plug-In Chainer).

On macOS, `Ctrl` shortcuts use `Cmd` and `Alt` uses `Option`.

## Install

Download the file for your system from the
[latest release](https://github.com/jastincheis/waveforge/releases).
The apps are not signed yet, so each system asks you to confirm the first time.

### macOS (11 Big Sur or newer, Apple Silicon and Intel)

1. Download `WaveForge-macOS.zip` and double-click it to unzip (Safari may do this for you).
2. Drag `WaveForge.app` into **Applications**.
3. Open it. macOS says it cannot check the app for malware:
   - **macOS 15 Sequoia and newer:** click **Done**, open **System Settings → Privacy & Security**,
     scroll down to *"WaveForge" was blocked* and click **Open Anyway**, then enter your password.
   - **macOS 14 and older:** right-click `WaveForge.app` → **Open** → **Open**.
4. The first time you record (Ctrl+R / Cmd+R), allow access to the microphone.

You only do this once. Alternatively, in Terminal:
`xattr -dr com.apple.quarantine /Applications/WaveForge.app`

### Windows (10 / 11, 64-bit)

1. Download `WaveForge-Windows.zip`, right-click it → **Extract All…** → **Extract**.
2. Double-click `WaveForge.exe`.
3. If **Windows protected your PC** appears, click **More info** → **Run anyway**.
4. Optional: right-click `WaveForge.exe` → **Show more options** → **Send to → Desktop (create shortcut)**.

WaveForge plays through Windows Audio. To use an audio interface's ASIO driver, tick
**Options > Audio Device > Use ASIO drivers** and restart WaveForge.

### Linux (x86_64 or ARM64)

1. Check your processor: `uname -m` prints `x86_64` or `aarch64`, then download
   `WaveForge-Linux-x86_64.tar.gz` or `WaveForge-Linux-arm64.tar.gz`.
2. Install libsamplerate (the only extra library it needs):
   - Debian / Ubuntu / Mint: `sudo apt install libsamplerate0`
   - Fedora: `sudo dnf install libsamplerate`
   - Arch: `sudo pacman -S libsamplerate`
3. Unpack and run:
   ```bash
   tar -xzf WaveForge-Linux-*.tar.gz
   ./WaveForge
   ```
4. Optional, to start it by name from anywhere: `mkdir -p ~/.local/bin && mv WaveForge ~/.local/bin/`

### If it doesn't start

WaveForge writes a start-up log that says how far it got (window, audio device,
timings, and the stack of a crash):

- Windows: type `%APPDATA%\WaveForge` in the File Explorer address bar → `waveforge.log`
- macOS: `~/Library/Application Support/WaveForge/waveforge.log`
- Linux: `~/.config/WaveForge/waveforge.log`

The previous start's log is kept next to it as `waveforge-previous.log`. Please attach
both to an [issue](https://github.com/jastincheis/waveforge/issues). If a start ever
gets stuck opening the audio device, the next one starts without ASIO and without
the saved device, and says so.

### Known limitations of the preview

- The apps are not signed (see the confirmation steps above).
- macOS and Windows builds cannot export MP3 yet (Linux can), and use a simpler sample-rate converter.
- ASIO on Windows is off by default (see above).

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
sample-rate conversion). Rubber Band 4.0.0 is downloaded automatically.
To use an existing JUCE checkout instead of downloading it, add
`-DWAVEFORGE_JUCE_DIR=/path/to/JUCE`.

## License

GNU Affero General Public License v3.0 — see [LICENSE](LICENSE). Built with
[JUCE](https://juce.com), used under the AGPLv3.
