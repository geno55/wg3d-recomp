"""Boot gate.

  1. Recompile gate (tools/verify_phase2.py), then the full CMake build of wg3d.exe (no warnings or
     errors from this project's sources).
  2. Smoke runs of wg3d.exe in --frames mode against a throw-away data dir (build/gate_data), so
     the user's %APPDATA% saves are never touched:
       A. first boot: runs to completion by itself, no runtime errors, display lists and non-silent
          audio flow, and the Controller Pak note is created (allocate + write);
       B. restart: the note written in A is found and read back;
       C. input + ports: scripted Start/A reaches the 4-port player screen with WG3D_FAKE_PADS=4,
          and all four ports show as joined (compared with an approved golden capture).
  3. Screenshots of run A are compared with a reference ares capture (logos, legal text, title):
     each reference screen must be matched by some frame of ours (normalised correlation of 64x48
     grayscale thumbnails >= MATCH_THRESHOLD), in the same order.
  4. Prints the manual checklist.

References and goldens are derived from the ROM, so they live in build/ref/ (never committed):
  build/ref/ares_NNN.png       ares boot capture (made automatically if missing; needs ares)
  build/ref/golden_players4.png approved capture for run C (--update-goldens writes it; review it!)

Usage: python tools/verify_phase3.py [--quick] [--update-goldens]
  --quick           skip the symbol chain inside the recompile gate
  --update-goldens  write the run C golden from this run instead of comparing
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
EXE = BUILD / "cmake" / "wg3d.exe"
# The ROM to boot: WG3D_ROM, else baserom.us.z64 in the repo root (made by tools/rom_prep.py).
ROM = Path(os.environ.get("WG3D_ROM") or ROOT / "baserom.us.z64").resolve()
DATA = BUILD / "gate_data"
REF = BUILD / "ref"
PS = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
PY = sys.executable

MATCH_THRESHOLD = 0.80
# Reference screens in the ares capture (seconds after launch).
ARES_SCREENS = [("logos", 6), ("legal text", 10), ("title", 16)]
ERROR_PATTERNS = re.compile(r"CRASH|Failed to find function|exited unexpectedly|UnhandledJumpTarget|"
                            r"unknown RSP task|unexpected audio ucode|Assertion|No registered RSP ucode")


def step(name, ok, detail=""):
    print(f"[{'ok' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))
    return ok


def thumb(path, box=None, size=(64, 48)):
    """Normalised grayscale thumbnail; box = (left, top, right, bottom) as fractions of the image."""
    im = Image.open(path).convert("L")
    if box:
        w, h = im.size
        im = im.crop((int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)))
    im = im.resize(size, Image.BILINEAR)
    a = np.asarray(im, dtype=np.float64)
    a -= a.mean()
    n = np.linalg.norm(a)
    return a / n if n else a


def similarity(a, b, box=None, size=(64, 48)):
    return float((thumb(a, box, size) * thumb(b, box, size)).sum())


# Run C compares only the port 2-4 panels (where "NO CTLR DETECTED" becomes "PRESS START TO JOIN").
PORTS_234 = (0.28, 0.08, 0.95, 0.95)


def run_game(name, frames, env_extra, shots=0, shot_delay=2.0):
    """Runs wg3d.exe --frames N; optionally takes `shots` screenshots one second apart starting
    `shot_delay` s after launch. Returns (exit code, stderr text, [screenshot paths])."""
    env = {**os.environ, **env_extra}
    for k in ("WG3D_INPUT_SCRIPT", "WG3D_FAKE_PADS", "WG3D_PAKS", "WG3D_PFS_LOG", "WG3D_DUMP_AFTER"):
        if k not in env_extra:
            env.pop(k, None)
    err_path, out_path = BUILD / f"{name}.err.txt", BUILD / f"{name}.out.txt"
    with open(err_path, "w") as err, open(out_path, "w") as out:
        proc = subprocess.Popen([str(EXE), "--frames", str(frames), "--data-dir", str(DATA), str(ROM)],
                                cwd=EXE.parent, env=env, stdout=out, stderr=err)
        paths = []
        if shots:
            time.sleep(shot_delay)
            for f in [*BUILD.glob(f"{name}_*.png"), BUILD / f"{name}.png"]:
                f.unlink(missing_ok=True)
            subprocess.run(PS + [str(ROOT / "tools" / "shot_wg3d.ps1"), "-Name", name, "-Count", str(shots),
                                 "-IntervalMs", "1000"], capture_output=True, text=True)
            # shot_wg3d.ps1 names a single capture <name>.png and a series <name>_NNN.png.
            paths = sorted(BUILD.glob(f"{name}_*.png")) if shots > 1 else [p for p in [BUILD / f"{name}.png"] if p.exists()]
        try:
            code = proc.wait(timeout=frames / 60 + 60)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = "timeout"
    return code, err_path.read_text(errors="replace"), paths


def check_log(name, code, log):
    ok = step(f"{name}: exits by itself", code == 0, f"exit code {code}")
    errors = sorted({m.group(0) for m in ERROR_PATTERNS.finditer(log)})
    ok &= step(f"{name}: no runtime errors", not errors, ", ".join(errors))
    return ok


def ensure_ares_reference():
    if (REF / "ares_016.png").exists():
        return True
    print("    making the ares reference capture (build/ref/ares_*.png)...")
    r = subprocess.run(PS + [str(ROOT / "tools" / "capture_series.ps1"), "-Target", "ares", "-Count", "20",
                             "-Name", "ares_gate"], capture_output=True, text=True)
    files = sorted(BUILD.glob("ares_gate_*.png"))
    if len(files) < 20:
        print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
        return False
    REF.mkdir(parents=True, exist_ok=True)
    for f in files:
        shutil.move(str(f), REF / f.name.replace("ares_gate_", "ares_"))
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="skip the symbol chain inside the recompile gate")
    ap.add_argument("--update-goldens", action="store_true")
    args = ap.parse_args()
    t0 = time.time()
    ok = True

    # 1. Builds.
    r = subprocess.run([PY, str(ROOT / "tools" / "verify_phase2.py")] + (["--skip-phase1"] if args.quick else []),
                       cwd=ROOT, capture_output=True, text=True)
    ok &= step("recompile gate", r.returncode == 0 and "PHASE 2 VERIFIED" in r.stdout,
               "" if r.returncode == 0 else r.stdout[-800:])
    r = subprocess.run(["cmd", "/c", str(ROOT / "tools" / "cmake_build.bat"), "wg3d"], cwd=ROOT,
                       capture_output=True, text=True, env={k: v for k, v in os.environ.items() if k != "WG3D_BUILD_DIR"})
    log = (BUILD / "cmake_build.log").read_text(errors="replace")
    ours = [l for l in log.splitlines() if ("warning:" in l or "error:" in l)
            and re.search(r"wg3d-recomp[/\\](src|include)[/\\]", l)]
    ok &= step("build wg3d.exe", r.returncode == 0 and EXE.exists() and not ours, "; ".join(ours[:5]))
    if not ok:
        print("PHASE 3 GATE FAILED (build)")
        return 1

    # 2A. First boot on an empty data dir.
    if DATA.exists():
        shutil.rmtree(DATA)
    code, log, shots = run_game("gateA", 24 * 60, {"WG3D_PFS_LOG": "1"}, shots=21, shot_delay=2.0)
    ok &= check_log("A first boot", code, log)
    m = re.search(r"smoke: \d+ frames reached \(dls=(\d+) audtasks=(\d+) aibufs=(\d+)\)", log)
    dls, aud, aibufs = (int(x) for x in m.groups()) if m else (0, 0, 0)
    peaks = [int(x) for x in re.findall(r"audio: rate=\d+ samples=\d+ peak=(\d+)", log)]
    ok &= step("A gfx + audio flow", dls > 300 and aud > 600 and aibufs > 600 and max(peaks, default=0) > 1000,
               f"dls={dls} audtasks={aud} aibufs={aibufs} max peak={max(peaks, default=0)}")
    ok &= step("A pak note created", "osPfsAllocateFile(channel 0, 1792 bytes) -> 0" in log
               and re.search(r"osPfsReadWriteFile\(channel 0, file 0, write, offset 0, 1792 bytes\) -> 0", log) is not None)

    # 3. Compare A's screenshots with the ares reference.
    if not ensure_ares_reference():
        ok &= step("ares reference", False, "could not capture (is ares installed? winget install ares-emulator.ares)")
    elif not shots:
        ok &= step("A screenshots", False, "no captures")
    else:
        last = -1
        for screen, sec in ARES_SCREENS:
            ref = REF / f"ares_{sec:03d}.png"
            scores = [similarity(ref, s) for s in shots]
            best = int(np.argmax(scores))
            ok &= step(f"matches ares '{screen}' (ares {sec}s)", scores[best] >= MATCH_THRESHOLD and best > last,
                       f"best {scores[best]:.2f} at capture #{best} of {len(shots)}")
            last = best
        # Sanity: the reference screens themselves must be distinguishable with this metric.
        cross = max(similarity(REF / f"ares_{a:03d}.png", REF / f"ares_{b:03d}.png")
                    for i, (_, a) in enumerate(ARES_SCREENS) for _, b in ARES_SCREENS[i + 1:])
        ok &= step("reference screens are distinct", cross < MATCH_THRESHOLD, f"max cross-similarity {cross:.2f}")

    # 2B. Restart: the note from A is read back.
    code, log, _ = run_game("gateB", 10 * 60, {"WG3D_PFS_LOG": "1"})
    ok &= check_log("B restart", code, log)
    ok &= step("B pak note read back", "osPfsFindFile(channel 0) -> 0" in log and
               re.search(r"osPfsReadWriteFile\(channel 0, file 0, read, offset 0, 1792 bytes\) -> 0", log) is not None)

    # 2C. Input and 4 ports: Start on the title, A on Play Game -> 4-port player screen.
    code, log, shots = run_game("gateC", 21 * 60, {"WG3D_INPUT_SCRIPT": "14:START;16:A", "WG3D_FAKE_PADS": "4"},
                                shots=1, shot_delay=19.5)
    ok &= check_log("C input", code, log)
    golden = REF / "golden_players4.png"
    if not shots:
        ok &= step("C screenshot", False, "no capture")
    elif args.update_goldens or not golden.exists():
        REF.mkdir(parents=True, exist_ok=True)
        shutil.copy(shots[0], golden)
        print(f"[note] wrote golden {golden.relative_to(ROOT)}; review it (all four ports should say PRESS START TO JOIN)")
    else:
        score = similarity(golden, shots[0], PORTS_234, (128, 96))
        ok &= step("C 4-port player screen matches golden", score >= 0.95, f"similarity {score:.3f}")

    print(f"total {time.time() - t0:.0f}s")
    print("\nManual checklist:")
    for item in ["startup screens and title look right (compare build/gateA_*.png with build/ref/ares_*.png)",
                 "menus navigable with keyboard and a real gamepad; hot-plug a pad",
                 "music and sound effects audible, right pitch and speed (compare with ares)",
                 "4 real pads detected (if available)",
                 "pak: play a Short Season game to the end (WG3D_PFS_LOG=1 shows a write), restart, restore it",
                 "period clock speed comparable with ares"]:
        print(f"  [ ] {item}")
    print("\nPHASE 3 GATE " + ("PASSED (automated part)" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
