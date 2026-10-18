"""Recompile gate. Rebuilds everything from the ROM and checks every result.

Steps:
  1. Symbol chain: rom_map, find_overlays, gen_libsyms, splat, export_splat, run_ghidra, gen_syms,
     check_indirect, verify_syms (the symbols gate, including its trial recompile).
  2. Recompile checks: hw_audit and check_aspmain (with --skip-symbols, hw_audit is skipped if the
     symbol chain's build/splat_jtbls.csv is missing).
  3. Clean regeneration: delete the generated recompile outputs, run gen_config (config, N64Recomp,
     declarations header, clang-cl syntax check), then a clean CMake build of WG3DRecompiled
     with 0 errors and 0 warnings.
  4. Determinism: the symbols file, config, declarations header and generated C hash the same
     as before the run.

Inputs that are kept (built in WSL with tools/wsl/*.sh): build/n64sym_*.txt, build/sigs/*.a.

Usage: python tools/verify_recompile.py [--skip-symbols]
"""
import argparse
import os
import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable

SYMBOL_STEPS = [
    ("ROM map", [PY, "tools/rom_map.py"], "."),
    ("overlays", [PY, "tools/find_overlays.py"], "."),
    ("library names", [PY, "tools/gen_libsyms.py"], "."),
    ("splat", [PY, "-m", "splat", "split", "wg3d.us.yaml"], "splat"),
    ("splat export", [PY, "tools/export_splat.py"], "."),
    ("Ghidra", [PY, "tools/run_ghidra.py"], "."),
    ("merge", [PY, "tools/gen_syms.py"], "."),
    ("indirect targets", [PY, "tools/check_indirect.py"], "."),
    ("symbols gate", [PY, "tools/verify_syms.py"], "."),
]
RECOMPILE_CHECKS = [
    ("hardware audit", [PY, "tools/hw_audit.py"], "."),
    ("aspMain check", [PY, "tools/check_aspmain.py"], "."),
]
# hw_audit needs the jump-table targets in build/splat_jtbls.csv, a symbol-chain output. With
# --skip-symbols on a machine that has never run the chain, the audit is skipped (docs/hw_audit.md
# stays from the last full run); without --skip-symbols the chain writes the file first.
SYMBOL_OUTPUTS = {"hardware audit": "build/splat_jtbls.csv"}
# Generated recompile outputs removed before regenerating (paths relative to ROOT).
# The clean build uses its own directory so the gate doesn't delete the dev build (build/cmake, wg3d.exe).
VERIFY_BUILD_DIR = "build/cmake-verify"
GENERATED = ["RecompiledFuncs", "rsp/aspMain.cpp", "include/wg3d_skipped.h", VERIFY_BUILD_DIR]
# Artifacts whose content must be identical before and after the full run.
DETERMINISTIC = ["syms/wg3d.us.syms.toml", "syms/libultra_symbols.txt", "wg3d.us.toml",
                 "include/wg3d_skipped.h", "RecompiledFuncs", "rsp/aspMain.cpp"]


def digest(rel: str) -> str:
    p = ROOT / rel
    h = hashlib.sha1()
    files = sorted(p.rglob("*")) if p.is_dir() else [p]
    for f in files:
        if f.is_file():
            h.update(str(f.relative_to(ROOT)).encode())
            h.update(f.read_bytes())
    return h.hexdigest() if p.exists() else "missing"


def run(name: str, cmd, cwd: str, env=None) -> bool:
    t = time.time()
    r = subprocess.run(cmd, cwd=ROOT / cwd, capture_output=True, text=True, env=env)
    ok = r.returncode == 0
    print(f"[{'ok' if ok else 'FAIL'}] {name} ({time.time() - t:.0f}s)")
    if not ok:
        print("    " + "\n    ".join((r.stdout + r.stderr).strip().splitlines()[-15:]))
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-symbols", action="store_true", help="skip step 1 (faster iteration)")
    args = ap.parse_args()
    t0 = time.time()
    before = {rel: digest(rel) for rel in DETERMINISTIC}
    failures = []

    steps = ([] if args.skip_symbols else SYMBOL_STEPS) + RECOMPILE_CHECKS
    for name, cmd, cwd in steps:
        needs = SYMBOL_OUTPUTS.get(name)
        if args.skip_symbols and needs and not (ROOT / needs).exists():
            print(f"[skip] {name}: {needs} missing (symbol chain not run on this machine)")
            continue
        if not run(name, cmd, cwd):
            failures.append(name)
    if failures:
        print("stopping: " + ", ".join(failures))
        return 1

    # Clean regeneration of the recompile outputs.
    for rel in GENERATED:
        p = (ROOT / rel).resolve()
        assert p.is_relative_to(ROOT) and p != ROOT
        if p.is_dir():
            shutil.rmtree(p)
        elif p.exists():
            p.unlink()
    if not run("config, recompile, declarations, syntax check", [PY, "tools/gen_config.py"], "."):
        failures.append("gen_config")
    elif not run("clean CMake build", ["cmd", "/c", str(ROOT / "tools" / "cmake_build.bat"), "WG3DRecompiled"], ".",
                 env={**os.environ, "WG3D_BUILD_DIR": VERIFY_BUILD_DIR}):
        failures.append("cmake build")
    else:
        log = (ROOT / "build" / "cmake_build.log").read_text(errors="replace")
        warnings = [l for l in log.splitlines() if "warning:" in l or "error:" in l]
        lib = ROOT / VERIFY_BUILD_DIR / "WG3DRecompiled.lib"
        print(f"    WG3DRecompiled.lib: {lib.stat().st_size if lib.exists() else 0} bytes, {len(warnings)} warnings/errors")
        if warnings or not lib.exists():
            failures.append("build diagnostics")
            print("    " + "\n    ".join(warnings[:10]))

    after = {rel: digest(rel) for rel in DETERMINISTIC}
    changed = [rel for rel in DETERMINISTIC if before[rel] != after[rel] and before[rel] != "missing"]
    print(f"[{'ok' if not changed else 'FAIL'}] determinism: " +
          ("all artifacts identical to the previous run" if not changed else "changed: " + ", ".join(changed)))
    if changed:
        failures.append("determinism")

    print(f"total {time.time() - t0:.0f}s")
    if failures:
        print("FAILED: " + ", ".join(failures))
        return 1
    print("RECOMPILE GATE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
