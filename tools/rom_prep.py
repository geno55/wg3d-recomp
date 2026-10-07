"""Normalize a Wayne Gretzky's 3D Hockey (US) ROM to big-endian .z64 and verify it.

Usage: python tools/rom_prep.py <input.{z64,v64,n64}> [output.z64]
Default output: baserom.us.z64 in the project root.
"""
import hashlib
import struct
import sys
import zlib
from pathlib import Path

EXPECTED_SHA1 = "400aa84811f1f2f6c62c756b43b54d534d5a5ec8"  # US V1.0, big-endian
EXPECTED_CRC = (0x6B45223F, 0xF00E5C56)

# CRC32 of IPL3 (ROM 0x40..0x1000) -> CIC variant
IPL3_CRCS = {
    0x6170A4A1: "6101",
    0x90BB6CB5: "6102",
    0x0B050EE0: "6103",
    0x98BC2C86: "6105",
    0xACC8580A: "6106",
    0x009E9EA3: "7102",
}

CIC_6102_SEED = 0xF8CA4DDC  # also used by 6101


def to_z64(data: bytes) -> tuple[bytes, str]:
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data, "z64"
    if magic == b"\x37\x80\x40\x12":  # 16-bit byteswapped
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b), "v64"
    if magic == b"\x40\x12\x37\x80":  # 32-bit little-endian
        n = len(data) // 4
        return struct.pack(f">{n}I", *struct.unpack(f"<{n}I", data)), "n64"
    raise ValueError(f"unrecognized ROM magic {magic.hex()}")


def header_checksum(rom: bytes) -> tuple[int, int]:
    """libultra header checksum over ROM 0x1000..0x101000, 6101/6102 variant."""
    M = 0xFFFFFFFF
    t1 = t2 = t3 = t4 = t5 = t6 = CIC_6102_SEED
    for d in struct.unpack(">262144I", rom[0x1000:0x101000]):
        if (t6 + d) & M < t6:
            t4 = (t4 + 1) & M
        t6 = (t6 + d) & M
        t3 ^= d
        s = d & 0x1F
        r = ((d << s) | (d >> (32 - s))) & M if s else d
        t5 = (t5 + r) & M
        t2 = t2 ^ r if t2 > d else t2 ^ (t6 ^ d)
        t1 = (t1 + (t5 ^ d)) & M
    return t6 ^ t4 ^ t3, t5 ^ t2 ^ t1


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "baserom.us.z64"

    rom, fmt = to_z64(src.read_bytes())
    sha1 = hashlib.sha1(rom).hexdigest()
    title = rom[0x20:0x34].decode("ascii", "replace").rstrip()
    game_code = rom[0x3B:0x3F].decode("ascii", "replace")
    entry, = struct.unpack(">I", rom[0x08:0x0C])
    crc1, crc2 = struct.unpack(">II", rom[0x10:0x18])
    cic = IPL3_CRCS.get(zlib.crc32(rom[0x40:0x1000]), "unknown")

    print(f"input format : {fmt}")
    print(f"title        : {title!r}  code {game_code}  rev {rom[0x3F]}")
    print(f"size         : {len(rom):#x}")
    print(f"entrypoint   : {entry:#010x}")
    print(f"header CRC   : {crc1:08X}-{crc2:08X}")
    print(f"CIC          : {cic}")
    print(f"sha1         : {sha1}")

    ok = True
    if sha1 != EXPECTED_SHA1:
        print("ERROR: sha1 mismatch (expected US V1.0)")
        ok = False
    if (crc1, crc2) != EXPECTED_CRC:
        print("ERROR: header CRC mismatch")
        ok = False
    if cic not in ("6101", "6102"):
        print(f"ERROR: unexpected CIC {cic}")
        ok = False
    else:
        calc = header_checksum(rom)
        print(f"computed CRC : {calc[0]:08X}-{calc[1]:08X}")
        if calc != (crc1, crc2):
            print("ERROR: computed checksum does not match header")
            ok = False
    if not ok:
        return 1

    dst.write_bytes(rom)
    print(f"OK -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
