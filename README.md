# XC3 Recomp

**XC3 Recomp** is a static recompilation of *Xenoblade Chronicles 3* (Nintendo Switch, 2022) into a native Windows x86-64 application ([docs/architecture.md](docs/architecture.md)). It is a fork of [XCX Recomp](https://github.com/jmars125/xcx-recomp), itself a fork of [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) by Valiantsin Dzerakh.

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Xenoblade Chronicles 3*, dumped from your own console: the base game and the 2.2.1 update (the add-on content is optional). Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## Status

Playable from New Game through the story, cutscenes and battles on Windows, with sound, text and the menu. 60 fps by
default; 30 to 360 in the settings, with game speed kept right at any rate.

Known issues:

- Some busy areas and cutscenes drop to around 40 fps on any GPU: the game's own CPU work bounds them. Frame generation
  helps there.
- The first visit to a place can stutter briefly while its shaders build; later visits do not.
- Linux and the Steam Deck: the setup builds and installs, but play is not yet tested on a real GPU.

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- Update 2.2.1 and all four add-on content packs, including Future Redeemed
- XC3-Setup: builds the game from your own NSP files and `prod.keys`
- In-game settings menu (F1) and performance overlay (F10)
- Keyboard, mouse and gamepad, with Xbox or PlayStation button prompts
- 60 fps and above (`fps_limit` up to 360 or the display's rate)
- Any game resolution and aspect ratio, including ultrawide (`output_res`)
- Upscaling and anti-aliasing (`upscaler`): NVIDIA DLSS and DLAA on RTX cards, AMD FSR 3 on any GPU, each in place of
  the game's own temporal reconstruction, with `render_scale` setting the share of the resolution the 3D scene draws
  in; FSR 1 for a window larger than the game resolution
- Frame generation (`frame_gen`; takes a restart): NVIDIA DLSS Frame Generation on RTX 40 and 50 cards, AMD FSR 3 frame
  generation on any GPU, a generated frame between every two the game draws. It needs a temporal upscaler (`fsr3`,
  `dlss` or `dlaa`). The frame limit counts the frames on screen, so 120 draws 60 and shows 120; the overlay counts
  the frames on screen too. The game's interface is kept out of the generated frames
- DLSS model choice (`dlss_preset`) and NVIDIA Reflex low latency (`reflex`) with DLSS or DLAA
- HDR10 output (`hdr`; Windows' HDR on): the scene's highlights expanded towards the display's peak, the interface
  kept at its own brightness
- Variable refresh rate (`vrr`): for G-SYNC and FreeSync monitors, each frame shows when it is ready and the frame
  rate stays a little under the refresh rate (116 at 120 Hz), inside the monitor's range
- RomFS mods made for emulators: unpack a mod into a `mods` folder next to `xc3.exe`

Every setting is in the F1 menu and in `xc3.ini` ([xc3.ini.example](xc3.ini.example) lists every key). The setup puts
the NVIDIA (Streamline, DLSS) and AMD (FidelityFX) libraries next to `xc3.exe`.

## Planned

- Linux and the Steam Deck tested on real hardware
- Dynamic resolution (the game's own scaling needs more reverse engineering)

## Installing

Requirements: Windows 10 or later, an x86-64 CPU with AVX2, a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, about 36 GiB free during setup (20 GiB after). DLSS needs an NVIDIA RTX card; DLSS Frame Generation an RTX 40 or 50 card with hardware-accelerated GPU scheduling on.

1. Download `XC3-Setup-<version>-windows.zip` from [Releases](../../releases/latest) and unpack it.
2. Start `XC3-Setup.exe`. If SmartScreen warns about an unsigned app, choose *More info*, then *Run anyway*.
3. Pick your game files, the language and an install folder, and press *Start*. The setup builds the game once.
4. Play `xc3.exe` from the install folder.

To update, run a newer `XC3-Setup.exe` on the same folder; saves and settings are kept. Controls: [docs/controls.md](docs/controls.md).

## Credits

XC3 Recomp builds on the projects below. What was taken from each, with its license, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [XCX Recomp](https://github.com/jmars125/xcx-recomp), the project XC3 Recomp is forked from
- [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) by Valiantsin Dzerakh, the project XC3 Recomp is forked from
- [LAS Recomp](https://github.com/peiikai/LASRecomp)
- [MacNativeGames](https://github.com/ly05010419/MacNativeGames)
- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [nxnative](https://github.com/seedds/nxnative)
- [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)
- [Ryujinx](https://github.com/Ryujinx/Ryujinx)
- [deko3d](https://github.com/devkitPro/deko3d)
- [hactool](https://github.com/SciresM/hactool)
- [SDL 3](https://libsdl.org), [Dear ImGui](https://github.com/ocornut/imgui), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR), [AMD FidelityFX SDK (FSR 3)](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), [NVIDIA Streamline (DLSS)](https://github.com/NVIDIA-RTX/Streamline), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [libopus](https://opus-codec.org), [libvpx](https://chromium.googlesource.com/webm/libvpx), [Zstandard](https://github.com/facebook/zstd)
- The [60FPS mod for XC3 1.0.1](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods) by Fl4sh_ and StevensND
- [Switchbrew](https://switchbrew.org) and the Switch homebrew community

## License

The project's own code is under the [MIT License](LICENSE); third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The license grants nothing for Nintendo's game.

XC3 Recomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo. *Xenoblade Chronicles 3* and *Nintendo Switch* are trademarks of Nintendo; Xenoblade Chronicles 3 is developed by Monolith Soft.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. Everything the build writes stays under `build\` and `generated\`, which git ignores; run `tools/check_content.py` before a commit. Testers: [docs/tester-guide.md](docs/tester-guide.md).

Most of the code and documentation is written by AI coding agents (Claude Code) under the maintainer's direction; the maintainer plays every build and decides what is merged. [AGENTS.md](AGENTS.md) holds the rules they work under.
