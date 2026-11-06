"""The destination list of the map-jump tour (runtime/tour.c, XC3_TOUR): every landmark's Skip Travel jump.

Adapted from Lucky7 (lucky7-xc3) @ 8b596bc (MIT, Copyright (c) 2026 Peiikai, Copyright (c) 2026 XCDE Recomp
contributors): tools/xc3/tour_gen.py (the list), tools/xc3/bdat4.py (BDAT v4 tables, murmur3 names) and
tools/xc2/arh.py (the arh1 index). Reads bf3.arh and bf3.ard inside the extracted RomFS image (romfs.bin, located by
index.json); no file is unpacked to disk.

    .venv-py314\\Scripts\\python tools\\tour_list.py build\\extracted\\upd\\romfs > build\\upd\\tour.txt

One line per destination: <jump id> <map> <formation hash, hex>, the jump ids every table of map.bdat names in a
MapJumpID column, looked up in sys.bdat's SYS_MapJumpList. The output holds numbers from the game's tables: keep it
under build/.

arh1 header (u32s after the magic): string table length, node count, string table offset and length, node table
offset and length, file table offset, file count, key. The string and node tables are XORed u32 by u32 with
key ^ 0xF3F35353. The node table is a double-array trie of (next, prev) i32 pairs: from node i the child for byte c
is next ^ c when its prev is i; next < 0 is a leaf whose rest of the path is a NUL-terminated string at string
offset -next, followed by the u32 file index. File entries (0x18 bytes): offset u64, compressed size, uncompressed
size, flags, id. Flag 2: an xbc1 block (type 1 zlib, 3 zstd; sizes at +8, +0xc; data at +0x30).
"""
import json
import struct
import sys
import zlib
from compression import zstd
from pathlib import Path


def murmur3(s, seed=0):
    d = s.encode()
    c1, c2, h = 0xcc9e2d51, 0x1b873593, seed
    rot = lambda x, r: ((x << r) | (x >> (32 - r))) & 0xffffffff  # noqa: E731
    n = len(d) // 4
    for i in range(n):
        k = struct.unpack_from('<I', d, i * 4)[0]
        k = rot((k * c1) & 0xffffffff, 15) * c2 & 0xffffffff
        h = (rot(h ^ k, 13) * 5 + 0xe6546b64) & 0xffffffff
    t, k = d[n * 4:], 0
    for i, b in enumerate(t):
        k |= b << (8 * i)
    if t:
        h ^= rot((k * c1) & 0xffffffff, 15) * c2 & 0xffffffff
    h ^= len(d)
    h ^= h >> 16
    h = h * 0x85ebca6b & 0xffffffff
    h ^= h >> 13
    h = h * 0xc2b2ae35 & 0xffffffff
    return h ^ (h >> 16)


class Arh:
    def __init__(self, data):
        if data[:4] != b'arh1':
            raise ValueError('not an arh1 archive')
        (_, _, str_off, str_len, node_off, node_len, file_off, file_count, key) = struct.unpack_from('<9I', data, 4)
        key ^= 0xF3F35353
        b = bytearray(data)
        for off, n in ((str_off, str_len), (node_off, node_len)):
            for i in range(off, off + (n & ~3), 4):
                struct.pack_into('<I', b, i, struct.unpack_from('<I', b, i)[0] ^ key)
        self.strings = bytes(b[str_off:str_off + str_len])
        self.nodes = [struct.unpack_from('<ii', b, node_off + 8 * i) for i in range(node_len // 8)]
        self.files = [struct.unpack_from('<QIIII', b, file_off + 0x18 * i) for i in range(file_count)]

    def paths(self):
        """(path, file index) for every leaf of the trie."""
        nodes, n = self.nodes, len(self.nodes)
        stack = [(0, b'')]
        while stack:
            i, pre = stack.pop()
            nxt, _ = nodes[i]
            if nxt < 0:
                s = -nxt
                e = self.strings.index(b'\0', s)
                idx = struct.unpack_from('<I', self.strings, e + 1)[0] if e + 5 <= len(self.strings) else None
                yield (pre + self.strings[s:e]).decode('latin1'), idx
                continue
            for c in range(1, 128):
                j = nxt ^ c
                if 0 < j < n and nodes[j][1] == i:
                    stack.append((j, pre + bytes([c])))


class Archive:
    """bf3.arh / bf3.ard inside romfs.bin."""

    def __init__(self, romfs):
        romfs = Path(romfs)
        files = {f['path']: (f['offset'], f['size']) for f in json.loads((romfs / 'index.json').read_text())['files']}
        self.image = open(romfs / 'romfs.bin', 'rb')
        arh_at, arh_size = files['bf3.arh']
        self.ard_at = files['bf3.ard'][0]
        self.image.seek(arh_at)
        self.arh = Arh(self.image.read(arh_size))
        self.index = dict(self.arh.paths())

    def read(self, path):
        off, csize, usize, flags, _ = self.arh.files[self.index[path]]
        self.image.seek(self.ard_at + off)
        if not flags & 2:
            return self.image.read(usize or csize)
        blob = self.image.read(csize + 0x30)
        kind, _, size = struct.unpack_from('<III', blob, 4)
        raw = blob[0x30:0x30 + size]
        return zstd.decompress(raw) if kind == 3 else zlib.decompress(raw) if kind == 1 else raw


SIZE = {1: 1, 2: 2, 3: 4, 4: 1, 5: 2, 6: 4, 7: 4, 8: 4, 9: 4, 10: 1, 11: 4, 12: 1, 13: 2}
UNPACK = {1: '<B', 12: '<B', 2: '<H', 13: '<H', 3: '<I', 9: '<I', 11: '<I', 4: '<b', 5: '<h', 6: '<i', 8: '<f'}


class Table:
    """One BDAT v4 table; column names are murmur3 hashes (or strings in tables that keep them)."""

    def __init__(self, b, o):
        self.b, self.o = b, o
        (self.ncol, self.nrow, self.base, _, col, _, self.rows, self.rowlen, self.str, _) = struct.unpack_from('<10I', b, o + 8)
        hashed = b[o + self.str] == 0
        self.name = struct.unpack_from('<I', b, o + self.str + 1)[0] if hashed else self._cstr(self.str + 1)
        self.cols, off = [], 0
        for i in range(self.ncol):
            t, n = struct.unpack_from('<BH', b, o + col + i * 3)
            name = struct.unpack_from('<I', b, o + self.str + n)[0] if hashed else self._cstr(self.str + n)
            self.cols.append((name, t, off))
            off += SIZE.get(t, 4)

    def _cstr(self, off):
        p = self.o + off
        return self.b[p:self.b.index(0, p)].decode('utf-8', 'replace')

    def row(self, i):
        p = self.o + self.rows + i * self.rowlen
        out = {}
        for name, t, off in self.cols:
            if t == 7:
                out[name] = self._cstr(self.str + struct.unpack_from('<I', self.b, p + off)[0])
            else:
                out[name] = struct.unpack_from(UNPACK.get(t, '<B'), self.b, p + off)[0]
        return out


def tables(blob):
    if blob[:4] != b'BDAT':
        raise ValueError('not BDAT')
    n = struct.unpack_from('<I', blob, 8)[0]
    return [Table(blob, struct.unpack_from('<I', blob, 16 + i * 4)[0]) for i in range(n)]


def main(romfs):
    ar = Archive(romfs)
    want = murmur3('MapJumpID')
    ids = set()
    for t in tables(ar.read('/bdat/map.bdat')):
        if t.nrow and any(c[0] == want for c in t.cols):
            ids.update(t.row(i)[want] for i in range(t.nrow))
    ids.discard(0)
    names = ('SYS_MapJumpList', murmur3('SYS_MapJumpList'))
    jump = next(t for t in tables(ar.read('/bdat/sys.bdat')) if t.name in names)
    col_map, col_hash = jump.cols[1][0], jump.cols[2][0]
    for i in sorted(ids):
        r = jump.row(i - jump.base)
        print(i, r[col_map], '%08x' % r[col_hash])


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
