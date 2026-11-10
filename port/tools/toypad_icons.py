"""Builds the Toy Pad web page's figure list and icons from the game's own data.

Reads every chars/collection.txt (base game and each additionalcontent pack) for the Toy Pad IDs
(`portal_model_id`), and every icons/characters/charactericontpage (a DXT1 atlas plus a contents list of
names) for the pictures, then writes <out>/<id>.png and <out>/figures.json ({id, name, kind}).

Usage: py -I toypad_icons.py <out dir> <uroot>... [--addcont <dir>]   (e.g. run/data/toypad/icons, both uroots and
       extracted/addcont)

figures.json gives each Year 2 figure the "pack" (add-on package label, e.g. OPUSSONIC0000001) whose world it
belongs to; the port hides figures whose pack is not installed (--addcont is accepted and ignored). Pack text files are "ZIPX" (u32 tag, u32 size, u32 size, then
RC4 with the 4 little-endian bytes of the size as the key, from TT's writer FUN_015c54f0).

Atlas (.tsh, big-endian): a CC4HSERHSER container; the "TXSH" chunk holds one 56-byte "1TXD"
record per contents line: {f32 u0, v0, u1, v1 (v from the bottom), u32 x, y, w, h, "1TXD", 4 x u32, u32 name hash}
(each record's rectangle sits before its marker), then a linear DDS.
The contents list maps each record's name hash to the character name.
"""
import json
import pathlib
import re
import struct
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import ttdat  # noqa: E402

ICON_HEIGHT = 128


def read_file(files, roots, dats, key):
    hdr, hi, off, stored, size, comp = files[key]
    dat = dats.setdefault(hdr, ttdat.find_dat(hdr, roots))
    with open(dat, 'rb') as f:
        f.seek(off)
        return ttdat.unpack(f.read(stored), size)


def unzipx(data):
    """TT's obfuscated text: 'ZIPX', u32 size, u32 size, then RC4 keyed with the size's 4 bytes."""
    if not data.startswith(b'ZIPX'):
        return data
    n = struct.unpack_from('<I', data, 4)[0]
    key = struct.pack('<I', n)
    box = list(range(256))
    j = 0
    for i in range(256):
        j = (j + box[i] + key[i & 3]) & 255
        box[i], box[j] = box[j], box[i]
    i = j = 0
    out = bytearray()
    for c in data[12:12 + n]:
        i = (i + 1) & 255
        j = (j + box[i]) & 255
        box[i], box[j] = box[j], box[i]
        out.append(c ^ box[(box[i] + box[j]) & 255])
    return bytes(out)


# Year 2 add-on packages (PS4 entitlement labels) by the franchise whose world each one adds. The packages' own
# collection.txt lists only their worlds' NPCs, so figures are matched by franchise; Ghostbusters is split: the
# Year 1 figures come with the game, the 2016 ones (Abby Yates, the new Ecto) need the 2016 story pack.
PACK_BY_FRANCHISE = {
    'Adventure Time': 'OPUSADVENTURETIM', 'The A-Team': 'OPUSATEAM0000001', 'The LEGO Batman Movie': 'OPUSBATMANSTORY1',
    'Beetlejuice': 'OPUSBEETLEJUICE1', 'E.T.': 'OPUSET0000000001', 'Fantastic Beasts': 'OPUSFANTASTICBEA',
    'The Goonies': 'OPUSGOONIES00001', 'Gremlins': 'OPUSGREMLINS0001', 'Harry Potter': 'OPUSHARRYPOTTER1',
    'Knight Rider': 'OPUSKNIGHTRIDER1', 'LEGO City: Undercover': 'OPUSLEGOCITY0001',
    'Mission: Impossible': 'OPUSMISSIONIMPOS', 'The Powerpuff Girls': 'OPUSPOWERPUFF001',
    'Sonic the Hedgehog': 'OPUSSONIC0000001', 'Teen Titans Go!': 'OPUSTEENTITANS01',
}
GHOSTBUSTERS_2016 = {47, 1197, 1198, 1199}


def figure_pack(fid, franchise):
    if franchise == 'Ghostbusters':
        return 'OPUSGHOSTBUSTERS' if fid in GHOSTBUSTERS_2016 else ''
    return PACK_BY_FRANCHISE.get(franchise, '')


# --- figures ---
COLLECT = re.compile(r'^\s*collect\s+"([^"]+)"\s+portal_model_id\s+(\d+)([^\n]*)', re.I | re.M)


FIXES = {'Delorian': 'DeLorean', 'Jurrasic': 'Jurassic', 'Gimlie': "Gimli's", 'Scoobie': 'Scooby',
         'Coptor': 'Copter', 'Balista': 'Ballista'}


def words(s):
    s = re.sub(r'(?<=[a-z])(?=[A-Z])|_', ' ', s)
    out = ' '.join(w if not w.islower() else w.capitalize() for w in s.split())
    for a, b in FIXES.items():
        out = out.replace(a, b)
    return out


def nice_name(raw, comment):
    m = re.match(r'(?i)ti_(.+?)_(\d)$', raw)
    if m:  # vehicles and gadgets: three builds each
        base, build = m.group(1), m.group(2)
        c = re.search(r'///<\s*(.+?)\s*-\s*Model', comment)  # "///< Bad Cop Car - Model 1"
        name = c.group(1).strip() if c else words(base)
        for a, b in FIXES.items():
            name = name.replace(a, b)
        return f'{name} (build {build})'
    # "GandalfTheGrey" -> "Gandalf The Grey", "TeenTitansGO_Raven" -> "Raven"
    if raw.lower().startswith('teentitansgo_'):
        raw = raw.split('_', 1)[1]
    return words(raw)


# --- atlas ---
def dxt1_block(b):
    c0, c1, bits = struct.unpack('<HHI', b)

    def rgb(c):
        return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    p0, p1 = rgb(c0), rgb(c1)
    if c0 > c1:
        pal = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
        alpha = [255] * 4
    else:
        pal = [p0, p1, tuple((a + b) // 2 for a, b in zip(p0, p1)), (0, 0, 0)]
        alpha = [255, 255, 255, 0]
    return [(pal[(bits >> (2 * i)) & 3], alpha[(bits >> (2 * i)) & 3]) for i in range(16)]


def crop_dxt1(data, width, x0, y0, w, h):
    """RGBA rows of the w x h region at (x0, y0) of a DXT1 image."""
    bw = (width + 3) // 4
    out = [[(0, 0, 0, 0)] * w for _ in range(h)]
    for by in range(y0 // 4, (y0 + h + 3) // 4):
        for bx in range(x0 // 4, (x0 + w + 3) // 4):
            o = (by * bw + bx) * 8
            if o + 8 > len(data):
                continue
            px = dxt1_block(data[o:o + 8])
            for i, (c, a) in enumerate(px):
                x, y = bx * 4 + (i & 3) - x0, by * 4 + (i >> 2) - y0
                if 0 <= x < w and 0 <= y < h:
                    out[y][x] = (*c, a)
    return out


def scale(rows, height):
    h, w = len(rows), len(rows[0])
    if h <= height:
        return rows
    nw = max(1, round(w * height / h))
    return [[rows[y * h // height][x * w // nw] for x in range(nw)] for y in range(height)]


def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(v for px in row for v in px) for row in rows)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def read_atlas(tsh, contents):
    names = {}
    for line in contents.decode('latin-1').splitlines():
        m = re.match(r'\s*\d+:\s*([0-9a-f]{8})\s+(\S+)', line)
        if m:
            names[int(m.group(1), 16)] = m.group(2).lower()
    t = tsh.find(b'1TXD')
    dds = tsh.find(b'DDS ')
    if t < 0 or dds < 0:
        return None
    height, width = struct.unpack('<II', tsh[dds + 12:dds + 20])
    if tsh[dds + 84:dds + 88] != b'DXT1':
        return None
    rects = {}
    for i in range(len(names)):
        r = tsh[t + 56 * i:t + 56 * i + 56]
        if r[:4] != b'1TXD':
            break
        h = struct.unpack('>I', r[20:24])[0]
        # A record's rectangle comes just before its "1TXD" marker (the first one before the list).
        u0, v0, u1, v1 = struct.unpack('>4f', tsh[t + 56 * i - 32:t + 56 * i - 16])
        if h in names and 0 <= u0 < u1 <= 1 and 0 <= v0 < v1 <= 1:
            x, y = round(u0 * width), round((1 - v1) * height)
            rects[names[h]] = (x, y, round((u1 - u0) * width), round((v1 - v0) * height))
    return {'width': width, 'data': tsh[dds + 128:], 'rects': rects}


# Each character's franchise (the pack it came in). Vehicles and gadgets take their owner's, from the
# collection's "can_be_upgraded_by" (VEHICLE_HINTS covers the few without one).
_F = {
    'DC Comics': [1, 4, 6, 12, 20, 23, 40, 43, 45, 46, 769],
    'The Lord of the Rings': [2, 18, 19, 29],
    'The LEGO Movie': [3, 5, 8, 16, 41],
    'The Simpsons': [7, 21, 27],
    'Portal 2': [9],
    'Ninjago': [10, 22, 24, 30, 32, 37, 44],
    'Legends of Chima': [11, 17, 28],
    'Doctor Who': [13, 15],
    'Back to the Future': [14, 31],
    'Jurassic World': [25, 33],
    'Midway Arcade': [26],
    'Ghostbusters': [34, 35, 39, 47],
    'Scooby-Doo': [36, 38],
    'The Wizard of Oz': [42],
    'Adventure Time': [48, 50, 51, 63],
    'Mission: Impossible': [49],
    'Harry Potter': [52, 53, 67],
    'Knight Rider': [54],
    'The A-Team': [55],
    'Fantastic Beasts': [56, 62],
    'Sonic the Hedgehog': [57],
    'Gremlins': [59, 60],
    'E.T.': [61],
    'The LEGO Batman Movie': [64, 65, 69],
    'The Goonies': [66],
    'LEGO City: Undercover': [68],
    'Teen Titans Go!': [70, 71, 77],
    'Beetlejuice': [72],
    'The Powerpuff Girls': [74, 75, 76],
}
FRANCHISE = {fid: name for name, ids in _F.items() for fid in ids}
VEHICLE_HINTS = {'lbm': 'The LEGO Batman Movie', 'tardis': 'Doctor Who', 'k9': 'Doctor Who', 'dalek': 'Doctor Who',
                 'portal': 'Portal 2', 'turret': 'Portal 2', 'ninjago': 'Ninjago', 'jurassic': 'Jurassic World',
                 'ecto': 'Ghostbusters', 'ghost': 'Ghostbusters', 'pirateship': 'The Goonies',
                 'swoopingevil': 'Fantastic Beasts', 'niffler': 'Fantastic Beasts', 'lumpy': 'Adventure Time',
                 'marceline': 'Adventure Time', 'bmo': 'Adventure Time', 'jakemob': 'Adventure Time'}


def write_atlas(path, icons, cell=ICON_HEIGHT, columns=32):
    """Every icon in one RGBA image for the port's in-game picker (no PNG decoder needed there).
    Format (little-endian): b'TPAT', u32 version 1, u32 width, u32 height, u32 count,
    count x {u32 id, u16 x, u16 y, u16 w, u16 h}, then width * height RGBA bytes."""
    ids = sorted(icons)
    rows_n = (len(ids) + columns - 1) // columns
    width, height = cell * columns, cell * rows_n
    pixels = bytearray(width * height * 4)
    entries = bytearray()
    for k, fid in enumerate(ids):
        rows = icons[fid]
        h, w = len(rows), len(rows[0])
        x0 = (k % columns) * cell + (cell - min(w, cell)) // 2
        y0 = (k // columns) * cell + (cell - min(h, cell)) // 2
        for y in range(min(h, cell)):
            row = rows[y]
            o = ((y0 + y) * width + x0) * 4
            pixels[o:o + min(w, cell) * 4] = bytes(v for px in row[:cell] for v in px)
        entries += struct.pack('<IHHHH', fid, x0, y0, min(w, cell), min(h, cell))
    path.write_bytes(b'TPAT' + struct.pack('<IIII', 1, width, height, len(ids)) + bytes(entries) + bytes(pixels))


def main():
    args = sys.argv[2:]
    addcont = None
    if '--addcont' in args:
        at = args.index('--addcont')
        addcont = args[at + 1]
        del args[at:at + 2]
    out = pathlib.Path(sys.argv[1])
    roots = [pathlib.Path(a) for a in args]
    del addcont  # packs are matched by franchise (see PACK_BY_FRANCHISE)
    files = ttdat.index(roots)
    dats = {}
    figures = {}
    for key in sorted(k for k in files if k.endswith('chars/collection.txt')):
        text = read_file(files, roots, dats, key).decode('latin-1')
        for raw, fid, comment in COLLECT.findall(text):
            fid = int(fid)
            owner = re.search(r'(?<!/)can_be_upgraded_by\s+(\w+)', comment)
            figures.setdefault(fid, (raw, nice_name(raw, comment), owner.group(1).lower() if owner else ''))
    atlases = []
    for key in sorted(k for k in files if k.endswith('icons/characters/charactericontpage_dx11.tsh')):
        contents = key.replace('_dx11.tsh', '_dx11_contents.txt')
        if contents in files:
            a = read_atlas(read_file(files, roots, dats, key), read_file(files, roots, dats, contents))
            if a:
                atlases.append(a)
    out.mkdir(parents=True, exist_ok=True)
    listing, missing = [], []
    icons = {}  # id -> RGBA rows, for the overlay's atlas
    by_raw = {raw.lower(): fid for fid, (raw, _n, _o) in figures.items() if fid < 1000}
    for fid, (raw, name, owner) in sorted(figures.items()):
        key = raw.lower()
        # A name keyword beats the owner: the game's data has a few odd owners (Lumpy Car -> Stay Puft).
        franchise = (FRANCHISE.get(fid) or next((f for k, f in VEHICLE_HINTS.items() if k in key), None)
                     or FRANCHISE.get(by_raw.get(owner, -1)) or 'Other')
        has_icon = False
        for a in atlases:
            if key in a['rects']:
                x, y, w, h = a['rects'][key]
                rows = scale(crop_dxt1(a['data'], a['width'], x, y, w, h), ICON_HEIGHT)
                write_png(out / f'{fid}.png', rows)
                icons[fid] = rows
                has_icon = True
                break
        if not has_icon:
            missing.append(f'{fid} {raw}')
        pack = figure_pack(fid, franchise)
        listing.append({'id': fid, 'name': name, 'kind': 'character' if fid < 1000 else 'vehicle', 'icon': has_icon,
                        'franchise': franchise, 'pack': pack})
    # One figure per line: the port's web server reads it line by line.
    write_atlas(out / 'atlas.bin', icons)
    (out / 'figures.json').write_text('[\n' + ',\n'.join(json.dumps(x) for x in listing) + '\n]\n')
    print(f'{len(listing)} figures, {len(listing) - len(missing)} with icons, {len(atlases)} atlases, '
          f'{sum(1 for x in listing if x["pack"])} from add-on packs')
    if missing:
        print('no icon:', ', '.join(missing))


if __name__ == '__main__':
    main()
