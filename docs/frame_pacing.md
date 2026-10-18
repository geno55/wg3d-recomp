# Frame-pacing baseline

Real-time (`--speed 1`) runs on the reference machine, made with
`python tools/run_scenario.py menus full_game replay --frame-log --jobs 1` and `python tools/frame_pacing.py menus full_game replay`
(2026-10-17, v0.4.0 development build). Column meanings are in `tools/frame_pacing.py`. Compare a new run against
this before a release: the game runs at 30 fps with every frame held 2 VIs; a few flip-backs at menu
transitions are in this baseline.

```
== menus
   scene (ends at capture)        VIs   fps  hold (VIs x count)           drop flip onscr
   logos                          233  29.7  2x115                           0    0     0
   legal                          240  29.9  2x119                           0    0     0
   title                          270  29.9  2x134                           0    0     0
   menu                           198  30.0  2x98                            0    0     0
   setup                          360  30.1  1x2 2x178                       0    1     1
   options                        193  29.8  2x95                            0    0     0
   menu_setup                     239  29.7  2x118                           0    0     0
   records                        240  29.9  2x119                           0    0     0
   records_gretzky                180  29.8  2x89                            0    0     0
   menu_records                   240  29.9  2x119                           0    0     0
   season_menu                    330  30.0  2x164                           0    0     0
   pak_restore                    181  29.8  2x89                            0    0     0
   menu_season                    180  29.7  2x88                            0    0     0
   players                        299  29.8  2x148                           0    0     0
   team_select                    150  29.8  2x74                            0    0     0
   line_select                    180  29.8  2x89                            0    0     0
   intro                          331  29.9  2x164                           0    0     0
   rink                           240  29.7  2x118                           0    0     0
   faceoff                        360  29.8  2x178                           0    0     0
   pause                          269  30.0  1x2 2x132                       0    1     1
   end                            121  29.5  2x59                            0    0     0
   TOTAL                         5034  30.0  1x4 2x2513                      0    2     2
== full_game
   scene (ends at capture)        VIs   fps  hold (VIs x count)           drop flip onscr
   start                         2394  30.0  2x1195                          0    0     0
   mid                           3601  30.0  1x4 2x1797                      0    2     2
   late                          3599  30.0  2x1798                          0    0     0
   game_stats                    1800  30.0  2x898                           0    0     0
   after_a1                       480  29.9  1x2 2x237                       0    1     1
   after_a2                       300  29.7  2x148                           0    0     0
   after_a3                       540  30.0  1x2 2x267                       0    1     1
   end                            181  29.8  2x89                            0    0     0
   TOTAL                        12895  30.0  1x8 2x6442                      0    4     4
== replay
   scene (ends at capture)        VIs   fps  hold (VIs x count)           drop flip onscr
   faceoff                       2274  30.0  2x1135                          0    0     0
   pause_replay                   330  29.9  1x2 2x162                       0    1     1
   replay_1                       210  29.6  2x103                           0    0     0
   replay_2                       300  29.8  2x148                           0    0     0
   replay_3                       300  29.7  2x148                           0    0     0
   replay_4                       420  29.8  2x208                           0    0     0
   end                            121  29.5  2x59                            0    0     0
   TOTAL                         3955  30.0  1x2 2x1975                      0    1     1
```
