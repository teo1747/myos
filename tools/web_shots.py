#!/usr/bin/env python3
"""web_shots.py -- the same page, side by side, ours and Firefox's.

web_verify.py answers "is the content there". This answers the other half, the
one a reader actually judges: is it in the right PLACE. A page can score 100%
of its visible words and still be a column of text where the design is three
columns and a hero.

Not a pixel diff. Our fonts, colours and spacing are our own, so pixels will
never agree and a number that can never reach zero is a number nobody reads.
What it produces is a PICTURE to look at and a small set of GEOMETRY findings
that do not depend on style:

  COLUMN   a word Firefox puts in the right-hand third that we put in the left
           (or the reverse) -- a layout that collapsed, or one that should have
  ORDER    two words whose reading order we reversed
  OFFPAGE  a word Firefox puts on screen that we draw past the bottom or the
           right edge

  python3 tools/web_shots.py [page.html ...]      (default: build/webreal/*)
    OUT=dir   where the pictures go (default build/shots)
"""
import glob
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ff_driver import Firefox                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "browser_render")
OUT = os.environ.get("OUT", os.path.join(ROOT, "build", "shots"))
WIDTH, HEIGHT = 1100, 900
# Our render draws its own chrome above the page; Firefox's viewport starts at
# the document. Comparing y without accounting for that reports every word on
# the page as displaced by the same constant.
CHROME_Y = 110
WORD = re.compile(r"[^\W\d_][\w'À-ɏ-]{3,}", re.UNICODE)


def key_words(text):
    return [w.lower() for w in WORD.findall(text or "")]


def our_runs(path):
    """(word, x, y) for every word we draw, from the renderer's own dump."""
    env = dict(os.environ, TEXTDUMP="1")
    r = subprocess.run([BIN, path, str(WIDTH), str(HEIGHT), "0"],
                       capture_output=True, text=True, errors="replace",
                       env=env, cwd=ROOT, timeout=300)
    runs = []
    for line in r.stdout.splitlines():
        if not line.startswith("RUN|"):
            continue
        f = line.split("|")
        if len(f) < 8:
            continue
        try:
            x, y = float(f[1]), float(f[2])
        except ValueError:
            continue
        for w in key_words(f[7]):
            runs.append((w, x, y))
    return runs


def our_shot(path, png):
    subprocess.run(["make", "browser-render", "DOC=" + path, "BW=%d" % WIDTH,
                    "BH=%d" % HEIGHT, "PNG=" + png],
                   cwd=ROOT, capture_output=True, timeout=900)
    # The harness writes a PPM under the .png name; convert if Pillow is here.
    try:
        from PIL import Image
        Image.open(png).save(png)
    except Exception:
        pass


def compare(name, ff_rects, ours):
    """Geometry findings that do not depend on style."""
    # A word is comparable when it appears exactly ONCE on each side: anything
    # repeated ("the", a nav label that is also a heading) cannot be matched to
    # a box without guessing, and a guess here invents a finding.
    fpos, fseen = {}, {}
    for r in ff_rects:
        for w in key_words(r["t"]):
            fseen[w] = fseen.get(w, 0) + 1
            fpos[w] = (r["x"], r["y"])
    opos, oseen = {}, {}
    for w, x, y in ours:
        oseen[w] = oseen.get(w, 0) + 1
        opos[w] = (x, y)
    common = [w for w in fpos if fseen[w] == 1 and oseen.get(w) == 1]

    col, off = [], []
    for w in common:
        fx, fy = fpos[w]
        ox, oy = opos[w]
        oy -= CHROME_Y
        if abs(fx - ox) > WIDTH * 0.25:
            col.append((w, int(fx), int(ox)))
        if fy < HEIGHT and (oy > HEIGHT or ox > WIDTH):
            off.append((w, int(fy), int(oy)))

    # ORDER: reading order is y then x. Count pairs we inverted, over a sample
    # -- every pair would be O(n^2) on a page with two thousand words.
    order = 0
    s = sorted(common, key=lambda w: (fpos[w][1], fpos[w][0]))[:400]
    for i in range(len(s) - 1):
        a, b = s[i], s[i + 1]
        if opos[a][1] - opos[b][1] > 40:
            order += 1

    print("  %-12s %4d words comparable   column-off %-4d order-inverted %-4d offpage %d"
          % (name, len(common), len(col), order, len(off)))
    for w, fx, ox in sorted(col, key=lambda c: -abs(c[1] - c[2]))[:6]:
        print("       COLUMN  %-22s firefox x=%-5d ours x=%d" % (w[:22], fx, ox))
    for w, fy, oy in off[:4]:
        print("       OFFPAGE %-22s firefox y=%-5d ours y=%d" % (w[:22], fy, oy))


def main():
    args = sys.argv[1:] or sorted(
        glob.glob(os.path.join(ROOT, "build", "webreal", "*", "index.html")))
    os.makedirs(OUT, exist_ok=True)
    print("=== web-shots: where the words are, ours against Firefox's ===")
    print("    pictures in %s" % OUT)
    ff = Firefox(WIDTH, HEIGHT)
    try:
        for p in args:
            name = os.path.basename(os.path.dirname(p)) or os.path.basename(p)
            try:
                rects = ff.text_rects(p)
                ff.screenshot(os.path.join(OUT, name + ".firefox.png"))
            except Exception as e:
                print("  %-12s reference failed: %s" % (name, e))
                continue
            our_shot(p, os.path.join(OUT, name + ".ours.png"))
            compare(name, rects, our_runs(p))
    finally:
        ff.close()


if __name__ == "__main__":
    main()
