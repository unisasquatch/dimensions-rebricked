"""Tile a folder of screenshot PNGs (from raw2png.py) into one labelled contact sheet.

Usage: py -I contact_sheet.py <folder> <out.png> [columns]
Each tile's top-left corner gets its frame number as a bar of dots (one dot per 100 frames).
"""
import pathlib
import struct
import sys
import zlib


def read_png(path):
    data = path.read_bytes()
    w, h = struct.unpack(">II", data[16:24])
    idat = b""
    pos = 8
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        if tag == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows = [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]  # filter type 0 only
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(tag, d):
        return struct.pack(">I", len(d)) + tag + d + struct.pack(">I", zlib.crc32(tag + d))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main(folder, out, columns=8):
    files = sorted(pathlib.Path(folder).glob("*.png"))
    tiles = [(f, *read_png(f)) for f in files]
    tw, th = tiles[0][1], tiles[0][2]
    rows_n = (len(tiles) + columns - 1) // columns
    W, H = columns * (tw + 2), rows_n * (th + 2)
    canvas = [bytearray(W * 3) for _ in range(H)]
    for i, (f, w, h, rows) in enumerate(tiles):
        ox, oy = (i % columns) * (tw + 2), (i // columns) * (th + 2)
        for y in range(min(h, th)):
            canvas[oy + y][ox * 3:(ox + tw) * 3] = rows[y][:tw * 3]
        frame = int(f.name.split("_")[0])
        for d in range(frame // 100):  # frame marker: yellow dots, one per 100 frames
            x, y = ox + 2 + (d % 40) * 3, oy + 2 + (d // 40) * 3
            if x + 1 < ox + tw:
                for yy in (y, y + 1):
                    canvas[yy][x * 3:(x + 2) * 3] = bytes((255, 220, 0) * 2)
    write_png(pathlib.Path(out), W, H, [bytes(r) for r in canvas])
    print(f"{len(tiles)} tiles -> {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 8)
