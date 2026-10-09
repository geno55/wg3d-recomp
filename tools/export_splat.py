"""Export function bounds, jump tables and data references from the splat/spimdisasm output.

Run splat first (from splat/): python -m splat split wg3d.us.yaml
Then:                           python tools/export_splat.py

Writes:
  build/splat_funcs.csv    vram,size,name,file
  build/splat_jtbls.csv    jtbl_vram,entries,func_vram,targets(;-separated)
  build/splat_datarefs.csv vram,name,refs   (%hi/%lo-referenced data symbols)
and prints cross-checks against the main .text range and the jump tables found by tools/rom_map.py.
"""
import csv
import re
import struct
import sys
from bisect import bisect_right
from collections import Counter
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_libsyms import list_from_cpp  # noqa: E402
from rom_map import TEXT_END, TEXT_START, VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ASM = ROOT / "build" / "splat" / "asm"

INSN = re.compile(r"/\* ([0-9A-F]+) ([0-9A-F]{8}) ([0-9A-F]{8}) \*/")
NONMATCHING = re.compile(r"^nonmatching (\S+), 0x([0-9A-F]+)")
GLABEL = re.compile(r"^\s*glabel (\S+)")
ALABEL = re.compile(r"^\s*alabel (\S+)")  # spimdisasm: extra entry point inside a function
DLABEL = re.compile(r"^dlabel (\S+)")
WORD_LABEL = re.compile(r"\.word \.L([0-9A-F]{8})")
HILO = re.compile(r"%(?:hi|lo)\(([A-Za-z_][\w.]*)\)")
SYMADDR = re.compile(r"_([0-9A-F]{8})$")


def parse_text():
    funcs = []  # (vram, size, name, file)
    datarefs = Counter()
    for f in sorted(ASM.glob("*.s")):
        if f.name == "header.s":
            continue
        pending_size = {}
        cur = None
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = NONMATCHING.match(line)
            if m:
                pending_size[m[1]] = int(m[2], 16)
                continue
            m = GLABEL.match(line) or ALABEL.match(line)
            if m:
                cur = m[1]
                continue
            m = INSN.search(line)
            if m and cur:
                funcs.append((int(m[2], 16), pending_size.get(cur), cur, f.name))
                cur = None
            for sym in HILO.findall(line):
                datarefs[sym] += 1
    return funcs, datarefs


def parse_jtbls():
    tables = []
    for f in sorted((ASM / "data").glob("*.s")):
        cur, targets = None, []
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = DLABEL.match(line)
            if m:
                if cur and cur.startswith("jtbl_") and targets:
                    tables.append((int(cur[5:], 16), targets))
                cur, targets = m[1], []
                continue
            m = WORD_LABEL.search(line)
            if m and cur:
                targets.append(int(m[1], 16))
        if cur and cur.startswith("jtbl_") and targets:
            tables.append((int(cur[5:], 16), targets))
    return tables


def jtbls_from_1_1(rom: bytes) -> set[int]:
    """Re-run the lui/lw/jr jump-table scan from tools/rom_map.py for comparison."""
    w = lambda v: struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
    ins = [rabbitizer.Instruction(w(v), v) for v in range(TEXT_START, TEXT_END, 4)]
    out = set()
    for i, x in enumerate(ins):
        if x.getOpcodeName() == "jr" and x.rs.value != 31:
            reg, lo, base = x.rs.value, None, None
            for j in range(i - 1, max(0, i - 12), -1):
                y = ins[j]
                n = y.getOpcodeName()
                if n == "lw" and y.rt.value == reg and lo is None:
                    lo, base = y.getProcessedImmediate(), y.rs.value
                elif n == "lui" and lo is not None and y.rt.value == base:
                    out.add(((y.getProcessedImmediate() << 16) + lo) & 0xFFFFFFFF)
                    break
    return out


def main() -> int:
    rom = (ROOT / "baserom.us.z64").read_bytes()
    funcs, datarefs = parse_text()
    funcs.sort()
    # Split functions at alternate entry points (alabel): trim each function to the next start.
    # The split is only safe when the code before it cannot fall through (jr/j + delay slot).
    word = lambda v: struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
    split_problems = []
    for i, (v, s, n, f) in enumerate(funcs):
        if s is None and i:
            pv, ps, pn, pf = funcs[i - 1]
            funcs[i - 1] = (pv, v - pv, pn, pf)
            funcs[i] = (v, pv + ps - v, n, f)
            ins = rabbitizer.Instruction(word(v - 8), v - 8)
            if not (ins.isJump() and not ins.isJumpWithAddress() or ins.getOpcodeName() in ("j", "b")):
                split_problems.append(f"{n} splits {pn} after a non-jump ({ins.disassemble()})")
    tables = parse_jtbls()
    starts = [f[0] for f in funcs]

    out = ROOT / "build"
    with open(out / "splat_funcs.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["vram", "size", "name", "file"])
        for v, s, n, f in funcs:
            wr.writerow([f"0x{v:08X}", f"0x{s:X}" if s is not None else "", n, f])
    with open(out / "splat_jtbls.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["jtbl_vram", "entries", "func_vram", "targets"])
        for a, tg in tables:
            fv = starts[bisect_right(starts, tg[0]) - 1] if tg else 0
            wr.writerow([f"0x{a:08X}", len(tg), f"0x{fv:08X}", ";".join(f"{x:08X}" for x in tg)])
    with open(out / "splat_datarefs.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["vram", "name", "refs"])
        for name, c in sorted(datarefs.items()):
            m = SYMADDR.search(name)
            wr.writerow([f"0x{int(m[1], 16):08X}" if m else "", name, c])

    # Cross-checks
    problems = list(split_problems)
    missing_size = [n for _, s, n, _ in funcs if s is None]
    if missing_size:
        problems.append(f"{len(missing_size)} functions without a size: {missing_size[:5]}")
    cursor = TEXT_START
    gaps = []
    for v, s, n, _ in funcs:
        if v < cursor:
            problems.append(f"overlap at {v:08X} ({n})")
        elif v > cursor:
            gap = rom[cursor + VRAM_TO_ROM:v + VRAM_TO_ROM]
            if any(gap):
                gaps.append((cursor, v))
        cursor = v + (s or 0)
    if cursor != TEXT_END and any(rom[cursor + VRAM_TO_ROM:TEXT_END + VRAM_TO_ROM]):
        gaps.append((cursor, TEXT_END))
    if gaps:
        problems.append("non-zero gaps between functions: " + ", ".join(f"{a:08X}-{b:08X}" for a, b in gaps[:10]))

    # Jump tables: entries must land inside the owning function.
    # Tables owned by functions N64Recomp skips (e.g. libultra's handwritten __osException, whose
    # table targets local labels such as the `panic` path) are never recompiled, so they only warrant a note.
    sl = ROOT / "lib/N64Recomp/src/symbol_lists.cpp"
    skipped = list_from_cpp(sl, "reimplemented_funcs") | list_from_cpp(sl, "ignored_funcs")
    ends = {v: v + (s or 0) for v, s, _, _ in funcs}
    names = {v: n for v, _, n, _ in funcs}
    bad_jt, skipped_jt = [], []
    for a, tg in tables:
        fv = starts[bisect_right(starts, tg[0]) - 1]
        if not all(fv <= x < ends[fv] for x in tg):
            (skipped_jt if names[fv] in skipped else bad_jt).append(a)
    for a in skipped_jt:
        print(f"note: jump table {a:08X} spans function bounds, but its owner is skipped by N64Recomp")
    if bad_jt:
        problems.append("jump tables with targets outside their function: " + ", ".join(f"{a:08X}" for a in bad_jt))
    jt_splat = {a for a, _ in tables}
    jt_11 = jtbls_from_1_1(rom)
    if jt_11 - jt_splat:
        problems.append("jump tables found by rom_map but not by spimdisasm: " + ", ".join(f"{a:08X}" for a in sorted(jt_11 - jt_splat)))

    # Every direct call must land on a function start (N64Recomp emits calls by function symbol).
    start_set = set(starts)
    bad_jal = set()
    for v in range(TEXT_START, TEXT_END, 4):
        w, = struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])
        if w >> 26 == 3:  # jal
            target = (v & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if TEXT_START <= target < TEXT_END and target not in start_set:
                bad_jal.add(target)
    if bad_jal:
        problems.append("jal targets that are not function starts: " + ", ".join(f"{a:08X}" for a in sorted(bad_jal)))

    lib_named = sum(1 for _, _, n, _ in funcs if not n.startswith("func_"))
    print(f"functions: {len(funcs)} ({lib_named} named from symbol_addrs, {len(funcs) - lib_named} func_XXXXXXXX)")
    print(f"jump tables: {len(tables)} (rom_map scan: {len(jt_11)}; spimdisasm-only: {len(jt_splat - jt_11)})")
    print(f"%hi/%lo-referenced symbols: {len(datarefs)}")
    print(f".text coverage: {TEXT_START:08X}-{TEXT_END:08X}, {len(gaps)} non-zero gaps")
    if problems:
        print("PROBLEMS:\n  " + "\n  ".join(problems))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
