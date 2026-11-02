"""The game's own icon as an .ico, read from the player's game files at install, for xc3.exe (installer/steps.py
Compile passes it to tools/build_game.py --icon, which builds runtime/xc3.rc with it).

    game_icon.py --base BASE.nsp --update UPDATE.nsp --keys prod.keys --out xc3-game.ico

The icon is icon_AmericanEnglish.dat (a 256x256 JPEG) in the RomFS of the Control NCA, the update's first, else the
base game's. Windows' System.Drawing (PowerShell) scales it to PNG images of 256, 48, 32 and 16 pixels, the images of
the .ico. Changing the resources of a linked xc3.exe instead (UpdateResourceW) drops the COFF string table that its
.debug_* section names need. The repository's own mark (runtime/xc3.ico) stays the icon of XC3-Setup.exe and of the
installer window. Game artwork is only written beside --out. Windows only; a failure exits 1 and writes no .ico.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
from pathlib import Path

VENDOR = Path(__file__).resolve().parent / 'vendor' / 'macnativegames'
sys.path.insert(0, str(VENDOR))   # the vendored scripts import each other by bare module name
from cryptography.hazmat.primitives.ciphers import modes  # noqa: E402
from extract_exefs import (common_tickets, container_entries, decrypt, load_keys, nca_ctr_key,  # noqa: E402
                           nca_inventory, read_at, record_offset, require)

CONTROL = 2   # NCA content type
ICON_NAMES = ('icon_AmericanEnglish.dat', 'icon_BritishEnglish.dat')
SIZES = (256, 48, 32, 16)
ROMFS_MAX = 64 << 20


def control_romfs(path, keys):
    """The RomFS of the Control NCA in path, or None."""
    with path.open('rb') as f:
        _, entries = container_entries(f, path.stat().st_size)
        tickets = common_tickets(f, entries)
        controls = [(r, h) for r, h in nca_inventory(f, entries, keys) if r['content_type'] == CONTROL]
        if not controls:
            return None
        record, hdr = controls[0]
        ctr_key = nca_ctr_key(hdr, keys, tickets)
        start, end = struct.unpack_from('<II', hdr, 0x240)
        fs = hdr[0x400:0x600]
        require(hashlib.sha256(fs).digest() == hdr[0x280:0x2a0], 'FS header hash mismatch')
        require(fs[2:4] == b'\x00\x03' and fs[4] == 3 and fs[8:12] == b'IVFC', 'Control section 0 is no AES-CTR RomFS')
        # IVFC level 6 (the last of six level headers at +0x18, 0x18 bytes each) holds the RomFS itself.
        offset, size = struct.unpack_from('<QQ', fs, 0x18 + 5 * 0x18)
        secstart, secsize = start * 512, (end - start) * 512
        require(offset + size <= secsize and size <= ROMFS_MAX, 'Control RomFS out of bounds')
        absolute = secstart + offset
        aligned = absolute & ~15
        iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
        data = read_at(f, record_offset(record) + aligned, size + absolute - aligned)
        return decrypt(ctr_key, modes.CTR(iv), data)[absolute - aligned:]


def romfs_file(romfs, names):
    """The first of names in the RomFS's file table, or None."""
    (_, _, _, _, _, _, _, meta_off, meta_size, data_off) = struct.unpack_from('<10Q', romfs, 0)
    found = {}
    pos = meta_off
    while pos + 0x20 <= meta_off + meta_size:
        offset, size = struct.unpack_from('<QQ', romfs, pos + 8)
        name_len = struct.unpack_from('<I', romfs, pos + 0x1c)[0]
        name = romfs[pos + 0x20:pos + 0x20 + name_len].decode('utf-8', 'replace')
        found[name] = romfs[data_off + offset:data_off + offset + size]
        pos += 0x20 + ((name_len + 3) & ~3)
    return next((found[n] for n in names if n in found), None)


PS_SCALE = r'''
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile($env:ICON_JPG)
foreach ($s in $env:ICON_SIZES.Split(',')) {
    $n = [int]$s
    $bmp = New-Object System.Drawing.Bitmap $n, $n, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($src, 0, 0, $n, $n)
    $bmp.Save((Join-Path $env:ICON_DIR "icon-$n.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}
$src.Dispose()
'''


def scaled_pngs(jpg, work):
    env = dict(os.environ, ICON_JPG=str(jpg), ICON_DIR=str(work), ICON_SIZES=','.join(map(str, SIZES)))
    subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', PS_SCALE], env=env, check=True,
                   capture_output=True, timeout=120)
    return [(n, (work / f'icon-{n}.png').read_bytes()) for n in SIZES]


def ico_bytes(images):
    """An .ico of PNG images [(size, png)]."""
    head = struct.pack('<HHH', 0, 1, len(images))
    dirs, blobs, at = b'', b'', 6 + 16 * len(images)
    for n, png in images:
        dirs += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(png), at + len(blobs))
        blobs += png
    return head + dirs + blobs


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--update', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    require(os.name == 'nt', 'game_icon.py runs on Windows only')
    keys = load_keys(args.keys)
    jpeg = None
    for source in (args.update, args.base):
        if source and source.is_file():
            romfs = control_romfs(source, keys)
            jpeg = romfs and romfs_file(romfs, ICON_NAMES)
            if jpeg:
                print(f'game_icon: icon of {source.name}, {len(jpeg)} bytes')
                break
    del keys
    require(jpeg and jpeg[:2] == b'\xff\xd8', 'no JPEG icon in the Control NCAs')
    work = args.out.parent
    work.mkdir(parents=True, exist_ok=True)
    jpg = work / 'icon.jpg'
    jpg.write_bytes(jpeg)
    images = scaled_pngs(jpg, work)
    args.out.write_bytes(ico_bytes(images))
    print(f'game_icon: {args.out.name}, {", ".join(str(n) for n, _ in images)} px')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as e:
        print(f'game_icon: {e}')
        sys.exit(1)
