# Library split: a common recomp library plus a small repo per game

**Goal:** pull what this project built into a common library repo, so each new game's repo holds only its own data and hooks.

**Finding:** most of this repo is already game-agnostic.
- About 95% of the C++ shell (around 2,900 lines in `src/` and `include/`) contains nothing specific to Wayne Gretzky's 3D Hockey.
- The game-specific facts sit in a few places:
  - the constants in `tools/rom_map.py`;
  - the symbol files in `syms/`;
  - a few dozen lines of `src/main/main.cpp`;
  - the data tables in `tools/gen_config.py` (`SPIN_FLAGS`, `TEST_HOOKS`).
- The split is mostly about moving that knowledge into one per-game config file, not rewriting code.

## Proposed shape

**Common library** (working name `recomp-kit`), in three parts:
1. **C++ app shell** on top of N64ModernRuntime and RT64: window, audio, input, settings, log, crash reporting and test hooks.
2. **Runtime patches** (0001–0003), until they're upstreamed.
3. **Python tooling:** ROM prep, symbol recovery, config generation, audits, the scenario harness and the analysis tools. Packaged as one CLI: `recomp-kit prep`, `syms`, `config`, `audit`, `scenario`.

**Each game's repo** contains only:
- `game.toml`: identity, ROM hashes, memory layout, microcode addresses, busy-wait flags and test hooks;
- `syms/`;
- game-specific hook functions;
- `tests/scenarios/`;
- an icon;
- a `CMakeLists.txt` of about 20 lines that calls a library function such as `recomp_add_game(...)`.

## Moves as-is

These need only a rename of the `wg3d_` / `WG3D_` prefixes to a neutral one.

| Area | Files | Notes |
|---|---|---|
| Log, crash handler | `src/main/log.cpp`, `src/main/crash_handler.cpp` | Only the window title in messages is game-specific; it becomes a parameter. |
| Settings | `src/main/config.cpp`, `include/wg3d_config.h` | Already generic: librecomp `Config`, every RT64 option, window placement. |
| Renderer | `src/main/rt64_render_context.cpp`, `include/wg3d_render.h` | No game references. |
| Input | `src/game/input.cpp`, `include/wg3d_input.h` | Script and log environment variables, the game-time clock, pak presence. The default key map becomes a default that 5.5 (remapping) makes configurable. |
| Test hooks | `capture.cpp`, `frame_log.cpp`, `dl_stats.cpp`, `trace.cpp`, `include/trace.h`, `register_overlays.cpp` | `dl_stats` assumes the F3D family of graphics microcode; keep it as an option. |
| PI | `src/game/pi.cpp` (`osPiRawReadIo`) | Generic open-bus behaviour; only a comment names this game. |
| Busy-wait yield | `wg3d_spin_yield` in `src/game/test_hooks.cpp` | Generic. The per-game part is which flags it watches, which is data. |
| Build helpers | `tools/cmake_build.bat`, `res/wg3d.rc.in`, the subsystem/PDB/ICF link settings in `CMakeLists.txt` | Become the `recomp_add_game(...)` CMake function. |
| Harness | `tools/run_scenario.py`, `frame_pacing.py`, `contact_sheet.py`, `shot_wg3d.ps1`, `capture_series.ps1`, `run_wg3d.ps1` | Only the ROM path and `GAME_START` are game data. |
| Runtime patches | `patches/runtime/0001`–`0003` | Benefit every game; see "Runtime patches" below. |

## Needs parameterising

The code is generic but has this game's constants inside.

| File | What moves into `game.toml` |
|---|---|
| `src/main/main.cpp` | Title, `game_id`, internal name, XXH3 ROM hash, sha1 text, audio microcode address (`0x8009E530`), save type. The rest becomes a library entry point, `run_game(GameSpec)`, and each game's `main.cpp` shrinks to about 20 lines. |
| `tools/rom_prep.py` | Expected sha1 and output file name. |
| `tools/rom_map.py` | Entry point, main segment ROM/VRAM, text/data/BSS bounds, microcode ranges. Its region-scanning logic is generic. |
| `tools/gen_config.py` | `SPIN_FLAGS`, `TEST_HOOKS` and `PHASE3_GAPS` become data in `game.toml`. The generator itself is generic. |
| `gen_syms.py`, `verify_syms.py`, `export_splat.py`, `run_ghidra.py`, `ghidra/WG3DExport.java`, `ghidra/WG3DSeed.java` | File names and seeds. `WG3DSeed.java`'s seed addresses become an input file. |
| `gen_libsyms.py`, `libmatch.py`, `wsl/*.sh` | Generic libultra identification against reference libraries; game-specific only in paths. |
| `hw_audit.py`, `check_indirect.py`, `find_spinloops.py`, `identify_ucode.py`, `check_aspmain.py`, `coverage_report.py`, `disasm.py` | Read `rom_map` and the symbols file. Generic once those come from `game.toml`. |
| `find_overlays.py` | The DMA-wrapper search is partly generic, but its heuristics were tuned to this game. Needs the most rework. |
| `verify_phase2.py`, `verify_phase3.py` | Tied to this project's phases. Replace with one generic `verify` command: recompile gate, boot gate, scenario gate. |

## Stays per game

- `syms/*`: `wg3d.us.syms.toml`, `segments.toml`, `overrides.toml`, `libultra_symbols.txt`, `dead_ignore.txt`.
- The seed hook (`func_80047E5C` at `0x80047F50`) and any future game patches.
- `tests/scenarios/*.toml` and their golden screenshots (local only; they're ROM-derived).
- The icon. `tools/make_icon.py` draws this game's artwork; the library just takes an `.ico`.
- `aspMain.toml` (the audio microcode address), unless the generator writes it from `game.toml`.

## Runtime patches

They're the most valuable thing to reuse, and the worst fit for a library.

| Patch | What it does | Plan |
|---|---|---|
| 0001 | VI null-mode fix | Already fixed upstream (N64ModernRuntime PR #153). Drop it once the submodule moves past that commit. |
| 0002 | Controller Pak per port, plus pak call log | Overlaps the open upstream Controller Pak PR #139. Wait for that PR and adapt. |
| 0003 | Lockstep speed multiplier for fast test runs | New, and useful to every recomp. Offer it upstream once it has proven itself on a second game. |

Until then, the library pins the runtime and applies its patch stack in one place, instead of every game repo repeating the three `git apply` lines.

## Things to settle before splitting

- **Licensing: settled (2026-10-09).**
  - Quest64-Recomp (Rainchus), Zelda64Recomp and N64ModernRuntime are GPL-3.0. N64Recomp and RT64 are MIT.
  - `src/main/main.cpp`, `src/main/rt64_render_context.cpp` and `include/wg3d_render.h` are adapted from Quest64-Recomp and carry SPDX headers with attribution.
  - This repo is now GPL-3.0-only as a whole (previously MIT for the project's own code). The library and the per-game repos should be GPL-3.0 too: anything that links N64ModernRuntime is GPL-3.0 already.
- **Prove it on a second game before freezing an API.** A library extracted from one game will carry that game's assumptions without showing them:
  - display-list stats for F3D only;
  - a single audio microcode;
  - libultra matching assumes the IDO compiler;
  - one main code segment with no overlays;
  - Controller Pak saves only.

  A small second target with a different shape, such as overlays or EEPROM saves, shows which abstractions hold.
- **Naming and comments.** Phase and chunk references ("chunk 3.3", "Phase 4") run through the comments and docstrings. They mean nothing in a shared repo; replace them with plain descriptions or links to design notes.

## Suggested order

1. **Add `game.toml` to this repo first.** Move every constant into it, without splitting the repo. Then run `python tools/run_scenario.py all` to confirm nothing changed. This is the risky refactor, so do it where the tests already exist.
2. **Shape the library inside this repo.**
   - Rename the prefixes.
   - Add `run_game(GameSpec)` and the `recomp_add_game()` CMake function.
   - Make the Python tools one package with a CLI.
3. **Split.** Move the library into its own repo and make this one depend on it (submodule or pinned version). The scenario harness must still pass.
4. **Second game.** Start one from a template repo (`game.toml` plus an empty `syms/`) and fix whatever it breaks before calling the API stable.
