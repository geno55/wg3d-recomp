"""Function-pointer and indirect-call sweep.

Indirect calls in recompiled code are dispatched by exact address (librecomp get_function), so every
code address the game can materialize must be a function start in syms/wg3d.us.syms.toml.
Sources checked:
  1. words in .data/.rodata pointing into .text (jump-table entries excluded)
  2. code-side pointer constants: lui + addiu/ori pairs that build a .text address
  3. osCreateThread entry points (a2)
  4. jalr sites, resolved statically where possible

Usage: python tools/check_indirect.py   -> docs/indirect_targets.md (fails on any bad target)
"""
import bisect
import csv
import struct
import sys
import tomllib
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from find_overlays import Text, field  # noqa: E402
from rom_map import DATA_END, RSP_TEXT_END, TEXT_END, TEXT_START, VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    rom = (ROOT / "baserom.us.z64").read_bytes()
    word = lambda v: struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
    syms = tomllib.loads((ROOT / "syms" / "wg3d.us.syms.toml").read_text(encoding="utf-8"))
    funcs = sorted((f["vram"], f["size"], f["name"]) for f in syms["section"][0]["functions"])
    starts = [f[0] for f in funcs]
    by_start = {f[0]: f for f in funcs}
    name_of = lambda a: by_start[a][2] if a in by_start else f"{a:08X}"

    def owner(a: int):
        return funcs[bisect.bisect_right(starts, a) - 1]

    jtbl_targets = set()
    jtbl_words = set()
    for r in csv.DictReader(open(ROOT / "build" / "splat_jtbls.csv")):
        jtbl_targets |= {int(x, 16) for x in r["targets"].split(";")}
        base = int(r["jtbl_vram"], 16)
        jtbl_words |= {base + 4 * i for i in range(int(r["entries"]))}

    t = Text(rom)
    # Rebuild function-start knowledge in Text from the final syms, so eval_at uses the right bounds.
    t.starts = starts

    problems = []
    refs = defaultdict(set)  # target -> {source descriptions}

    # 1. Data words
    data_hits = Counter()
    for v in range(RSP_TEXT_END, DATA_END, 4):
        w = word(v)
        if TEXT_START <= w < TEXT_END and v not in jtbl_words:
            refs[w].add(f"data@{v:08X}")
            data_hits["start" if w in by_start else "mid"] += 1

    # 2. Code-side pointer constants (lui + addiu/ori in the same function, linear sweep).
    code_hits = Counter()
    for f, size, _ in funcs:
        hi = {}
        for v in range(f, f + size, 4):
            x = t.ins[t.idx(v)]
            op = x.getOpcodeName()
            rs, rt = field(x, "rs"), field(x, "rt")
            if op == "lui":
                hi[rt] = x.getProcessedImmediate() << 16
            elif op in ("addiu", "ori") and rs in hi:
                imm = x.getProcessedImmediate()
                a = (hi[rs] + imm if op == "addiu" else hi[rs] | (imm & 0xFFFF)) & 0xFFFFFFFF
                if TEXT_START <= a < TEXT_END:
                    refs[a].add(f"code@{v:08X}")
                    code_hits["start" if a in by_start else "mid"] += 1
                hi.pop(rt, None)  # rt now holds a full address (or something else), not a %hi
            else:
                # Any other write to a register invalidates a pending %hi in it.
                if x.modifiesRt():
                    hi.pop(rt, None)
                if x.modifiesRd():
                    hi.pop(field(x, "rd"), None)
            if x.isFunctionCall():
                hi.clear()

    # 3. osCreateThread entry points
    thread_entries = []
    for s in t.calls.get(0x8008B530, []):
        regs, _ = t.eval_at(s)
        v = regs.get(6)
        if v and v[0] == "const":
            thread_entries.append((s, v[1]))
            refs[v[1]].add(f"osCreateThread@{s:08X}")
        else:
            problems.append(f"osCreateThread call at {s:08X}: entry point not resolved statically")

    # 4. jalr sites
    jalr_kinds = Counter()
    jalr_static = []
    for i, x in enumerate(t.ins):
        if x.getOpcodeName() != "jalr":
            continue
        site = TEXT_START + i * 4
        regs, _ = t.eval_at(site)
        v = regs.get(field(x, "rs"))
        if v and v[0] == "const":
            jalr_kinds["constant"] += 1
            jalr_static.append((site, v[1]))
            refs[v[1]].add(f"jalr@{site:08X}")
        elif v and v[0] == "load":
            jalr_kinds["loaded from fixed address"] += 1
            if RSP_TEXT_END <= v[1] < DATA_END:
                init = word(v[1])
                if init:
                    jalr_static.append((site, init))
                    refs[init].add(f"jalr@{site:08X} via *{v[1]:08X}")
        elif v and v[0] == "arg":
            jalr_kinds["function-pointer argument"] += 1
        else:
            jalr_kinds["computed (struct field, table index)"] += 1

    # 5. ROM asset data (loaded at runtime): look for code-pointer tables. Most of it is compressed,
    # so compare the hits against what random data would give.
    asset_start, asset_end = 0xB7790, 0x7B36AA
    aligned_hits, start_hits = 0, []
    for off in range(asset_start, asset_end - 3, 4):
        w, = struct.unpack(">I", rom[off:off + 4])
        if TEXT_START <= w < TEXT_END and w % 4 == 0:
            aligned_hits += 1
            if w in by_start:
                start_hits.append((off, w))
    expected = aligned_hits * len(funcs) / ((TEXT_END - TEXT_START) // 4)
    # A real pointer table shows up as several start hits in a row; flag any two within 64 bytes.
    clustered = [(a, b) for a, b in zip(start_hits, start_hits[1:]) if b[0] - a[0] <= 0x40]
    if clustered:
        problems.append("possible code-pointer table in ROM assets near " + ", ".join(f"{a[0]:#x}" for a, _ in clustered))

    # Classify every referenced code address.
    bad = []
    for a, src in sorted(refs.items()):
        if a in by_start:
            continue
        if a in jtbl_targets and all(s.startswith("data@") for s in src):
            continue
        o = owner(a)
        bad.append((a, o, sorted(src)))
    for a, o, src in bad:
        problems.append(f"{a:08X} (inside {o[2]} +{a - o[0]:#x}) referenced from {', '.join(src[:4])}")

    referenced_starts = {a for a in refs if a in by_start}
    called = set(t.calls)
    unreached = [f for f in funcs if f[0] not in called and f[0] not in referenced_starts]

    md = ["# Indirect targets — W.G. 3D Hockey (US V1.0)", "",
          "Generated by `tools/check_indirect.py`. Do not edit by hand.", "",
          "Every code address the game can build must be a function start, because recompiled indirect calls are "
          "looked up by exact address.", "",
          "## Summary", "",
          f"- Data words pointing into `.text` (jump tables excluded): {sum(data_hits.values())} "
          f"({data_hits['start']} at function starts, {data_hits['mid']} mid-function)",
          f"- Code-side `lui`+`addiu/ori` constants into `.text`: {sum(code_hits.values())} "
          f"({code_hits['start']} at starts, {code_hits['mid']} mid-function)",
          f"- `osCreateThread` entry points: {len(thread_entries)}",
          f"- `jalr` sites: {sum(jalr_kinds.values())}: " + ", ".join(f"{k}: {v}" for k, v in jalr_kinds.most_common()),
          f"- Distinct function starts reached only indirectly (no `jal`): "
          f"{len(referenced_starts - called)}",
          f"- Functions with no `jal` and no pointer reference (dead code, or reached via pointers built at runtime): {len(unreached)}",
          f"- ROM asset words that are 4-aligned `.text` addresses: {aligned_hits}; exact function starts: "
          f"{len(start_hits)} (~{expected:.1f} expected by chance; none clustered)" if not clustered else
          f"- ROM asset words at function starts: {len(start_hits)}, **clustered: possible pointer table**",
          f"- **Bad targets: {len(bad)}**", ""]
    md += ["## Thread entry points", "", "| Created at | Entry |", "|---|---|"]
    md += [f"| `{s:08X}` | `{name_of(e)}` |" for s, e in thread_entries]
    md += ["", "## Statically resolved `jalr` targets", "", "| Site | Target |", "|---|---|"]
    md += [f"| `{s:08X}` | `{name_of(tg)}` |" for s, tg in jalr_static]
    md += ["", "## Functions reached only indirectly", "", "| Function | Referenced from |", "|---|---|"]
    for a in sorted(referenced_starts - called):
        md.append(f"| `{name_of(a)}` | {', '.join(sorted(refs[a])[:3])}{' …' if len(refs[a]) > 3 else ''} |")
    md += ["", "## Unreached functions", "", ", ".join(f"`{f[2]}`" for f in unreached), ""]
    if problems:
        md += ["## Problems", ""] + [f"- {p}" for p in problems] + [""]
    (ROOT / "docs" / "indirect_targets.md").write_text("\n".join(md), encoding="utf-8")

    print("\n".join(md[7:16]))
    if problems:
        print("PROBLEMS:\n  " + "\n  ".join(problems))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
