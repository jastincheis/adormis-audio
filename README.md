# WaveForge

A destructive audio editor in the spirit of classic Sound Forge: the same menu
layout and default keyboard shortcuts, running natively on **macOS** (Apple
Silicon and Intel), **Linux** and **Windows**, with VST3 plug-in support planned.

WaveForge is an independent project. It is not affiliated with, endorsed by or
derived from Sound Forge or its owners; it only follows the publicly documented
menu structure and default shortcuts so the workflow feels familiar.

## Features

- Open WAV / FLAC / AIFF / OGG / MP3; save WAV / FLAC / AIFF / OGG and MP3, with sample-rate conversion
- Several files in overlapping windows; drag a selection out into a new file, or onto another file to mix it in (volumes, live preview, snapping to markers)
- Cut / copy / paste (also to a new file), trim / crop; selection per channel (L / R), markers, undo / redo
- Drag audio files from Explorer / Finder / a file manager into the window to open them
- Processes: fade in / out, mute, normalize, reverse, invert, DC offset, volume
- Effects with live preview: parametric EQ, compressor, delay / echo, reverb
- Time Stretch (Rubber Band), De-esser, Bass Finder (key, tempo, notes, bass line)
- MP3 Restore: grows back the top end an MP3 cut off (by saturation or spectral copy), smooths the "swirl", adds warmth and punch
- Insert Silence, Resample, Bit-Depth Converter (with dither), Channel Converter, Auto Trim/Crop, Detect Clipping, Copy Other Channel
- Set Selection, Go To, Mark In / Out, Mix (Ctrl+M), Repeat Last Process (Ctrl+Y), Level Zoom, Loop Playback, Save All
- Record (Ctrl+R) with input meter, mono / stereo and bit depth
- File > New from a tempo template with beat markers

Planned: regions, more effects, VST3 plug-ins.

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

### Known limitations of the preview

- macOS and Windows builds cannot export MP3 yet (Linux can).
- ASIO on Windows is untested.

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
