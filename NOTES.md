# XC3 Recomp: bring-up notes

Fork of XCX Recomp (`xcx` remote, master 1025eac). Game data stays out of the repository.

## Inputs

| File | Title id | Notes |
|---|---|---|
| base | `010074F013262000` | 14.8 GB |
| update 2.2.1 (v720896) | `010074F013262800` | 600 MB; RomFS through BKTR, only `main` differs from the base |
| add-on content 1-4 | `010074F013263001`-`4` | index 4 is Future Redeemed; mounted from `build/extracted/upd/aoc/<index>` |

## Survey

- NintendoSDK 12.3.9. Modules `main`, `sdk`, `subsdk0`, `rtld`. Init arrays: main 838, subsdk0 45, sdk 112.
- First `nvnBootstrapLoader` returns to main+0xe9906c.
- Lifted: 135,411 functions, 485 MB of C, 0 unsupported instructions, 33 s.
- NACP languages (SupportedLanguageFlag 0x70de, NACP bit order): en-GB, ja, fr, de, es, it, ko, zh-Hant, zh-Hans. There is no
  en-US data; `ov_system.c game_language` makes the console's fallback.

## Build and run

```powershell
.venv-py314\Scripts\python tools\extract_xc3.py --base game\base.nsp --update game\update-2.2.1.nsp --keys <prod.keys> --dlc game\dlc1.nsp game\dlc2.nsp game\dlc3.nsp game\dlc4.nsp --hactool <hactool.exe>
.venv\Scripts\python recomp\gen.py --exefs build\extracted\upd\exefs --out generated\upd
tools\build_xc3.ps1
build\upd\xc3.exe
```

Test runs: `XC3_ASSUME_FOCUS=1 XC3_BACKGROUND_WINDOW=1`, `XC3_INPUT_SCRIPT=<file>`, `--seconds N --shot x.png`,
`XC3_SHOTS=<from>-<to>:<step>`, `XC3_DUMP=<frame>`. `--shot` and `XC3_SHOTS` ignore the window crop: a 1280x720 picture in
a corner of 1920x1080 is the capture, not the screen.

## Test shortcuts

`XC3_MOVIE_SKIP=<frames>` ends every movie after that many frames. The opening still has timed subtitle scenes, so the first
battle comes sooner by the movies' length, not at once.

## Known gaps

- Transform feedback is not replayed (`nvn_api_cmd.c xfb_skipped`): it needs `VK_EXT_transform_feedback` and translator support.
- No patch table: the game runs at its own frame rate (about 30 fps) and resolution. `recomp/patches-*.txt` were XCX's and were
  removed. Comments that say XC3 in `frame_rate.c`, `resolution.c` and the `UI_*` sites of `nvn_api_cmd.c` describe XCX addresses;
  those paths are inert until the sites are found in XC3's `main`.
- `docs/` and `tests/` still describe XCX behaviour under the renamed names.
