"""Chunk 1.6: merge splat (1.4), Ghidra (1.5) and library names (1.3) into the N64Recomp symbols file.

Rules (docs/phase1.md 1.5/1.6):
  - splat/spimdisasm is primary for starts and sizes. Library names come from symbol_addrs and are applied there.
  - A start is valid only if the code before it cannot fall through: skipping trailing nop padding,
    the last two instructions are an unconditional jump / eret / call to a noreturn function, plus
    its delay slot. This is checked for every final start; splat-only starts are accepted on it.
  - A Ghidra-only start is rejected automatically if it lies inside a function N64Recomp skips
    (e.g. labels in libultra's handwritten exception code).
  - A Ghidra body may never be longer than splat's size for the same start.
  - Anything else must be settled in syms/overrides.toml, or the script fails.

Inputs: build/splat_funcs.csv, build/ghidra_funcs.csv, syms/overrides.toml, baserom.us.z64
Outputs: syms/wg3d.us.syms.toml, docs/syms_report.md

Usage: python tools/gen_syms.py
"""
import bisect
import collections
import csv
import re
import struct
import sys
import tomllib
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from find_overlays import Text  # noqa: E402
from gen_libsyms import list_from_cpp  # noqa: E402
from rom_map import MAIN_ROM, RSP_TEXT_END, TEXT_END, TEXT_START, UCODE_DATA_END, VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
C_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def main() -> int:
    rom = (ROOT / "baserom.us.z64").read_bytes()
    word = lambda v: struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
    sl = ROOT / "lib/N64Recomp/src/symbol_lists.cpp"
    skipped = list_from_cpp(sl, "reimplemented_funcs") | list_from_cpp(sl, "ignored_funcs")
    ov = tomllib.loads((ROOT / "syms" / "overrides.toml").read_text(encoding="utf-8"))

    splat = {int(r["vram"], 16): [int(r["size"], 16), r["name"]] for r in csv.DictReader(open(ROOT / "build/splat_funcs.csv"))}
    ghidra = {int(r["entry"], 16): r for r in csv.DictReader(open(ROOT / "build/ghidra_funcs.csv"))}

    errors, decisions = [], []  # decisions: (vram, kind, decision, reason)

    # Calls that never return, so the code after them cannot fall through.
    noreturn = {a for a, (_, n) in splat.items() if n in ("osDestroyThread",)}

    def falls_through_into(a: int) -> bool:
        v = a - 4
        while v >= TEXT_START and word(v) == 0:
            v -= 4
        # v is the last non-nop instruction; it is either a delay slot (jump at v-4) or a jump whose
        # delay slot is a nop that was skipped (jump at v or v-4).
        for j in (v - 4, v):
            ins = rabbitizer.Instruction(word(j), j)
            n = ins.getOpcodeName()
            if n in ("jr", "j", "b", "eret") or (n == "beq" and ins.rs.value == 0 and ins.rt.value == 0):
                return False
            if n == "jal" and ins.getInstrIndexAsVram() in noreturn:
                return False
        return True

    # Pointer references, for the report.
    t = Text(rom)
    data_ptrs = collections.Counter(struct.unpack(f">{(UCODE_DATA_END - RSP_TEXT_END) // 4}I",
                                                  rom[RSP_TEXT_END + VRAM_TO_ROM:UCODE_DATA_END + VRAM_TO_ROM]))
    hilo = collections.Counter()
    for r in csv.DictReader(open(ROOT / "build/splat_datarefs.csv")):
        if r["vram"]:
            hilo[int(r["vram"], 16)] += int(r["refs"])

    # 1. splat-only starts
    for a in sorted(set(splat) - set(ghidra)):
        refs = len(t.calls.get(a, [])) + data_ptrs[a] + hilo[a]
        kind = "referenced (pointer/call)" if refs else ("empty stub" if splat[a][0] == 8 and word(a) == 0x03E00008 else "unreferenced")
        if falls_through_into(a):
            errors.append(f"splat-only start {a:08X} may be reached by fall-through; decide in overrides.toml")
            decisions.append((a, "splat-only", "UNRESOLVED", kind))
        else:
            decisions.append((a, "splat-only", "accept", f"{kind}; previous code ends in an unconditional jump"))

    # 2. Ghidra-only starts
    starts = sorted(splat)
    rejected = {e["vram"] for e in ov.get("reject_ghidra", [])}
    for a in sorted(set(ghidra) - set(splat)):
        owner = starts[bisect.bisect_right(starts, a) - 1]
        if a in rejected:
            decisions.append((a, "ghidra-only", "reject", "overrides.toml"))
        elif splat[owner][1] in skipped:
            decisions.append((a, "ghidra-only", "reject", f"inside {splat[owner][1]}, which N64Recomp skips"))
        else:
            errors.append(f"Ghidra-only start {a:08X} inside {splat[owner][1]}; decide in overrides.toml")
            decisions.append((a, "ghidra-only", "UNRESOLVED", f"inside {splat[owner][1]}"))

    # 3. Sizes: Ghidra may be shorter (unreachable tails), never longer.
    longer = []
    for a in set(splat) & set(ghidra):
        g = ghidra[a]
        if int(g["ranges"]) == 1 and int(g["max"], 16) + 1 - a > splat[a][0]:
            longer.append(a)
            errors.append(f"Ghidra body at {a:08X} is longer than splat's size")

    # 4. Overrides
    for e in ov.get("merge", []):
        a = e["vram"]
        prev = starts[starts.index(a) - 1]
        splat[prev][0] += splat.pop(a)[0]
        starts.remove(a)
        decisions.append((a, "override", "merge", e["reason"]))
    for e in ov.get("split", []):
        a = e["vram"]
        owner = starts[bisect.bisect_right(starts, a) - 1]
        end = owner + splat[owner][0]
        splat[owner][0] = a - owner
        splat[a] = [end - a, e.get("name", f"func_{a:08X}")]
        bisect.insort(starts, a)
        decisions.append((a, "override", "split", e["reason"]))
    for e in ov.get("rename", []):
        splat[e["vram"]][1] = e["name"]
        decisions.append((e["vram"], "override", "rename", e["reason"]))

    # 5. Validate the final list.
    funcs = sorted((a, s, n) for a, (s, n) in splat.items())
    cursor = TEXT_START
    for a, s, n in funcs:
        if a & 3 or s & 3 or s <= 0:
            errors.append(f"{n}: bad alignment/size")
        if a < cursor:
            errors.append(f"{n} overlaps the previous function")
        elif a > cursor and any(rom[cursor + VRAM_TO_ROM:a + VRAM_TO_ROM]):
            errors.append(f"non-zero bytes not covered before {n}")
        if not C_IDENT.match(n):
            errors.append(f"{n} is not a valid C identifier")
        cursor = a + s
    if cursor > TEXT_END:
        errors.append("last function runs past .text")
    for a, _, n in funcs[1:]:
        if falls_through_into(a):
            errors.append(f"{n}: code before it can fall through into it")
    dup = [n for n, c in collections.Counter(n for _, _, n in funcs).items() if c > 1]
    if dup:
        errors.append("duplicate names: " + ", ".join(dup))
    starts_set = {a for a, _, _ in funcs}
    bad_jal = sorted(tg for tg in t.calls if TEXT_START <= tg < TEXT_END and tg not in starts_set)
    if bad_jal:
        errors.append("jal targets not at a function start: " + ", ".join(f"{a:08X}" for a in bad_jal))

    # Outputs
    lines = [
        "# N64Recomp symbols file for W.G. 3D Hockey (US V1.0). Generated by tools/gen_syms.py; do not edit.",
        "# Inputs: splat (1.4), Ghidra (1.5), library names (1.3), syms/overrides.toml.",
        "",
        "[[section]]",
        'name = ".text"',
        f"rom = 0x{MAIN_ROM:X}",
        f"vram = 0x{TEXT_START:08X}",
        f"size = 0x{TEXT_END - TEXT_START:X}",
        "",
        "functions = [",
    ]
    lines += [f'    {{ name = "{n}", vram = 0x{a:08X}, size = 0x{s:X} }},' for a, s, n in funcs]
    lines += ["]", ""]
    (ROOT / "syms" / "wg3d.us.syms.toml").write_text("\n".join(lines), encoding="utf-8")

    cnt = collections.Counter((k, d) for _, k, d, _ in decisions)
    md = ["# Symbols merge report — W.G. 3D Hockey (US V1.0)", "",
          "Generated by `tools/gen_syms.py`. Do not edit by hand.", "",
          f"- Functions: **{len(funcs)}** covering `{TEXT_START:08X}`–`{TEXT_END:08X}` "
          f"({sum(1 for _, _, n in funcs if not n.startswith('func_'))} named).",
          f"- Starts in both splat and Ghidra: {len(set(ghidra) & set(splat))}; splat-only: "
          f"{sum(v for (k, _), v in cnt.items() if k == 'splat-only')}; Ghidra-only: "
          f"{sum(v for (k, _), v in cnt.items() if k == 'ghidra-only')}.",
          f"- Sizes: splat's. Ghidra bodies are never longer ({len(longer)} exceptions); shorter ones leave out unreachable tails.",
          f"- Overrides applied: {sum(v for (k, _), v in cnt.items() if k == 'override')}.",
          f"- Unresolved: {sum(v for (_, d), v in cnt.items() if d == 'UNRESOLVED')}.", "",
          "| VRAM | Size | Kind | Decision | Reason |", "|---|---|---|---|---|"]
    for a, k, d, r in sorted(decisions):
        md.append(f"| `{a:08X}` | `{splat[a][0]:#x}` | {k} | {d} | {r} |" if a in splat else f"| `{a:08X}` | | {k} | {d} | {r} |")
    (ROOT / "docs" / "syms_report.md").write_text("\n".join(md) + "\n", encoding="utf-8")

    print(f"{len(funcs)} functions -> syms/wg3d.us.syms.toml")
    for (k, d), v in sorted(cnt.items()):
        print(f"  {k:12s} {d:10s} {v}")
    if errors:
        print("ERRORS:\n  " + "\n  ".join(errors))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
