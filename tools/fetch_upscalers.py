"""Fetch the upscaler libraries xc3.exe loads at run time into a build folder: fetch_upscalers.py build/upd.

- AMD's signed amd_fidelityfx_vk.dll (FidelityFX SDK 1.1.4, FSR 3.1, MIT) for upscaler = fsr3 (runtime/vk_tup.c) and
  frame_gen = fsr (runtime/vk_fg.c).
- NVIDIA's signed Streamline 2.14.16 DLLs for upscaler = dlss or dlaa and frame_gen = dlss (runtime/vk_sl.cpp):
  sl.interposer.dll, sl.common.dll, sl.dlss.dll, sl.dlss_g.dll, sl.reflex.dll and sl.pcl.dll (MIT),
  NvLowLatencyVk.dll, which sl.common.dll loads, and the DLSS and frame generation models nvngx_dlss.dll and
  nvngx_dlssg.dll (under NVIDIA's SDK licences, which let an application ship them). They come out of NVIDIA's release
  zip (276 MB); --no-dlss skips it.

Every file is pinned by its SHA-256 (the zip by its own) and lands next to xc3.exe. Stdlib only.
"""
import argparse
import hashlib
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

FFX = ('amd_fidelityfx_vk.dll',
       'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/PrebuiltSignedDLL/amd_fidelityfx_vk.dll',
       'a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78')
SL_ZIP = ('https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip',
          '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b')
SL_FILES = ['sl.interposer.dll', 'sl.common.dll', 'NvLowLatencyVk.dll', 'sl.dlss.dll', 'nvngx_dlss.dll',
            'sl.dlss_g.dll', 'nvngx_dlssg.dll', 'sl.reflex.dll', 'sl.pcl.dll']   # in bin/x64 of the zip


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url):
    with urllib.request.urlopen(url, timeout=600) as r:
        return r.read()


def fetch_ffx(exe_dir):
    name, url, want = FFX
    dest = exe_dir / name
    if dest.exists() and sha256(dest.read_bytes()) == want:
        print(f'{dest}: up to date')
        return
    data = fetch(url)
    got = sha256(data)
    if got != want:
        sys.exit(f'fetch_upscalers: {url} has SHA-256 {got}, expected {want}; nothing written')
    dest.write_bytes(data)
    print(f'{dest}: {len(data)} bytes')


def fetch_streamline(exe_dir, zip_path):
    url, want = SL_ZIP
    with tempfile.TemporaryDirectory() as tmp:
        if zip_path is None:
            zip_path = Path(tmp) / 'streamline.zip'
            print(f'downloading {url} (276 MB)')
            zip_path.write_bytes(fetch(url))
        got = sha256(zip_path.read_bytes())
        if got != want:
            sys.exit(f'fetch_upscalers: {zip_path} has SHA-256 {got}, expected {want}; nothing written')
        with zipfile.ZipFile(zip_path) as z:
            names = {n.replace('\\', '/'): n for n in z.namelist()}
            for f in SL_FILES:
                member = names.get(f'bin/x64/{f}')
                if member is None:
                    sys.exit(f'fetch_upscalers: bin/x64/{f} is not in the Streamline zip')
                data = z.read(member)
                (exe_dir / f).write_bytes(data)
                print(f'{exe_dir / f}: {len(data)} bytes')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('exe_dir', type=Path, help='folder of xc3.exe')
    ap.add_argument('--no-dlss', action='store_true', help='only the FSR 3 library')
    ap.add_argument('--streamline-zip', type=Path, help='an already downloaded streamline-sdk-v2.14.16.zip')
    a = ap.parse_args()
    if not a.exe_dir.is_dir():
        sys.exit(f'fetch_upscalers: {a.exe_dir} is not a folder')
    fetch_ffx(a.exe_dir)
    if not a.no_dlss:
        fetch_streamline(a.exe_dir, a.streamline_zip)


if __name__ == '__main__':
    main()
