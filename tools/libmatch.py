"""Fuzzy-match unnamed library functions against reference libultra objects.

The game ships libultra 2.0D. n64sym has no 2.0D signatures, and 2.0E differs just enough to
break its exact matching. Instead, each function in the game's library range becomes a
sequence of opcode mnemonics, with registers, immediates and relocations ignored. That
sequence is compared against every function in reference archives built by ultralib.

Usage: python tools/libmatch.py <ref.a> [ref.a ...]
Prints one line per library-range function: address, current name (if any), best match, score.
"""
import bisect
import difflib
import io
import re
import struct
import sys
from pathlib import Path

import rabbitizer
from elftools.elf.elffile import ELFFile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from find_overlays import Text  # noqa: E402
from rom_map import LIBULTRA_TEXT_START, TEXT_END  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def ar_members(data: bytes):
    assert data[:8] == b"!<arch>\n"
    off = 8
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[:16].decode().strip()
        size = int(hdr[48:58].decode().strip())
        body = data[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)
        if name not in ("/", "//") and body[:4] == b"\x7fELF":
            yield name.rstrip("/"), body


def ops(words) -> list[str]:
    return [rabbitizer.Instruction(w).getOpcodeName() for w in words]


def load_refs(paths) -> dict[str, list[str]]:
    refs = {}
    for p in paths:
        for _, body in ar_members(Path(p).read_bytes()):
            elf = ELFFile(io.BytesIO(body))
            text = elf.get_section_by_name(".text")
            symtab = elf.get_section_by_name(".symtab")
            if not text or not symtab or not text.data_size:
                continue
            tdata = text.data()
            tidx = elf.get_section_index(".text")
            funcs = sorted((s["st_value"], s.name) for s in symtab.iter_symbols()
                           if s["st_shndx"] == tidx and s["st_info"]["type"] in ("STT_FUNC", "STT_NOTYPE")
                           and s.name and not s.name.startswith((".", "$")))
            for i, (start, name) in enumerate(funcs):
                end = funcs[i + 1][0] if i + 1 < len(funcs) else len(tdata)
                key = name if name not in refs else f"{name}@{Path(p).stem}"
                if end - start >= 8 and key not in refs:
                    refs[key] = ops(struct.unpack(f">{(end - start) // 4}I", tdata[start:end]))
    return refs


def load_data_names(paths) -> set[str]:
    """Names of data objects (STT_OBJECT) defined in the reference archives."""
    out = set()
    for p in paths:
        for _, body in ar_members(Path(p).read_bytes()):
            symtab = ELFFile(io.BytesIO(body)).get_section_by_name(".symtab")
            if symtab:
                out |= {s.name for s in symtab.iter_symbols()
                        if s["st_info"]["type"] == "STT_OBJECT" and s["st_shndx"] != "SHN_UNDEF" and s.name}
    return out


def load_n64sym(path: Path) -> dict[int, str]:
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r"([0-9A-F]{8}) (\S+)", line)
        if m:
            out.setdefault(int(m[1], 16), m[2])
    return out


def main() -> int:
    refs = load_refs(sys.argv[1:])
    rom = (ROOT / "baserom.us.z64").read_bytes()
    t = Text(rom)
    named = {}
    for f in ("n64sym_ram_t.txt", "n64sym_ram_E.txt"):
        named.update({a: n for a, n in load_n64sym(ROOT / "build" / f).items()
                      if LIBULTRA_TEXT_START <= a < TEXT_END})
    # Real function starts: named, or called by someone. Heuristic starts with no callers are
    # usually the tail of a function that has an early `jr ra`.
    real = sorted({a for a in t.starts if LIBULTRA_TEXT_START <= a < TEXT_END and (a in named or t.calls.get(a))}
                  | {LIBULTRA_TEXT_START})
    ref_items = list(refs.items())
    print(f"# {len(refs)} reference functions, {len(real)} library-range functions")
    for i, a in enumerate(real):
        end = real[i + 1] if i + 1 < len(real) else TEXT_END
        mine = ops(t.words[t.idx(a):t.idx(end)])
        while mine and mine[-1] == "nop":
            mine.pop()
        best = []
        for name, ro in ref_items:
            if not (0.6 < len(ro) / max(1, len(mine)) < 1.6):
                continue
            sm = difflib.SequenceMatcher(None, mine, ro, autojunk=False)
            if sm.real_quick_ratio() < 0.75 or sm.quick_ratio() < 0.75:
                continue
            best.append((sm.ratio(), name))
        best.sort(reverse=True)
        top = best[0] if best else (0.0, "-")
        second = best[1] if len(best) > 1 else (0.0, "-")
        cur = named.get(a, "")
        print(f"{a:08X} size {(end - a):#6x} cur {cur or '-':28s} best {top[1]:28s} {top[0]:.3f}  "
              f"2nd {second[1]} {second[0]:.3f}  callers {len(t.calls.get(a, []))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
