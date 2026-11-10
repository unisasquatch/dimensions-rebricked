"""List and extract files from the game's TT Games archives (<name>.HDR index + <name>.DAT data, "CC40TAD" v-11).

HDR (big-endian): u32 size, ".CC40TAD", i32 version (-11), u32 ?, u32 files, u32 name entries, u32 names size,
names block (NUL-separated, padded), u32 ?, name entries {u32 name offset, u16 parent, u16 file, u16 ?, u16 ?},
i32 version, u32 files, u32 ?, then file entries {u32 high, u32 offset, u32 stored size, u32 size | 0x80000000 when
compressed}, then a sorted u32 hash per file. A name entry's `file` indexes the file table (0 for folders). Compressed data is a run of
"DFLT" chunks: u32 stored size, u32 size (little-endian), raw deflate with bit 2 of
the first byte cleared (the high bit of the first block type: set it back before inflating).

Usage:
  py -I ttdat.py list <uroot>... [--grep text]           every path, with its archive
  py -I ttdat.py extract <out dir> <regex> <uroot>...    extract matching paths (later archives win)
The uroots are searched in order; patch archives (PATCH*) override base ones for the same path.
"""
import pathlib
import re
import struct
import sys
import zlib


def read_hdr(path):
    d = path.read_bytes()
    if d[4:12] != b'.CC40TAD':
        return []
    _ver, _a, files, names, nsize = struct.unpack('>iIIII', d[12:32])
    nb = d[32:32 + nsize]
    p = 32 + nsize + 4
    entries = [struct.unpack('>IHHHH', d[p + 12 * i:p + 12 * i + 12]) for i in range(names)]
    p += 12 * names + 8
    table = [struct.unpack('>IIII', d[p + 16 * i:p + 16 * i + 16]) for i in range(files)]
    out = []
    full = [None] * names
    for i, (no, parent, f, _n, _z) in enumerate(entries):
        name = nb[no:nb.index(b'\0', no)].decode('latin-1') if no < nsize else ''
        base = full[parent] if 0 < parent < i and full[parent] else ''
        full[i] = (base + '/' + name) if base else name
        if f and '.' in name and f < files:
            hi, off, stored, size = table[f]
            out.append((full[i], hi, off, stored, size & 0x7FFFFFFF, bool(size & 0x80000000)))
    return out


def find_dat(hdr, roots):
    if hdr.suffix.upper() == '.HDR2':  # add-on packages: DLCnn.HDR2 / DLCnn.DAT2 side by side
        dat = hdr.with_suffix('.DAT2')
        return dat if dat.exists() else None
    name = hdr.stem + '.DAT'
    for r in roots:
        for c in sorted((r / 'data').glob('chunk*')):
            if (c / name).exists():
                return c / name
    return None


def archives(roots):
    for r in roots:
        for hdr in sorted((r / 'data').glob('chunk*/*.HDR')):
            yield hdr
        for hdr in sorted(r.glob('*.HDR2')):  # an add-on package's uroot
            yield hdr


def inflate_chunk(chunk, size):
    # TT clears bit 2 of the first byte (the high bit of the first block's type: dynamic blocks
    # read as stored); set it back. Fall back to the byte as is, or flipped, if that fails.
    first = chunk[0]
    for b0 in (first | 4, first, first ^ 4):
        chunk[0] = b0
        try:
            out = zlib.decompressobj(-15).decompress(bytes(chunk))
            if len(out) == size:
                return out
        except zlib.error:
            pass
    raise ValueError(f'chunk does not inflate (first byte {first:#x})')


def unpack(data, size):
    if not data.startswith(b'DFLT'):
        return data
    out = bytearray()
    p = 0
    while p < len(data) and data[p:p + 4] == b'DFLT':
        stored, _n = struct.unpack('<II', data[p + 4:p + 12])
        chunk = bytearray(data[p + 12:p + 12 + stored])
        if stored == _n:
            out += chunk  # stored as is
        else:
            out += inflate_chunk(chunk, _n)
        p += 12 + stored
    return bytes(out[:size])


def index(roots):
    files = {}
    for hdr in archives(roots):
        for path, hi, off, stored, size, comp in read_hdr(hdr):
            key = path.lower()
            # Patch archives override; otherwise the first archive that has it wins.
            if key not in files or hdr.stem.upper().startswith('PATCH'):
                files[key] = (hdr, hi, off, stored, size, comp)
    return files


def main():
    cmd = sys.argv[1]
    if cmd == 'list':
        args = sys.argv[2:]
        pat = None
        if '--grep' in args:
            i = args.index('--grep')
            pat = re.compile(args[i + 1], re.I)
            args = args[:i] + args[i + 2:]
        for path, (hdr, hi, off, stored, size, comp) in sorted(index([pathlib.Path(a) for a in args]).items()):
            if not pat or pat.search(path):
                print(f'{hdr.stem}\t{size}\t{path}')
    elif cmd == 'extract':
        out_dir, pat = pathlib.Path(sys.argv[2]), re.compile(sys.argv[3], re.I)
        roots = [pathlib.Path(a) for a in sys.argv[4:]]
        dats = {}
        n = 0
        for path, (hdr, hi, off, stored, size, comp) in sorted(index(roots).items()):
            if not pat.search(path):
                continue
            dat = dats.setdefault(hdr, find_dat(hdr, roots))
            with open(dat, 'rb') as f:
                f.seek(off + (hi << 32 if hi < 16 else 0))
                data = unpack(f.read(stored), size)
            dst = out_dir / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
            n += 1
        print(f'extracted {n} files')


if __name__ == '__main__':
    main()
