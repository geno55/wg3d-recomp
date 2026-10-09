"""Final check of the audio microcode (aspMain) recompilation.

RSPRecomp only knows two kinds of indirect-jump targets: return addresses of jal/jalr (site + 8),
and `extra_indirect_branch_targets` from aspMain.toml. Any other computed jump would fail at
runtime with RspExitReason::UnhandledJumpTarget. This script:
  1. checks the text bounds (fits IMEM, ends with a jump + delay slot, nop padding only after it),
  2. checks every instruction decodes as a valid RSP instruction,
  3. classifies every jr/jalr by where its register comes from (link register, DMEM table load,
     constant) and checks the target set,
  4. scans the ucode's DMEM data for halfwords that look like IMEM code addresses (other jump
     tables) and requires each to be a known target,
  5. checks the generated rsp/aspMain.cpp switch covers exactly the expected targets.

Usage: python tools/check_aspmain.py
"""
import re
import struct
import sys
import tomllib
from pathlib import Path

import rabbitizer

ROOT = Path(__file__).resolve().parent.parent
DATA_ROM, DATA_SIZE = 0xB7440, 0x350        # aspMain data (ucode_data = 0x800B8040), loaded at DMEM 0
DISPATCH_DMEM, DISPATCH_COUNT = 0x10, 16    # u16 command handler table in DMEM


def main() -> int:
    cfg = tomllib.loads((ROOT / "aspMain.toml").read_text())
    rom = (ROOT / "baserom.us.z64").read_bytes()
    base, size, imem = cfg["text_offset"], cfg["text_size"], cfg["text_address"]
    words = struct.unpack(f">{size // 4}I", rom[base:base + size])
    ins = [rabbitizer.Instruction(w, imem + 4 * i, rabbitizer.InstrCategory.RSP) for i, w in enumerate(words)]
    lo = lambda a: a & 0x1FFF
    at = {id(x): lo(imem + 4 * i) for i, x in enumerate(ins)}  # IMEM address of each instruction
    errors, notes = [], []

    # 1. Bounds
    end = (imem & 0x1FFF) + size
    if end > 0x2000:
        errors.append(f"text ends at IMEM {end:#x}, past 0x2000")
    last = max(i for i, w in enumerate(words) if w)
    if not (ins[last - 1].isJump() or ins[last].isJump() or ins[last - 1].getOpcodeName() in ("j", "jr", "b")):
        errors.append(f"last non-zero word at IMEM {lo(imem + 4 * last):#x} is not a jump or its delay slot")
    notes.append(f"text: ROM {base:#x}+{size:#x} -> IMEM {lo(imem):#x}-{end:#x}; last code at IMEM "
                 f"{lo(imem + 4 * last):#x} ({ins[last].disassemble()}), {(len(words) - 1 - last) * 4} bytes of padding after")

    # 2. Valid instructions
    bad = [(at[id(x)], f"{w:08X}") for x, w in zip(ins, words) if w and not x.isValid()]
    if bad:
        errors.append(f"{len(bad)} words don't decode as RSP instructions: {bad[:5]}")

    # 3. Indirect jumps
    starts = {at[id(x)] for x in ins}
    link_targets = {at[id(x)] + 8 for x in ins if x.getOpcodeName() in ("jal", "jalr")}
    dispatch = [struct.unpack(">H", rom[DATA_ROM + DISPATCH_DMEM + 2 * k:DATA_ROM + DISPATCH_DMEM + 2 * k + 2])[0]
                for k in range(DISPATCH_COUNT)]
    extra = set(cfg.get("extra_indirect_branch_targets", []))
    if set(dispatch) != extra:
        errors.append(f"extra_indirect_branch_targets {sorted(map(hex, extra))} != dispatch table {sorted(map(hex, dispatch))}")
    kinds = {}
    for i, x in enumerate(ins):
        if x.getOpcodeName() not in ("jr", "jalr"):
            continue
        reg = x.rs.value
        src = "unknown"
        for j in range(i - 1, max(-1, i - 24), -1):
            y = ins[j]
            n = y.getOpcodeName()
            if n in ("jal", "jalr") and reg == 31:
                src = "link register (return)"
                break
            try:
                writes = y.modifiesRt() and y.rt.value == reg
            except RuntimeError:
                writes = False
            if writes:
                if n in ("lh", "lhu"):
                    src = f"DMEM table load ({y.disassemble()})"
                elif n in ("addi", "addiu", "add", "addu", "or") and y.rs.value == 31 and y.getProcessedImmediate() == 0                         if n in ("addi", "addiu") else n in ("add", "addu", "or") and 31 in (y.rs.value, y.rt.value):
                    src = "link register (return, via copy of $ra)"
                elif n in ("addi", "addiu", "ori") and y.rs.value == 0:
                    src = f"constant {y.getProcessedImmediate():#x}"
                else:
                    src = f"computed ({y.disassemble()})"
                break
        if reg == 31 and src == "unknown":
            src = "link register (return)"
        kinds.setdefault(src, []).append(at[id(x)])
    for k, sites in kinds.items():
        notes.append(f"jr/jalr from {k}: sites {', '.join(f'{s:#x}' for s in sites)}")
        if k.startswith("constant"):
            t = int(k.split()[1], 16)
            if t not in link_targets | extra:
                errors.append(f"constant jump target {t:#x} not covered")
        elif k.startswith("computed") or k == "unknown":
            errors.append(f"jump register source not understood at {sites}: {k}")

    # 4. Other IMEM-address tables in DMEM data
    data = rom[DATA_ROM:DATA_ROM + DATA_SIZE]
    suspects = []
    for off in range(0, DATA_SIZE, 2):
        h, = struct.unpack(">H", data[off:off + 2])
        if lo(imem) <= h < end and h % 4 == 0 and h in starts and not (DISPATCH_DMEM <= off < DISPATCH_DMEM + 2 * DISPATCH_COUNT):
            suspects.append((off, h))
    uncovered = [(o, h) for o, h in suspects if h not in link_targets | extra]
    # The only data-driven jump indexes the dispatch table with the command byte: the generated code
    # cannot reach these halfwords unless a command byte >= 16 is issued, and libaudio defines exactly
    # 16 audio commands (A_SPNOOP .. A_SETLOOP). So they are data (filter coefficients etc.).
    table_jumps = [k for k in kinds if k.startswith("DMEM table load")]
    if table_jumps != [f"DMEM table load (lh          $2, 0x{DISPATCH_DMEM:X}($2))"]:
        errors.append(f"unexpected data-driven jumps: {table_jumps}; the DMEM halfword scan can no longer be dismissed")
    notes.append(f"other DMEM halfwords that look like IMEM code addresses: {len(suspects)} "
                 f"({len(uncovered)} not already jump targets; unreachable: the only table jump reads the "
                 f"{DISPATCH_COUNT}-entry dispatch table at DMEM {DISPATCH_DMEM:#x}, indexed by the command byte)")
    for o, h in uncovered:
        notes.append(f"  DMEM {o:#05x}: {h:#06x}")

    # 5. Generated switch
    cpp = (ROOT / "rsp" / "aspMain.cpp").read_text()
    sw = cpp[cpp.index("do_indirect_jump:"):]
    cases = {int(m, 16) for m in re.findall(r"case 0x([0-9A-Fa-f]+):", sw)}
    if cases != link_targets | extra:
        errors.append(f"switch cases differ from expected: missing {sorted(map(hex, (link_targets | extra) - cases))}, "
                      f"extra {sorted(map(hex, cases - (link_targets | extra)))}")
    notes.append(f"generated switch: {len(cases)} cases = {len(link_targets)} jal return addresses + {len(extra)} dispatch entries")

    print("\n".join(notes))
    if errors:
        print("ERRORS:\n  " + "\n  ".join(errors))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
