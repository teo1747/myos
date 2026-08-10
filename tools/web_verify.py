#!/usr/bin/env python3
"""web_verify.py -- what a reader would see, against what we drew.

The corpus checks what somebody thought to assert. This asks the question that
actually drives a browser forward and needs nobody to write an expectation
first: given a page, what is on it that we are not showing -- and what are we
showing that should not be there?

The reference is FIREFOX, driven over WebDriver, asked for one thing:
`document.body.innerText`. That is the visible text and only the visible text --
a menu behind a dropdown, a mobile header at desktop width and a <noscript>
fallback are all excluded by the same rule a reader's eyes use. Comparing
against the DOCUMENT instead (which is where this started) reports every hidden
menu as content we lost, and MDN's reference pages score 32% for rendering
perfectly.

Two directions, because both are bugs:

  MISSING   visible to a reader, absent from our render. Content the reader
            lost -- the worst thing a browser can do.
  EXTRA     in our render, not visible to a reader. Content we revealed that
            the page hid: this is what a mishandled <noscript>, an ignored
            :focus-within, or a media query we could not parse looks like from
            the outside.

Firefox is not consulted about STYLE. Our fonts, colours and spacing are our
own and always will be; this is only about what the page amounts to.

  python3 tools/web_verify.py [page.html ...]     (default: build/webreal/*)
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# WHICH RENDERER. Vellum's host harness by default; RENDERER=... points it at
# the NetSurf port. Both take (doc, w, h, ...) and both emit RUN| under
# TEXTDUMP, which is what lets one instrument score two engines.
#
# THE TWO SCORES ARE NOT COMPARABLE AS THEY STAND, and it is the instrument's
# fault rather than either engine's. Vellum's harness walks the laid-out SCENE
# and reports every text node in the document; NetSurf plots only what falls
# inside the viewport it was given, so anything below the fold is never drawn
# and therefore never counted. Wikipedia at 1100x7200 emits 1225 runs and is
# still taller than that -- which is most of the 51.6% it scores here.
#
# Fixing it means asking the core for the document height
# (browser_window_get_extents) and sizing the surface to it before redrawing,
# in ports/netsurf/frontend/main.c. Until then, read a NetSurf number as
# "of the text in the first 7200 pixels", and do not put the two engines in
# the same table.
BIN = os.environ.get("RENDERER") or os.path.join(ROOT, "build", "browser_render")
WIDTH, HEIGHT = 1100, 900

# Words our own chrome draws, which are not the page's.
CHROME = {"vellum", "open"}
WORD = re.compile(r"[^\W\d_][\w'À-ɏ-]{2,}", re.UNICODE)


def words(text):
    return [w.lower() for w in WORD.findall(text or "") if w.lower() not in CHROME]


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ff_driver import Firefox                                   # noqa: E402


def our_words(path):
    env = dict(os.environ, TEXTDUMP="1")
    try:
        last = "/dev/null" if "nsemblink" in os.path.basename(BIN) else "0"
        r = subprocess.run([BIN, path, str(WIDTH), str(HEIGHT * 8), last],
                           capture_output=True, text=True, errors="replace",
                           env=env, cwd=ROOT, timeout=300)
    except subprocess.TimeoutExpired:
        return None
    out = []
    for line in r.stdout.splitlines():
        if line.startswith("RUN|"):
            f = line.split("|")
            if len(f) >= 8:
                out += words(f[7])
    return out


def verify(ff, path):
    name = os.path.basename(os.path.dirname(path)) or os.path.basename(path)
    try:
        ref = words(ff.visible_text(path))
    except Exception as e:
        print("  %-12s reference failed: %s" % (name, e))
        return
    ours = our_words(path)
    if ours is None:
        print("  %-12s RENDER TIMED OUT" % name)
        return
    rs, oset = set(ref), set(ours)
    # WORD BOUNDARIES DIFFER, and that is not a bug in either renderer.
    # innerText concatenates adjacent inline runs -- Firefox reports Google's
    # header as one word "GmailImages" where we (correctly) draw two -- and
    # Wikipedia's table of contents comes back as
    # "managementinterruptsmemory". Reported as missing content, that noise
    # buries the real findings. So a word that is not in the other's word set
    # is checked against its LETTERS before being called missing.
    ours_run = "".join(ours)
    ref_run = "".join(ref)
    missing = [w for w in dict.fromkeys(ref) if w not in oset and w not in ours_run]
    extra = [w for w in dict.fromkeys(ours) if w not in rs and w not in ref_run]
    cover = 100.0 * (len(rs) - len(missing)) / len(rs) if rs else 100.0
    print("  %-12s see %5.1f%% of %4d visible words   (%d missing, %d extra)" %
          (name, cover, len(rs), len(missing), len(extra)))
    if missing:
        print("       MISSING: " + " ".join(missing[:12]) +
              (" ... +%d" % (len(missing) - 12) if len(missing) > 12 else ""))
    if extra:
        print("       EXTRA:   " + " ".join(extra[:12]) +
              (" ... +%d" % (len(extra) - 12) if len(extra) > 12 else ""))


def main():
    args = sys.argv[1:] or sorted(
        glob.glob(os.path.join(ROOT, "build", "webreal", "*", "index.html")))
    print("=== web-verify: our render against what Firefox says is visible ===")
    ff = Firefox()
    try:
        for p in args:
            verify(ff, p)
    finally:
        ff.close()


if __name__ == "__main__":
    main()
