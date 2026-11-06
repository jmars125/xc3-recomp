"""Merge the pipelines and shader variants met in play into the lists the setup ships, and install the pipeline list.

A run of xc3.exe records every graphics pipeline it builds in pipeline-keys.bin and every shader variant it translates
in shader-answers.bin, next to its pipeline cache (runtime/vk_pipeline.c "pipeline keys", runtime/gfx_shader.c
"answers met in play"). Neither holds game code: a pipeline record is the Vulkan state with its shaders named by input
id, a variant record a shader's code hash and bind-state answers. Merged from map-jump tours (docs/controls.md) and
played installs, they become

    tools/data/pipeline-keys-xc3-2.2.1.bin     the pipelines, built at first start by the boot prewarm
    tools/data/shader-variants-xc3-2.2.1.bin   the variants, translated at install by tools/prepare_shaders.py

so a new install meets the first visit of a region with its shaders translated and its pipelines built.

    pipeline_list.py merge [--keys <pipeline-keys.bin>...] [--answers <shader-answers.bin>...]
    pipeline_list.py install <build directory>

`merge` adds the records of the given files that the lists do not have yet and prints the counts. `install` (run by
prepare_shaders.py after the preparation) appends the shipped pipeline records missing from the build's
pipeline-keys.bin. Input ids depend on the translator's use masks: a record whose shaders a newer translator names
otherwise is dropped by the prewarm ("dropped (shader neither cached nor stored)" in the log), and a new tour refreshes
the list. Stdlib only.
"""
import argparse
import struct
import sys
from pathlib import Path

DATA = Path(__file__).resolve().parent / 'data'
KEYS = DATA / 'pipeline-keys-xc3-2.2.1.bin'
VARIANTS = DATA / 'shader-variants-xc3-2.2.1.bin'
KEYS_MAGIC, KEYS_VERSION, KEY_REC = b'XC3PKEY\0', 3, 432        # vk_pipeline.c KeysHeader, KEYS_VERSION, KeyRec
ANS_MAGIC, ANS_VERSION, ANS_REC = b'XC3SANS\0', 1, 4 + 32 + 32 + 147 + 1   # gfx_shader.c RecHeader, ANS_VERSION, ANS_REC


def read(path, magic, version, size):
    """The records of a record file, [] when it is missing or has another header; a torn tail is cut off."""
    data = Path(path).read_bytes() if Path(path).is_file() else b''
    if len(data) < 16 or data[:8] != magic or struct.unpack_from('<II', data, 8) != (version, size):
        return []
    return [data[i:i + size] for i in range(16, len(data) - size + 1, size)]


def write(path, magic, version, size, recs):
    tmp = Path(str(path) + '.tmp')
    tmp.write_bytes(magic + struct.pack('<II', version, size) + b''.join(recs))
    tmp.replace(path)


def answer_id(r):
    """A variant by stage, code hash and answers: the input id (bytes 36..68) is the translator's, the last byte the
    priority, and neither makes another variant."""
    return r[:36] + r[68:215]


def merge(paths, target, magic, version, size, ident):
    recs = read(target, magic, version, size)
    seen = {ident(r) for r in recs}
    added = 0
    for p in paths:
        new = read(p, magic, version, size)
        if not new and Path(p).is_file():
            print(f'{p}: not a {magic[:7].decode()} file of version {version}', file=sys.stderr)
        for r in new:
            if ident(r) not in seen:
                seen.add(ident(r))
                recs.append(r)
                added += 1
    write(target, magic, version, size, recs)
    print(f'{target.name}: {len(recs)} records, {added} added')


def install(build):
    shipped = read(KEYS, KEYS_MAGIC, KEYS_VERSION, KEY_REC)
    if not shipped:
        return 0
    dest = Path(build) / 'pipeline-keys.bin'
    have = read(dest, KEYS_MAGIC, KEYS_VERSION, KEY_REC)
    seen = set(have)
    missing = [r for r in shipped if r not in seen]
    if missing:
        write(dest, KEYS_MAGIC, KEYS_VERSION, KEY_REC, have + missing)
    print(f'pipeline list: {len(missing)} of {len(shipped)} shipped pipelines added to {dest.name} '
          f'({len(have) + len(missing)} records)', flush=True)
    return len(missing)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge', help='add recorded pipelines and variants to the shipped lists')
    m.add_argument('--keys', type=Path, nargs='*', default=[], help='pipeline-keys.bin files')
    m.add_argument('--answers', type=Path, nargs='*', default=[], help='shader-answers.bin files')
    i = sub.add_parser('install', help="append the shipped pipelines to a build's pipeline-keys.bin")
    i.add_argument('build', type=Path, help='build directory holding xc3.exe')
    a = ap.parse_args()
    if a.cmd == 'install':
        install(a.build)
        return 0
    if a.keys:
        merge(a.keys, KEYS, KEYS_MAGIC, KEYS_VERSION, KEY_REC, lambda r: r)
    if a.answers:
        merge(a.answers, VARIANTS, ANS_MAGIC, ANS_VERSION, ANS_REC, answer_id)
    return 0


if __name__ == '__main__':
    sys.exit(main())
