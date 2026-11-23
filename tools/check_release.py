"""Check a packed release as a player receives it, on a machine that did not build it.

    python tools/check_release.py XC3-Setup-<version>-windows.zip
    python tools/check_release.py XC3-Setup-<version>-linux.AppImage
    python tools/check_release.py XC3-Setup-<version>-linux.tar.gz

Unpacks the package into build/release-verify, then runs the checks of tools/make_release.py and
tools/make_release_linux.py over the result: check_content over every file (and the glibc floor on Linux), then the
check-only installer pass and the window's --selftest with the package's own python. The tarball also runs the window's
--selftest through its AppRun, as a launcher that unpacks it would. No game files are involved. On Linux the window needs a display (xvfb-run). The host check needs a Vulkan 1.3 device on both, as on a player's machine.
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_release as mr  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('package', type=Path, help='the .zip, .AppImage or .tar.gz')
    package = ap.parse_args().package.resolve()
    work = mr.MAIN_ROOT / 'build' / 'release-verify'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    if package.suffix == '.zip':
        with zipfile.ZipFile(package) as z:
            z.extractall(work)
        out = work / 'XC3-Setup'
        mr.check_folder(out)
        mr.check_bundle(out)
    elif package.name.endswith('.tar.gz'):
        import tarfile
        import make_release_linux as mrl
        with tarfile.open(package) as t:
            t.extractall(work, filter='tar')   # keeps the relative symlinks and the exec bits
        out = work / 'XC3-Setup-linux'
        mr.check_folder(out)
        mrl.check_glibc(out)
        mrl.check_bundle(out)
        r = subprocess.run([str(out / 'AppRun'), '--selftest'], cwd='/', capture_output=True, text=True, timeout=300)
        if r.returncode:
            sys.exit(f'AppRun --selftest exit {r.returncode}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
        print('tarball check: AppRun started the window from the unpacked folder and closed')
    else:
        import make_release_linux as mrl
        package.chmod(0o755)
        subprocess.run([str(package), '--appimage-extract'], cwd=work, check=True, stdout=subprocess.DEVNULL,
                       env=dict(os.environ, APPIMAGE_EXTRACT_AND_RUN='1'))
        out = work / 'squashfs-root'
        mr.check_folder(out)
        mrl.check_glibc(out)
        mrl.check_bundle(out)


if __name__ == '__main__':
    main()
