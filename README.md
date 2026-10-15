# WaveForge

A destructive audio editor in the spirit of classic Sound Forge: the same menu
layout and default keyboard shortcuts, running natively on **macOS** (Apple
Silicon and Intel), **Linux** and **Windows**, with VST3 plug-in support planned.

WaveForge is an independent project. It is not affiliated with, endorsed by or
derived from Sound Forge or its owners; it only follows the publicly documented
menu structure and default shortcuts so the workflow feels familiar.

## Status

| Stage | Scope | State |
|---|---|---|
| 1 | Main window, full menu bar, Sound Forge default shortcuts, shortcut list (F1) | ✅ |
| 2 | Open WAV/MP3/FLAC in overlapping data windows, waveform, zoom, selection, playback, save (WAV/FLAC/AIFF/OGG) | ✅ |
| 3 | Editing: delete, undo/redo, process toolbar (fades, mute, normalize, ±3 dB, DC offset, reverse, invert), drag a selection out to a new file or onto another file to mix (with volumes, live preview and marker snapping); cut/copy/paste/trim still to do | 🚧 |
| 4 | VST3 plug-ins applied to a selection | planned |
| 5 | Markers (M, double-click between markers selects), regions, more processes and effects | 🚧 (markers done) |

On macOS, `Ctrl` shortcuts use `Cmd` and `Alt` uses `Option`.

## Download

Every push builds macOS, Linux and Windows versions on GitHub Actions: open the
latest run under **Actions** and download the artifact for your system.

The macOS app is not signed. On first launch, right-click `WaveForge.app` and
choose **Open**.

## Build

Requires CMake 3.22+ and a C++20 compiler. JUCE 9.0.4 is downloaded
automatically (pinned by commit and checksum).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Linux needs the usual JUCE development packages (ALSA, JACK, FreeType,
fontconfig, X11, Xcomposite, Xcursor, Xext, Xinerama, Xrandr, Xrender, OpenGL).
To use an existing JUCE checkout instead of downloading it, add
`-DWAVEFORGE_JUCE_DIR=/path/to/JUCE`.

## License

GNU Affero General Public License v3.0 — see [LICENSE](LICENSE). Built with
[JUCE](https://juce.com), used under the AGPLv3.
