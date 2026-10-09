"""Find busy-wait (polling) loops in the recompiled code.

On the N64 a thread spinning on a memory flag gets preempted by the interrupt-driven thread
that clears it. ultramodern only switches game threads inside OS calls, so a loop such as

    L: lh   $t7, 0($v0)        # no calls, no stores
       bne  $t7, $zero, L

spins forever. This script lists every backward branch whose loop body has no calls, no
stores and at least one load (a polling loop), the polled address where it is a constant, and
every store to that address (who is expected to release the loop).

Usage: python tools/find_spinloops.py   -> docs/spinloops.md
"""
import struct
import sys
import tomllib
from collections import defaultdict
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from find_overlays import field  # noqa: E402
from rom_map import VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
LOADS = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "lwc1", "ldc1"}
STORES = {"sb", "sh", "sw", "sd", "swc1", "sdc1", "swl", "swr"}
MAX_BODY = 24  # instructions


def const_sweep(a, insns):
    """Linear lui/addiu/ori constant tracking (no merge resets). Returns per-instruction reg maps
    (state *before* each instruction) — an approximation good enough for flag addresses, which are
    almost always built right before the loop."""
    regs, states = {}, []
    for x in insns:
        states.append(dict(regs))
        op = x.getOpcodeName()
        rs, rt, rd = field(x, "rs"), field(x, "rt"), field(x, "rd")
        if op == "lui":
            regs[rt] = (x.getProcessedImmediate() << 16) & 0xFFFFFFFF
            continue
        if op in ("addiu", "ori") and rs in regs:
            imm = x.getProcessedImmediate()
            regs[rt] = (regs[rs] + imm if op == "addiu" else regs[rs] | (imm & 0xFFFF)) & 0xFFFFFFFF
            continue
        if rt is not None and x.modifiesRt():
            regs.pop(rt, None)
        if rd is not None and x.modifiesRd():
            regs.pop(rd, None)
        if x.isFunctionCall():
            for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                regs.pop(r, None)
    return states


def find_loops():
    """Returns (loops, stores): loops = [(func, branch_vram, head_vram, body_len, [(load_vram, addr|None, disasm)])],
    stores = {addr: [(func, vram, op)]} for constant-address stores."""
    rom = (ROOT / "baserom.us.z64").read_bytes()
    word = lambda v: struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
    syms = tomllib.loads((ROOT / "syms" / "wg3d.us.syms.toml").read_text(encoding="utf-8"))
    funcs = sorted((f["vram"], f["size"], f["name"]) for f in syms["section"][0]["functions"])

    loops = []  # (func, branch_vram, target, body_len, [(load_vram, addr|None, disasm)])
    stores = defaultdict(list)  # addr -> [(func, vram, size)]
    for a, size, name in funcs:
        insns = [rabbitizer.Instruction(word(v), v) for v in range(a, a + size, 4)]
        states = const_sweep(a, insns)
        for i, x in enumerate(insns):
            op = x.getOpcodeName()
            rs = field(x, "rs")
            if op in STORES and rs in states[i]:
                addr = (states[i][rs] + x.getProcessedImmediate()) & 0xFFFFFFFF
                stores[addr].append((name, a + 4 * i, op))
            if not x.isBranch() and op not in ("b", "j"):
                continue
            target = x.getBranchVramGeneric() if x.isBranch() else x.getInstrIndexAsVram()
            v = a + 4 * i
            if not (a <= target <= v) or (v - target) // 4 + 2 > MAX_BODY:
                continue
            body = range((target - a) // 4, min(i + 2, len(insns)))
            body_ops = [insns[j].getOpcodeName() for j in body]
            if any(insns[j].isFunctionCall() or insns[j].isJump() and insns[j].getOpcodeName() == "jr" for j in body):
                continue
            if any(o in STORES or o == "syscall" for o in body_ops):
                continue
            body_loads = []
            for j in body:
                if body_ops[j - body.start] in LOADS:
                    y = insns[j]
                    base = field(y, "rs")
                    # Loop-invariant base: take its constant from the state at the loop head.
                    st = states[body.start]
                    addr = (st[base] + y.getProcessedImmediate()) & 0xFFFFFFFF if base in st else None
                    body_loads.append((a + 4 * j, addr, y.disassemble()))
            # A spin loop re-reads a fixed location: every load address is a known constant and no
            # load base register is modified inside the body (array scans / pointer walks fail this).
            bases = {field(insns[j], "rs") for j in body if body_ops[j - body.start] in LOADS}
            modified = set()
            for j in body:
                y = insns[j]
                for r, m in ((field(y, "rt"), y.modifiesRt), (field(y, "rd"), y.modifiesRd)):
                    if r is not None and m():
                        modified.add(r)
            if body_loads and not (bases & modified):
                loops.append((name, v, target, len(body), body_loads))

    return loops, stores


def main() -> int:
    loops, stores = find_loops()
    md = ["# Polling loops", "",
          "Generated by `tools/find_spinloops.py`. Backward branches whose body (at most "
          f"{MAX_BODY} instructions) has loads but no calls or stores, re-reading fixed locations (loop-invariant base registers). On the N64 these are released by "
          "a preempting thread or interrupt; under ultramodern (cooperative switching inside OS calls) "
          "they spin forever unless the releasing store comes from another *host* thread.", "",
          f"Found {len(loops)} loops in {len({l[0] for l in loops})} functions.", "",
          "| Function | Branch | Loop head | Body | Polled address(es) | Stores to it (function @ vram) |",
          "|---|---|---|---|---|---|"]
    for name, v, target, n, loads in loops:
        addrs = sorted({ad for _, ad, _ in loads if ad is not None})
        writers = []
        for ad in addrs:
            for w in stores.get(ad, []):
                writers.append(f"{w[0]} @ {w[1]:08X} ({w[2]})")
        addr_s = ", ".join(f"{ad:08X}" for ad in addrs) or "(pointer: " + "; ".join(d for _, _, d in loads) + ")"
        md.append(f"| {name} | {v:08X} | {target:08X} | {n} | {addr_s} | {'<br>'.join(writers) or '—'} |")
    out = ROOT / "docs" / "spinloops.md"
    out.write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"{len(loops)} polling loops -> {out.relative_to(ROOT)}")
    for name, v, target, n, loads in loops:
        addrs = sorted({ad for _, ad, _ in loads if ad is not None})
        print(f"  {name:16} {v:08X} body={n:2} polls={[f'{ad:08X}' for ad in addrs] or '?'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
