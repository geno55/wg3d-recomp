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
git -C lib/N64ModernRuntime apply ../../patches/runtime/0003-ultramodern-speed-lockstep.patch
python tools/rom_prep.py path/to/your/rom.v64          # -> baserom.us.z64 (checks sha1 and CIC)
cmake -S lib/N64Recomp -B lib/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build lib/N64Recomp/build                      # N64Recomp.exe + RSPRecomp.exe
python tools/gen_config.py                             # wg3d.us.toml -> RecompiledFuncs/
tools\cmake_build.bat wg3d                             # -> build/cmake/wg3d.exe
```

## Run
```
build\cmake\wg3d.exe                          # first run asks for the ROM and stores it under %APPDATA%\WG3DRecomp
build\cmake\wg3d.exe path\to\your\rom.v64     # or give it on the command line (also how --frames test runs get it)
```
Keyboard (port 1): arrows/WASD stick, X = A, C = B, Z/LShift = Z, Enter = Start, Q/E = L/R, IJKL = C buttons,
TFGH = D-pad. Alt+Enter or F11 toggles fullscreen. Gamepads (Xbox layout) take ports 1–4 in connection order.
Saves: `%APPDATA%\WG3DRecomp\saves\controllerpak_port<N>\`.

`wg3d.exe` has no console window. Its output goes to `%APPDATA%\WG3DRecomp\logs\wg3d.log` (the previous two
runs are kept as `wg3d.1.log` and `wg3d.2.log`); crash reports are saved there as `crash-<date>-<time>.txt`.
`--console` also shows the output in a console, and configuring with `-DWG3D_CONSOLE=ON` builds a console program.

Settings live next to the saves and are created with defaults on first run; edit them while the game is closed.
`general.json` has `pak_port_1`–`pak_port_4` (which ports have a Controller Pak). `graphics.json` has `resolution`
(`Original`/`Original2x`/`Auto`), `downsampling` (1–4), `window_mode` (`Windowed`/`Fullscreen`), `aspect_ratio`
(`Original`/`Expand`), `hud_ratio`, `antialiasing` (`None`/`MSAA2X`/`MSAA4X`/`MSAA8X`), `refresh_rate`
(`Original`/`Display`/`Manual`), `refresh_rate_manual`, `high_precision_framebuffer` and `graphics_api`
(`Auto`/`D3D12`/`Vulkan`). `window.json` remembers the window's size and position.

Verification scripts: `tools/verify_*.py`. Scenario tests: `python tools/run_scenario.py all` (runs the game at
10x speed, 3 sessions at a time; `--speed 1 --jobs 1` for real time).

## License
Copyright (C) 2026 geno55. This repository (`src/`, `include/`, `tools/`, tests, configs, docs) is licensed under
the GNU General Public License v3.0 only; see [LICENSE](LICENSE). Parts of the runtime glue
(`src/main/main.cpp`, `src/main/rt64_render_context.cpp`, `include/wg3d_render.h`) are adapted from
[Quest64-Recomp](https://github.com/Rainchus/Quest64-Recomp) (GPL-3.0).
The submodules keep their own licenses: N64ModernRuntime is GPL-3.0; N64Recomp and RT64 are MIT. Built binaries
are distributed under the GPL-3.0. Releases up to v0.2.0 described this project's own code as MIT; from now on
it is GPL-3.0.

The game itself is not covered: its code and data belong to their rights holders, and nothing derived from
the ROM is in this repository.

## Runtime patches
`lib/N64ModernRuntime` is pinned to sonicdcer's `controller_pak_super_rebase` branch (Controller Pak support);
`patches/runtime/` holds this project's three changes on top (VI null-mode fix, per-port paks + pak call log,
and a lockstep speed multiplier for fast test runs).
