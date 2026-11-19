# Mods

XC3 Recomp loads the RomFS part of mods made for Ryujinx and yuzu. This page covers installing them, mods built for
the Skyline plugin xc3-file-loader (most text, font and model mods), and what does not carry over.

## Installing a mod

Unpack each mod into the `mods` folder next to `xc3.exe`, the way an emulator expects it:

```
mods\<mod name>\romfs\...
mods\contents\010074F013262000\<mod name>\romfs\...     (also accepted)
```

Only the `romfs` folder of a mod is used. Do not copy its files into the game's own `romfs` folder (the one with
`romfs.bin`) or into `exefs`: the game reads neither as a mod, and a copied `exefs\main.npdm` replaces the game's.

`mods` in `xc3.ini` names another folder; `mods = 0` turns mods off.

## Mods made for xc3-file-loader

Almost every game file sits inside the archives `bf3.arh`/`bf3.ard` (the RomFS) and `bf3_dlc0N.arh`/`bf3_dlc0N.ard`
(each add-on's folder under `aoc`). On a Switch emulator a mod replaces such a file with a loose copy under `romfs`,
for example `romfs\bdat\gb\evt\ev01\msg_ev01010100.bdat`, and needs the Skyline plugin xc3-file-loader
(`exefs\subsdk9`, `exefs\main.npdm`, `romfs\skyline\plugins\libxc3_file_loader.nro`) to make the game read it.

This build does the plugin's work itself, so install such a mod as above and leave its Skyline files where they
are. `exefs` and `romfs\skyline` are skipped (the log says so). Every mod file whose path matches an archive entry
replaces that entry, in the base game's archive and in the add-ons' alike, and a file bigger than the original works:
the game reads the loose file with its own size, as it does with the plugin.

How it works: the game looks a path up in its archives first (`ml::DevFileArchiver::getFileInfo`) and only on a miss
opens the loose file, in `aoc4:` to `aoc1:` and then `rom:`. The patch set `modfiles` (recomp/patches-upd.txt,
runtime/mod_archive.c) makes the lookup miss for every path a mod adds to the RomFS, the way the plugin does on a
console or an emulator. It is part of the generated code: an install made before it needs the lift and compile steps
again.

The F1 menu's Game tab has the switch ("Mods replace archive files", after a restart), as does `mod_archive` in
`xc3.ini`. Off, the game keeps its archive entries and mods replace only the plain RomFS files (movies and sound
banks).

## Language packs

A translation into a language XC3 does not ship, such as Brazilian Portuguese, replaces the text of one it does
(`romfs\bdat\gb\` for English). Installed as a mod it covers English for good. Installed as a language pack it is a
language of its own, next to the game's:

```
languages\pt-BR\romfs\...        (the translation mod's folder, as it comes; exefs and skyline are skipped)
```

Pick it as the game language: Portuguese (Brasil) in the F1 menu's Game tab (it is listed when the pack is there), or
`lang = pt-BR` in `xc3.ini`. With Windows set to Brazilian Portuguese the automatic choice picks it too. The game then
runs in the language the pack replaces (English for `bdat\gb\`), with that language's voices and fonts, and reads the
pack's files in its place; any other language plays without it. A mod that replaces one of the pack's files wins over
the pack, so fixes to a translation can stay a separate mod. The log names the pack:

```
oe: language pack pt-BR (...\languages/pt-BR): the game runs in en-GB, whose text the pack replaces
mods: language pack pt-BR: 4120 files from ...\languages/pt-BR
```

## Which mod wins

Mods apply in the order of their folder paths. When two mods have the same file, the first by folder name wins and
the log names the file the second one loses. A file name differing only in upper and lower case is the same file: the
game asks for lower-case paths, so a mod's files are matched in lower case.

## Text and language

The game reads the text of the language it runs in: `bdat\gb\` for English (`locale = en-US` or `en-GB`), `bdat\fr\`,
`bdat\ge\`, `bdat\it\`, `bdat\sp\`, `bdat\jp\`, `bdat\kr\`, `bdat\cn\`, `bdat\tw\`. A translation mod replaces one of
these folders, so set the language whose folder it replaces. A translation that replaces `bdat\gb\` (the Brazilian
Portuguese one, for example) needs English.

## The log

At the start the log lists each mod and the archive entries the mods replace:

```
mods: mods/PTBR/exefs skipped: it patches the Switch code, which this build replaces
mods: mods/PTBR/romfs/skyline skipped: Skyline plugins patch the Switch code, which this build replaces
mods: mods/PTBR/romfs: 4120 files
mods: rom:/bf3.arh: 3556 entries replaced
mods: aoc4:/bf3_dlc04.arh: 515 entries replaced
mods: archive overlay: 3556 entries replaced in bf3.arh, 598 in add-on archives, 0 loose files kept, 0 archives unreadable, 0.05 s
```

A path in both the base archive and an add-on's counts in each. "Loose files kept" are mod files with no archive
entry: they stay plain RomFS files, which the game reads when it asks for that path. With `trace = fs` the log has a
line for every replaced entry, with the original's size and the mod file's.

## What does not carry over

- `exefs` mods (code patches, Skyline plugins) and `cheats`: this build runs translated code, not the Switch's. The
  frame-rate, resolution and other fixes such mods make are settings in `xc3.ini`.
- Mods for another game version whose paths 2.2.1 lacks: those files stay plain RomFS files that the game does not ask
  for.
