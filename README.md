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
Pak. A correctness pass is in progress; see [docs/ROADMAP.md](docs/ROADMAP.md) for the plan to v1.0.0.

## Requirements (Windows)
- Visual Studio 2022 Build Tools with clang-cl, Windows SDK 10.0.26100 (RT64's D3D12 backend needs it)
- CMake, Ninja, Git
- Python 3.11+ with `rabbitizer`, `pyelftools`, `xxhash`, `numpy`, `pillow` (the symbol-recovery tools also use `splat64`/`spimdisasm`
  and Ghidra, but their outputs are committed in `syms/`, so they aren't needed to build)

## Build
```
git clone --recursive https://github.com/geno55/wg3d-recomp.git
cd wg3d-recomp
python tools/rom_prep.py path/to/your/rom.v64          # -> baserom.us.z64 (checks sha1 and CIC)
cmake -S lib/N64Recomp -B lib/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build lib/N64Recomp/build                      # N64Recomp.exe + RSPRecomp.exe
python tools/gen_config.py                             # wg3d.us.toml -> RecompiledFuncs/
tools\cmake_build.bat wg3d                             # -> build/cmake/wg3d.exe
```
Configuring applies `patches/runtime/*.patch` to `lib/N64ModernRuntime` (patches already in the tree are
skipped). Run `tools\cmake_build.bat` from PowerShell or cmd, not Git Bash.

`python tools/package_release.py` packages a Release build as `build/release/wg3d-recomp-v<version>-windows-x64.zip`.

## Run
```
build\cmake\wg3d.exe                          # the launcher: Choose ROM... stores it under %APPDATA%\WG3DRecomp
build\cmake\wg3d.exe path\to\your\rom.v64     # or give it on the command line (also how --frames test runs get it)
```
The launcher has Start game, Choose ROM..., Settings and Quit. In game, Esc or a controller's Back (View) button
opens the settings menu over the paused game: General, Controls (bindings, keyboard port, deadzone, which
controller goes on which port), Graphics and Sound. Changes apply at once and are saved. The menu works with
a keyboard, a controller or the mouse. `--frames` test runs skip the launcher.
Default keyboard (port 1): arrows/WASD stick, X = A, C = B, Z/LShift = Z, Enter = Start, Q/E = L/R, IJKL = C buttons,
TFGH = D-pad. Alt+Enter or F11 toggles fullscreen. Gamepads (Xbox layout) take ports 1–4 in connection order.
Saves: `%APPDATA%\WG3DRecomp\saves\controllerpak_port<N>\`.

`wg3d.exe` has no console window. Its output goes to `%APPDATA%\WG3DRecomp\logs\wg3d.log` (the previous two
runs are kept as `wg3d.1.log` and `wg3d.2.log`); crash reports are saved there as `crash-<date>-<time>.txt`.
`--console` also shows the output in a console, and configuring with `-DWG3D_CONSOLE=ON` builds a console program.

Settings live next to the saves and are created with defaults on first run. The settings menu edits them; to edit
the files by hand, close the game first.
`general.json` has `pak_port_1`–`pak_port_4` (which ports have a Controller Pak; all four by default) and
`pause_when_unfocused`.
`controls.json` has `keyboard_port` (1–4, 0 = off), `deadzone` (percent), `pads` (per port, part of a controller's
name to put on that port) and the `keyboard` and `gamepad` bindings: up to two SDL key names or controller inputs
(`a`, `leftshoulder`, `dpup`, `leftx-`, `righttrigger`, ...) per N64 button and stick direction.
`sound.json` has `master_volume` (0–100) and `mute_when_unfocused`. Widescreen: `aspect_ratio` `Expand` draws the rink
as wide as the window; menus and the HUD stay 4:3 in the middle. `graphics.json` has `resolution`
(`Original`/`Original2x`/`Auto`), `downsampling` (1–4), `window_mode` (`Windowed`/`Fullscreen`), `aspect_ratio`
(`Original`/`Expand`), `hud_ratio`, `antialiasing` (`None`/`MSAA2X`/`MSAA4X`/`MSAA8X`), `refresh_rate`
(`Original`/`Display`/`Manual`), `refresh_rate_manual`, `high_precision_framebuffer` and `graphics_api`
(`Auto`/`D3D12`/`Vulkan`). `window.json` remembers the window's size and position. High frame rate: `refresh_rate`
`Display` (or `Manual`) has RT64 generate frames between the game's 30, up to the monitor's rate; camera cuts are
shown without interpolation.

Verification scripts: `tools/verify_syms.py`, `tools/verify_recompile.py`, `tools/verify_boot.py`. Scenario tests:
`python tools/run_scenario.py quick` after a change, `all` before a release (runs the game at 10x speed, 3 sessions
at a time; `--speed 1 --jobs 1` for real time). What has been checked, and how: [docs/correctness.md](docs/correctness.md).

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

## Submodule patches
`lib/N64ModernRuntime` is pinned to sonicdcer's `controller_pak_super_rebase` branch (Controller Pak support);
`patches/runtime/` holds this project's changes on top (VI null-mode fix, per-port paks + pak call log, a lockstep
speed multiplier for fast test runs, the full-pak check, and pausing the game for the menu). `patches/rt64/` holds
four changes to RT64 for frame interpolation: camera cuts are presented without it and not matched to the frame
before; object pairs that jump more than a set distance aren't interpolated; presents repeated over the paused
game (settings menu) show the last frame without interpolation; and a race between destroying render targets on
a settings change and presenting them is fixed. Both are applied when
CMake configures.
