"""Every Maxwell shader of XC3: in the .ard archives (bf3.ard and each add-on's bf3_dlc0N.ard) and the exefs main.

An .ard is a run of stored files, most of them "xbc1" containers: magic, compression (1 zlib, 3 zstd), decompressed
size, compressed size, hash, a 28-byte name, then the data. The .arh index is not needed: scanning for the magic finds
every container in seconds, and a false match fails the checks on the header or the decompression. Effect packs nest
more streams inside the decompressed data: xbc1 again, and "cmp\\0zlib" (u32 compressed size, u32 size, zlib data).

Shaders come in three wrappers:

    HCPS   alone in a .wishp or embedded in models and effects. Header words used (index of u32, offsets from the
           container): 8 code offset, 9 code size, 10 control offset, 11 control size.
    SLCT   standalone in main, the boot shaders: an HCPS's selector without the HCPS. Words 10 to 13, offsets from the
           SLCT: control offset, control size, code offset, code size.
    GLSLC  whole compiler outputs in the effect packs (glslc_shaders has the layout).

In HCPS and SLCT the control region is a run of NVN control sections, each padded to 8 bytes, its own length at +0x20
(0x880, shorter when it carries text). The code region holds each section's code in the same order, so the sections'
lengths (control + 0x704) walk both. A blob that does not start with one of CODE_MAGICS ends the walk of that container
and is counted.

Needs Python 3.14 (compression.zstd). As a script it prints the census of a RomFS image and its add-on content:

    .venv-py314\\Scripts\\python tools\\ard_shaders.py build\\extracted\\upd
"""
import compression.zstd as zstd
import bisect
import concurrent.futures
import hashlib
import json
import os
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
from nso import Nso  # noqa: E402

CONTROL = 0x880
CONTROL_MAGIC = b'\x34\x12\x76\x98'
CODE_MAGICS = (bytes.fromhex('78563412'), bytes.fromhex('79563412'))   # graphics, compute
HCPS = b'HCPS'
SLCT = b'SLCT'
GLSLC = bytes.fromhex('91688619')
XBC1 = b'xbc1'
CMP = b'cmp\0zlib'
RAW_MARKS = (HCPS, GLSLC, CMP)                      # what a stored file can hold outside a compressed container
SPH_STAGES = {1: 'vs', 4: 'gs', 5: 'ps'}            # shader type in SPH word 0; tessellation has no prepared form
SCAN_CHUNK = 256 << 20
# Containers without shaders, by name: textures and animation, more than half of the data.
SKIP = ('.witx', '.witex', '.mot', '.bdat')
BATCH = 256   # entries a worker takes at a time, spread over the archive so a batch's data is about the average


def stage_of(code):
    """vs, gs, ps or cs from the code's header, None for the others: graphics code opens with 0x30 bytes before the SPH."""
    if code[:4] == CODE_MAGICS[1]:
        return 'cs'
    if code[:4] != CODE_MAGICS[0] or len(code) < 0x34:
        return None
    return SPH_STAGES.get(struct.unpack_from('<I', code, 0x30)[0] >> 10 & 0xf)


def shader(data, c, code):
    """(key, stage, control, code). The control is 0x880 bytes whatever its own length: what follows it is what the runtime
    reads and hashes (gfx_shader.c build_code_hash)."""
    control = data[c:c + CONTROL].ljust(CONTROL, b'\0')
    return hashlib.sha256(control + code).hexdigest(), stage_of(code), control, code


def walk(data, c, control_end, pos, code_end, damaged):
    """The shaders of a control region and its code region. damaged[0] counts walks ended by a bad blob."""
    while c < control_end:
        if c + 0x708 > control_end:
            damaged[0] += 1
            return
        size = struct.unpack_from('<I', data, c + 0x20)[0]
        c1, length = struct.unpack_from('<II', data, c + 0x700)
        if data[c:c + 4] != CONTROL_MAGIC or not 0x760 <= size <= CONTROL or c + size > control_end or not 0 < length \
                or pos + length > code_end or data[pos:pos + 4] not in CODE_MAGICS or c1 > length:
            damaged[0] += 1
            return
        found = shader(data, c, data[pos:pos + length])
        if found[1]:
            yield found
        c += (size + 7) & ~7
        pos += length if data[pos + length:pos + length + 4] in CODE_MAGICS else (length + 0xff) & ~0xff


def hcps_shaders(data, at, damaged):
    words = struct.unpack_from('<12I', data, at)
    code_at, code_size, control_at, control_size = at + words[8], words[9], at + words[10], words[11]
    if code_at + code_size <= len(data) and control_at + control_size <= len(data):
        yield from walk(data, control_at, control_at + control_size, code_at, code_at + code_size, damaged)


def slct_shaders(data, at, damaged):
    if at + 72 > len(data):
        return
    c, control_size, k, code_size = struct.unpack_from('<4I', data, at + 40)
    if min(c, k) >= 72 and at + max(c + control_size, k + code_size) <= len(data):
        yield from walk(data, at + c, at + c + control_size, at + k, at + k + code_size, damaged)


def glslc_shaders(data, at):
    """The shaders of a GLSLC output. Output +0x44 is its whole length, +0x4c its section count; section i's header is at
    +0x90 + 0x90 i: size, offset from the output, type (0 GPU code), and for GPU code at +0x30 the control offset, code
    offset and code length, from the section's data."""
    if at + 0x90 > len(data):
        return
    size, _, count = struct.unpack_from('<3I', data, at + 0x44)
    if not 0 < count <= 16 or not 0x90 * (count + 1) <= size <= len(data) - at:
        return
    for head in range(at + 0x90, at + 0x90 + 0x90 * count, 0x90):
        section_size, offset, kind = struct.unpack_from('<3I', data, head)
        control_at, code_at, code_size = struct.unpack_from('<3I', data, head + 0x30)
        if kind or offset + section_size > size or max(control_at + 0x708, code_at + code_size) > section_size:
            continue
        c, code = at + offset + control_at, at + offset + code_at
        found = shader(data, c, data[code:code + code_size])
        if data[c:c + 4] == CONTROL_MAGIC and found[1]:
            yield found


def inflated(data, at):
    """The bytes of the compressed stream at `at`, None for a stored stream or bytes that only look like a header."""
    try:
        if data[at:at + 4] == XBC1:
            kind, size, packed = struct.unpack_from('<3I', data, at + 4)
            if kind not in (1, 3) or not packed or at + 48 + packed > len(data):
                return None
            body = data[at + 48:at + 48 + packed]
            return zstd.decompress(body) if kind == 3 else zlib.decompress(body)
        packed, size = struct.unpack_from('<II', data, at + 8)
        return zlib.decompress(data[at + 16:at + 16 + packed]) if packed and at + 16 + packed <= len(data) else None
    except (struct.error, zstd.ZstdError, zlib.error):
        return None


def marks(data, magic):
    at = -1
    while (at := data.find(magic, at + 1)) >= 0:
        yield at


def data_shaders(data, damaged):
    """The shaders of one decompressed file, the streams nested in it included. A stored xbc1 needs no step of its own:
    its contents are in `data` as they are."""
    for magic in (XBC1, CMP):
        for at in marks(data, magic):
            inner = inflated(data, at)
            if inner:
                yield from data_shaders(inner, damaged)
    for at in marks(data, HCPS):
        if at + 48 <= len(data):
            yield from hcps_shaders(data, at, damaged)
    for at in marks(data, GLSLC):
        yield from glslc_shaders(data, at)


def scan(path, offset, size):
    """Entries of an archive stored at offset, (offset, compression, decompressed, compressed, name), and the shader
    markers found outside them, each of which runs to the next container: its size is not known."""
    entries, raw = [], []
    with open(path, 'rb') as f:
        for pos in range(0, size, SCAN_CHUNK):
            f.seek(offset + pos)
            chunk = f.read(min(SCAN_CHUNK, size - pos) + 48)
            limit = min(SCAN_CHUNK, len(chunk) - 48)
            at = 0
            while 0 <= (at := chunk.find(XBC1, at)) < limit:
                kind, dec, comp, _ = struct.unpack_from('<4I', chunk, at + 4)
                name = chunk[at + 20:at + 48].split(b'\0')[0]
                if kind in (1, 3) and dec and comp and all(32 <= c < 127 for c in name):
                    entries.append((offset + pos + at, kind, dec, comp, name.decode()))
                at += 4
            for magic in RAW_MARKS:
                raw += [offset + pos + at for at in marks(chunk, magic) if at < limit]
    entries.sort()
    raw.sort()
    starts = [e[0] for e in entries]
    return [e for e in entries if not e[4].endswith(SKIP)], raw, starts


def worker(job):
    path, entries, raw = job
    found, bad, damaged = {}, 0, [0]
    with open(path, 'rb') as f:
        for at, kind, dec, comp, _ in entries:
            f.seek(at + 48)
            try:
                packed = f.read(comp)
                data = zstd.decompress(packed) if kind == 3 else zlib.decompress(packed)
            except (zstd.ZstdError, zlib.error):
                bad += 1
                continue
            for key, stage, control, code in data_shaders(data, damaged):
                found.setdefault(key, (stage, control, code))
        for at, size in raw:
            f.seek(at)
            for key, stage, control, code in data_shaders(f.read(size), damaged):
                found.setdefault(key, (stage, control, code))
    return found, bad, damaged[0]


def archives(root):
    """(path of the image, offset, size, name) of each .ard under an install-shaped folder: romfs/
    (romfs.bin and index.json) and aoc/<index>/."""
    index = json.loads((root / 'romfs' / 'index.json').read_text(encoding='utf-8'))
    for row in index['files']:
        if row['path'].endswith('.ard'):
            yield root / 'romfs' / 'romfs.bin', row['offset'], row['size'], row['path']
    for path in sorted((root / 'aoc').glob('*/*.ard')):
        yield path, 0, path.stat().st_size, path.name


def main_shaders(root):
    """The shaders in main's image: its SLCT, and any GLSLC output. No main (a folder of data alone) gives none."""
    path = root / 'exefs' / 'main'
    if not path.is_file():
        return
    image = bytes(Nso(path).image)
    for at in marks(image, SLCT):
        yield from slct_shaders(image, at, [0])   # a chance SLCT in code: the walk stops at once
    for at in marks(image, GLSLC):
        yield from glslc_shaders(image, at)


def inventory(root, progress=None, workers=None):
    """Every unique shader under root: key -> dict(stage, control, code, source). Also returns the problems found:
    entries that failed to decompress and HCPS walks that ended early. progress(done, total) counts entries."""
    shaders, bad, damaged = {}, 0, 0
    workers = workers or max(1, min(os.cpu_count() or 1, 16))
    sources = list(archives(root))
    jobs = []
    for path, offset, size, name in sources:
        entries, raw, starts = scan(path, offset, size)
        spans = {}
        for at in raw:   # a stored run: from its first marker up to the next xbc1, within what one model can hold
            i = bisect.bisect_right(starts, at)
            spans.setdefault(starts[i] if i < len(starts) else offset + size, at)
        spans = [(at, min(stop - at, 256 << 20)) for stop, at in spans.items()]
        n = max(1, len(entries) // BATCH)
        jobs += [(name, path, entries[i::n], spans if i == 0 else []) for i in range(n)]
    total = sum(len(j[2]) for j in jobs)
    done = 0
    for key, stage, control, code in main_shaders(root):
        shaders.setdefault(key, dict(stage=stage, control=control, code=code, source='main', origin='main', startup=True))
    with concurrent.futures.ProcessPoolExecutor(workers) as pool:
        for (name, _, entries, _), (found, b, d) in zip(jobs, pool.map(worker, [j[1:] for j in jobs])):
            for key, (stage, control, code) in found.items():
                shaders.setdefault(key, dict(stage=stage, control=control, code=code, source=name, origin='ard',
                                             startup=name == 'bf3.ard'))
            bad += b
            damaged += d
            done += len(entries)
            if progress:
                progress(done, total)
    return shaders, dict(undecoded=bad, damaged=damaged)


def main():
    root = Path(sys.argv[1])
    shaders, problems = inventory(root, lambda d, t: print(f'\r{d} of {t}', end='', flush=True))
    print()
    by = {}
    for s in shaders.values():
        by[s['source']] = by.get(s['source'], 0) + 1
    print(f'{len(shaders)} shaders, code {sum(len(s["code"]) for s in shaders.values()) >> 20} MiB, {by}, {problems}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
