# Roadmap to v1.0.0

v0.2.0 is a playable Windows port. Menus, exhibition games, seasons, replays and attract mode have been
checked against ares. What it still lacks for 1.0:
- a defined, finished correctness pass
- a repeatable release process
- the settings UI and display options players expect from a recomp

**v1.0.0 means** the game plays like the N64 original in every mode, can be set up without editing files,
and supports widescreen and high frame rate. Windows only; other platforms come after 1.0.

Costs: **S** = an evening, **M** = a few days, **L** = a week or more, or needs game reverse-engineering.
There is no CI. Every check runs locally, on one machine, so the testing below is sized for limited
compute: expensive runs are rare and their results are kept.

## v0.3.0 — Build and release hygiene (released)

- **Apply the runtime patches automatically.** **S–M**
  - Today the Build steps in the README `git apply` three patches by hand.
  - `src/main/main.cpp` won't build without 0003 (`set_speed_multiplier`).
  - Either apply `patches/runtime/*.patch` from `CMakeLists.txt` at configure time, or shrink the set:
    - 0001 is fixed upstream (N64ModernRuntime PR #153) and can go with a submodule bump
    - follow PR #139 for 0002
    - offer 0003 upstream

  See `docs/library-split.md`.
- **`tools/package_release.py`.** **S** Builds `wg3d-recomp-vX.Y.Z-windows-x64.zip` from a Release build:
  - `wg3d.exe` and `wg3d.pdb`
  - the SDL2, dxil and dxcompiler DLLs
  - the VC++ runtime DLLs
  - `LICENSE.txt` (GPL-3.0) and `THIRD-PARTY-NOTICES.txt`
  - `README.txt` generated from a committed template, using the version in `CMakeLists.txt`

  The script can optionally publish with `gh release create`. Today the zip is assembled by hand.
- **Doc and naming cleanup.** **S**
  - `docs/libultra_coverage.md` still lists `osPiRawReadIo` as a gap; `src/game/pi.cpp` implements it.
  - Remove the leftover phase/chunk wording in the generated `include/wg3d_skipped.h`.
  - Rename `tools/verify_phase2.py` to `verify_recompile.py`, and `tools/verify_phase3.py` to
    `verify_boot.py`.

**Exit:** a fresh clone builds with the README steps alone, and one command produces the release zip.

## v0.4.0 — Correctness pass

Status: done. Every row in `docs/correctness.md` passes or can't apply to wg3d (n/a, with the reason).
Coverage went from 81.9% to 93.9% of the compiled bytes. The pass found and fixed two bugs: a full
Controller Pak overwrote another note (runtime patch 0004), and a crash on quit. Gameplay goldens turned out
to be only partly possible: gameplay branches between test runs even with the fixed seed, so only menus and
the start and end of a game have them.

This is the core of 1.0. Playing the game counts as evidence: not everything has to be automated.

- **`docs/correctness.md`.** **S** Lists every game mode and system with its status (pass / fail /
  untested) and evidence (scenario name, ares comparison or manual play). This is what "done" means for
  the pass.
- **Two scenario tiers** in `tools/run_scenario.py`. **S**
  - *quick* (a few minutes at 10x, `--jobs 1`): boot, menus, one short game, pak write/read. Run after
    each change.
  - *full*: every scenario. Run before a release or after a runtime/patch change.
  - The 330 s `attract` soak runs in the full tier only, shortened if possible.
- **Deterministic gameplay goldens.** **M**
  - Run the gameplay scenarios with `WG3D_FIXED_SEED` (`src/game/test_hooks.cpp`).
  - In-game captures then stop being `nogolden`, and a golden match is a regression check that needs
    no ares run.
- **Capture ares once per scenario** and keep the result under `build/ref/`. **S** Re-capture only when a
  scenario changes; `--target wg3d` becomes the normal mode.
- **Fill the scenario gaps that can be scripted.** **M**
  - a full season game with progression
  - a 2-player game (4 ports are covered by `players_4p_nopak`)
  - overtime / shootout
  - password entry

  Fights, penalties and changed options get checked by hand while playing.
- **Coverage, once.** **M**
  - One `WG3D_TRACE` build and one full-tier run, through `tools/coverage_report.py`, produce a committed
    `docs/coverage.md`.
  - Review every unentered function of 0x100 bytes or more.
  - Review every runtime stub whose call site was reached (e.g. `osContReset` asserts).
  - Redo it once more in v0.9.0.
- **Timing baselines.** **S** One `tools/frame_pacing.py` run per scene, committed, plus period clock
  speed and audio pitch compared with ares by hand.
- **FTZ on vs off.** **S** Compare one fixed-seed game with `WG3D_FTZ=0` and with the default, and write
  down the result.
- **Bug reports.** **S** A GitHub issue template that asks for `wg3d.log` and any crash report, plus a
  `correctness` label. Player reports cover hardware and play styles one machine can't.

**Exit:**
- every row in `docs/correctness.md` passes
- the full tier passes once
- the coverage review is done
- no open `correctness` issues

## v0.5.0 — Input and audio

Status: done.
- `controls.json`: remapping, keyboard port, deadzone, and pads reserved by name.
- `sound.json`: master volume and mute when unfocused.
- A pak in every port by default.

The octagonal stick range needed no work: ultramodern already maps the stick onto the N64's octagon
(`convert_to_n64_range`, radius 82). Music and SFX volume stay separate work (stretch goal).

- **Remapping.** **M** A `controls.json` with up to two keys and two pad inputs per N64 button and stick
  direction. It replaces the fixed tables in `read_pad` / `read_keyboard` (`src/game/input.cpp`).
- **Pads and paks.** **S–M**
  - Choose which pad drives which port; the keyboard can sit on any port.
  - Configurable deadzone (now the `StickDeadzone` constant, 0.15).
  - Optional N64-like octagonal stick range (about ±80); compare the feel with ares.
  - A Controller Pak in every connected port by default.
- **Audio.** **S**
  - A `sound.json` with master volume (a multiply in `queue_samples`, `src/main/main.cpp`).
  - Optional mute when unfocused.
  - Separate music and SFX volume is a stretch goal; it needs RE of the game's audio library. **M**

**Exit:** the new settings persist, and the quick tier still passes with default settings.

## v0.6.0 — Launcher and in-game menu

- **RmlUi**, ported from Quest64-Recomp / Zelda64Recomp. **M–L**
  - A launcher: ROM status, Start, Settings.
  - A settings menu over the paused game, opened with a hotkey, with General / Controls / Graphics /
    Sound tabs.
  - Graphics changes apply live through `RT64Context::update_config`.
  - Write settings with `save_config_json`. librecomp's `update_option_value` does nothing when the file
    didn't exist at load time, i.e. on first run.
- **Pause on focus loss** (option) and button glyphs that match the active device. **S**

**Exit:** every setting in the JSON files can be changed from the UI, and the UI works with a controller
alone.

## v0.7.0 — Widescreen

- **Expand aspect ratio.** **M–L** Start from RT64's `Expand` (`aspect_ratio` in `graphics.json`, now
  untested). Then add game patches as N64Recomp hooks (`tools/gen_config.py`):
  - widen culling so players don't pop in at the screen sides
  - anchor the HUD (score clock, player names) to the screen edges

**Exit:** 16:9 and 21:9 goldens for the gameplay scenarios, and no pop-in or misplaced HUD in any mode.

## v0.8.0 — High frame rate

- **Measure the native rate** per scene with `tools/frame_pacing.py`. **S**
- **Tag the matrices.** **L** Find where the rink camera, player and puck matrices and display lists are
  built. Patch them to carry stable IDs, so RT64 can interpolate between frames.
- **A frame rate option.** **S** Original stays the default, and must still match the v0.4.0 goldens
  exactly.

**Exit:** no interpolation artefacts (camera cuts, replays, menu transitions) and steady pacing on the
reference machine.

## v0.9.0 — Release candidate

- **Feature freeze and a public beta.** Fix only bugs from here.
- **Clean-machine test.** Try the release zip in a fresh Windows VM or Windows Sandbox (no dev tools, no
  VC++ redist). Test D3D12 and Vulkan on the reference machine. Other GPU vendors are covered by beta
  testers.
- **Final checks.** One full-tier run and the coverage redo, with widescreen and high frame rate off.
- **Docs.** Refresh `README.md` and the release `README.txt`.

**Exit:** two weeks of beta with no new blocker bugs.

## v1.0.0

Tag it, package it with `tools/package_release.py`, and publish it as a full release marked Latest.

## Release checklist (every version)

1. Bump `VERSION` in `CMakeLists.txt`.
2. Run the verify gates and the full scenario tier. The quick tier is enough for a patch release.
3. Run `tools/package_release.py`, then smoke-test the zip outside the build tree.
4. Tag `vX.Y.Z`, push the tag, and publish the GitHub release with the zip. It is a full release, not a
   pre-release.

## After 1.0

- **Linux and macOS.** These need ports of:
  - the crash handler and log redirection (`src/main/crash_handler.cpp`, `src/main/log.cpp`)
  - `src/main/capture.cpp`
  - the MSVC-only CMake flags and SDL2 download
  - the Windows-only tools (`cmake_build.bat`, `run_scenario.py` input injection)
- **Texture packs** (`.rtz`, the plumbing dropped from `src/main/rt64_render_context.cpp`) and **`.nrm`
  code mods** (librecomp `recomp::mods`).
- **Extras:** rumble, skipping the intro logos, keeping game options between sessions, and a debug menu.
- **The `recomp-kit` library split** described in `docs/library-split.md`.
