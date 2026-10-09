"""Recompiled-code coverage report.

Inputs: build/scen/*/coverage.txt (function names first entered during a run of a WG3D_TRACE build;
see tools/run_scenario.py --coverage). Every compiled function (syms minus N64Recomp-skipped library
functions and the dead-code ignore list) is classified:

  entered               reached in at least one run
  caller entered        not entered, but a function that `jal`s it was: a branch not taken in testing
  callers not entered   only `jal`ed from functions that never ran either
  pointer only          no `jal` callers; its address is taken (data word or lui/addiu constant), so
                        it is an indirect-call target (a missing one would be a runtime exit)
  unreferenced          no `jal` and no pointer reference: dead code, or an address built at runtime

Usage: python tools/coverage_report.py [--min-size 0x100]  -> docs/coverage.md
"""
import argparse
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
from rom_map import DATA_END, RSP_TEXT_END, TEXT_END, TEXT_START, VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-size", type=lambda x: int(x, 0), default=0x100,
                    help="unentered functions at least this big are listed individually for review")
    args = ap.parse_args()

    rom = (ROOT / "baserom.us.z64").read_bytes()
    word = lambda v: struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
    syms = tomllib.loads((ROOT / "syms" / "wg3d.us.syms.toml").read_text(encoding="utf-8"))
    funcs = sorted((f["vram"], f["size"], f["name"]) for f in syms["section"][0]["functions"])
    by_start = {f[0]: f for f in funcs}
    sl = ROOT / "lib/N64Recomp/src/symbol_lists.cpp"
    skipped = list_from_cpp(sl, "reimplemented_funcs") | list_from_cpp(sl, "ignored_funcs")
    dead = set((ROOT / "syms" / "dead_ignore.txt").read_text().split())
    compiled = [f for f in funcs if f[2] not in skipped and f[2] not in dead]
    compiled_names = {f[2] for f in compiled}

    # Static references: jal call graph, pointer constants in code, pointer words in data.
    callers = defaultdict(set)
    pointer_refs = defaultdict(set)
    strings = defaultdict(list)  # function -> ASCII strings it builds a pointer to (feature hints)

    def ascii_at(v):
        out = bytearray()
        while RSP_TEXT_END <= v < DATA_END and len(out) < 40:
            b = rom[v + VRAM_TO_ROM]
            if b == 0:
                break
            if not 0x20 <= b < 0x7F:
                return None
            out.append(b)
            v += 1
        return out.decode() if len(out) >= 4 and sum(c.isalpha() for c in out.decode()) >= 3 else None
    for a, size, name in funcs:
        hi = {}
        for v in range(a, a + size, 4):
            x = rabbitizer.Instruction(word(v), v)
            op = x.getOpcodeName()
            if op == "jal":
                callers[x.getInstrIndexAsVram()].add(name)
            rs, rt = field(x, "rs"), field(x, "rt")
            if op == "lui":
                hi[rt] = x.getProcessedImmediate() << 16
            elif op in ("addiu", "ori") and rs in hi:
                imm = x.getProcessedImmediate()
                t = (hi[rs] + imm if op == "addiu" else hi[rs] | (imm & 0xFFFF)) & 0xFFFFFFFF
                if t in by_start:
                    pointer_refs[t].add(f"code {name}")
                elif RSP_TEXT_END <= t < DATA_END:
                    text = ascii_at(t)
                    if text and text not in strings[name]:
                        strings[name].append(text)
                hi.pop(rt, None)
            elif rt is not None and x.modifiesRt():
                hi.pop(rt, None)
    jtbl_words = set()
    for r in csv.DictReader(open(ROOT / "build" / "splat_jtbls.csv")):
        base = int(r["jtbl_vram"], 16)
        jtbl_words |= {base + 4 * i for i in range(int(r["entries"]))}
    for v in range(RSP_TEXT_END, DATA_END, 4):
        w = word(v)
        if TEXT_START <= w < TEXT_END and w in by_start and v not in jtbl_words:
            pointer_refs[w].add(f"data {v:08X}")

    # Coverage from the runs.
    runs = sorted((ROOT / "build" / "scen").glob("*/coverage.txt"))
    if not runs:
        print("no build/scen/*/coverage.txt; run tools/run_scenario.py all --exe build/cmake-trace/wg3d.exe --coverage")
        return 1
    per_run = {p.parent.name: set(p.read_text().split()) for p in runs}
    entered = set().union(*per_run.values())
    unknown = sorted(entered - compiled_names)

    cls = {}
    for a, size, name in compiled:
        if name in entered:
            cls[name] = "entered"
        elif callers.get(a):
            cls[name] = "caller entered" if callers[a] & entered else "callers not entered"
        elif pointer_refs.get(a):
            cls[name] = "pointer only"
        else:
            cls[name] = "unreferenced"
    order = ["entered", "caller entered", "callers not entered", "pointer only", "unreferenced"]
    counts = {c: [f for f in compiled if cls[f[2]] == c] for c in order}
    total_bytes = sum(f[1] for f in compiled)

    md = ["# Recompiled-code coverage", "",
          "Generated by `tools/coverage_report.py` from a `WG3D_TRACE` build run through the scenario harness "
          "(`tools/run_scenario.py all --exe build/cmake-trace/wg3d.exe --coverage`). Do not edit by hand.", "",
          "## Runs", "", "| Session | Functions entered |", "|---|---|"]
    md += [f"| `{n}` | {len(s)} |" for n, s in per_run.items()]
    md += [f"| **union** | **{len(entered & compiled_names)}** of {len(compiled)} compiled |", "",
           "## Classification", "", "| Class | Functions | Bytes | Share of compiled code |", "|---|---|---|---|"]
    for c in order:
        b = sum(f[1] for f in counts[c])
        md.append(f"| {c} | {len(counts[c])} | 0x{b:X} | {100 * b / total_bytes:.1f}% |")
    if unknown:
        md += ["", f"Entered names not in the compiled set (should be empty): {', '.join(unknown)}"]

    md += ["", "## Pointer-only functions (indirect-call targets)", "",
           "Reached only through a function pointer. Each is a function start in the syms (tools/check_indirect.py), so an indirect "
           "call to it resolves; the entered ones are confirmed at runtime.", "",
           "| Function | Size | Entered | References |", "|---|---|---|---|"]
    for a, size, name in sorted(f for f in compiled if not callers.get(f[0]) and pointer_refs.get(f[0])):
        refs = sorted(pointer_refs[a])
        md.append(f"| `{name}` | 0x{size:X} | {'yes' if name in entered else '**no**'} | "
                  f"{', '.join(refs[:3])}{' …' if len(refs) > 3 else ''} |")

    md += ["", f"## Unentered functions of 0x{args.min_size:X} bytes or more", "",
           "Largest first. These are the review list: code that no scenario reached.", "",
           "| Function | Size | Class | Callers (entered callers in bold) | Strings referenced |", "|---|---|---|---|---|"]
    big = sorted((f for f in compiled if cls[f[2]] != "entered" and f[1] >= args.min_size), key=lambda f: -f[1])
    for a, size, name in big:
        cs = sorted(callers.get(a, ()))
        cs_s = ", ".join(f"**{c}**" if c in entered else c for c in cs[:6]) + (" …" if len(cs) > 6 else "")
        if not cs and pointer_refs.get(a):
            cs_s = "(pointer: " + ", ".join(sorted(pointer_refs[a])[:2]) + ")"
        st = "; ".join(f"`{t.strip()[:24]}`" for t in strings.get(name, [])[:3]).replace("|", "/")
        md.append(f"| `{name}` | 0x{size:X} | {cls[name]} | {cs_s or '—'} | {st} |")

    # Runtime-provided (N64Recomp-skipped) functions the game calls: were any of their call sites reached?
    # One only called from never-entered code has never run in testing; its runtime implementation
    # needs a review (some ultramodern functions are stubs, e.g. osContReset is assert(false)).
    md += ["", "## Runtime-provided functions and whether their call sites ran", "",
           "Library functions N64Recomp skips are implemented by the runtime (librecomp/ultramodern) or this project. "
           "**Not exercised** means every caller is unentered code, so the runtime version has never run here.", "",
           "| Function | Callers | Entered callers | Status |", "|---|---|---|---|"]
    not_exercised = []
    for a, size, name in funcs:
        if name not in skipped or not callers.get(a):
            continue
        live = {c for c in callers[a] if c in compiled_names}
        if not live:
            continue  # only called from other skipped/dead library code
        ent = live & entered
        status = "exercised" if ent else "**not exercised**"
        if not ent:
            not_exercised.append(name)
        md.append(f"| `{name}` | {len(live)} | {len(ent)} | {status} |")

    out = ROOT / "docs" / "coverage.md"
    out.write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"{len(entered & compiled_names)}/{len(compiled)} compiled functions entered "
          f"({100 * sum(f[1] for f in counts['entered']) / total_bytes:.1f}% of compiled bytes) -> {out.relative_to(ROOT)}")
    for c in order:
        print(f"  {c:20} {len(counts[c]):5}")
    print(f"  unentered >= 0x{args.min_size:X} bytes: {len(big)}")
    print(f"  runtime functions never exercised: {', '.join(not_exercised) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
