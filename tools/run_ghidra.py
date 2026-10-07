"""Chunk 1.5: headless Ghidra analysis of the main segment.

Imports ROM 0x1000-0xB7790 raw as MIPS:BE:64:64-32addr (VR4300 / MIPS III, 32-bit addresses) at
0x80001C00. tools/ghidra/WG3DSeed.java splits memory and seeds entry points and library names;
auto-analysis runs; tools/ghidra/WG3DExport.java writes build/ghidra_*.csv.

Usage: python tools/run_ghidra.py
Needs Ghidra 12.x and a JDK 21: GHIDRA_HOME / JAVA_HOME from the environment, or `ghidra_home` /
`java_home` in local_paths.toml at the repo root (git-ignored, machine-specific).
"""
import csv
import os
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCAL = ROOT / "local_paths.toml"
_local = tomllib.loads(LOCAL.read_text(encoding="utf-8")) if LOCAL.exists() else {}


def _required(env: str, key: str) -> str:
    value = os.environ.get(env) or _local.get(key)
    if not value:
        raise SystemExit(f"run_ghidra: set {env} or `{key}` in {LOCAL.name} (repo root, git-ignored)")
    return value


GHIDRA = Path(_required("GHIDRA_HOME", "ghidra_home"))
JAVA = _required("JAVA_HOME", "java_home")


def main() -> int:
    work = ROOT / "build" / "ghidra"
    (work / "proj").mkdir(parents=True, exist_ok=True)
    rom = (ROOT / "baserom.us.z64").read_bytes()
    (work / "main.bin").write_bytes(rom[0x1000:0xB7790])

    cmd = [
        str(GHIDRA / "support" / "analyzeHeadless.bat"), str(work / "proj"), "wg3d",
        "-import", str(work / "main.bin"), "-overwrite",
        "-processor", "MIPS:BE:64:64-32addr", "-loader", "BinaryLoader", "-loader-baseAddr", "0x80001C00",
        "-scriptPath", str(ROOT / "tools" / "ghidra"),
        "-preScript", "WG3DSeed.java", str(ROOT / "syms" / "libultra_symbols.txt"),
        "-postScript", "WG3DExport.java", str(ROOT / "build"),
        "-deleteProject",
    ]
    log = work / "analyze.log"
    with open(log, "w") as fh:
        rc = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, env={**os.environ, "JAVA_HOME": JAVA}).returncode
    text = log.read_text(errors="replace")
    for line in text.splitlines():
        if "WG3D" in line or "ERROR" in line or "Exception" in line:
            print(line.strip())
    if rc != 0 or "WG3DExport: done" not in text:
        print(f"analyzeHeadless failed (rc={rc}); see {log}")
        return 1

    funcs = list(csv.DictReader(open(ROOT / "build" / "ghidra_funcs.csv")))
    jumps = list(csv.DictReader(open(ROOT / "build" / "ghidra_jumps.csv")))
    unc = list(csv.DictReader(open(ROOT / "build" / "ghidra_uncovered.csv")))
    resolved = [j for j in jumps if j["targets"]]
    noncontig = [f for f in funcs if int(f["ranges"]) > 1]
    print(f"functions: {len(funcs)} ({len(noncontig)} with non-contiguous bodies)")
    print(f"register jumps (jr != ra): {len(jumps)}, with resolved targets: {len(resolved)}")
    print(f"uncovered non-zero .text ranges: {len(unc)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
