"""Chunk 2.1: hardware-access audit of the code that will be recompiled.

Recompiled loads/stores go to rdram + (addr - 0x80000000). librecomp commits 512MB of that
(0x80000000-0x9FFFFFFF), so KSEG1 (0xA0000000+), MMIO, PIF and cartridge addresses fault.
Every such access in compiled code (not skipped by N64Recomp, not in the dead-code ignore
list) needs a decision in DECISIONS below.

Also checked:
  - cop0 / cache / eret / tlb / sync instructions (N64Recomp would reject most of them),
  - FPU control (ctc1/cfc1), which the runtime models (rounding mode),
  - odd FPRs used as doubles, which decides `uses_mips3_float_mode`.

Usage: python tools/hw_audit.py   -> docs/hw_audit.md, exits 1 on any undecided hit
"""
import csv
import struct
import sys
import tomllib
from collections import defaultdict
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from find_overlays import field  # noqa: E402
from gen_libsyms import list_from_cpp  # noqa: E402
from rom_map import DATA_END, RSP_TEXT_END, VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

REGIONS = [
    (0x80000000, 0x80800000, "RDRAM (KSEG0)", True),
    (0x80800000, 0xA0000000, "KSEG0 beyond 8MB", True),
    (0xA0000000, 0xA0800000, "RDRAM via KSEG1 (uncached)", False),
    (0xA3F00000, 0xA4000000, "RDRAM registers", False),
    (0xA4000000, 0xA4100000, "SP (RSP DMEM/IMEM/regs)", False),
    (0xA4100000, 0xA4300000, "DP (RDP) regs", False),
    (0xA4300000, 0xA4400000, "MI regs", False),
    (0xA4400000, 0xA4500000, "VI regs", False),
    (0xA4500000, 0xA4600000, "AI regs", False),
    (0xA4600000, 0xA4700000, "PI regs", False),
    (0xA4700000, 0xA4800000, "RI regs", False),
    (0xA4800000, 0xA4900000, "SI regs", False),
    (0xA5000000, 0xBFC00000, "Cartridge / 64DD domains", False),
    (0xBFC00000, 0xBFC00800, "PIF ROM/RAM", False),
]

# Decisions for hits, keyed by function name. Each: (decision, reason).
#   stub     - function becomes a no-op in [patches] stubs
#   patch    - instruction patch in [patches] instruction
#   runtime  - replaced/implemented by the runtime (Phase 3)
#   harmless - address is computed but never dereferenced by recompiled code
DECISIONS: dict[str, tuple[str, str]] = {}


def region(addr: int):
    for lo, hi, name, ok in REGIONS:
        if lo <= addr < hi:
            return name, ok
    return ("unmapped (< 0x80000000)" if addr < 0x80000000 else "unmapped (KSEG2/3)"), False


MEMOPS = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "lwl", "lwr", "ldl", "ldr", "ll", "lld",
          "sb", "sh", "sw", "sd", "swl", "swr", "sdl", "sdr", "sc", "scd", "lwc1", "ldc1", "swc1", "sdc1"}
DOUBLE_OPS = {"ldc1", "sdc1", "dmtc1", "dmfc1"}
# Skipped libultra functions that really access MMIO: the scanner must flag each of them.
SELFTEST = ["__osSiRawStartDma", "__osViSwapContext", "osPiRawStartDma", "__osSpSetStatus", "osAiSetNextBuffer"]


def scan(funcs, word, jtbl_targets):
    """Constant-propagating linear sweep per function, restarted at control-flow merge points."""
    hits, special, fpu_ctrl, odd_double = [], [], [], []
    for a, size, name in funcs:
        insns = [rabbitizer.Instruction(word(v), v) for v in range(a, a + size, 4)]
        merges = set(jtbl_targets)
        for x in insns:
            if x.isBranch() or x.getOpcodeName() in ("b", "j"):
                merges.add(x.getBranchVramGeneric() if x.isBranch() else x.getInstrIndexAsVram())
        regs = {}  # reg -> constant built from lui/addiu/ori
        reset_after = None
        likely_slot = False
        for i, x in enumerate(insns):
            v = a + 4 * i
            op = x.getOpcodeName()
            rs, rt, rd = field(x, "rs"), field(x, "rt"), field(x, "rd")
            if v in merges or v == reset_after:
                regs = {}
            if op in ("mfc0", "mtc0", "cache", "eret", "sync") or op.startswith("tlb"):
                special.append((name, v, x.disassemble()))
            if op in ("ctc1", "cfc1"):
                fpu_ctrl.append((name, v, x.disassemble()))
            if op in DOUBLE_OPS or ".d" in op:
                for f in ("fs", "ft", "fd"):
                    r = field(x, f)
                    if r is not None and r % 2:
                        odd_double.append((name, v, x.disassemble()))
                        break
            if likely_slot:
                # Delay slot of a branch-likely: on the fall-through path it is cancelled.
                likely_slot = False
                for r, m in ((rt, x.modifiesRt), (rd, x.modifiesRd)):
                    if r is not None and m():
                        regs.pop(r, None)
                continue
            if op in ("jr", "j", "b") or (op == "beq" and rs == 0 and rt == 0):
                reset_after = v + 8  # code after the delay slot is reachable only by a branch
            if x.isBranchLikely():
                likely_slot = True

            if op == "lui":
                regs[rt] = (x.getProcessedImmediate() << 16) & 0xFFFFFFFF
                continue
            if op in ("addiu", "ori"):
                if rs in regs:
                    imm = x.getProcessedImmediate()
                    val = (regs[rs] + imm if op == "addiu" else regs[rs] | (imm & 0xFFFF)) & 0xFFFFFFFF
                    regs[rt] = val
                    # Only MMIO / PIF / uncached-RDRAM constants are reported. Values in other ranges
                    # are mostly display-list command words (G_SETOTHERMODE 0xB9/0xBA, G_MOVEWORD 0xBC,
                    # G_SETCOMBINE 0xFC...), i.e. data. Real dereferences of any range are caught as
                    # load/store hits below.
                    if 0xA0000000 <= val < 0xA4900000 or 0xBFC00000 <= val < 0xBFC00800:
                        hits.append((name, v, "address constant", val, region(val)[0], x.disassemble()))
                else:
                    regs.pop(rt, None)
                continue
            if op in MEMOPS and rs in regs:
                addr = (regs[rs] + x.getProcessedImmediate()) & 0xFFFFFFFF
                name_r, ok = region(addr)
                if not ok:
                    hits.append((name, v, "load/store", addr, name_r, x.disassemble()))
            if rt is not None and x.modifiesRt():
                regs.pop(rt, None)
            if rd is not None and x.modifiesRd():
                regs.pop(rd, None)
            if x.isFunctionCall():
                for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                    regs.pop(r, None)
    return hits, special, fpu_ctrl, odd_double


def main() -> int:
    rom = (ROOT / "baserom.us.z64").read_bytes()
    word = lambda v: struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
    syms = tomllib.loads((ROOT / "syms" / "wg3d.us.syms.toml").read_text(encoding="utf-8"))
    funcs = sorted((f["vram"], f["size"], f["name"]) for f in syms["section"][0]["functions"])
    sl = ROOT / "lib/N64Recomp/src/symbol_lists.cpp"
    skipped = list_from_cpp(sl, "reimplemented_funcs") | list_from_cpp(sl, "ignored_funcs")
    dead = set((ROOT / "build" / "dead_ignore.txt").read_text().split())
    compiled = [f for f in funcs if f[2] not in skipped and f[2] not in dead]
    jtbl_targets = set()
    for r in csv.DictReader(open(ROOT / "build" / "splat_jtbls.csv")):
        jtbl_targets |= {int(x, 16) for x in r["targets"].split(";")}

    selftest_funcs = [f for f in funcs if f[2] in SELFTEST]
    st_hits = scan(selftest_funcs, word, jtbl_targets)[0]
    missed = sorted(set(SELFTEST) - {h[0] for h in st_hits})
    if missed or len(selftest_funcs) != len(SELFTEST):
        print(f"SELF-TEST FAILED: scanner misses MMIO in {missed or 'missing functions'}")
        return 1

    hits, special, fpu_ctrl, odd_double = scan(compiled, word, jtbl_targets)

    # Broad checks that don't depend on constant tracking:
    # (a) any `lui` of a hardware high half in compiled code (catches PHYS_TO_K1(x) built with `or`),
    hw_hi = lambda h: 0xA000 <= h < 0xA490 or h == 0xBFC0
    hw_lui = [(n, v, word(v)) for a, sz, n in compiled for v in range(a, a + sz, 4)
              if word(v) >> 26 == 0xF and hw_hi(word(v) & 0xFFFF)]
    # (b) words in .data/.rodata that fall in MMIO/PIF/KSEG1-RDRAM ranges (stored hardware pointers).
    #     Expected benign classes: the low half of a double constant (8-byte aligned pair whose high
    #     word has a plausible exponent), and 4bpp image/palette data (every nibble is 0, 1, a, d, e, f).
    data_words = []
    for v in range(RSP_TEXT_END, DATA_END, 4):
        w = word(v)
        if not (0xA0000000 <= w < 0xA4900000 or 0xBFC00000 <= w < 0xBFC00800):
            continue
        hi_word = word(v - 4)
        exp = (hi_word >> 20) & 0x7FF
        if v % 8 == 4 and 0x3E0 <= exp <= 0x420:
            cls = "low half of a double"
        elif all(c in "01adef" for c in f"{w:08x}"):
            cls = "image/palette data"
        else:
            cls = "UNEXPLAINED"
        data_words.append((v, w, cls))

    by_func = defaultdict(list)
    for h in hits:
        by_func[h[0]].append(h)
    undecided = [f for f in by_func if f not in DECISIONS]

    md = ["# Hardware-access audit (chunk 2.1)", "",
          "Generated by `tools/hw_audit.py`. Do not edit by hand. Scope: the "
          f"{len(compiled)} functions that will be recompiled ({len(funcs)} total, minus N64Recomp-skipped "
          "library functions and the dead-code ignore list).", "",
          "## Summary", "",
          f"- Hardware/KSEG1 address hits: {len(hits)} in {len(by_func)} functions "
          f"({len(undecided)} undecided)",
          f"- `lui` of a hardware high half in compiled code: {len(hw_lui)}",
          f"- Data words in MMIO/PIF/KSEG1 ranges: {len(data_words)} ("
          + ", ".join(f"{c}: {sum(1 for d in data_words if d[2] == c)}" for c in sorted({d[2] for d in data_words})) + ")",
          f"- cop0/cache/eret/tlb/sync instructions: {len(special)}",
          f"- FPU control (`ctc1`/`cfc1`): {len(fpu_ctrl)}",
          f"- Detector self-test: flags MMIO in all {len(SELFTEST)} known MMIO libultra functions "
          f"({', '.join(SELFTEST)}) with {len(st_hits)} hits",
          f"- Odd FPRs used as doubles: {len(odd_double)} -> `uses_mips3_float_mode = "
          f"{'true' if odd_double else 'false'}`", ""]
    md += ["## Hits by function", "", "| Function | Decision | Site | Kind | Address | Region | Instruction |",
           "|---|---|---|---|---|---|---|"]
    for f, hs in sorted(by_func.items(), key=lambda kv: kv[1][0][1]):
        dec = DECISIONS.get(f, ("**UNDECIDED**", ""))
        for i, (fn, v, kind, addr, rname, ins) in enumerate(hs):
            md.append(f"| `{fn}` | {dec[0] if i == 0 else ''} | `{v:08X}` | {kind} | `{addr:08X}` | {rname} | `{ins}` |")
    md.append("")
    if DECISIONS:
        md += ["## Decisions", ""] + [f"- `{f}`: **{d}** — {r}" for f, (d, r) in sorted(DECISIONS.items())] + [""]
    md += ["## Data words in hardware ranges", "", "| Address | Value | Class |", "|---|---|---|"]
    md += [f"| `{v:08X}` | `{w:08X}` | {c} |" for v, w, c in data_words] + [""]
    md += ["## System instructions", ""] + [f"- `{n}` `{v:08X}`: `{i}`" for n, v, i in special] + (["- None."] if not special else []) + [""]
    md += ["## FPU control", ""] + [f"- `{n}` `{v:08X}`: `{i}`" for n, v, i in fpu_ctrl] + [""]
    md += ["## Odd FPRs used as doubles", ""] + [f"- `{n}` `{v:08X}`: `{i}`" for n, v, i in odd_double[:50]] + (["- None."] if not odd_double else []) + [""]
    (ROOT / "docs" / "hw_audit.md").write_text("\n".join(md), encoding="utf-8")

    print("\n".join(md[6:11]))
    for f in undecided:
        print(f"  UNDECIDED {f}: " + "; ".join(f"{h[1]:08X} {h[2]} {h[3]:08X} ({h[4]})" for h in by_func[f][:4]))
    stale = [f for f in DECISIONS if f not in by_func]
    if stale:
        print("  stale decisions (no longer hit): " + ", ".join(stale))
    bad_data = [d for d in data_words if d[2] == "UNEXPLAINED"]
    for n, v, w in hw_lui:
        print(f"  hardware lui in {n} at {v:08X}: {w:08X}")
    for v, w, _ in bad_data:
        print(f"  unexplained hardware-range data word at {v:08X}: {w:08X}")
    return 1 if undecided or stale or special or hw_lui or bad_data else 0


if __name__ == "__main__":
    sys.exit(main())
