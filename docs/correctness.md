# Correctness checklist

What "the game plays like the N64 original" means for this project, and the evidence for each part. The
v0.4.0 exit criterion (docs/ROADMAP.md) is every row at **pass**. A row passes on a scenario run, a
comparison with ares, or someone playing it and checking. Each row says which.

Scenarios are in `tests/scenarios/`, run with `python tools/run_scenario.py quick|all` (see its header). Goldens and ares
captures live in `build/ref/` (git-ignored, because they come from the ROM).

Status: **pass**, **fail** (with an issue link), **untested**, **n/a** (can't happen in wg3d, or no way to test it; the row says why).

## Boot and menus

| Area | Status | Evidence |
|---|---|---|
| Logos, legal screen, title | pass | `menus` goldens; match ares (boot gate, and `menus` 0.99–1.00) |
| Main menu, Setup, Options, Controls, Audio screens | pass | `menus`, `menus2` goldens |
| Records: Wayne Gretzky, High Scores, Team Stats | pass | `menus`, `menus2` goldens |
| Player screen, 4 ports, no Controller Pak | pass | `players_4p_nopak` golden; boot gate run C |
| Team and line select | pass | `menus`, `two_player` goldens |
| Initials entry, "STATS WILL NOT BE SAVED" | pass | `initials` goldens (INITIALS without a pak) |
| "NAME TAKEN" | pass | `name_taken` golden: with paks in ports 1 and 2, player 2 can't register the name player 1 just registered |

## Controller Pak

| Area | Status | Evidence |
|---|---|---|
| Pak notes list, delete a note (L+R), pak stats | pass | `menus2` goldens and expected `osPfsDeleteFile` |
| Season restore screen | pass | `menus` golden |
| New season file written | pass | `season_game` expects the allocate and write |
| Season saved after each game | pass | `season_save` expects the second write after the first game. A season is saved only when it is tied to a pak by entering a name on CTLR PAK1; otherwise the game shows a password after each game (`func_800790E8`) |
| Written pak read back after a restart | pass | boot gate runs A and B |
| Pak full (no free note slot, or not enough free pages) | pass | `pak_full`, `pak_full_pages` (`pak_notes` fills the pak with another game's notes): the game gets PFS_DIR_FULL / PFS_DATA_FULL and opens CTLR PAK NOTES to delete notes. Needed runtime patch 0004: before it, a full pak silently overwrote note 0 |
| Pak removed or swapped while playing ("NO CONTROLLER PAK", "CONTROLLER PAK HAS CHANGED", "PRESS A TO TRY AGAIN") | n/a | wg3d can't remove or swap a pak while running (paks come from `general.json` at start), so these can't happen. Without a pak the game simply doesn't offer pak options |
| Pak repair (`osPfsChecker`), "CORRUPTED", "IS DEAD" | n/a | ultramodern emulates the pak at the file level (`saves/controllerpak_portN/`) and never reports an inconsistent pak; `osPfsChecker` is a no-op, so the game's repair path can't run |

## Gameplay

| Area | Status | Evidence |
|---|---|---|
| Exhibition: a whole game and the statistics screens | pass | `full_game` (start and stats screens have goldens) |
| Pause menu, instant replay | pass | `replay` goldens |
| Short season: first game, standings, league scores, second game | pass | `season`, `season_game` |
| Two human players on one team | pass | `two_player` goldens up to the faceoff, then plays on |
| Attract mode (5.5 minutes CPU vs CPU) | pass | `attract`: runs without errors (its gameplay captures have no goldens, see below) |
| Fights | pass | `penalties_fights` expects `fight starts`: fight camera, health bars, knockdown, FIGHTING 1:15 majors. A fight starts when a check finds the checked player's fight meter at 100; scripted checks add 2–3 points and it falls back, so the test fills it with `WG3D_FIGHT_FORCE` (`src/game/test_hooks.cpp`) |
| Penalties and power plays | pass | `penalties_fights` (Options -> SIMULATION unlocks PENALTIES): HOOKING calls, penalty counts on the statistics screen, ICING and OFFSIDES in Simulation rules |
| Overtime | pass | `overtime` holds the score at 0-0 through regulation (`WG3D_POKE`): OVERTIME!, SUDDEN DEATH, the O.T. clock, and the game ends on the first goal or tied. The game has no shootout (no such string or screen in the ROM) |
| Password entry accepted | pass | `password` restores a season from a password ares generated (NF96N3-3S: Hartford after one loss); its next game, Montreal at Hartford, matches the ares schedule. Passwords are nine characters on two lines |
| Stanley Cup endings (`func_80077FB4`: `CHAMPION`, `CUPCRZY`, `LOSEROOM*`, `LOSETV*`) | pass | Both start from a playoff password at the Finals. `stanley_cup_win`: YOU WON THE STANLEY CUP, which runs until START (game state 4, expected in the log) and then returns to the logos. `stanley_cup_lose`: the series lost 3-4, YOU LOSE in the locker room, GAME OVER |
| "On fire" net effect (`NETFIRE*`) | pass | play-tested (2026-10-18) |
| Period clock speed matches ares | pass | `full_game` on both: 51 s of game clock on wg3d against 54 s on ares over the same 60 s, which fits the clock stopping at different faceoffs and goals |

## Graphics, audio, input

| Area | Status | Evidence |
|---|---|---|
| Frame pacing | pass | steady 30 fps, every frame held 2 VIs: `docs/frame_pacing.md` |
| Screens match ares | pass | `build/scen/<name>/compare.png` for `menus`, `menus2`, `season`, `replay`, `full_game`: every menu, records, setup, pak and season screen scores 0.98–1.00 against ares. The exceptions all have a cause: the main menu (0.64–0.68, animated background), the player screen (ares has 4 pads configured), and team select and gameplay (ares picks a random default team; wg3d's seed is fixed). Replay and the stats screens look the same |
| Music and sound effects, pitch and speed | pass | play-tested (2026-10-18) |
| Keyboard controls | pass | every scenario drives port 1 through the same path as the keyboard |
| Real gamepad, hot-plug | pass | play-tested (2026-10-18) |
| 4 real pads | n/a | not available for testing; 4 ports are covered with fake pads (`players_4p_nopak`, boot gate run C) |

## Runtime

| Area | Status | Evidence |
|---|---|---|
| No runtime errors (crash, unknown RSP task, assertion) | pass | every scenario fails on these log patterns |
| Clean exit | pass | Quitting while a game thread was in a busy-wait could crash after librecomp freed RDRAM (4 of 6 runs on the initials screen). `wg3d_spin_yield` now parks the thread once the game is quitting (0 of 12) |
| FPU flush-to-zero | pass | Doesn't matter for this game: with it on or off the only exception flag raised is inexact (0x20); no denormal or underflow ever. Golden captures are identical with `WG3D_FTZ=0` |
| Code coverage | pass | `docs/coverage.md`: 1105 of 1304 compiled functions entered (93.9% of the bytes; 956 and 81.9% before the scenarios below were added). The unentered ones are below |
| Hardware access, indirect jumps | pass | `docs/hw_audit.md`, `docs/indirect_targets.md` |

### Unentered code

41 functions of 0x100 bytes or more never run in any scenario (`docs/coverage.md`). Only one references an
identifying string: `func_80010120` (`ARENA`, `ATTRLOGO`), reached only through a function pointer. The rest are
branches in code that does run (`caller entered`), helpers of those, or library code the game links but doesn't
use (`_bcopy`, `alRaw16Pull`). They are compiled exactly like the code that runs, and every feature on this
checklist reaches its code, so none of them is a known gap.

The burning net (`func_8005C85C`) was play-tested rather than scripted.

## Known issue: gameplay is not repeatable in test runs

With `WG3D_FIXED_SEED`, menus and the start and end screens of a game repeat exactly. Gameplay, attract mode
included, branches at a different moment each run, sometimes within the first minute. That's why those captures are `nogolden`.
This only affects lockstep test runs (`--speed` above 1): normal play has no fixed seed and no lockstep.
Things that were ruled out:
- the RNG seed
- the input timing (identical input logs)
- the wall-clock part of lockstep `osGetTime` (making it exact made attract mode branch too)

## Test-only settings

These change what the game does, so only scenarios use them (`src/game/test_hooks.cpp`):
- `WG3D_FIXED_SEED`: pins the RNG seed (every scenario).
- `WG3D_FIGHT_FORCE`: fills a checked player's fight meter.
- `WG3D_POKE`: holds a memory value for a stretch of game time (overtime: both scores).
- `pak_notes` / `pak_note_pages` (scenario keys): start with another game's notes on the pak.

`WG3D_FIGHT_LOG` and `WG3D_WATCH` only log.

## Play-test

Play with the coverage build, so the functions you reach are added to `docs/coverage.md`:

```
mkdir build\scen\playtest
set WG3D_COVERAGE_FILE=%CD%\build\scen\playtest\coverage.txt
build\cmake-trace\wg3d.exe
```

Then run `python tools/coverage_report.py`. Check off each **untested** row above that you can reach, and
note anything that looks or sounds different from ares.
