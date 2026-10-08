"""Build the library (libultra/libaudio) function names for the main segment and check them
against what N64Recomp and the runtime expect.

Inputs (regenerate with tools/wsl/*.sh, run as: wsl -d Ubuntu -- sh <path>; see docs/phase1.md 1.3):
  build/n64sym_ram_t.txt   n64sym -s -t on build/ram_image.bin (built-in signatures)
  build/n64sym_ram_E.txt   n64sym -t with ultralib 2.0E archives
  build/sigs/*.a           fuzzy-match references (mk64 libultra at -O2/-O1, ultralib 2.0E)

Outputs:
  syms/libultra_symbols.txt  splat-format function symbols (library range only)
  docs/libultra_coverage.md  checklist vs N64Recomp's reimplemented/ignored lists and the runtime

Usage: python tools/gen_libsyms.py
"""
import bisect
import difflib
import re
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from find_overlays import Text  # noqa: E402
from libmatch import load_data_names, load_n64sym, load_refs, ops  # noqa: E402
from rom_map import DATA_END, LIBULTRA_TEXT_START, RSP_TEXT_END, TEXT_END, TEXT_START, VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
REFS = ["build/sigs/libultra_mk64_O2.a", "build/sigs/libultra_mk64.a", "build/sigs/libultra_rom.E.a"]

# Identified by fuzzy matching (tools/libmatch.py, score in the coverage report) plus call-graph
# checks. Each one overrides or adds to the n64sym name at that address.
MANUAL = {
    0x8008B2A0: ("osInitialize", "writes PIF RAM 0x1FC007FC, copies exception preamble"),
    0x8008BAD0: ("osContInit", ""),
    0x8008C500: ("osViBlack", "n64sym said osDestroyThread (duplicate); sets VI_STATE_BLACK 0x20"),
    0x8008C5B0: ("osViGetNextFramebuffer", "identical shape to GetCurrent, which precedes it"),
    0x80094BA0: ("__osPiGetAccess", "n64sym said __osSiGetAccess (duplicate); follows __osPiCreateAccessQueue"),
    0x80094BE4: ("__osPiRelAccess", "n64sym said __osSiRelAccess (duplicate)"),
    0x80095020: ("__osSiCreateAccessQueue", "precedes the SI Get/RelAccess pair, which cont/pfs code calls"),
    0x8008CDB4: ("viMgrMain", "n64sym placeholder vimgr_text_0184; VI manager thread created by osCreateViManager"),
    0x80098000: ("alSynSetPriority", "n64sym also named 0x80098008, which is mid-function"),
    0x800988F0: ("alMainBusParam", "n64sym said alAuxBusParam (duplicate); precedes alMainBusPull"),
    # Controller Pak (2.0D-era API: osPfsInit, not osPfsInitPak). References: mk64 libultra built at -O2.
    0x8008D850: ("osPfsDeleteFile", ""),
    0x8008DAD0: ("__osPfsReleasePages", "called only by osPfsDeleteFile"),
    0x8008DCD4: ("__osBlockSum", ""),
    0x8008DDC0: ("osPfsAllocateFile", ""),
    0x8008E1B4: ("__osPfsDeclearPage", "called only by osPfsAllocateFile"),
    0x8008E3B4: ("__osClearPage", "static in pfsallocatefile.c; called by __osPfsDeclearPage, calls SelectBank/ContRamWrite"),
    0x8008E460: ("__osPfsGetNextPage", "static in pfsreadwritefile.c; called by osPfsReadWriteFile, calls RWInode"),
    0x8008E540: ("osPfsReadWriteFile", ""),
    0x8008E8B0: ("osPfsFileState", ""),
    0x8008EB60: ("osPfsInit", "calls SiGetAccess, __osPfsGetStatus, SiRelAccess, __osGetId, osPfsChecker"),
    0x8008EC04: ("__osPfsGetStatus", ""),
    0x8008ECE0: ("osPfsFindFile", ""),
    0x8008EE80: ("osPfsChecker", ""),
    0x8008F46C: ("corrupted_init", "static in pfschecker.c"),
    0x8008F750: ("corrupted", "static in pfschecker.c"),
    0x8008F8F0: ("osPfsFreeBlocks", ""),
    0x80095EC0: ("__osSumcalc", ""),
    0x80095F3C: ("__osIdCheckSum", "leaf; called by RepairPackId/CheckPackId/GetId"),
    0x80096038: ("__osRepairPackId", "calls osGetCount"),
    0x80096384: ("__osCheckPackId", ""),
    0x800964E8: ("__osGetId", ""),
    0x800966EC: ("__osCheckId", ""),
    0x80096804: ("__osPfsRWInode", ""),
    0x80096AA0: ("__osPfsSelectBank", ""),
    0x80096B10: ("__osContRamRead", ""),
    0x80096D3C: ("__osPackRamReadData", "called only by __osContRamRead"),
    0x80096EA0: ("__osContRamWrite", ""),
    0x800970C4: ("__osPackRamWriteData", "called only by __osContRamWrite"),
    0x80097230: ("osPfsIsPlug", "no direct callers"),
    0x800973CC: ("__osPfsRequestData", "__osPackRequestData is already at 0x8008BD9C"),
    0x800974A4: ("__osPfsGetInitData", "__osContGetInitData is already at 0x8008BCCC"),
}
# n64sym names that are wrong (a function interior, not a start).
DROP = {0x80098008}


def list_from_cpp(path: Path, var: str) -> set[str]:
    s = path.read_text(encoding="utf-8")
    a = s.index(f"N64Recomp::{var} {{")
    return set(re.findall(r'"([^"]+)"', s[a:s.index("};", a)]))


def runtime_funcs() -> set[str]:
    out = set()
    for d in ("librecomp/src", "ultramodern/src"):
        for f in (ROOT / "lib/N64ModernRuntime" / d).glob("*.cpp"):
            out |= set(re.findall(r"void\s+([A-Za-z0-9_]+)_recomp\s*\(", f.read_text(encoding="utf-8", errors="replace")))
    return out


def main() -> int:
    rom = (ROOT / "baserom.us.z64").read_bytes()
    t = Text(rom)
    in_lib = lambda a: LIBULTRA_TEXT_START <= a < TEXT_END

    # n64sym output has no symbol types. It also names relocation targets (data) of matched
    # functions, sometimes at .text addresses. Keep a name only if it isn't a data object in the
    # reference archives and the address is a real function start.
    data_names = load_data_names([ROOT / p for p in REFS])
    word = lambda v: t.words[t.idx(v)]

    def plausible_start(a: int) -> bool:
        if t.calls.get(a):
            return True
        v = a - 4
        while v >= LIBULTRA_TEXT_START and word(v) == 0:
            v -= 4
        for j in (v - 4, v):
            ins = t.ins[t.idx(j)]
            n = ins.getOpcodeName()
            if n in ("jr", "j", "b", "eret") or (n == "beq" and ins.rs.value == 0 and ins.rt.value == 0):
                return True
        return False

    names, rejected = {}, []
    for f in ("n64sym_ram_t.txt", "n64sym_ram_E.txt"):
        for a, n in load_n64sym(ROOT / "build" / f).items():
            if not in_lib(a) or a in DROP or a in names:
                continue
            if n in data_names or not plausible_start(a):
                rejected.append((a, n, "data object" if n in data_names else "not a function start"))
            else:
                names[a] = n
    for a, (n, _) in MANUAL.items():
        names[a] = n

    errors = []
    dups = [n for n, c in Counter(names.values()).items() if c > 1]
    if dups:
        errors.append("duplicate names: " + ", ".join(dups))

    # Fuzzy-match evidence for the manual names.
    refs = load_refs([ROOT / p for p in REFS])
    starts = sorted(set(names) | {a for a in t.starts if in_lib(a)})

    def score(a: int, name: str) -> float:
        i = bisect.bisect_right(starts, a)
        end = starts[i] if i < len(starts) else TEXT_END
        mine = ops(t.words[t.idx(a):t.idx(end)])
        while mine and mine[-1] == "nop":
            mine.pop()
        cands = [r for k, r in refs.items() if k.split("@")[0] == name]
        return max((difflib.SequenceMatcher(None, mine, r, autojunk=False).ratio() for r in cands), default=0.0)

    # syms/libultra_symbols.txt
    (ROOT / "syms").mkdir(exist_ok=True)
    lines = ["// Generated by tools/gen_libsyms.py. Library-range function names (libultra 2.0D + libaudio)."]
    lines += [f"{n} = 0x{a:08X}; // type:func" for a, n in sorted(names.items())]
    (ROOT / "syms" / "libultra_symbols.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    # Checklist
    sl = ROOT / "lib/N64Recomp/src/symbol_lists.cpp"
    reimpl, ignored = list_from_cpp(sl, "reimplemented_funcs"), list_from_cpp(sl, "ignored_funcs")
    runtime = runtime_funcs()
    by_name = {n: a for a, n in names.items()}

    skipped = reimpl | ignored

    # Functions that will run as recompiled code: everything reachable by `jal` from game code
    # without passing through a skipped function, because the runtime replaces those. Library
    # code reached only via skipped functions (or never called) is dead after recompilation.
    # Edges: direct calls, plus function addresses built in code (lui + addiu/ori), which covers
    # callbacks and thread entries passed around by the caller. Words in data that point at a
    # function make it a root.
    callees = {}
    hi = {}
    for i, x in enumerate(t.ins):
        v = TEXT_START + i * 4
        op = x.getOpcodeName()
        if op == "jal":
            callees.setdefault(t.func_of(v), set()).add(x.getInstrIndexAsVram())
            hi.clear()
        elif op == "lui":
            hi[x.rt.value] = x.getProcessedImmediate() << 16
        elif op in ("addiu", "ori") and x.rs.value in hi:
            imm = x.getProcessedImmediate()
            a = (hi[x.rs.value] + imm if op == "addiu" else hi[x.rs.value] | (imm & 0xFFFF)) & 0xFFFFFFFF
            if TEXT_START <= a < TEXT_END:
                callees.setdefault(t.func_of(v), set()).add(a)
            hi.pop(x.rt.value, None)
        else:
            if x.modifiesRt():
                hi.pop(x.rt.value, None)
            if x.modifiesRd():
                hi.pop(x.rd.value, None)
        if op in ("jr", "j") or x.isFunctionCall():
            hi.clear()
    data_roots = set()
    for v in range(RSP_TEXT_END, DATA_END, 4):
        w, = struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])
        if TEXT_START <= w < TEXT_END:
            data_roots.add(w)
    live_funcs = {f for f in t.starts if not in_lib(f)} | {f for f in data_roots if names.get(f) not in skipped}
    todo = list(live_funcs)
    while todo:
        for c in callees.get(todo.pop(), ()):
            if c not in live_funcs and names.get(c) not in skipped:
                live_funcs.add(c)
                todo.append(c)

    def callers_outside_ignored(a: int) -> list[int]:
        return [c for c in t.calls.get(a, []) if t.func_of(c) in live_funcs]

    md = ["# libultra coverage — W.G. 3D Hockey (US V1.0)", "",
          "Generated by `tools/gen_libsyms.py`. Do not edit by hand.", "",
          f"- Library range `{LIBULTRA_TEXT_START:08X}`–`{TEXT_END:08X}`: {len(names)} named functions "
          f"({len(MANUAL)} identified by hand, the rest by n64sym).",
          "- n64sym hits **outside** the library range are all rejected as false positives: tiny generic bodies, "
          "or matches that aren't function starts (see docs/phase1.md 1.3).", ""]

    md += ["## Rejected n64sym names in the library range", "",
           "| Address | Name | Reason |", "|---|---|---|"]
    md += [f"| `{a:08X}` | `{n}` | {r} |" for a, n, r in sorted(set(rejected)) if a not in MANUAL]
    md.append("")

    md += ["## Hand identifications", "", "| Address | Name | Fuzzy score | Evidence |", "|---|---|---|---|"]
    for a, (n, ev) in sorted(MANUAL.items()):
        md.append(f"| `{a:08X}` | `{n}` | {score(a, n):.2f} | {ev} |")
    md.append("")

    # Present functions N64Recomp skips: does the runtime provide them, and who calls them?
    md += ["## Functions N64Recomp skips (reimplemented ∪ ignored) that are present in the ROM", "",
           "\"Called from\" counts only callers that will actually run as recompiled code: game code, plus library code reachable from game code without passing through a skipped function "
           "(function-pointer-only entry points such as thread mains are not followed). A skipped function with such callers **must** have a runtime "
           "implementation (`<name>_recomp`).", "",
           "| Name | Address | Runtime impl | Called from recompiled code | Status |", "|---|---|---|---|---|"]
    gaps = []
    for n in sorted((reimpl | ignored) & set(by_name)):
        a = by_name[n]
        live = callers_outside_ignored(a)
        has = n in runtime
        status = "ok" if has or not live else "**GAP**"
        if status != "ok":
            gaps.append((n, a, live))
        md.append(f"| `{n}` | `{a:08X}` | {'yes' if has else 'no'} | {len(live)} | {status} |")
    md.append("")

    md += ["## Reimplemented functions not present in the ROM", "",
           ", ".join(f"`{n}`" for n in sorted(reimpl - set(by_name))), ""]

    # Link level: every compiled (non-skipped) function that calls a skipped function with no
    # runtime implementation needs that symbol at link time, even if the caller is dead code.
    link_gaps = []
    for n in sorted((reimpl | ignored) & set(by_name)):
        if n in runtime:
            continue
        a = by_name[n]
        callers = sorted({t.func_of(c) for c in t.calls.get(a, []) if names.get(t.func_of(c)) not in skipped})
        if callers:
            link_gaps.append((n, a, [(c, c in live_funcs) for c in callers]))
    # Ignoring a function removes its definition, so its own (dead) callers must be ignored too:
    # close the set upward. A live caller cannot appear, because it would make the callee live.
    dead = {c for _, _, cs in link_gaps for c, live in cs if not live}
    todo = list(dead)
    while todo:
        f = todo.pop()
        for c in {t.func_of(s) for s in t.calls.get(f, [])}:
            if c not in dead and names.get(c) not in skipped:
                if c in live_funcs:
                    errors.append(f"live function {c:08X} calls dead {f:08X}")
                    continue
                dead.add(c)
                todo.append(c)
    dead_to_ignore = sorted(dead)

    md += ["## Link-level gaps", "",
           "Skipped functions without a runtime implementation that compiled code still calls. Live callers are "
           "real gaps (below). **Dead** callers are only reachable through skipped functions, so Phase 2 marks them "
           "`ignored` in the N64Recomp config. Otherwise they fail to link.", "",
           "| Skipped callee | Callers (live / dead) |", "|---|---|"]
    for n, a, cs in link_gaps:
        md.append(f"| `{n}` | " + ", ".join(f"`{names.get(c, f'func_{c:08X}')}` ({'live' if lv else 'dead'})" for c, lv in cs) + " |")
    md += ["", "Dead library functions for Phase 2 `[patches] ignored`: "
           + ", ".join(f"`{names.get(c, f'func_{c:08X}')}`" for c in dead_to_ignore), ""]
    (ROOT / "syms" / "dead_ignore.txt").write_text("\n".join(names.get(c, f"func_{c:08X}") for c in dead_to_ignore) + "\n")

    md += ["## Gaps for Phase 2/3", ""]
    if gaps:
        for n, a, live in gaps:
            who = ", ".join(sorted({f"`{t.func_of(c):08X}`" for c in live}))
            md.append(f"- `{n}` (`{a:08X}`): skipped by N64Recomp, no runtime implementation, called from {who}.")
    else:
        md.append("- None.")
    md.append("")

    (ROOT / "docs" / "libultra_coverage.md").write_text("\n".join(md), encoding="utf-8")

    print(f"{len(names)} library names written; {len(gaps)} gap(s): " + ", ".join(n for n, _, _ in gaps))
    low = [(a, n) for a, (n, _) in MANUAL.items() if score(a, n) < 0.75]
    print("manual names with fuzzy score < 0.75 (call-graph evidence only):", ", ".join(f"{n}@{a:08X}" for a, n in low))
    if errors:
        print("ERRORS: " + "; ".join(errors))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
