# W.G. 3D Hockey: Recompiled

A native Windows port of *Wayne Gretzky's 3D Hockey* (N64, USA V1.0) built by static recompilation:
[N64Recomp](https://github.com/N64Recomp/N64Recomp) turns the game's MIPS code into C, which runs on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) (librecomp + ultramodern) and renders through
[RT64](https://github.com/rt64/rt64). Personal project.

**No game data is included.** You need your own ROM (sha1 `400aa84811f1f2f6c62c756b43b54d534d5a5ec8` as `.z64`;
the byteswapped `.v64` dump works too). The ROM, everything extracted from it and all generated code are
git-ignored and produced locally by the build.

## Status
The game boots, renders, plays audio, takes keyboard and gamepad input (4 ports) and saves to the Controller
Pak. A correctness pass is in progress.

## Requirements (Windows)
- Visual Studio 2022 Build Tools with clang-cl, Windows SDK 10.0.26100 (RT64's D3D12 backend needs it)
- CMake, Ninja, Git
- Python 3.11+ with `rabbitizer`, `pyelftools`, `xxhash`, `numpy`, `pillow` (the symbol-recovery tools also use `splat64`/`spimdisasm`
  and Ghidra, but their outputs are committed in `syms/`, so they aren't needed to build)

## Build
```
git clone --recursive https://github.com/geno55/wg3d-recomp.git
cd wg3d-recomp
git -C lib/N64ModernRuntime apply ../../patches/runtime/0001-ultramodern-vi-null-mode.patch
git -C lib/N64ModernRuntime apply ../../patches/runtime/0002-ultramodern-pfs-per-port-and-log.patch
python tools/rom_prep.py path/to/your/rom.v64          # -> baserom.us.z64 (checks sha1 and CIC)
cmake -S lib/N64Recomp -B lib/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build lib/N64Recomp/build                      # N64Recomp.exe + RSPRecomp.exe
python tools/gen_config.py                             # wg3d.us.toml -> RecompiledFuncs/
tools\cmake_build.bat wg3d                             # -> build/cmake/wg3d.exe
```

## Run
```
build\cmake\wg3d.exe path\to\your\rom.v64     # first run stores the ROM under %APPDATA%\WG3DRecomp
build\cmake\wg3d.exe                          # later runs
```
Keyboard (port 1): arrows/WASD stick, X = A, C = B, Z/LShift = Z, Enter = Start, Q/E = L/R, IJKL = C buttons,
TFGH = D-pad. Gamepads (Xbox layout) take ports 1–4 in connection order. Saves:
`%APPDATA%\WG3DRecomp\saves\controllerpak_port<N>\`.

Settings live next to the saves and are created with defaults on first run; edit them while the game is closed.
`general.json` has `pak_port_1`–`pak_port_4` (which ports have a Controller Pak). `graphics.json` has `resolution`
(`Original`/`Original2x`/`Auto`), `downsampling` (1–4), `window_mode` (`Windowed`/`Fullscreen`), `aspect_ratio`
(`Original`/`Expand`), `hud_ratio`, `antialiasing` (`None`/`MSAA2X`/`MSAA4X`/`MSAA8X`), `refresh_rate`
(`Original`/`Display`/`Manual`), `refresh_rate_manual`, `high_precision_framebuffer` and `graphics_api`
(`Auto`/`D3D12`/`Vulkan`). `window.json` remembers the window's size and position.

Verification scripts: `tools/verify_*.py`.

## License
This project's own code (`src/`, `include/`, `tools/`, configs, docs) is MIT-licensed; see [LICENSE](LICENSE).
The submodules keep their own licenses. N64ModernRuntime is GPL-3.0 and is linked into `wg3d.exe`, so built
binaries are distributed under the GPL-3.0. The game itself is not covered: its code and data belong to their
rights holders, and nothing derived from the ROM is in this repository.

## Runtime patches
`lib/N64ModernRuntime` is pinned to sonicdcer's `controller_pak_super_rebase` branch (Controller Pak support);
`patches/runtime/` holds this project's two changes on top (VI null-mode fix, per-port paks + pak call log).
