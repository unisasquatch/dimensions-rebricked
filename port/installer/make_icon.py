# Draws the launcher / installer icon (a 2x2 LEGO brick with a portal ring) into icon.ico.
import sys
from PIL import Image, ImageDraw


def brick(size):
    s = 4  # supersample
    n = size * s
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # Portal ring behind the brick.
    d.ellipse([n * 0.02, n * 0.02, n * 0.98, n * 0.98], fill=(20, 110, 230, 255))
    d.ellipse([n * 0.10, n * 0.10, n * 0.90, n * 0.90], fill=(120, 210, 255, 255))
    d.ellipse([n * 0.15, n * 0.15, n * 0.85, n * 0.85], fill=(15, 40, 90, 255))
    # Brick body.
    x0, y0, x1, y1 = n * 0.22, n * 0.42, n * 0.78, n * 0.76
    d.rounded_rectangle([x0, y0, x1, y1], radius=n * 0.03, fill=(230, 40, 30, 255))
    d.rectangle([x0, y0, x1, y0 + n * 0.05], fill=(255, 90, 70, 255))
    # Studs.
    for cx in (0.36, 0.64):
        d.rounded_rectangle([n * (cx - 0.09), n * 0.31, n * (cx + 0.09), n * 0.44], radius=n * 0.02, fill=(200, 30, 25, 255))
        d.ellipse([n * (cx - 0.09), n * 0.27, n * (cx + 0.09), n * 0.35], fill=(255, 110, 90, 255))
    return im.resize((size, size), Image.LANCZOS)


out = sys.argv[1] if len(sys.argv) > 1 else "icon.ico"
big = brick(256)
big.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
big.save(out.rsplit(".", 1)[0] + ".png")
print("wrote", out)
