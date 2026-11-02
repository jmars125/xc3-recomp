r"""Extract Xenoblade Chronicles 3: ExeFS of the base game and the update, the update's merged RomFS, and the RomFS of
each add-on content file.

    .venv-py314Scriptspython toolsextract_xc3.py --base <base.nsp> --update <update.nsp> --keys <prod.keys>
        [--dlc <dlc.nsp> ...] [--hactool <hactool.exe>] [--work build]

Writes build/extracted/v0/exefs, build/extracted/upd/{exefs,romfs} and build/extracted/upd/aoc/<index>, the layout
runtime/fs_host.c mount_aoc reads. Key values are read by the tools from the keys file and never printed.
"""
import argparse
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import extract  # noqa: E402

APP_ID = 0x010074F013262000
AOC_BASE = APP_ID + 0x1000
AOC_DATA = 5   # NCA content type of add-on content (PublicData)


def extract_dlc(path, keys_path, hactool, out, logs):
    """The add-on index of the NSP at path; its RomFS goes to out/<index>. Titlekeys come from the NSP's own ticket."""
    keys = extract.load_keys(keys_path)
    with path.open('rb') as f:
        _, entries = extract.container_entries(f, path.stat().st_size)
        tickets = extract.common_tickets(f, entries)
        records = extract.nca_inventory(f, entries, keys)
    picked = [r for r, _ in records if r['content_type'] == AOC_DATA and 0 < int(r['title_id'], 16) - AOC_BASE < 2000]
    extract.require(len(picked) == 1, f'{path.name}: expected one add-on content NCA, found {len(picked)}')
    record = picked[0]
    index = int(record['title_id'], 16) - AOC_BASE
    target = out / str(index)
    if target.is_dir():
        extract.say(f'  add-on content {index} already extracted')
        return index
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dlc-', dir=out) as tmp:
        tmp = Path(tmp)
        nca, title_keys, hactool_keys = tmp / 'dlc.nca', tmp / 'title.keys', tmp / 'hactool.keys'
        extract.copy_range(path, extract.record_offset(record), record['size'], nca)
        fd = os.open(title_keys, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            for rights, key in tickets.items():
                f.write(f'{rights.hex()} = {key.hex()}\n')
        extract.hactool_prod_keys(keys_path, hactool_keys)
        result = tmp / 'romfs'
        code, log = extract.run_hactool([str(hactool), '-y', '--disablekeywarns', '--suppresskeys', '-k',
                                         str(hactool_keys), f'--romfsdir={result}', str(nca)],
                                        dict(os.environ, MNG_TITLE_KEYS=str(title_keys)))
        (logs / f'dlc-{index}.log').write_text(log, encoding='utf-8')
        extract.require(code == 0 and result.is_dir(), f'{path.name}: hactool failed; see {logs / f"dlc-{index}.log"}')
        result.rename(target)
    extract.say(f'  {path.name} is add-on content {index} ({record["title_id"]})')
    return index


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--update', type=Path, required=True)
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--dlc', type=Path, nargs='*', default=[])
    ap.add_argument('--hactool', type=Path, default=extract.ROOT / 'toolchain' / 'hactool' / 'hactool.exe')
    ap.add_argument('--work', type=Path, default=extract.ROOT / 'build')
    args = ap.parse_args()
    work = args.work.resolve()
    out = work / 'extracted' / 'upd'
    logs = work / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    if not (work / 'extracted' / 'v0' / 'exefs' / 'main').is_file():
        extract.say('ExeFS of the base game')
        extract.extract_exefs(args.base, args.keys, work / 'extracted' / 'v0', logs, update=False)
    if not (out / 'exefs' / 'main').is_file():
        extract.say('ExeFS of the update')
        extract.extract_exefs(args.update, args.keys, out, logs, update=True)
    for path in args.dlc:
        extract_dlc(path, args.keys, args.hactool.resolve(), out / 'aoc', logs)
    if not (out / 'romfs' / 'index.json').is_file():
        extract.say('RomFS: base merged with the update')
        extract.merge_update_romfs(args.base, args.update, args.keys, args.hactool.resolve(), out / 'romfs', logs)
    extract.say(f'done: {out}')


if __name__ == '__main__':
    main()
