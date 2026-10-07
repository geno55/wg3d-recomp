"""Locate the RSP microcodes in the main code segment and identify them against RT64's database.

Only the WG3DH F3D variant is used by tasks; the stock F3D copy is referenced only as
rspbootTextEnd. The gfx task is built at 0x80001DF8 and the audio task at 0x80073008. Both reference rspboot,
plus their ucode text and data, through lui/addiu pairs. The addresses below were taken
from those sites.
RT64 identifies a ucode by XXH3-64 over the first N bytes of text and of data, with the
data read as N64 RDRAM (32-bit words in host byte order). See lib/rt64/src/gbi/rt64_gbi.cpp.

Usage: python tools/identify_ucode.py [baserom.us.z64]
"""
import hashlib
import struct
import sys
from pathlib import Path

import xxhash

MAIN_ROM, MAIN_VRAM = 0x1000, 0x80001C00

# name, vram, size, rt64 hash length, expected rt64 hash (or None)
SEGMENTS = [
    ("rspboot text",            0x8009BC50, 0x0D0,  None,   None),
    ("F3D SDK 2.0E text (dead)", 0x8009BD20, 0x1410, 0x1408, 0x9C0926F5E466BE70),
    ("F3D WG3DH text (gfx task)", 0x8009D130, 0x1400, 0x1400, 0x2963554DB650E3B2),
    ("aspMain text",            0x8009E530, 0xE20,  None,   None),
    ("F3D SDK 2.0E data (dead)", 0x800B7040, 0x800,  0x800,  0xEEB10D73400213B3),
    ("F3D WG3DH data (gfx task)", 0x800B7840, 0x800,  0x800,  0x53AF11FA4205F4E7),
    ("aspMain data",            0x800B8040, 0x350,  None,   None),
]


def word_swap(b: bytes) -> bytes:
    n = len(b) // 4
    return struct.pack(f"<{n}I", *struct.unpack(f">{n}I", b))


def main() -> int:
    rom_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "baserom.us.z64"
    rom = rom_path.read_bytes()
    ok = True
    for name, vram, size, hlen, expected in SEGMENTS:
        off = vram - MAIN_VRAM + MAIN_ROM
        blob = rom[off:off + size]
        line = f"{name:30s} vram {vram:08X} rom {off:06X} size {size:#06x} sha1 {hashlib.sha1(blob).hexdigest()[:16]}"
        if hlen:
            h = xxhash.xxh3_64_intdigest(word_swap(blob[:hlen]))
            match = h == expected
            ok &= match
            line += f"  rt64 {h:016X} {'MATCH' if match else 'MISMATCH'}"
        print(line)
    print("all RT64 hashes match" if ok else "ERROR: RT64 hash mismatch")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
