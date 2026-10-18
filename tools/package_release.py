"""Release package: build/release/wg3d-recomp-v<version>-windows-x64.zip from a Release build.

The version comes from project(... VERSION x.y.z) in CMakeLists.txt. The zip holds one folder,
wg3d-recomp-v<version>/, with:
  wg3d.exe, wg3d.pdb, SDL2.dll, dxil.dll, dxcompiler.dll   from the build directory
  msvcp140*.dll, vcruntime140*.dll                         Visual C++ runtime (VS redist folder)
  README.txt, THIRD-PARTY-NOTICES.txt                      res/release/*.txt.in with @VERSION@ filled in
  LICENSE.txt                                              LICENSE

--publish also creates the GitHub release v<version> (a full release, marked Latest) with the zip
attached. Its notes are the --notes file followed by README.txt. Tag v<version> must already be
pushed to origin.

Usage: python tools/package_release.py [--build-dir build/cmake] [--vc-redist DIR]
                                       [--publish --notes CHANGES.txt]
"""
import argparse
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_FILES = ["wg3d.exe", "wg3d.pdb", "SDL2.dll", "dxil.dll", "dxcompiler.dll"]
VC_RUNTIME = ["msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll"]
TEMPLATES = ["README.txt", "THIRD-PARTY-NOTICES.txt"]


def project_version() -> str:
    m = re.search(r"project\(\s*\w+\s+VERSION\s+([0-9.]+)", (ROOT / "CMakeLists.txt").read_text())
    if not m:
        sys.exit("CMakeLists.txt: no project(... VERSION x.y.z)")
    return m.group(1)


def find_vc_redist() -> Path:
    # Newest x64 CRT folder of any installed Visual Studio 2022 edition.
    base = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022")
    if not base.exists():
        base = Path(r"C:\Program Files\Microsoft Visual Studio\2022")
    found = sorted(base.glob("*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT"),
                   key=lambda p: [int(x) for x in p.parents[1].name.split(".") if x.isdigit()])
    if not found:
        sys.exit("Visual C++ redist folder not found; pass --vc-redist")
    return found[-1]


def crlf(text: str) -> bytes:
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def git(*args) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build/cmake")
    ap.add_argument("--vc-redist", help="folder with the VC++ runtime DLLs (default: newest VS 2022 redist)")
    ap.add_argument("--publish", action="store_true", help="create the GitHub release with gh")
    ap.add_argument("--notes", help="text file with the changes since the last release (needed for --publish)")
    args = ap.parse_args()

    version = project_version()
    tag = f"v{version}"
    build = ROOT / args.build_dir
    redist = Path(args.vc_redist) if args.vc_redist else find_vc_redist()
    if args.publish and not args.notes:
        sys.exit("--publish needs --notes")

    missing = [str(build / f) for f in BUILD_FILES if not (build / f).exists()]
    missing += [str(redist / f) for f in VC_RUNTIME if not (redist / f).exists()]
    if missing:
        sys.exit("missing:\n  " + "\n  ".join(missing))
    # WG3D_VERSION is compiled in as a string; a stale exe from an older version won't contain it.
    if f"{version}".encode() not in (build / "wg3d.exe").read_bytes():
        sys.exit(f"{build / 'wg3d.exe'} does not contain version {version}; rebuild it")

    texts = {name: (ROOT / "res" / "release" / f"{name}.in").read_text(encoding="utf-8").replace("@VERSION@", version)
             for name in TEMPLATES}
    texts["LICENSE.txt"] = (ROOT / "LICENSE").read_text(encoding="utf-8")

    folder = f"wg3d-recomp-{tag}"
    out = ROOT / "build" / "release" / f"{folder}-windows-x64.zip"
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in BUILD_FILES:
            z.write(build / f, f"{folder}/{f}")
        for f in VC_RUNTIME:
            z.write(redist / f, f"{folder}/{f}")
        for name, text in texts.items():
            z.writestr(f"{folder}/{name}", crlf(text))
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size} bytes (VC++ runtime from {redist})")

    if not args.publish:
        return 0
    if not git("ls-remote", "--tags", "origin", f"refs/tags/{tag}"):
        sys.exit(f"tag {tag} is not on origin; tag and push it first")
    notes = Path(args.notes).read_text(encoding="utf-8").rstrip() + "\n\n" + texts["README.txt"]
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        f.write(notes)
    r = subprocess.run(["gh", "release", "create", tag, str(out), "--verify-tag", "--title", tag,
                        "--notes-file", f.name, "--latest"], cwd=ROOT)
    Path(f.name).unlink()
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
