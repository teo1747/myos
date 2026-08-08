#!/usr/bin/env python3
"""Every Icon* in the toolkit must be a codepoint the shipped font can draw.

The icon set is Unicode codepoints rendered from DejaVuSans, which is the font
mkfs bakes into the image. Seven of them were in the emoji planes -- a bell, a
clock, a magnifier, a folder, a document, a trash can, a person -- and DejaVu
has none of those. Each one drew a tofu box, in every app that asked for it,
and had done since the icon set was written: nothing checks a codepoint at
build time, and a missing glyph is not an error at run time either. The browser
put one in its toolbar and that is how they were noticed.

    python3 tools/checkicons.py        # fail if any icon has no glyph

The cmap reader here is deliberately minimal -- format 4 is all DejaVu's
Unicode subtable uses, and this reads a file the build already trusts.
"""
import os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "ui", "dsl", "em.h")
# The same file mkfs bakes in as /system/fonts/font.ttf.
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def cmap_of(path):
    """Every codepoint the font maps, from its format-4 subtables."""
    d = open(path, "rb").read()
    off = None
    for i in range(struct.unpack(">H", d[4:6])[0]):
        e = 12 + 16 * i
        if d[e:e + 4] == b"cmap":
            off = struct.unpack(">I", d[e + 8:e + 12])[0]
    if off is None:
        raise SystemExit("no cmap table in " + path)
    out = set()
    for i in range(struct.unpack(">H", d[off + 2:off + 4])[0]):
        sub = off + struct.unpack(">I", d[off + 8 + 8 * i:off + 12 + 8 * i])[0]
        if struct.unpack(">H", d[sub:sub + 2])[0] != 4:
            continue
        segx2 = struct.unpack(">H", d[sub + 6:sub + 8])[0]
        sc = segx2 // 2
        ends = [struct.unpack(">H", d[sub + 14 + 2 * j:sub + 16 + 2 * j])[0] for j in range(sc)]
        sp = sub + 16 + segx2
        starts = [struct.unpack(">H", d[sp + 2 * j:sp + 2 + 2 * j])[0] for j in range(sc)]
        for a, b in zip(starts, ends):
            if b != 0xFFFF:
                out |= set(range(a, b + 1))
    return out


def main():
    if not os.path.exists(FONT):
        print("icon-check: %s not present, skipped" % FONT)
        return 0
    have = cmap_of(FONT)
    bad = []
    for m in re.finditer(r"#define\s+(Icon\w+)\s+(0x[0-9A-Fa-f]+)", open(HEADER).read()):
        cp = int(m.group(2), 16)
        if cp not in have:
            bad.append((m.group(1), cp))
    if bad:
        print("icon-check: %d icon(s) have NO GLYPH in %s -- each draws a tofu box:"
              % (len(bad), os.path.basename(FONT)))
        for name, cp in bad:
            print("    %-14s U+%04X" % (name, cp))
        return 1
    print("icon-check: OK (every Icon* in em.h has a glyph)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
