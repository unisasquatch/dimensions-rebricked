"""Count GCN (Sea Islands / gfx7) instruction encodings and opcodes across dumped shaders.

Usage: py -I gcn_census.py <run/capture/shaders>
"""
import collections
import pathlib
import struct
import sys

# (name, mask, value, size in dwords, opcode extractor)
ENCODINGS = [
    ("SOP1", 0xFF800000, 0xBE800000, 1, lambda w: (w >> 8) & 0xFF),
    ("SOPC", 0xFF800000, 0xBF000000, 1, lambda w: (w >> 16) & 0x7F),
    ("SOPP", 0xFF800000, 0xBF800000, 1, lambda w: (w >> 16) & 0x7F),
    ("SOPK", 0xF0000000, 0xB0000000, 1, lambda w: (w >> 23) & 0x1F),
    ("SMRD", 0xF8000000, 0xC0000000, 1, lambda w: (w >> 22) & 0x1F),
    ("VINTRP", 0xFC000000, 0xC8000000, 1, lambda w: (w >> 16) & 0x3),
    ("VOP3", 0xFC000000, 0xD0000000, 2, lambda w: (w >> 17) & 0x1FF),
    ("DS", 0xFC000000, 0xD8000000, 2, lambda w: (w >> 18) & 0xFF),
    ("FLAT", 0xFC000000, 0xDC000000, 2, lambda w: (w >> 18) & 0x7F),
    ("MUBUF", 0xFC000000, 0xE0000000, 2, lambda w: (w >> 18) & 0x7F),
    ("MTBUF", 0xFC000000, 0xE8000000, 2, lambda w: (w >> 16) & 0x7),
    ("MIMG", 0xFC000000, 0xF0000000, 2, lambda w: (w >> 18) & 0x7F),
    ("EXP", 0xFC000000, 0xF8000000, 2, lambda w: 0),
    ("SOP2", 0xC0000000, 0x80000000, 1, lambda w: (w >> 23) & 0x7F),
    ("VOPC", 0xFE000000, 0x7C000000, 1, lambda w: (w >> 17) & 0xFF),
    ("VOP1", 0xFE000000, 0x7E000000, 1, lambda w: (w >> 9) & 0xFF),
    ("VOP2", 0x80000000, 0x00000000, 1, lambda w: (w >> 25) & 0x3F),
]


def has_literal(enc, w):
    if enc in ("SOP2", "SOPC"):
        return (w & 0xFF) == 0xFF or ((w >> 8) & 0xFF) == 0xFF
    if enc == "SOP1":
        return (w & 0xFF) == 0xFF
    if enc in ("VOP1", "VOP2", "VOPC"):
        return (w & 0x1FF) == 0xFF
    return False


def decode(words):
    i = 0
    while i < len(words):
        w = words[i]
        for name, mask, value, size, op in ENCODINGS:
            if w & mask == value:
                yield name, op(w)
                i += size + (1 if has_literal(name, w) else 0)
                break
        else:
            yield "UNKNOWN", w
            i += 1


def main(folder):
    per_stage = collections.Counter()
    ops = collections.Counter()
    shaders_using = collections.Counter()
    total = 0
    for path in sorted(pathlib.Path(folder).glob("*.bin")):
        data = path.read_bytes()
        words = struct.unpack(f"<{len(data) // 4}I", data[: len(data) // 4 * 4])
        per_stage[path.name.split("_")[0]] += 1
        seen = set()
        for enc, op in decode(words):
            ops[(enc, op)] += 1
            seen.add((enc, op))
            total += 1
        for key in seen:
            shaders_using[key] += 1
    print("shaders by stage:", dict(per_stage), "instructions:", total, "distinct opcodes:", len(ops))
    by_enc = collections.Counter()
    for (enc, _), n in ops.items():
        by_enc[enc] += n
    print("by encoding:", dict(by_enc.most_common()))
    for (enc, op), n in sorted(ops.items()):
        print(f"{enc:7} {op:#05x} x{n:<6} in {shaders_using[(enc, op)]} shaders")


if __name__ == "__main__":
    main(sys.argv[1])
