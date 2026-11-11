# Installer

A public release that turns the player's own game files into a playable XC3 folder. The release carries no game
data and no code derived from the game: the installer builds the game on the player's machine. The installed folder
holds only the game; the installer and its tools stay outside it.

## What the player does

1. Downloads `XC3-Setup-<version>-windows.zip` from GitHub Releases (none is published yet), unpacks it anywhere and
   starts `XC3-Setup.exe`.
2. Picks:
   - the base game (XCI or NSP), the 2.2.1 update (NSP) and any of the four add-on content NSPs;
   - their `prod.keys`;
   - the game language and the install folder.
3. Waits for the setup, one time. The window shows each step and its progress.
4. Plays `xc3.exe` from the install folder. The installer and the source NSP or XCI can be deleted.

The install folder (`main.c` `portable_paths()` reads this layout; it stays portable):

```
<install folder>/
  xc3.exe, SDL3.dll, libopus-0.dll, xc3_xlate.exe
  amd_fidelityfx_vk.dll, sl.*.dll, nvngx_*.dll, NvLowLatencyVk.dll   FSR 3, DLSS and frame generation (bin/upscalers)
  xc3.ini                       language; the other keys as commented defaults (xc3.ini.example)
  exefs/, romfs/                 extracted from the player's files
  profile/                       saves
  logs/                          xc3.log of the last run, xc3.1.log to xc3.4.log of the runs before it
  shader and pipeline caches     prepared by the setup
  xc3-install.json              what built this folder: release version, input hashes, finished steps
```

`XC3_PROFILE_DIR` overrides the `profile/` folder. A `profile/` folder with no saves is skipped while the profile baked
into the exe at build time has some (a developer build copied into an install folder): the log says so and how to move
them.

## Updating the port

The player runs a newer installer on the same folder. It reads `xc3-install.json`, keeps `romfs/`, `exefs/`,
`profile/` and `xc3.ini` after checking their hashes, rebuilds `xc3.exe` and re-prepares the shaders. A newer
release that changes nothing game-derived skips the rebuild. Re-preparing keeps `shader-cache/` while the translator identity
(`xc3_xlate.id`, a hash of its sources) is unchanged and translates only the shaders missing from it; each release
builds the translator again with other bytes, so the bytes do not decide (`steps.xlate_identity`).

## Setup steps

1. **Check** before writing anything: the files open with the keys, the update is 2.2.1 (by hash, as
   `tools/extract.py`), the keys are new enough for the update, enough space is free on the target disk (about 32 GiB with the add-on content at the peak
   of a first install, 25 GiB after it), the CPU has
   AVX2 (`-march=x86-64-v3`), a Vulkan 1.3 GPU exists. Each failure is one clear message.
2. **Extract** ExeFS and RomFS into the install folder and verify the hashes.
3. **Add-on content**: the RomFS of each chosen NSP goes to `aoc/<index>/`, decrypted with the NSP's own ticket.
4. **Lift** the ExeFS to C (`recomp/gen.py`) into a temp folder next to the installer.
5. **Compile** the generated C, `modules.c`, `romfs.c`, `patches.c` and `ov_zstd.c` (they bake the per-install layout and
   RomFS hashes) and link them with the prebuilt runtime library into `xc3.exe` (`build_game.py --runtime-lib`).
6. **Prepare** the shaders. `tools/ard_shaders.py` finds every shader in the player's `bf3.ard` and each add-on's
   `.ard` (about 28,400 unique, 30 s), `tools/prepare_shaders.py` writes each one's default variant as seed files, and
   `xc3.exe --prepare-shaders` translates them into `shader-cache/` (about 1.1 GB) and builds them into the GPU driver's
   disk cache: under 4 minutes on 16 threads. A failure here is a warning: the game then translates shaders on first use.
   Then `tools/pack_shader_cache.py` folds the cache's loose files (about 200,000) into `shader-cache/shader-cache.pack`;
   xc3.exe reads the pack first and the loose files after it, and writes new translations as loose files, which the
   next run of the tool folds in.
7. **Clean up** the temp folder (generated C and objects, about 1.3 GB).

A cancelled or failed setup resumes from the last finished step (`xc3-install.json`).

## Command line

`installer/` (stdlib only; `installer/layout.py` maps the release folder and, in a repository, the dev toolchain):

```
python -m installer --base FILE --update FILE --keys FILE [--dlc FILE ...] --lang CODE --dest DIR [--root DIR] [--temp DIR]
                     [--verify] [--progress-lines]
```

- On a folder with `xc3-install.json` only `--dest` is needed; the game files only when the extraction must run again.
- `--dlc` takes any number of files, after one option or by repeating it (`--dlc=a.nsp --dlc=b.nsp`).
- `--progress-lines` prints `progress <0 to 1> <text>` lines in place of the JSON events, the setup protocol of a
  launcher that runs this without a window (`LineReporter` in `installer/events.py`). Every step weighs the same, so the
  number is a position, not a time estimate.
- stdout: one JSON event per line, types `start`, `plan`, `step` (started, done, skipped), `progress` (step, phase,
  done/total/unit), `space` (bytes needed and free on the install disk and the temp disk), `warning`, `error`, `end` (`installer/events.py`). Messages are ids with parameters;
  `installer/text/en.json` holds their English text. The human-readable log goes to `<temp>/logs/`.
- Exit codes: 0 done, 1 failed, 2 bad command line, 3 cancelled. The window cancels with Ctrl+Break to the
  installer's process group.
- Each step records a key over its inputs (runner.py). A step with the recorded key and intact output is skipped, so
  resume and update are one rule; output kept from another release is re-hashed first.
- Peak space on the install disk is about 36 GiB, not 22: the extraction copies both Program NCAs (16.3 GiB) next to
  the merged RomFS (15.9 GiB) before it deletes them. The check computes the exact figure.
- Window (`python -m installer.gui`, `installer/gui.py` draws, `installer/guilogic.py` decides): form, step list with
  progress, space line, collapsible log, Cancel (Ctrl+Break to the CLI's process group, then it waits for the exit),
  finish screen with Open folder. English or Russian, chosen from the Windows display language, switchable in the
  window; every sentence is an id in `installer/text/<en|ru>.json`. A folder with `xc3-install.json` needs only the
  folder. `--selftest [--shots DIR]` builds every screen in both languages and closes itself.
- `tests/installer_gui_test.py`: event model, form, catalogs, cancel, the self-closing window and one end-to-end run
  of the CLI in `--verify` mode (needs `build/install-test` and `build/extracted/v121`; see its docstring).
- `tests/installer_test.py`: bookkeeping with fake steps. `tests/installer_e2e.py`: a dev-mode run with the files of
  `config.local.toml`.

## Release layout

The unpacked release; `tools/make_release.py` assembles it and `--zip` packs it:

```
XC3-Setup.exe        starts python/pythonw.exe -m installer.gui (installer/launcher/launcher.c, no console)
installer/            the installer: steps, CLI, window, en/ru text catalog
python/               Python 3.14.9 embeddable + numpy, lz4, cryptography, cffi, pycparser (+ tkinter for the window)
recomp/, tools/       the lifter and the build, extraction and shader scripts the steps run (copied from the repo)
llvm/                 llvm-mingw, x86_64-only subset (Apache-2.0 with LLVM exception; mingw-w64 parts permissive)
runtime/              libxc3rt.a and the headers the generated code and modules.c/romfs.c/patches.c include
bin/                  xc3_xlate.exe, hactool.exe, SDL3.dll, libopus-0.dll, ninja.exe
bin/upscalers/        AMD FSR 3 (MIT) and NVIDIA Streamline DLLs (MIT) with the DLSS and frame generation models (NVIDIA's
                      SDK licences), fetched and hash-checked by tools/fetch_upscalers.py
data/                 shader_list.tsv (MacNativeGames, MIT)
THIRD_PARTY_NOTICES.md
xc3.ini.example, VERSION  the default ini; the release string (the repository's VERSION plus the commit)
```

- SDL3 comes from the upstream release, not MSYS2: MSYS2's DLL imports the LGPL libiconv.
- Every bundled third-party piece gets a row in `THIRD_PARTY_NOTICES.md`.
- The release contains nothing game-derived; `make_release.py` runs `tools/check_content.py` over the folder before it
  finishes.

## Linux release

`tools/make_release_linux.py` builds the same setup for Linux and packs it as `XC3-Setup-<version>-linux.AppImage` (the static
type-2 runtime: no libfuse2 needed on the host). The player makes it executable and starts it; on a Steam Deck, from
Desktop Mode. Building the release also needs `make` and `pkg-config`. The bundled Tcl/Tk 9.0.4 is rebuilt with
Xft against the sniper sysroot so Russian text uses system TrueType fonts. The setup builds the game into the folder they pick, as on Windows, and the game is `xc3` there with
`libSDL3.so.0` beside it. The folder inside the AppImage:

```
AppRun                starts python/bin/python3 -m installer.gui (no bytecode written into the read-only mount)
xc3-setup.desktop, xc3-setup.png
installer/, recomp/, tools/, data/, licenses/, THIRD_PARTY_NOTICES.md, xc3.ini.example, VERSION   as on Windows
python/               python-build-standalone 3.14 (with tkinter) + the installer's packages; a .pth puts the folder,
                      tools/ and recomp/ on sys.path
llvm/                 LLVM 23 (clang, lld, llvm-ar) and llvm/sysroot: the files of the Steam Runtime 3 "sniper" SDK the
                      game's compile and link read (about 130 files)
runtime/              libxc3rt.a, headers, the install-side sources, xc3.png; deps/ with the Vulkan headers, VMA and a
                      static zstd
bin/                  xc3_xlate, hactool, ninja, libSDL3.so.0
```

- What we compile is built against the sniper sysroot (glibc 2.31): the game, hostcheck, hactool and the translator
  need no newer glibc. The game links Opus, libvpx, zstd and libstdc++ statically and needs only glibc, the system's
  Vulkan loader and the SDL3 beside it. The setup itself needs glibc 2.34 (Ubuntu 22.04, Debian 12, Fedora 35,
  SteamOS 3): the official LLVM binaries and the `cryptography` wheel are built against it. `make_release_linux.py`
  checks every ELF file in the folder against that floor (`check_glibc`), and the setup's check step stops with one
  message on an older system.
- The setup's temp folder is `.setup-temp` inside the install folder (`--temp` changes it): the AppImage is mounted
  read-only, and a Steam Deck's `/home` is often too full for the generated C while the game goes to the SD card.
- The game keeps Mesa's shader cache in `mesa-cache` beside `xc3` unless `MESA_SHADER_CACHE_DIR` is set, for the
  same reason: RADV fills its default folder in `~/.cache` up to 1 GB.
- The window follows `LANG` for English or Russian, cancels through SIGTERM to the CLI's process group (the CLI handles
  it as Ctrl+Break on Windows) and opens the finished folder with `xdg-open`.
- Requirements beyond Windows': Linux 6.7 or later (the write watch, `docs/building.md`) and glibc 2.34.

## Making a release

`.github/workflows/release.yml` builds both packages with no game files. `libxc3rt.a` takes `MOD_COUNT` and
`MOD_MAIN_BASE`, all the runtime reads of the module layout, from `tools/runtime-layout/layout.h`; nothing game-derived
reaches a runner or a package.

Each run builds `XC3-Setup-<version>-windows.zip` on Windows and `XC3-Setup-<version>-linux.AppImage` on Linux, with
`check_content`, the check-only installer pass and the window's `--selftest` inside `tools/make_release.py` and
`tools/make_release_linux.py`. Runners have no GPU, so the host check runs on Mesa's CPU Vulkan driver
(`.github/actions/software-vulkan`). A second runner then unpacks each package and repeats those checks
(`tools/check_release.py`). Downloads are cached from `toolchain/release-dl/`.

For a test build, start the Release workflow on a branch from the Actions tab (or `gh workflow run release.yml --ref
<branch>`). The packages are the run's artifacts.

Every push to `master` that changes more than Markdown or `docs/` (a merged pull request) builds, checks and publishes
a release on its own: `v<VERSION>` when that tag is new, else the next patch number after the highest
`v<major>.<minor>.*` tag. Bump `VERSION` in the pull request for a new minor or major version. These releases skip the
local `--prove` step below.

For a proved release:

1. On the release branch, bump `VERSION` and push.
2. Tag that commit `v<VERSION>` and push the tag. A tag that differs from `VERSION` stops the run. A passing run
   creates a draft release with both packages and generated notes.
3. Prove the packages locally. `--prove` reads `generated/upd`, `build/extracted/upd` and `build/upd/shaders`, which are
   game-derived, so it never runs on a runner. Its shader probes need a census: make one once, from the shaders a local
   2.0.0 build met in play (`build/upd/shader-programs.bin`). Download both packages from the draft, then:

   ```sh
   .venv-py314\Scripts\python tools\shader_census.py --version upd --played
   # Windows, with the zip unpacked to build/release/XC3-Setup
   .venv-py314\Scripts\python tools\make_release.py --out build/release/XC3-Setup --prove
   # Linux or WSL: run the AppImage with --appimage-extract, move squashfs-root to build/release/XC3-Setup-linux
   .venv/bin/python tools/make_release_linux.py --out build/release/XC3-Setup-linux --prove
   ```

4. Install from both packages with real game files and play the build.
5. Publish the draft (a person, on the release page or with `gh release edit v<VERSION> --draft=false --latest`), then
   merge the release branch.

Without Actions, the same two scripts build the packages locally; on Windows run `tools/build_hactool.py` and
`tools/build_xlate.py` first (`tools/make_release.py` copies their output).

## Constraints

- The installer steps contain no Win32 calls: Linux runs the same steps with its own compiler ("Linux release").
- The keys file path is passed to the tools; key values never appear in the UI, logs or files.
- The window is a front end over the CLI (`python -m installer --base ... --update ... --keys ... --lang ... --dest
  ...`), which prints one JSON progress event per line. Tests and CI use the CLI.
- Paths may hold any Unicode characters (a Cyrillic user name puts the temp and install folders under it). Every exe
  the setup runs or builds reads its arguments and paths as UTF-8 through an `activeCodePage` manifest: hactool
  (our patch, [building.md](building.md)), xc3.exe (`runtime/xc3.rc`), ninja (upstream). The Python steps write
  their files and logs as UTF-8, and `build_game.py` quotes paths with spaces on its Ninja command lines.
- A game file whose data fails its hash checks or ends early stops the setup with `check.input_damaged`, which names
  the file and asks to copy it again (`extract.DamagedFile`). A failed merge checks the base Program NCA alone to tell
  which file is damaged.
- Unsigned exe: SmartScreen warns on first start.

## Later

- Re-warm the driver cache on start after a move, GPU or driver change.
- NSZ and XCZ input.
