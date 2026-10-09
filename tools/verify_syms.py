"""Symbols gate. Verifies syms/wg3d.us.syms.toml and runs a trial recompilation.

Checks:
  1. Function list: 100% .text coverage (gaps must be zero padding), no overlaps, word alignment,
     unique C names, every jal target is a start, no start reachable by fall-through.
  2. Library checklist: the only live gaps are the known runtime gaps (EXPECTED_GAPS).
  3. Indirect targets: tools/check_indirect.py passes.
  4. Trial N64Recomp run (entrypoint + symbols, dead library code ignored): exits 0 with no
     warnings except the known entry-stub tail call.
  5. Every generated C file passes a clang-cl syntax check. The only allowed diagnostics are
     calls to undeclared skipped functions that the runtime implements or that are EXPECTED_GAPS.

Usage: python tools/verify_syms.py
"""
import collections
import re
import struct
import subprocess
import sys
import tomllib
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_indirect  # noqa: E402
from gen_libsyms import runtime_funcs  # noqa: E402
from rom_map import MAIN_ROM, TEXT_END, TEXT_START, VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TRIAL = ROOT / "build" / "trial"
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

# Skipped functions with live callers and no runtime implementation; src/game/ implements them.
EXPECTED_GAPS = {"osPiRawReadIo"}
# The entry stub ends with `jr $t2` into boot_main (0x80003480).
EXPECTED_RECOMP_MESSAGES = {"[Info] Indirect tail call in recomp_entrypoint"}


def check_functions(rom: bytes, funcs) -> list[str]:
    errs = []
    word = lambda v: struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
    starts = {a for a, _, _ in funcs}
    names = {a: n for a, _, n in funcs}
    cursor = TEXT_START
    for a, s, n in funcs:
        if a & 3 or s & 3 or s <= 0:
            errs.append(f"{n}: misaligned or empty")
        if a < cursor:
            errs.append(f"{n}: overlaps previous function")
        elif a > cursor and any(rom[cursor + VRAM_TO_ROM:a + VRAM_TO_ROM]):
            errs.append(f"non-zero bytes {cursor:08X}-{a:08X} not in any function")
        cursor = a + s
    if cursor > TEXT_END or any(rom[cursor + VRAM_TO_ROM:TEXT_END + VRAM_TO_ROM]):
        errs.append("end of .text not covered correctly")
    for n, c in collections.Counter(n for _, _, n in funcs).items():
        if c > 1 or not re.match(r"^[A-Za-z_]\w*$", n):
            errs.append(f"bad or duplicate name {n}")
    noreturn = {a for a, n in names.items() if n == "osDestroyThread"}
    for v in range(TEXT_START, TEXT_END, 4):
        w = word(v)
        if w >> 26 == 3:
            tgt = (v & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if tgt not in starts:
                errs.append(f"jal at {v:08X} targets non-start {tgt:08X}")
    for a, _, n in funcs[1:]:
        v = a - 4
        while word(v) == 0:
            v -= 4
        ok = False
        for j in (v - 4, v):
            ins = rabbitizer.Instruction(word(j), j)
            op = ins.getOpcodeName()
            if op in ("jr", "j", "b", "eret") or (op == "beq" and ins.rs.value == 0 and ins.rt.value == 0) \
                    or (op == "jal" and ins.getInstrIndexAsVram() in noreturn):
                ok = True
        if not ok:
            errs.append(f"{n}: reachable by fall-through")
    return errs


def check_library() -> list[str]:
    md = (ROOT / "docs" / "libultra_coverage.md").read_text(encoding="utf-8")
    gaps = set(re.findall(r"^- `([^`]+)` \(`[0-9A-F]{8}`\): skipped by N64Recomp", md, re.M))
    errs = []
    if gaps != EXPECTED_GAPS:
        errs.append(f"library gaps changed: {sorted(gaps)} (expected {sorted(EXPECTED_GAPS)})")
    return errs


def trial_recompile() -> list[str]:
    errs = []
    TRIAL.mkdir(parents=True, exist_ok=True)
    dead = [n for n in (ROOT / "syms" / "dead_ignore.txt").read_text().split() if n]
    cfg = [
        "# Trial recompile: entrypoint + symbols; dead library code that calls unimplemented skipped",
        "# functions is ignored (list from tools/gen_libsyms.py -> syms/dead_ignore.txt).",
        "[input]",
        f"entrypoint = 0x{TEXT_START:08X}",
        'symbols_file_path = "../../syms/wg3d.us.syms.toml"',
        'rom_file_path = "../../baserom.us.z64"',
        'output_func_path = "RecompiledFuncs"',
        "",
        "[patches]",
        "ignored = [" + ", ".join(f'"{n}"' for n in dead) + "]",
        "",
    ]
    (TRIAL / "trial.toml").write_text("\n".join(cfg))
    out = TRIAL / "RecompiledFuncs"
    for f in out.glob("*") if out.exists() else []:
        f.unlink()
    r = subprocess.run([str(ROOT / "lib/N64Recomp/build/N64Recomp.exe"), "trial.toml"], cwd=TRIAL,
                       capture_output=True, text=True)
    (TRIAL / "n64recomp.log").write_text(r.stdout + r.stderr)
    if r.returncode != 0:
        return [f"N64Recomp exited {r.returncode}: {(r.stdout + r.stderr)[-500:]}"]
    for line in (r.stdout + r.stderr).splitlines():
        line = line.strip()
        if line and not line.startswith(("Function count:", "Working dir:")) and line not in EXPECTED_RECOMP_MESSAGES:
            errs.append(f"N64Recomp: {line}")

    # Syntax-check every generated file with clang-cl (MSVC environment via vcvars64).
    files = sorted(out.glob("funcs_*.c"))
    inc = ROOT / "lib/N64Recomp/include"
    bat = TRIAL / "syntax_check.bat"
    bat.write_text("@echo off\r\n" + f'call "{VCVARS}" >nul\r\n' + f'cd /d "{out}"\r\n' + "".join(
        f'clang-cl /nologo /Zs /TC -Wno-unused-variable -Wno-unused-label -Wno-unused-but-set-variable '
        f'/I "{inc}" {f.name} > "{TRIAL / ("cc_" + f.stem + ".log")}" 2>&1\r\n' for f in files))
    subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True)
    runtime = runtime_funcs()
    undeclared = collections.Counter()
    for f in files:
        log = (TRIAL / f"cc_{f.stem}.log").read_text(errors="replace")
        for line in log.splitlines():
            m = re.search(r"undeclared function '(\w+)_recomp'", line)
            if m:
                undeclared[m[1]] += 1
            elif re.search(r"\b(error|warning)\b", line) and "errors generated" not in line and "error generated" not in line:
                errs.append(f"{f.name}: {line.strip()}")
    for name in undeclared:
        if name not in runtime and name not in EXPECTED_GAPS:
            errs.append(f"generated code calls {name}_recomp, which neither the runtime nor the expected gaps provide")
    print(f"trial: {len(files)} C files; undeclared skipped callees: "
          + ", ".join(f"{n} ({'runtime' if n in runtime else 'expected gap'})" for n in sorted(undeclared)))
    return errs


def main() -> int:
    rom = (ROOT / "baserom.us.z64").read_bytes()
    syms = tomllib.loads((ROOT / "syms" / "wg3d.us.syms.toml").read_text(encoding="utf-8"))
    sec = syms["section"][0]
    funcs = sorted((f["vram"], f["size"], f["name"]) for f in sec["functions"])
    errs = []
    if (sec["rom"], sec["vram"], sec["size"]) != (MAIN_ROM, TEXT_START, TEXT_END - TEXT_START):
        errs.append("section header does not match the ROM map")
    if funcs[0][0] != TEXT_START:
        errs.append("no function at the entrypoint")

    steps = [("function list", lambda: check_functions(rom, funcs)),
             ("library checklist", check_library),
             ("indirect targets", lambda: [] if check_indirect.main() == 0 else ["tools/check_indirect.py failed"]),
             ("trial recompile", trial_recompile)]
    for name, fn in steps:
        e = fn()
        print(f"[{'ok' if not e else 'FAIL'}] {name}")
        errs += e
    print(f"{len(funcs)} functions, {sum(s for _, s, _ in funcs):#x} bytes of code")
    if errs:
        print("ERRORS:\n  " + "\n  ".join(errs[:40]))
        return 1
    print("PHASE 1 VERIFIED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
