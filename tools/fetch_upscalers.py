"""Fetch the upscaler libraries xc3 loads at run time into a build folder: fetch_upscalers.py build/upd.

Windows (--windows, default on Windows or when --streamline-zip is passed):
- AMD's signed amd_fidelityfx_vk.dll (FidelityFX SDK 1.1.4, FSR 3.1, MIT) for upscaler = fsr3 (runtime/vk_tup.c) and
  frame_gen = fsr (runtime/vk_fg.cpp).
- NVIDIA's signed Streamline 2.14.16 DLLs for upscaler = dlss or dlaa and frame_gen = dlss (runtime/vk_sl.cpp):
  sl.interposer.dll, sl.common.dll, sl.dlss.dll, sl.dlss_g.dll, sl.reflex.dll and sl.pcl.dll (MIT),
  NvLowLatencyVk.dll, which sl.common.dll loads, and the DLSS and frame generation models nvngx_dlss.dll and
  nvngx_dlssg.dll (under NVIDIA's SDK licences, which let an application ship them). They come out of NVIDIA's release
  zip (276 MB); --no-dlss skips it.

Linux (--linux, default on Linux when neither --windows nor --streamline-zip is passed):
- AMD's signed FidelityFX SDK 2.4.0 Vulkan DLLs (MIT): amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll.
- ffx-vk-linux (MIT, https://github.com/DuIslingr/ffx-vk-linux): a small native Linux PE-loader bridge built via CMake
  into libffx_vk_linux.so so xc3 can load AMD's official Vulkan upscaler DLLs natively on x86-64 Linux without Wine.

Every download is pinned by its SHA-256 and lands next to xc3 / xc3.exe. Stdlib only (plus cmake on Linux).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

FFX = ('amd_fidelityfx_vk.dll',
       'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/PrebuiltSignedDLL/amd_fidelityfx_vk.dll',
       'a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78')
FFX_LINUX_DLLS = [
    ('amd_fidelityfx_loader_vk.dll',
     'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll',
     '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9'),
    ('amd_fidelityfx_upscaler_vk.dll',
     'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll',
     '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'),
]
FFX_VK_LINUX_COMMIT = 'e1d5a415ed78dd5db7c6d00258d06acccb424307'
FFX_VK_LINUX = (
    f'https://github.com/DuIslingr/ffx-vk-linux/archive/{FFX_VK_LINUX_COMMIT}.tar.gz',
    '99d52fc6e29edcdc7774e71c653de2155c0c2955454b52f83612ad4903eb7b06',
)
VULKAN_HEADERS_VER = '1.4.363'
VULKAN_HEADERS = (
    f'https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v{VULKAN_HEADERS_VER}.tar.gz',
    'cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68',
)
SPIRV_HEADERS_TAG = 'vulkan-sdk-1.4.363.0'
SPIRV_HEADERS = (
    f'https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/{SPIRV_HEADERS_TAG}.tar.gz',
    '5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9',
)
SL_ZIP = ('https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip',
          '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b')
SL_FILES = ['sl.interposer.dll', 'sl.common.dll', 'NvLowLatencyVk.dll', 'sl.dlss.dll', 'nvngx_dlss.dll',
            'sl.dlss_g.dll', 'nvngx_dlssg.dll', 'sl.reflex.dll', 'sl.pcl.dll']   # in bin/x64 of the zip


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url):
    with urllib.request.urlopen(url, timeout=600) as r:
        return r.read()


def fetch_pinned(dest, url, want):
    if dest.exists() and sha256(dest.read_bytes()) == want:
        print(f'{dest}: up to date')
        return
    data = fetch(url)
    got = sha256(data)
    if got != want:
        sys.exit(f'fetch_upscalers: {url} has SHA-256 {got}, expected {want}; nothing written')
    dest.write_bytes(data)
    print(f'{dest}: {len(data)} bytes')


def fetch_ffx(exe_dir):
    name, url, want = FFX
    fetch_pinned(exe_dir / name, url, want)


def fetch_ffx_linux(exe_dir, rebuild=False):
    for name, url, want in FFX_LINUX_DLLS:
        fetch_pinned(exe_dir / name, url, want)
    so_dest = exe_dir / 'libffx_vk_linux.so'
    if so_dest.exists() and not rebuild:
        print(f'{so_dest}: up to date')
        return
    if shutil.which('cmake') is None:
        sys.exit('fetch_upscalers: cmake is required to build libffx_vk_linux.so on Linux')
    url, want = FFX_VK_LINUX
    data = fetch(url)
    got = sha256(data)
    if got != want:
        sys.exit(f'fetch_upscalers: {url} has SHA-256 {got}, expected {want}; nothing written')
    with tempfile.TemporaryDirectory(prefix='ffx-vk-linux-') as tmp:
        tmp_path = Path(tmp)
        tgz = tmp_path / 'ffx-vk-linux.tar.gz'
        tgz.write_bytes(data)
        with tarfile.open(tgz, 'r:gz') as tf:
            tf.extractall(tmp_path, filter='data')
        src = tmp_path / f'ffx-vk-linux-{FFX_VK_LINUX_COMMIT}'
        bld = tmp_path / 'build'
        cmake_args = ['cmake', '-S', str(src), '-B', str(bld), '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF']
        if not (Path('/usr/include/vulkan/vulkan.h').is_file() and Path('/usr/share/vulkan/registry/vk.xml').is_file()):
            vh_url, vh_want = VULKAN_HEADERS
            vh_data = fetch(vh_url)
            vh_got = sha256(vh_data)
            if vh_got != vh_want:
                sys.exit(f'fetch_upscalers: {vh_url} has SHA-256 {vh_got}, expected {vh_want}; nothing written')
            vh_tgz = tmp_path / 'vulkan-headers.tar.gz'
            vh_tgz.write_bytes(vh_data)
            with tarfile.open(vh_tgz, 'r:gz') as tf:
                tf.extractall(tmp_path, filter='data')
            vh_root = tmp_path / f'Vulkan-Headers-{VULKAN_HEADERS_VER}'
            cmake_args.extend([f'-DVulkan_INCLUDE_DIR={vh_root / "include"}',
                               f'-DVK_XML={vh_root / "registry" / "vk.xml"}'])
        if not Path('/usr/include/spirv/unified1/spirv.hpp11').is_file():
            sh_url, sh_want = SPIRV_HEADERS
            sh_data = fetch(sh_url)
            sh_got = sha256(sh_data)
            if sh_got != sh_want:
                sys.exit(f'fetch_upscalers: {sh_url} has SHA-256 {sh_got}, expected {sh_want}; nothing written')
            sh_tgz = tmp_path / 'spirv-headers.tar.gz'
            sh_tgz.write_bytes(sh_data)
            with tarfile.open(sh_tgz, 'r:gz') as tf:
                tf.extractall(tmp_path, filter='data')
            sh_root = tmp_path / f'SPIRV-Headers-{SPIRV_HEADERS_TAG}'
            cmake_args.append(f'-DCMAKE_CXX_FLAGS=-I{sh_root / "include"}')
        try:
            subprocess.run(cmake_args, check=True)
            subprocess.run(['cmake', '--build', str(bld), '--target', 'ffx_vk_linux',
                            f'-j{os.cpu_count() or 4}'], check=True)
        except subprocess.CalledProcessError as e:
            sys.exit(f'fetch_upscalers: building libffx_vk_linux.so failed ({e}); install CMake, C++20 compiler, '
                     'OpenSSL, libdrm/libdrm_amdgpu and SPIRV-Tools')
        built = bld / 'libffx_vk_linux.so'
        if not built.is_file():
            sys.exit(f'fetch_upscalers: {built} was not produced by CMake')
        shutil.copy2(built, so_dest)
        print(f'{so_dest}: {so_dest.stat().st_size} bytes')


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
    ap.add_argument('exe_dir', type=Path, help='folder of xc3 / xc3.exe')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--linux', action='store_true', help='fetch AMD SDK 2.4.0 DLLs and build libffx_vk_linux.so')
    mode.add_argument('--windows', action='store_true', help='fetch Windows FSR 3 and Streamline DLLs')
    ap.add_argument('--no-dlss', action='store_true', help='Windows: only the FSR 3 library')
    ap.add_argument('--rebuild', action='store_true', help='Linux: rebuild libffx_vk_linux.so even if present')
    ap.add_argument('--streamline-zip', type=Path, help='Windows: an already downloaded streamline-sdk-v2.14.16.zip')
    a = ap.parse_args()
    if not a.exe_dir.is_dir():
        sys.exit(f'fetch_upscalers: {a.exe_dir} is not a folder')
    use_linux = a.linux or (sys.platform == 'linux' and not a.windows and a.streamline_zip is None)
    if use_linux:
        fetch_ffx_linux(a.exe_dir, rebuild=a.rebuild)
        return
    fetch_ffx(a.exe_dir)
    if not a.no_dlss:
        fetch_streamline(a.exe_dir, a.streamline_zip)


if __name__ == '__main__':
    main()
