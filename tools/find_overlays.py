"""Hunt for runtime-loaded code (overlays) in W.G. 3D Hockey.

The ROM map (tools/rom_map.py) shows the ROM holds no uncompressed code outside the main segment. So any overlay
would have to be compressed code that is DMA'd or decompressed into RAM and then run. This
script collects the evidence for or against that:

  1. Every ROM DMA call (osPiStartDma, osPiRawStartDma), followed up through wrapper functions.
     Destination, ROM source and size are resolved statically where possible.
  2. Instruction-cache maintenance (`cache` with an I-cache op). Code loaded at runtime needs an
     I-cache invalidate before it can be run safely.
  3. TLB writes, and who reaches them (TLB-mapped code).
  4. Indirect calls (jalr). Each one is classified by where its target register comes from.

Usage: python tools/find_overlays.py [baserom.us.z64]   -> writes docs/overlays.md
"""
import struct
import sys
from collections import defaultdict
from pathlib import Path

import rabbitizer

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from rom_map import (BSS_SIZE, DATA_END, LIBULTRA_TEXT_START, RSP_TEXT_END, TEXT_END,  # noqa: E402
                     TEXT_START, UCODE_DATA_END, VRAM_TO_ROM, rom_of)

# Library functions identified by n64sym (first ROM survey).
LIB_FUNCS = {
    0x8008C6A0: "osPiStartDma",
    0x80094C30: "osPiRawStartDma",
    0x8008C5F0: "osInvalDCache",
    0x8008CA60: "osWritebackDCache",
    0x8008C7B0: "osVirtualToPhysical",
    0x8008B2A0: "osInitialize",
}
DMA_FUNCS = {0x8008C6A0: "osPiStartDma", 0x80094C30: "osPiRawStartDma"}
# Arguments: name -> location in the callee's frame (register, or caller's outgoing stack slot).
DMA_ARGS = {
    "osPiStartDma": {"direction": "a2", "devAddr": "a3", "vAddr": "stack+0x10", "nbytes": "stack+0x14"},
    "osPiRawStartDma": {"direction": "a0", "devAddr": "a1", "vAddr": "a2", "nbytes": "a3"},
}

REG_NAMES = {4: "a0", 5: "a1", 6: "a2", 7: "a3"}
SP, RA, ZERO = 29, 31, 0
BSS_END = UCODE_DATA_END + BSS_SIZE


def region_of(vram: int) -> str:
    v = (vram & 0x1FFFFFFF) | 0x80000000  # fold KSEG1 / physical addresses into KSEG0
    if TEXT_START <= v < TEXT_END:
        return "TEXT"
    if TEXT_END <= v < RSP_TEXT_END:
        return "rsp-text"
    if RSP_TEXT_END <= v < UCODE_DATA_END:
        return "data"
    if UCODE_DATA_END <= v < BSS_END:
        return "bss"
    if 0x80000000 <= v < TEXT_START:
        return "low-ram"
    if BSS_END <= v < 0x80800000:
        return "heap/free-ram"
    return "?"


def field(x, name: str):
    """Register operand number, or None if the (pseudo-)instruction has no such operand."""
    try:
        return getattr(x, name).value
    except RuntimeError:
        return None


class Text:
    def __init__(self, rom: bytes):
        self.rom = rom
        n = (TEXT_END - TEXT_START) // 4
        words = struct.unpack(f">{n}I", rom[rom_of(TEXT_START):rom_of(TEXT_END)])
        self.words = words
        self.ins = [rabbitizer.Instruction(w, TEXT_START + i * 4) for i, w in enumerate(words)]
        # Function starts: jal targets, plus the first non-nop after each `jr ra` + delay slot.
        starts = {TEXT_START}
        self.calls = defaultdict(list)  # target -> [call sites]
        for i, x in enumerate(self.ins):
            if x.getOpcodeName() == "jal":
                t = x.getInstrIndexAsVram()
                self.calls[t].append(TEXT_START + i * 4)
                if TEXT_START <= t < TEXT_END:
                    starts.add(t)
            elif words[i] == 0x03E00008:
                j = i + 2
                while j < n and words[j] == 0:
                    j += 1
                if j < n:
                    starts.add(TEXT_START + j * 4)
        self.starts = sorted(starts)

    def idx(self, vram: int) -> int:
        return (vram - TEXT_START) // 4

    def func_of(self, vram: int) -> int:
        lo, hi = 0, len(self.starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.starts[mid] <= vram:
                lo = mid
            else:
                hi = mid - 1
        return self.starts[lo]

    def eval_at(self, site: int):
        """Linear-sweep symbolic evaluation from the containing function's start up to `site`.

        Returns (regs, stack). Values are ('const', v), ('arg', name), ('load', addr) or None.
        Branches are ignored, so this is an approximation. The caller treats it as a hint.
        """
        f = self.func_of(site)
        regs = {r: ("arg", n) for r, n in REG_NAMES.items()}
        regs[ZERO] = ("const", 0)
        frame = 0
        stack = {}
        for k in range(self.idx(f), self.idx(site) + 2):  # include the delay slot
            if k * 4 + TEXT_START > site + 4:
                break
            x = self.ins[k]
            op = x.getOpcodeName()
            rs, rt, rd = (field(x, n) for n in ("rs", "rt", "rd"))
            imm = x.getProcessedImmediate() if x.isIType() else None
            c = lambda r: regs.get(r) if regs.get(r) and regs[r][0] == "const" else None
            if op == "addiu" and rs == SP and rt == SP:
                frame -= imm if imm < 0 else 0
                continue
            if op == "move":
                regs[rd] = regs.get(rs)
            elif op == "lui":
                regs[rt] = ("const", (imm << 16) & 0xFFFFFFFF)
            elif op in ("addiu", "ori") and rt is not None:
                base = regs.get(rs)
                if base and base[0] == "const":
                    v = base[1] + imm if op == "addiu" else base[1] | (imm & 0xFFFF)
                    regs[rt] = ("const", v & 0xFFFFFFFF)
                elif op == "addiu" and imm == 0:
                    regs[rt] = base
                else:
                    regs[rt] = None
            elif op in ("addu", "or") and rd is not None:
                a, b = regs.get(rs), regs.get(rt)
                if rt == ZERO:
                    regs[rd] = a
                elif rs == ZERO:
                    regs[rd] = b
                elif a and b and a[0] == b[0] == "const":
                    regs[rd] = ("const", (a[1] + b[1] if op == "addu" else a[1] | b[1]) & 0xFFFFFFFF)
                else:
                    regs[rd] = None
            elif op == "sw" and rs == SP:
                stack[imm] = regs.get(rt)
            elif op == "lw" and rs == SP:
                if imm in stack:
                    regs[rt] = stack[imm]
                elif frame and imm >= frame + 0x10:
                    regs[rt] = ("arg", f"stack+{imm - frame:#x}")
                else:
                    regs[rt] = None
            elif op == "lw" and rt is not None:
                base = regs.get(rs)
                addr = (base[1] + imm) & 0xFFFFFFFF if base and base[0] == "const" else 0
                regs[rt] = ("load", addr) if addr >= 0x80000000 else None
            elif op == "jal" and k * 4 + TEXT_START != site:
                for r in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                    regs[r] = None
            elif rd is not None and x.isRType() and op not in ("jr", "jalr", "mult", "multu", "div", "divu"):
                regs[rd] = None
            elif rt is not None and x.isIType() and not (x.isBranch() or op.startswith("s") or op.startswith("cache")):
                if op not in ("sw", "sh", "sb", "swc1", "sdc1"):
                    regs[rt] = None
        return regs, stack

    def arg_value(self, site: int, loc: str):
        regs, stack = self.eval_at(site)
        if loc.startswith("stack+"):
            return stack.get(int(loc[6:], 16))
        return regs.get({"a0": 4, "a1": 5, "a2": 6, "a3": 7}[loc])

    def resolve(self, site: int, loc: str, depth: int = 0, path=()):
        """Resolve an argument at a call site, following ('arg', ..) values up through callers."""
        v = self.arg_value(site, loc)
        if v and v[0] == "arg" and depth < 4:
            f = self.func_of(site)
            callers = self.calls.get(f, [])
            if not callers:
                return [(v, path + (f,))]
            out = []
            for cs in callers:
                out += self.resolve(cs, v[1], depth + 1, path + (f,))
            return out
        return [(v, path)]


def fmt(v) -> str:
    if v is None:
        return "dynamic"
    kind, val = v
    if kind == "const":
        return f"`{val:08X}` ({region_of(val)})" if val >= 0x80000000 else f"`{val:#x}`"
    if kind == "load":
        return f"*`{val:08X}` ({region_of(val)} var)"
    return f"arg {val} (unresolved)"


def main() -> int:
    rom_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "baserom.us.z64"
    rom = rom_path.read_bytes()
    t = Text(rom)
    name = lambda f: LIB_FUNCS.get(f, f"func_{f:08X}")
    out = ["# Overlay hunt — W.G. 3D Hockey (US V1.0)", "", "Generated by `tools/find_overlays.py`. Do not edit by hand.", ""]
    exec_dest = []

    # 1. DMA call graph
    out += ["## 1. ROM DMA call sites", ""]
    for fa, fn in DMA_FUNCS.items():
        sites = t.calls.get(fa, [])
        out.append(f"### `{fn}` ({fa:08X}): {len(sites)} direct call sites")
        out += ["", "| Call site | In function | direction | devAddr (ROM) | vAddr (RAM dest) | nbytes |", "|---|---|---|---|---|---|"]
        for s in sites:
            f = t.func_of(s)
            cells = []
            for arg in ("direction", "devAddr", "vAddr", "nbytes"):
                res = t.resolve(s, DMA_ARGS[fn][arg])
                vals = sorted({fmt(v) for v, _ in res})
                cells.append("<br>".join(vals[:6]) + (f"<br>(+{len(vals) - 6} more)" if len(vals) > 6 else ""))
                if arg == "vAddr":
                    for v, _ in res:
                        if v and v[0] == "const" and region_of(v[1]) in ("TEXT", "low-ram"):
                            exec_dest.append((s, v[1]))
            out.append(f"| `{s:08X}` | `{name(f)}` | " + " | ".join(cells) + " |")
        out.append("")

        # Wrappers: callers of the function containing each site, two levels up.
        out += ["Callers via wrapper functions (2 levels up):", ""]
        seen = set()
        frontier = {t.func_of(s) for s in sites}
        for level in (1, 2):
            nxt = set()
            for f in sorted(frontier):
                if f in seen:
                    continue
                seen.add(f)
                callers = t.calls.get(f, [])
                cf = sorted({t.func_of(c) for c in callers})
                where = "libultra" if f >= LIBULTRA_TEXT_START else "game"
                out.append(f"- L{level} `{name(f)}` ({where}): {len(callers)} call sites in {len(cf)} functions")
                nxt |= set(cf)
            frontier = nxt
        out.append("")

    # 2. I-cache maintenance
    out += ["## 2. Instruction-cache maintenance", ""]
    icache = []
    for i, w in enumerate(t.words):
        if w >> 26 == 0x2F:
            op = (w >> 16) & 0x1F
            if op & 3 == 0:  # cache target 0 = primary I-cache
                icache.append((TEXT_START + i * 4, op))
    funcs = sorted({t.func_of(a) for a, _ in icache})
    for f in funcs:
        callers = t.calls.get(f, [])
        cf = sorted({t.func_of(c) for c in callers})
        ops = sorted({f"{op:#x}" for a, op in icache if t.func_of(a) == f})
        out.append(f"- `{name(f)}` (cache ops {', '.join(ops)}): called from {len(callers)} sites: "
                   + ", ".join(f"`{name(c)}`" for c in cf))
    if not funcs:
        out.append("- None found.")
    out.append("")

    # 3. TLB
    out += ["## 3. TLB writes", ""]
    tlb = [TEXT_START + i * 4 for i, w in enumerate(t.words) if w in (0x42000002, 0x42000006)]  # tlbwi, tlbwr
    for a in tlb:
        f = t.func_of(a)
        callers = t.calls.get(f, [])
        out.append(f"- `{a:08X}` in `{name(f)}`: called from "
                   + (", ".join(f"`{name(t.func_of(c))}` @ `{c:08X}`" for c in callers) or "nowhere (directly)"))
    out.append("")

    # 4. Indirect calls
    out += ["## 4. Indirect calls (`jalr`)", ""]
    kinds = defaultdict(list)
    bad_targets = []
    for i, x in enumerate(t.ins):
        if x.getOpcodeName() != "jalr":
            continue
        site = TEXT_START + i * 4
        regs, _ = t.eval_at(site)
        v = regs.get(x.rs.value)
        if v is None:
            kinds["dynamic (struct field / computed)"].append(site)
        elif v[0] == "const":
            kinds["constant target"].append(site)
            if region_of(v[1]) != "TEXT":
                bad_targets.append((site, v[1]))
        elif v[0] == "load":
            kinds["loaded from fixed variable/table"].append(site)
        else:
            kinds["function-pointer argument"].append(site)
    total = sum(len(v) for v in kinds.values())
    out.append(f"{total} `jalr` sites:")
    out.append("")
    for k, v in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        out.append(f"- {k}: {len(v)}")
    out.append("")

    # Code pointers stored in data: every word in .data/.rodata/ucode-data that points into
    # executable-looking RAM outside .text.
    dstart, dend = rom_of(RSP_TEXT_END), rom_of(UCODE_DATA_END)
    ptrs_text = ptrs_low = 0
    for off in range(dstart, dend, 4):
        w, = struct.unpack(">I", rom[off:off + 4])
        if TEXT_START <= w < TEXT_END and t.func_of(w) == w:
            ptrs_text += 1
        elif 0x80000400 <= w < TEXT_START:
            ptrs_low += 1
    out += ["## 5. Code pointers in data", "",
            f"- Words in data equal to a function start in `.text`: {ptrs_text}. These are function-pointer tables, input for tools/check_indirect.py.",
            f"- Words pointing into low RAM `80000400`–`80001C00`: {ptrs_low}", ""]

    # Verdict
    icache_game = [f for f in funcs if any(t.func_of(c) < LIBULTRA_TEXT_START for c in t.calls.get(f, []))]
    tlb_game = [a for a in tlb if any(t.func_of(c) < LIBULTRA_TEXT_START for c in t.calls.get(t.func_of(a), []))]
    out += ["## Verdict", ""]
    verdict = [
        f"- DMA with a constant destination in `.text` or low RAM: {len(exec_dest)}",
        f"- I-cache invalidation reachable from game code: {'yes' if icache_game else 'no'}",
        f"- TLB writes reachable from game code: {'yes' if tlb_game else 'no'}",
        f"- `jalr` with a constant target outside `.text`: {len(bad_targets)}",
    ]
    out += verdict
    overlay_risk = bool(exec_dest or icache_game or tlb_game or bad_targets)
    out += ["", "**Overlays: possible, investigate the items above.**" if overlay_risk else
            "**No evidence of overlays.** Treat the main segment as the only code section.", ""]

    (ROOT / "docs" / "overlays.md").write_text("\n".join(out), encoding="utf-8")
    print("\n".join(verdict))
    print("OVERLAY RISK" if overlay_risk else "no evidence of overlays")
    return 0


if __name__ == "__main__":
    sys.exit(main())
