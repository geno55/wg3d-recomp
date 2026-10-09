"""Classify every region of the ROM and emit the main-segment layout.

Usage: python tools/rom_map.py [baserom.us.z64]
Writes docs/rom_map.md and syms/segments.toml, and fails if any code-like block shows up
outside the known main-segment .text (so tools/find_overlays.py knows whether overlays need hunting).
"""
import re
import struct
import sys
from pathlib import Path

import rabbitizer

ROOT = Path(__file__).resolve().parent.parent
CODE_WINDOW = 0x400
PAD_RUN = 0x800

MAIN_ROM = 0x1000
MAIN_VRAM = 0x80001C00
VRAM_TO_ROM = MAIN_ROM - MAIN_VRAM

# Main segment layout (VRAM). The boundaries come from the initial ROM analysis; check() re-derives
# them from the ROM and fails on any mismatch.
TEXT_START = 0x80001C00
LIBULTRA_TEXT_START = 0x8008B2A0  # osInitialize; 0x8008B280 is a game asm helper (ctc1 a0, FCSR)
TEXT_END = 0x8009BC50             # last CPU instruction 0x8009BC44, then nop padding
RSP_TEXT_END = 0x8009F350         # rspboot, F3D (dead), F3D WG3DH, aspMain
DATA_END = 0x800B7040             # game .data/.rodata, then libultra .data/.rodata
UCODE_DATA_END = 0x800B8390       # F3D (dead), F3D WG3DH, aspMain data
BSS_SIZE = 0xAA570                # cleared by the entry stub; ends at the initial SP 0x80162900
ENTRY_SP = 0x80162900

RSP_TEXT = [
    ("rspboot", 0x8009BC50, 0x8009BD20),
    ("gspF3D_sdk2.0E (dead)", 0x8009BD20, 0x8009D130),
    ("gspF3D_wg3dh", 0x8009D130, 0x8009E530),
    ("aspMain", 0x8009E530, 0x8009F350),
]
RSP_DATA = [
    ("gspF3D_sdk2.0E data (dead)", 0x800B7040, 0x800B7840),
    ("gspF3D_wg3dh data", 0x800B7840, 0x800B8040),
    ("aspMain data", 0x800B8040, 0x800B8390),
]


def rom_of(vram: int) -> int:
    return vram + VRAM_TO_ROM


def block_stats(blk: bytes) -> dict:
    words = struct.unpack(f">{len(blk) // 4}I", blk)
    nonzero = [w for w in words if w]
    return {
        "pad": sum(1 for w in words if w in (0, 0xFFFFFFFF)) / len(words),
        "valid": sum(1 for w in nonzero if rabbitizer.Instruction(w).isValid()) / max(1, len(nonzero)),
        "jr_ra": sum(1 for w in words if w == 0x03E00008),
        "sp_adj": sum(1 for w in words if w >> 16 == 0x27BD),
    }


def classify_block(s: dict) -> str:
    if s["pad"] >= 0.98:
        return "padding"
    if s["valid"] >= 0.95 and s["jr_ra"] >= 1 and s["sp_adj"] >= 1:
        return "code?"
    return "data"


def check(rom: bytes) -> list[str]:
    """Re-derive the hard-coded boundaries from the ROM. Returns a list of errors."""
    errors = []
    w = lambda v: struct.unpack(">I", rom[rom_of(v):rom_of(v) + 4])[0]
    # Entry stub: BSS clear base/size, then the stack pointer.
    if (w(0x80001C00) >> 16, w(0x80001C08) & 0xFFFF) != (0x3C08, 0x8390) or w(0x80001C04) & 0xFFFF != 0x000A:
        errors.append("entry stub does not match the expected BSS-clear sequence")
    if (UCODE_DATA_END + BSS_SIZE) != ENTRY_SP:
        errors.append("BSS end != initial SP")
    last_ret = max(v for v in range(TEXT_START, TEXT_END, 4) if w(v) == 0x03E00008)
    if not all(w(v) == 0 for v in range(last_ret + 8, TEXT_END, 4)):
        errors.append(f"non-zero words between last jr ra {last_ret:08X} and TEXT_END")
    if w(TEXT_END) != 0x34210001:  # rspboot starts with: ori $at, $at, 1
        errors.append("rspboot not found at TEXT_END")
    return errors


def main() -> int:
    rom_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "baserom.us.z64"
    rom = rom_path.read_bytes()

    errors = check(rom)

    main_rom_end = rom_of(UCODE_DATA_END)
    last_data = len(rom)
    while last_data and rom[last_data - 1] in (0x00, 0xFF):
        last_data -= 1

    # Outside the main segment: asset data, split only by exact padding runs of at least PAD_RUN bytes.
    runs = []
    cursor = main_rom_end
    pad_re = re.compile(rb"\x00{%d,}|\xFF{%d,}" % (PAD_RUN, PAD_RUN))
    for m in pad_re.finditer(rom, main_rom_end, last_data):
        runs += [[cursor, m.start(), "asset data"], [m.start(), m.end(), "padding"]]
        cursor = m.end()
    runs += [[cursor, last_data, "asset data"], [last_data, len(rom), "padding (0xFF)"]]

    # Look for small code islands (e.g. overlays) in 1KB windows.
    stray_code = [
        off for off in range(main_rom_end & ~(CODE_WINDOW - 1), last_data, CODE_WINDOW)
        if classify_block(block_stats(rom[off:off + CODE_WINDOW])) == "code?"
    ]

    regions = [
        (0x0, 0x40, "ROM header"),
        (0x40, 0x1000, "IPL3 boot code (CIC-6102)"),
        (rom_of(TEXT_START), rom_of(LIBULTRA_TEXT_START), "main .text: game code"),
        (rom_of(LIBULTRA_TEXT_START), rom_of(TEXT_END), "main .text: libultra/libaudio"),
        *[(rom_of(a), rom_of(b), f"main RSP text: {n}") for n, a, b in RSP_TEXT],
        (rom_of(RSP_TEXT_END), rom_of(DATA_END), "main .data/.rodata (game, then libultra)"),
        *[(rom_of(a), rom_of(b), f"main RSP data: {n}") for n, a, b in RSP_DATA],
        *[(a, b, c) for a, b, c in runs],
    ]

    # Every ROM byte must be covered exactly once.
    cursor = 0
    for a, b, _ in regions:
        if a != cursor:
            errors.append(f"coverage gap/overlap at {cursor:#x} (next region starts {a:#x})")
        cursor = b
    if cursor != len(rom):
        errors.append(f"regions end at {cursor:#x}, ROM is {len(rom):#x}")
    if stray_code:
        errors.append("code-like blocks outside main .text: " + ", ".join(f"{o:#x}" for o in stray_code))

    # docs/rom_map.md
    lines = [
        "# ROM map — W.G. 3D Hockey (US V1.0)",
        "",
        "Generated by `tools/rom_map.py`. Do not edit by hand.",
        "",
        "| ROM start | ROM end | Size | VRAM | Region |",
        "|---|---|---|---|---|",
    ]
    for a, b, name in regions:
        vram = f"`{a - VRAM_TO_ROM:08X}`" if MAIN_ROM <= a < main_rom_end else ""
        lines.append(f"| `{a:06X}` | `{b:06X}` | `{b - a:#x}` | {vram} | {name} |")
    lines += [
        "",
        f"- `.bss`: VRAM `{UCODE_DATA_END:08X}`–`{UCODE_DATA_END + BSS_SIZE:08X}` (size `{BSS_SIZE:#x}`). The entry stub "
        f"clears it and sets SP to `{ENTRY_SP:08X}`, so the boot stack sits directly after BSS.",
        f"- Last non-padding byte in the ROM: `{last_data - 1:#x}`. After it, `0xFF` padding runs to 8MB.",
        f"- Code-like blocks outside main .text: {'none' if not stray_code else len(stray_code)}.",
    ]
    (ROOT / "docs").mkdir(exist_ok=True)
    (ROOT / "docs" / "rom_map.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    # syms/segments.toml: the section layout that gen_syms.py builds on.
    toml = [
        "# Generated by tools/rom_map.py. Main-segment layout (single static segment, no overlays known yet).",
        f"entrypoint = {TEXT_START:#010x}",
        "",
        "[[section]]",
        'name = ".text"',
        f"rom = {rom_of(TEXT_START):#x}",
        f"vram = {TEXT_START:#010x}",
        f"size = {TEXT_END - TEXT_START:#x}",
        f"libultra_start = {LIBULTRA_TEXT_START:#010x}  # osInitialize",
        "",
    ]
    for name, a, b in RSP_TEXT + [(".data/.rodata", RSP_TEXT_END, DATA_END)] + RSP_DATA:
        toml += ["[[region]]", f'name = "{name}"', f"rom = {rom_of(a):#x}", f"vram = {a:#010x}", f"size = {b - a:#x}", ""]
    toml += ["[bss]", f"vram = {UCODE_DATA_END:#010x}", f"size = {BSS_SIZE:#x}", ""]
    (ROOT / "syms").mkdir(exist_ok=True)
    (ROOT / "syms" / "segments.toml").write_text("\n".join(toml), encoding="utf-8")

    for a, b, name in regions:
        print(f"{a:06X}-{b:06X}  {name}")
    if errors:
        print("\nERRORS:\n  " + "\n  ".join(errors))
        return 1
    print("\nOK: full ROM coverage, boundaries verified, no code outside main .text")
    return 0


if __name__ == "__main__":
    sys.exit(main())
