"""Turn `ldport <eboot.elf> --imports` output into a named import list.

Usage: ldport eboot.elf --imports | py -I name_imports.py <nid_db.xml> > docs/imports.tsv
The NID database ships with the GhidraOrbis extension (data/nid_db.xml).
"""
import re
import sys

names = dict(re.findall(r'obf="([^"]+)" sym="([^"]*)"', open(sys.argv[1], encoding="utf-8").read()))

rows, unknown = [], 0
for line in sys.stdin:
    parts = line.rstrip("\n").split("\t")
    if len(parts) != 3:
        continue
    lib, nid, uses = parts
    sym = names.get(nid, "")
    unknown += not sym
    rows.append((lib, sym or "?", nid, uses))

print("library\tsymbol\tnid\tcall_sites")
for row in sorted(rows):
    print("\t".join(row))
print(f"{len(rows)} imports, {unknown} unnamed", file=sys.stderr)
