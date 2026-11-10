"""Convert render-target dumps (run/dump/*.raw from LDPORT_DUMP) to PNGs.

Name format: <address>_<w>x<h>_<vkformat>[_display].raw. Formats: 37 = R8G8B8A8_UNORM (memory order;
the display buffer is B,G,R,A), 97 = R16G16B16A16_SFLOAT. Others are written as raw RGBA8 guesses.
Usage: py -I raw2png.py <folder> [scale]   (scale 4 = quarter size, for quick looks)
"""
import pathlib
import struct
import sys
import zlib


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def half(h):
    s, e, m = (h >> 15) & 1, (h >> 10) & 0x1F, h & 0x3FF
    if e == 0:
        v = m / 1024.0 * 2 ** -14
    elif e == 31:
        v = 65504.0
    else:
        v = (1 + m / 1024.0) * 2 ** (e - 15)
    return -v if s else v


def main(folder, scale=1):
    for f in sorted(pathlib.Path(folder).glob("*.raw")):
        parts = f.stem.split("_")
        w, h = map(int, parts[1].split("x"))
        fmt = int(parts[2])
        display = f.stem.endswith("_display")
        data = f.read_bytes()
        out = bytearray(w * h * 3)
        if fmt == 97:
            for i in range(w * h):
                r, g, b = (half(v) for v in struct.unpack_from("<3H", data, i * 8))
                out[i * 3:i * 3 + 3] = bytes(max(0, min(255, int(c ** (1 / 2.2) * 255 if c > 0 else 0))) for c in (r, g, b))
        else:
            for i in range(w * h):
                b0, b1, b2 = data[i * 4], data[i * 4 + 1], data[i * 4 + 2]
                out[i * 3:i * 3 + 3] = bytes((b2, b1, b0) if display else (b0, b1, b2))
        if scale > 1:
            sw, sh = w // scale, h // scale
            small = bytearray(sw * sh * 3)
            for y in range(sh):
                for x in range(sw):
                    i = ((y * scale) * w + x * scale) * 3
                    small[(y * sw + x) * 3:(y * sw + x) * 3 + 3] = out[i:i + 3]
            out, w, h = small, sw, sh
        write_png(f.with_suffix(".png"), w, h, bytes(out))
        print(f.with_suffix(".png").name)


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 1)
