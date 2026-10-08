"""Chunk 4.1: scenario harness.

A scenario (tests/scenarios/<name>.toml) is a repeatable path into the game: timed input, timed
screenshots, extra WG3D_* settings and expected log lines. This runs it on wg3d.exe (unattended) and/or
on ares (interactive: keystrokes go to the focused window, see below), and compares the screenshots
with approved goldens and with each other.

Scenario file:
    description = "..."
    seconds = 20                          # run length (wg3d: --frames seconds*60)
    input = [[14.0, "START"], [15.5, "Y=-1", 0.2]]   # [time, spec, optional duration]; spec as
                                          # WG3D_INPUT_SCRIPT: A B Z START L R DU DD DL DR CU CD CL CR, X=/Y=
    captures = [[17.0, "menu"]]           # [time, label] or [time, label, "nogolden"] (varies run to run,
                                          # e.g. in-game shots: the game's random seed depends on timing)
    env = { WG3D_FAKE_PADS = "4" }        # optional extra environment for wg3d
    expect = ["regex", ...]               # optional: must appear in wg3d's stderr
    threshold = 0.95                      # optional: golden similarity threshold (default 0.95)
    ares = true                           # optional: false if the scenario can't run on ares
    pak = true                            # optional: false to start without a Controller Pak

Times are seconds from window creation. wg3d's clock (wg3d::input::script_time) counts VIs from game
start, which is about GAME_START seconds after the window opens, so the harness subtracts that. ares
boots about ARES_OFFSET seconds after its window opens (it runs the PIF/IPL boot), so its events are
shifted by that much.

Speed: wg3d runs --speed 10 by default: VIs in lockstep with the game, rendering only just before each
capture, audio muted, and the game held on each capture's exact VI (src/main/capture.cpp). Sessions run
--jobs 3 at a time. --speed 1 --jobs 1 is the old real-time behaviour; --frame-log forces --speed 1.

Outputs (ROM-derived, git-ignored):
    build/scen/<name>/wg3d/<label>.png, build/scen/<name>/ares/<label>.png, build/scen/<name>/compare.png
    goldens: build/ref/scen/<name>/<label>.png (written by --update-goldens; review them!)

ares runs need --interactive: the harness focuses the ares window and sends keystrokes with SendInput.
Before every keystroke it checks that ares is still the foreground window and aborts otherwise, so input
never lands in another application. Don't use the mouse or keyboard during an ares run.

Usage:
    python tools/run_scenario.py all|<name>... [--target wg3d|ares|both] [--interactive] [--update-goldens]
                                 [--speed N] [--jobs N]
    python tools/run_scenario.py --list
    python tools/run_scenario.py all --exe build/cmake-trace/wg3d.exe --coverage   (chunk 4.2 coverage)
Exit code 1 if any wg3d run fails (errors, missing expects, golden mismatch).
"""
import argparse
import concurrent.futures
import ctypes
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
from ctypes import wintypes
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
EXE = BUILD / "cmake" / "wg3d.exe"
# The ROM to boot: WG3D_ROM, else baserom.us.z64 in the repo root (made by tools/rom_prep.py).
ROM = Path(os.environ.get("WG3D_ROM") or ROOT / "baserom.us.z64").resolve()
SCEN_DIR = ROOT / "tests" / "scenarios"
OUT = BUILD / "scen"
GOLDEN = BUILD / "ref" / "scen"
ARES_OFFSET = 2.5
# wg3d's game starts ~0.4 s after its window opens ("[wg3d] game started ..." in its log); scenario times
# were recorded from window creation.
GAME_START = 0.4
ERROR_PATTERNS = re.compile(r"CRASH|Failed to find function|exited unexpectedly|UnhandledJumpTarget|"
                            r"unknown RSP task|unexpected audio ucode|Assertion|No registered RSP ucode|capture: .* FAILED")


# ---------------------------------------------------------------------------------------------
# Image comparison (same metric as tools/verify_phase3.py)
# ---------------------------------------------------------------------------------------------
def thumb(path, size=(64, 48)):
    a = np.asarray(Image.open(path).convert("L").resize(size, Image.BILINEAR), dtype=np.float64)
    a -= a.mean()
    n = np.linalg.norm(a)
    return a / n if n else a


def similarity(a, b):
    return float((thumb(a) * thumb(b)).sum())


def load(name):
    sc = tomllib.loads((SCEN_DIR / f"{name}.toml").read_text(encoding="utf-8"))
    sc["name"] = name
    sc.setdefault("input", [])
    sc.setdefault("captures", [])
    sc.setdefault("env", {})
    sc.setdefault("expect", [])
    sc.setdefault("threshold", 0.95)
    sc.setdefault("ares", True)
    sc.setdefault("pak", True)
    return sc


def game_time(t):
    return round(max(0.0, t - GAME_START), 3)


def input_script(sc):
    parts = []
    for ev in sc["input"]:
        parts.append(f"{game_time(ev[0])}:{ev[1]}" + (f":{ev[2]}" if len(ev) > 2 else ""))
    return ";".join(parts)


# ---------------------------------------------------------------------------------------------
# wg3d
# ---------------------------------------------------------------------------------------------
def run_wg3d(sc, update_goldens, exe=EXE, coverage=False, frame_log=False, extra_env=None, tag="", speed=1):
    name = sc["name"]
    out_root = OUT / (name + (f"@{tag}" if tag else ""))
    out = out_root / "wg3d"
    data = out_root / "data"
    for d in (out, data):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("WG3D_")}
    env.update({k: str(v) for k, v in sc["env"].items()})
    env.update(extra_env or {})
    env["WG3D_INPUT_SCRIPT"] = input_script(sc)
    env["WG3D_CAPTURE"] = ",".join(f"{game_time(c[0])}:{c[1]}" for c in sc["captures"])
    env["WG3D_CAPTURE_DIR"] = str(out)
    if not sc["pak"]:
        env["WG3D_PAKS"] = "none"
    if coverage:
        env["WG3D_COVERAGE_FILE"] = str(out_root / "coverage.txt")
    if frame_log:
        env["WG3D_FRAME_LOG"] = str(out_root / "frames.log")
    err_path = out_root / "wg3d.err.txt"
    with open(err_path, "w") as err, open(out_root / "wg3d.out.txt", "w") as so:
        proc = subprocess.Popen([str(exe), "--frames", str(int(sc["seconds"] * 60)), "--speed", str(speed),
                                 "--data-dir", str(data), str(ROM)], cwd=exe.parent, env=env, stdout=so, stderr=err)
        try:
            code = proc.wait(timeout=sc["seconds"] + 60)  # real time is the worst case at any speed
        except subprocess.TimeoutExpired:
            proc.kill()
            code = "timeout"
    log = err_path.read_text(errors="replace")

    problems = []
    if code != 0:
        problems.append(f"exit code {code}")
    problems += sorted({f"log: {m.group(0)}" for m in ERROR_PATTERNS.finditer(log)})
    problems += [f"missing expected log line /{p}/" for p in sc["expect"] if not re.search(p, log)]
    for bmp in out.glob("*.bmp"):
        Image.open(bmp).save(bmp.with_suffix(".png"))
        bmp.unlink()
    results = {}
    for cap in sc["captures"]:
        label = cap[1]
        png = out / f"{label}.png"
        if not png.exists():
            problems.append(f"capture '{label}' missing")
            continue
        golden = GOLDEN / name / f"{label}.png"
        if len(cap) > 2 and cap[2] == "nogolden":
            results[label] = "captured"
        elif update_goldens:
            golden.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(png, golden)
            results[label] = "golden written"
        elif golden.exists():
            s = similarity(golden, png)
            results[label] = f"golden {s:.3f}"
            if s < sc["threshold"]:
                problems.append(f"capture '{label}' differs from golden ({s:.3f} < {sc['threshold']})")
        else:
            results[label] = "no golden"
    return problems, results


# ---------------------------------------------------------------------------------------------
# ares (interactive)
# ---------------------------------------------------------------------------------------------
user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32")
user32.SetProcessDPIAware()  # real pixel sizes for GetClientRect/PrintWindow on scaled displays

# ares keyboard (rawinput) key indices, for its settings.bml mapping strings "0x1/0/<index>".
ARES_KEY_INDEX = {"X": 58, "C": 37, "Z": 60, "Return": 91, "Q": 51, "E": 39, "I": 43, "K": 45, "J": 44, "L": 46,
                  "T": 54, "G": 41, "F": 40, "H": 42, "Up": 86, "Down": 87, "Left": 88, "Right": 89}
# Same keys as wg3d's keyboard map (src/game/input.cpp): (ares setting name, key, scancode, extended)
ARES_BINDINGS = {
    "A": ("A", "X", 0x2D, False), "B": ("B", "C", 0x2E, False), "Z": ("Z", "Z", 0x2C, False),
    "START": ("Start", "Return", 0x1C, False), "L": ("L", "Q", 0x10, False), "R": ("R", "E", 0x12, False),
    "CU": ("C-Up", "I", 0x17, False), "CD": ("C-Down", "K", 0x25, False), "CL": ("C-Left", "J", 0x24, False),
    "CR": ("C-Right", "L", 0x26, False), "DU": ("Up", "T", 0x14, False), "DD": ("Down", "G", 0x22, False),
    "DL": ("Left", "F", 0x21, False), "DR": ("Right", "H", 0x23, False),
    # ares v148 drops bindings on the N64 pad's L-Up/L-Down/... entries when it loads the settings file;
    # the stick is bound through the X-Axis/Y-Axis pairs (Lo = left/up, Hi = right/down).
    "STICK_UP": ("Y-Axis/Lo", "Up", 0x48, True), "STICK_DOWN": ("Y-Axis/Hi", "Down", 0x50, True),
    "STICK_LEFT": ("X-Axis/Lo", "Left", 0x4B, True), "STICK_RIGHT": ("X-Axis/Hi", "Right", 0x4D, True),
}


def ares_exe():
    base = Path(os.environ["LOCALAPPDATA"]) / "Microsoft" / "WinGet" / "Packages"
    found = sorted(base.glob("ares-emulator.ares_*/ares-v*/ares.exe"))
    return found[-1] if found else None


def ares_settings(exe):
    """Copy of ares's settings.bml with N64 port 1 bound to wg3d's keyboard keys and Input/Defocus = Block."""
    src = (exe.parent / "settings.bml").read_text(encoding="utf-8").splitlines()
    want = {setting: f"0x1/0/{ARES_KEY_INDEX[key]};;" for setting, key, _, _ in ARES_BINDINGS.values()}
    out, path = [], []
    for line in src:
        stripped = line.lstrip(" ")
        depth = (len(line) - len(stripped)) // 2
        key = stripped.split(":", 1)[0]
        path = path[:depth] + [key]
        sub = "/".join(path[4:])
        if path[:4] == ["Nintendo64", "Input", "Controller.Port.1", "Gamepad"] and sub in want:
            line = " " * (depth * 2) + f"{key}: {want[sub]}"
        # Keep emulating when unfocused (so background captures work) but ignore input then; a digital
        # key on the stick gives full tilt at once (default modes ramp it, which short taps may not survive).
        if path == ["Input", "Defocus"]:
            line = " " * (depth * 2) + "Defocus: Block"
            out.append(line)
            line = " " * (depth * 2) + "DigitalToAnalog: Immediate"
        elif path == ["Input", "DigitalToAnalog"]:
            continue
        out.append(line)
    dst = BUILD / "ares" / "settings.bml"
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(out) + "\n", encoding="utf-8")
    return dst


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def send_key(scancode, extended, up):
    flags = 0x0008 | (0x0001 if extended else 0) | (0x0002 if up else 0)  # SCANCODE | EXTENDEDKEY | KEYUP
    inp = INPUT(type=1, ki=KEYBDINPUT(0, scancode, flags, 0, 0))
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def find_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def window_pid(hwnd):
    p = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
    return p.value


def window_title(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, buf, 256)
    return buf.value


def largest_window(pid):
    """ares can show more than one top-level window; the game view is the largest."""
    best, best_area = None, 0

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        nonlocal best, best_area
        if window_pid(hwnd) == pid and user32.IsWindowVisible(hwnd):
            rc = wintypes.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(rc))
            if rc.right * rc.bottom > best_area:
                best, best_area = hwnd, rc.right * rc.bottom
        return True
    user32.EnumWindows(cb, 0)
    return best


def bring_to_front(hwnd):
    """SetForegroundWindow is refused for background processes; attaching to the current foreground
    window's input thread for the call is the standard workaround (no keystrokes involved)."""
    kernel32 = ctypes.WinDLL("kernel32")
    fg = user32.GetForegroundWindow()
    fg_thread = user32.GetWindowThreadProcessId(fg, None)
    me = kernel32.GetCurrentThreadId()
    attached = fg_thread and fg_thread != me and user32.AttachThreadInput(me, fg_thread, True)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    if attached:
        user32.AttachThreadInput(me, fg_thread, False)


def print_window(hwnd, path):
    rc = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    w, h = rc.right, rc.bottom
    hdc_screen = user32.GetDC(None)
    hdc = gdi32.CreateCompatibleDC(hdc_screen)
    bmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    old = gdi32.SelectObject(hdc, bmp)
    user32.PrintWindow(hwnd, hdc, 3)
    gdi32.SelectObject(hdc, old)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                    ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                    ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]
    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(hdc)
    user32.ReleaseDC(None, hdc_screen)
    Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB").save(path)


def stick_keys(spec_part):
    axis, value = spec_part.split("=")
    v = float(value)
    if axis == "X":
        return ["STICK_RIGHT"] if v > 0.5 else ["STICK_LEFT"] if v < -0.5 else []
    return ["STICK_UP"] if v > 0.5 else ["STICK_DOWN"] if v < -0.5 else []


def run_ares(sc):
    exe = ares_exe()
    if not exe:
        return ["ares not installed (winget install ares-emulator.ares)"]
    name = sc["name"]
    out = OUT / name / "ares"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    settings = ares_settings(exe)
    proc = subprocess.Popen([str(exe), "--kiosk", "--no-file-prompt", "--settings-file", str(settings),
                             "--system", "Nintendo 64", str(ROM)])
    try:
        hwnd = None
        for _ in range(150):
            hwnd = find_window(proc.pid)
            if hwnd:
                break
            time.sleep(0.1)
        if not hwnd:
            return ["ares window did not appear"]
        t0 = time.monotonic()
        for _ in range(5):
            bring_to_front(hwnd)
            time.sleep(0.3)
            if window_pid(user32.GetForegroundWindow()) == proc.pid:
                break
        if window_pid(user32.GetForegroundWindow()) != proc.pid:
            return ["could not bring ares to the foreground (click it, then rerun)"]

        # Timeline of (time, kind, payload) on ares's clock.
        events = []
        for ev in sc["input"]:
            t, spec = ev[0] + ARES_OFFSET, ev[1]
            dur = ev[2] if len(ev) > 2 else 0.15
            keys = []
            for part in spec.split("+"):
                keys += stick_keys(part) if "=" in part else [part]
            for k in keys:
                if k not in ARES_BINDINGS:
                    return [f"no ares binding for '{k}'"]
                events.append((t, "down", k))
                events.append((t + dur, "up", k))
        for c in sc["captures"]:
            events.append((c[0] + ARES_OFFSET, "capture", c[1]))
        events.sort(key=lambda e: (e[0], e[1] != "up"))

        held = set()
        blank_captures = []
        try:
            for t, kind, payload in events:
                wait = t - (time.monotonic() - t0)
                if wait > 0:
                    time.sleep(wait)
                if kind == "capture":
                    # PrintWindow on ares's OpenGL window sometimes returns an all-black image (seen on 8
                    # of 26 captures, including mid-gameplay frames); retry briefly before keeping one.
                    path = out / f"{payload}.png"
                    for attempt in range(8):
                        print_window(largest_window(proc.pid) or hwnd, path)
                        if np.asarray(Image.open(path).convert("L")).max() > 0:
                            break
                        time.sleep(0.04)
                    else:
                        blank_captures.append(payload)
                    continue
                fg = user32.GetForegroundWindow()
                if window_pid(fg) != proc.pid:
                    return [f"ares lost focus at {t:.1f}s (foreground: '{window_title(fg)}', pid {window_pid(fg)}); "
                            "aborted before sending a key elsewhere"]
                _, _, scancode, ext = ARES_BINDINGS[payload]
                send_key(scancode, ext, kind == "up")
                (held.discard if kind == "up" else held.add)(payload)
        finally:
            if window_pid(user32.GetForegroundWindow()) == proc.pid:
                for k in held:
                    _, _, scancode, ext = ARES_BINDINGS[k]
                    send_key(scancode, ext, True)
        if blank_captures:
            return [f"ares capture still black after retries: {', '.join(blank_captures)}"]
    finally:
        proc.kill()
    return []


# ---------------------------------------------------------------------------------------------
def side_by_side(sc):
    rows = []
    for c in sc["captures"]:
        label = c[1]
        ours = OUT / sc["name"] / "wg3d" / f"{label}.png"
        theirs = OUT / sc["name"] / "ares" / f"{label}.png"
        if ours.exists() or theirs.exists():
            rows.append((label, ours if ours.exists() else None, theirs if theirs.exists() else None))
    if not rows:
        return None
    w, h = 320, 240
    sheet = Image.new("RGB", (2 * w, len(rows) * (h + 16)), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for i, (label, a, b) in enumerate(rows):
        y = i * (h + 16)
        for j, p in enumerate((a, b)):
            if p:
                sheet.paste(Image.open(p).convert("RGB").resize((w, h)), (j * w, y + 16))
        sim = f"  similarity {similarity(a, b):.2f}" if a and b else ""
        d.text((4, y + 2), f"{label}: wg3d (left) | ares (right){sim}", fill=(255, 255, 0))
    path = OUT / sc["name"] / "compare.png"
    sheet.save(path)
    return path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--target", choices=["wg3d", "ares", "both"], default="wg3d")
    ap.add_argument("--interactive", action="store_true", help="required for ares runs (sends keystrokes)")
    ap.add_argument("--update-goldens", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--exe", type=Path, default=EXE, help="wg3d.exe to run (e.g. build/cmake-trace/wg3d.exe)")
    ap.add_argument("--env", action="append", default=[], metavar="K=V", help="extra environment for wg3d (repeatable)")
    ap.add_argument("--tag", default="", help="write to build/scen/<name>@<tag>/ (repeat runs side by side)")
    ap.add_argument("--frame-log", action="store_true",
                    help="write build/scen/<name>/frames.log for tools/frame_pacing.py (chunk 4.4)")
    ap.add_argument("--coverage", action="store_true",
                    help="write build/scen/<name>/coverage.txt (needs a WG3D_TRACE build, see --exe)")
    ap.add_argument("--speed", type=int, default=10, help="wg3d game speed (1 = real time)")
    ap.add_argument("--jobs", type=int, default=3, help="wg3d sessions to run at once")
    args = ap.parse_args()
    if args.frame_log and args.speed != 1:
        print("--frame-log measures real-time pacing: using --speed 1")
        args.speed = 1

    all_names = sorted(p.stem for p in SCEN_DIR.glob("*.toml"))
    if args.list or not args.names:
        for n in all_names:
            print(f"{n:22} {load(n).get('description', '')}")
        return 0
    names = all_names if args.names == ["all"] else args.names
    if args.target != "wg3d" and not args.interactive:
        print("ares runs send keystrokes to the ares window; pass --interactive and don't touch the keyboard/mouse.")
        return 2

    failed = False
    start = time.time()
    if args.target in ("wg3d", "both"):
        # Each session has its own process, data folder and output folder, so they can run side by side.
        extra = dict(kv.split("=", 1) for kv in args.env)

        def one(n):
            t = time.time()
            problems, results = run_wg3d(load(n), args.update_goldens, args.exe.resolve(), args.coverage,
                                         args.frame_log, extra, args.tag, args.speed)
            return n, problems, results, time.time() - t

        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for n, problems, results, secs in pool.map(one, names):
                status = "ok" if not problems else "FAIL"
                print(f"[{status}] {n} wg3d ({secs:.0f}s) " + ", ".join(f"{k}: {v}" for k, v in results.items()))
                for p in problems:
                    print(f"    {p}")
                failed |= bool(problems)
    for n in names:
        sc = load(n)
        if args.target in ("ares", "both"):
            if not sc["ares"]:
                print(f"[skip] {n} ares (scenario not runnable on ares)")
            else:
                problems = run_ares(sc)
                print(f"[{'ok' if not problems else 'WARN'}] {n} ares" + "".join(f"\n    {p}" for p in problems))
        sheet = side_by_side(sc) if not args.tag else None
        if sheet:
            print(f"    -> {sheet.relative_to(ROOT)}")
    print(f"total {time.time() - start:.0f}s (speed {args.speed}, {args.jobs} jobs)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
