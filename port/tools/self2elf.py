"""Unwrap an unencrypted (fake-signed) PS4 SELF into a plain ELF.

Usage: py -I self2elf.py <eboot.bin> <out.elf>
"""
import struct
import sys

SELF_MAGIC = b"\x4f\x15\x3d\x1d"
FLAG_ENCRYPTED = 0x2
FLAG_COMPRESSED = 0x8
FLAG_BLOCKED = 0x800


def main(src, dst):
    data = open(src, "rb").read()
    if data[:4] != SELF_MAGIC:
        sys.exit("not a SELF")
    seg_count = struct.unpack_from("<H", data, 0x18)[0]
    elf_off = 0x20 + seg_count * 32
    if data[elf_off:elf_off + 4] != b"\x7fELF":
        sys.exit("ELF header not found after segment table")

    e_phoff, = struct.unpack_from("<Q", data, elf_off + 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, elf_off + 0x36)
    phdrs = []
    for i in range(e_phnum):
        p = struct.unpack_from("<IIQQQQQQ", data, elf_off + e_phoff + i * e_phentsize)
        phdrs.append(p)  # type, flags, offset, vaddr, paddr, filesz, memsz, align

    size = max([elf_off + e_phoff + e_phnum * e_phentsize - elf_off] +
               [p[2] + p[5] for p in phdrs])
    out = bytearray(size)
    head = e_phoff + e_phnum * e_phentsize
    out[:head] = data[elf_off:elf_off + head]

    for i in range(seg_count):
        flags, offset, comp_size, size_ = struct.unpack_from("<QQQQ", data, 0x20 + i * 32)
        if not flags & FLAG_BLOCKED:
            continue
        if flags & (FLAG_ENCRYPTED | FLAG_COMPRESSED):
            sys.exit(f"segment {i} is encrypted or compressed; need a real dump")
        ph = phdrs[(flags >> 20) & 0xFFF]
        out[ph[2]:ph[2] + size_] = data[offset:offset + size_]

    open(dst, "wb").write(out)
    print(f"wrote {dst}: {len(out):#x} bytes, {e_phnum} program headers")
    for p in phdrs:
        print(f"  type={p[0]:#010x} flags={p[1]} off={p[2]:#x} vaddr={p[3]:#x} filesz={p[5]:#x} memsz={p[6]:#x}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
