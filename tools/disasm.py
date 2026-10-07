"""Disassembles functions from the ROM by name or vram (debug helper).
Usage: python tools/disasm.py func_80003898 0x8000392C ..."""
import struct
import sys
import tomllib
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rom_map import VRAM_TO_ROM  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
rom = (ROOT / "baserom.us.z64").read_bytes()
syms = tomllib.loads((ROOT / "syms" / "wg3d.us.syms.toml").read_text(encoding="utf-8"))
funcs = {f["name"]: f for f in syms["section"][0]["functions"]}
by_vram = {f["vram"]: f for f in funcs.values()}
for arg in sys.argv[1:]:
    f = by_vram.get(int(arg, 16)) if arg.startswith("0x") else funcs.get(arg)
    if not f:
        print(f"?? {arg}")
        continue
    print(f"--- {f['name']} @ {f['vram']:08X} size 0x{f['size']:X}")
    for v in range(f["vram"], f["vram"] + f["size"], 4):
        w = struct.unpack(">I", rom[v + VRAM_TO_ROM:v + VRAM_TO_ROM + 4])[0]
        x = rabbitizer.Instruction(w, v)
        extra = ""
        if x.isFunctionCall() and x.getOpcodeName() == "jal":
            t = x.getInstrIndexAsVram()
            extra = f"   ; {by_vram[t]['name'] if t in by_vram else hex(t)}"
        print(f"  {v:08X}: {x.disassemble()}{extra}")
