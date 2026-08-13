#!/usr/bin/env python3
"""mkpictures.py -- the pictures the viewer opens, generated rather than shipped.

A photo viewer with nothing to view is hard to judge, and checking a few
megabytes of someone else's photographs into an OS repository to fix that is
not a good trade. So the sample album is generated, deterministically, from
about a hundred lines.

The important one is the ZONE PLATE (chart.png): a pattern whose spatial
frequency rises with the distance from the centre, so a single image sweeps
smoothly from "easy to resample" at the middle to "impossible" at the corners.
It is the standard torture test for a scaling filter, and it is unusually
honest because you do not need to measure it to read the result:

  * Area averaging -- the rings stay rings, and fade to flat grey exactly where
    the detail gets finer than the screen can show. Nothing is invented.
  * Bilinear or point sampling -- large ghost rings appear in the outer field,
    curving the WRONG WAY, at frequencies that are not in the source at all.
    That is aliasing, and once seen in a zone plate it is recognisable in every
    photograph of a brick wall or a striped shirt.

photo_test.c measures the same effect as a number (area off by 1, bilinear off
by 101 on a checkerboard). This is the version you can look at.

Writes PNG directly -- zlib is in the standard library and a PNG is a header,
one IDAT of filtered scanlines and a trailer, which is less code than taking a
dependency would be.
"""
import math
import os
import shutil
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "pictures")


def write_png(path, width, height, rows, greyscale=False):
    """rows: a list of bytearrays, one per scanline, already in RGB or grey."""
    ctype = 0 if greyscale else 2
    raw = bytearray()
    for r in rows:
        raw.append(0)              # filter type 0 (None) -- the data is
        raw += r                   # synthetic, so filtering buys little

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, ctype, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


def zone_plate(width, height):
    """sin(k * r^2): frequency rises linearly with radius, so one image covers
    every scale from flat to beyond-Nyquist."""
    rows = []
    cx, cy = width / 2.0, height / 2.0

    # THE CONSTANT IS THE TEST, so it is derived rather than tuned by eye.
    #
    # With phase(r) = a*r^2, the local spatial frequency is a*r/pi cycles per
    # pixel, and the pixel grid cannot represent more than 0.5 of those (the
    # Nyquist limit). A zone plate whose frequency never reaches 0.5 is a
    # picture of fine rings, not a hard case -- and the first version of this
    # generator was exactly that: it topped out at 0.23 cycles/pixel in the
    # corners, so every filter drew it correctly and the image proved nothing.
    #
    # So: place the Nyquist crossing at 55% of the half-diagonal. Inside that
    # radius the rings are real and a correct filter must show them; outside it
    # the source is genuinely beyond what any grid can carry, and a correct
    # filter must show FLAT GREY while a sampling filter invents ghost rings.
    # One image, both halves of the claim.
    half_diag = math.hypot(width, height) / 2.0
    a = 0.5 * math.pi / (0.55 * half_diag)

    # SUPERSAMPLED, and this is not an optimisation -- it is what makes the
    # image an instrument instead of a picture.
    #
    # Sampling sin(a*r^2) once per pixel is itself point sampling, so beyond
    # the Nyquist radius the FILE gets aliased: broad ghost rings, baked into
    # the PNG at frequencies low enough to survive any amount of later
    # resizing. A viewer that then displayed those ghosts faithfully would look
    # like it was aliasing when it was doing exactly the right thing with a
    # corrupt source -- and I read the first render that way before checking.
    #
    # Averaging SS x SS samples per pixel band-limits the source, so the outer
    # field is smooth grey IN THE FILE. Any ring structure visible after that
    # was invented downstream, which is precisely the question being asked.
    SS = 4
    off = [(i + 0.5) / SS - 0.5 for i in range(SS)]
    inv = 1.0 / (SS * SS)

    for y in range(height):
        row = bytearray()
        for x in range(width):
            acc = 0.0
            for oy in off:
                dy = y + oy - cy
                dy2 = dy * dy
                for ox in off:
                    dx = x + ox - cx
                    acc += math.sin(a * (dx * dx + dy2))
            v = acc * inv
            row.append(int((v + 1.0) * 127.5))
        rows.append(row)
    return rows


def spectrum(width, height):
    """A calibration card: hue across, lightness down, with a row of neutral
    steps. Flat areas and hard edges -- what catches a resampler that shifts
    colour or rings at boundaries, which the zone plate does not test."""
    rows = []
    for y in range(height):
        row = bytearray()
        fy = y / float(height - 1)
        for x in range(width):
            fx = x / float(width - 1)
            if fy > 0.82:                       # neutral step wedge
                step = min(11, int(fx * 12)) / 11.0
                v = int(step * 255)
                row += bytes((v, v, v))
                continue
            h = fx * 6.0
            i = int(h) % 6
            f = h - int(h)
            # value falls with y so the top is saturated and the bottom is dark
            top = 1.0 - fy * 0.55
            p, q, t = 0.0, top * (1.0 - f), top * f
            r, g, b = [(top, t, p), (q, top, p), (p, top, t),
                       (p, q, top), (t, p, top), (top, p, q)][i]
            row += bytes((int(r * 255), int(g * 255), int(b * 255)))
        rows.append(row)
    return rows


def main():
    os.makedirs(OUT, exist_ok=True)

    n = write_png(os.path.join(OUT, "chart.png"), 1024, 768,
                  zone_plate(1024, 768), greyscale=True)
    print("  chart.png     1024x768 grey   %6d bytes  (zone plate)" % n)

    n = write_png(os.path.join(OUT, "spectrum.png"), 900, 600,
                  spectrum(900, 600))
    print("  spectrum.png   900x600 rgb    %6d bytes  (hue/step card)" % n)

    # A real JPEG, so the album exercises the other decoder rather than only
    # the one whose output we generated ourselves.
    src = os.path.join(ROOT, "system", "web", "photo.jpg")
    if os.path.exists(src):
        shutil.copyfile(src, os.path.join(OUT, "photo.jpg"))
        print("  photo.jpg      (copied)        %6d bytes  (JPEG path)"
              % os.path.getsize(src))
    return 0


if __name__ == "__main__":
    sys.exit(main())
