"""Drives WaveForge.exe on a real Windows desktop (GitHub Actions) like a user:
menus through UI Automation, keys through SendInput, and saves a screenshot
after every step to shots/. Every step is logged; a failing step does not stop
the others. Usage: python windows_ui_test.py path\\to\\WaveForge.exe"""

import math, struct, subprocess, sys, time, traceback, wave
from pathlib import Path

import pyautogui
from PIL import ImageGrab
from pywinauto import Desktop
from pywinauto.keyboard import send_keys

EXE = Path(sys.argv[1]).resolve()
SHOTS = Path("shots"); SHOTS.mkdir(exist_ok=True)
LOG = open("ui-test-log.txt", "w", encoding="utf-8")
counter = 0
failures = 0

def log(text):
    print(text, flush=True); LOG.write(text + "\n"); LOG.flush()

def shot(name):
    global counter
    counter += 1
    path = SHOTS / f"{counter:02d}-{name}.png"
    ImageGrab.grab(all_screens=True).save(path)
    log(f"   screenshot {path.name}")

def step(name):
    def wrap(fn):
        global failures
        log(f"== {name}")
        try:
            fn()
            log("   ok")
        except Exception:
            failures += 1
            log("   FAILED\n" + traceback.format_exc())
        try:
            shot(name)
        except Exception:
            log("   (no screenshot)")
        return fn
    return wrap

def make_wav(path, seconds=6, rate=44100):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
        frames = bytearray()
        for i in range(seconds * rate):
            l = int(9000 * math.sin(2 * math.pi * 220 * i / rate) * (0.3 + 0.7 * (i % rate) / rate))
            r = int(9000 * math.sin(2 * math.pi * 330 * i / rate))
            frames += struct.pack("<hh", l, r)
        w.writeframes(bytes(frames))

def main_window():
    """The main window: of WaveForge's top-level windows whose title ends in
    "WaveForge", the largest (popups such as an open list can match too)."""
    best = None
    for w in Desktop(backend="uia").windows(process=app.pid):
        if w.window_text().endswith("WaveForge"):
            r = w.rectangle()
            if best is None or r.width() * r.height() > best[0]:
                best = (r.width() * r.height(), w.handle)
    if best is None:
        return Desktop(backend="uia").window(title_re=r".*WaveForge$")
    return Desktop(backend="uia").window(handle=best[1])

def menu(*path):
    """Clicks a menu bar item, then each sub-item by its name."""
    win = main_window()
    win.set_focus()
    win.child_window(title=path[0], control_type="MenuItem").click_input()
    time.sleep(0.6)
    for name in path[1:]:
        find_popup_item(name).click_input()
        time.sleep(0.6)

def find_popup_item(name, timeout=5):
    """A menu item in an open popup menu (popups are windows of their own)."""
    end = time.time() + timeout
    while time.time() < end:
        for w in Desktop(backend="uia").windows(process=app.pid):
            for item in w.descendants(control_type="MenuItem"):
                if item.window_text() == name:
                    return item
        time.sleep(0.3)
    raise RuntimeError(f"menu item not found: {name}")

def close_top_window():
    send_keys("{ESC}"); time.sleep(0.4)

def waveform_point(fx, fy=0.5):
    """A point inside the active data window's waveform, as fractions of it."""
    win = main_window().wrapper_object().rectangle()
    # The data windows cascade from the top-left of the workspace; the first
    # one covers about two thirds of it.
    x = win.left + int((win.width() * 2 / 3) * (0.05 + 0.9 * fx))
    y = win.top + 120 + int((win.height() / 2 - 120) * fy)
    return x, y

# ---------------------------------------------------------------------------
wav = Path("test.wav").resolve()
make_wav(wav)
log(f"WaveForge: {EXE}")
app = subprocess.Popen([str(EXE), str(wav)])
time.sleep(8)

@step("start-with-file")
def _():
    w = main_window(); w.wait("visible", timeout=20); w.maximize(); time.sleep(1)

@step("uia-tree")
def _():
    # What UI Automation sees (names of menus, buttons...) for later steps.
    main_window().print_control_identifiers(depth=4, filename="uia-tree.txt")
    send_keys("{ESC}"); time.sleep(0.3)   # reading the tree can open a list

for name in ["File", "Edit", "View", "Insert", "Process", "Effects", "Tools", "FX Favorites", "Options", "Window", "Help"]:
    @step(f"menu-{name}")
    def _(name=name):
        menu(name); time.sleep(0.5)
    close_top_window()

@step("select-all-ctrl-a")
def _():
    pyautogui.click(*waveform_point(0.5)); time.sleep(0.3)
    send_keys("^a"); time.sleep(0.5)

for title, keys in [("copy-ctrl-c", "^c"), ("click-cursor", None), ("paste-ctrl-v", "^v"),
                    ("undo-ctrl-z", "^z"), ("paste-to-new-ctrl-e", "^e")]:
    @step(title)
    def _(keys=keys):
        if keys is None:
            pyautogui.click(*waveform_point(0.3))
        else:
            send_keys(keys)
        time.sleep(0.8)

@step("drag-select-then-trim-ctrl-t")
def _():
    main_window().set_focus()
    x1, y = waveform_point(0.25); x2, _y = waveform_point(0.6)
    pyautogui.moveTo(x1, y); pyautogui.dragTo(x2, y, duration=0.6); time.sleep(0.4)
    send_keys("^t"); time.sleep(0.8)

@step("undo-trim")
def _():
    send_keys("^z"); time.sleep(0.6)

for path in [("Process", "EQ", "Parametric EQ..."), ("Effects", "Dynamics", "Compressor..."),
             ("Effects", "Delay/Echo..."), ("Effects", "Reverb..."), ("Process", "Volume..."),
             ("Process", "Time Stretch..."), ("Tools", "De-esser..."), ("Tools", "Bass Finder..."),
             ("Tools", "MP3 Restore..."), ("Insert", "Silence..."), ("Process", "Resample..."),
             ("Process", "Bit-Depth Converter..."), ("Process", "Channel Converter..."), ("Process", "Auto Trim/Crop..."),
             ("Tools", "Detect Clipping..."), ("Edit", "Set Selection..."), ("Edit", "Go To..."),
             ("Options", "Audio Device..."), ("Help", "Keyboard Shortcuts"), ("Help", "About WaveForge")]:
    @step("open-" + path[-1].strip(".").replace(" ", "-").replace("/", "-"))
    def _(path=path):
        menu(*path); time.sleep(2)
    # Close it: Escape, then the window's own close button if still there.
    close_top_window()
    try:
        for w in Desktop(backend="uia").windows():
            t = w.window_text()
            if t and "WaveForge" not in t and w.process_id() == app.pid:
                w.close()
    except Exception:
        pass
    time.sleep(0.5)

@step("record-ctrl-r")
def _():
    main_window().set_focus(); send_keys("^r"); time.sleep(2)
close_top_window()

# WaveForge's own start-up log (audio device, timings, any crash).
import os, shutil
startup_log = Path(os.environ.get("APPDATA", "")) / "WaveForge" / "waveforge.log"
if startup_log.exists():
    shutil.copy(startup_log, "waveforge.log")
    log("\n== waveforge.log\n" + startup_log.read_text(errors="replace"))
else:
    log("\nno waveforge.log at " + str(startup_log))

log(f"\n{failures} step(s) failed")
app.kill()
sys.exit(0)
