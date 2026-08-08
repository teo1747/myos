#!/usr/bin/env python3
"""Fetch real web pages and render them, so the browser is measured against
pages nobody here wrote.

The corpus in tests/web is written by the same person who writes the engine,
which can only confirm what was already understood. This fetches actual sites
-- with the stylesheets they link, because most of the web's CSS is in separate
files and a page rendered without them is not that page -- and reports what
each one costs and what broke.

    make web-real            fetch (if needed) and triage
    make web-real SHOTS=1    ...and write a PNG of each, to LOOK at

The pages are NOT committed. They are someone else's content, they go stale,
and a corpus that cannot be refetched is a corpus nobody can check. What is
committed is this file, so the run is repeatable.

The only thing rewritten in a fetched page is each <link> href and each <img>
src, which become the local filenames those files were saved under. Markup,
CSS and image bytes are exactly what the server sent -- otherwise the triage
measures our edits.

Images matter more than they look: a browser that has never been seen
rendering one has a whole dimension nobody has checked. The renderer decodes
PNG and JPEG by SIGNATURE, so what a site actually serves -- increasingly WebP
and AVIF -- is reported here rather than silently rendering as nothing.

Some sites answer a plain fetch with a bot-check page rather than content.
That is not a rendering bug, and the run says so rather than counting it as
one: an "enable JavaScript" interstitial parses to about forty nodes and would
otherwise read as a catastrophic parse failure.
"""
import html as htmlmod
import os, re, subprocess, sys, glob
from urllib.parse import urljoin

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN  = os.path.join(ROOT, "build", "browser_render")
UA   = "Mozilla/5.0 (X11; Linux x86_64) Vellum/1.0"

# Chosen to span what the web actually is: hand-written HTML, a table-layout
# news site, documentation, a framework-built marketing page, and a couple of
# giants that will not fit and should say so.
SITES = [
    ("example",    "https://example.com/"),
    ("danluu",     "https://danluu.com/"),
    ("suckless",   "https://suckless.org/"),
    ("sqlite",     "https://www.sqlite.org/index.html"),
    ("nginx",      "https://nginx.org/en/"),
    ("xkcd",       "https://xkcd.com/"),
    ("kernelorg",  "https://www.kernel.org/"),
    ("hackernews", "https://news.ycombinator.com/"),
    ("lobsters",   "https://lobste.rs/"),
    ("gnu",        "https://www.gnu.org/"),
    ("rustlang",   "https://www.rust-lang.org/"),
    ("pythonorg",  "https://www.python.org/"),
    ("wikipedia",  "https://en.wikipedia.org/wiki/Operating_system"),
    ("mdn",        "https://developer.mozilla.org/en-US/docs/Web/CSS/flex"),
    ("bbc",        "https://www.bbc.com/news"),
    ("craigslist", "https://sfbay.craigslist.org/"),
    ("github",     "https://github.com/"),
]

BOT_CHECK = ("enable javascript and cookies", "just a moment",
             "cf-browser-verification", "challenge-platform")


def get(url, timeout=25):
    # --compressed, or a server that gzips unconditionally hands back bytes
    # that look exactly like a parser bug. That cost an hour once.
    r = subprocess.run(["curl", "-sL", "--compressed", "--max-time", str(timeout),
                        "-A", UA, url], capture_output=True)
    return r.stdout if r.returncode == 0 else b""


def fetch(name, url, root):
    d = os.path.join(root, name)
    os.makedirs(d, exist_ok=True)
    html = get(url)
    if not html:
        print("  %-12s FETCH FAILED" % name)
        return
    txt = html.decode("utf-8", "replace")
    n = 0
    for m in re.finditer(r'<link\b[^>]*>', txt, re.I):
        tag = m.group(0)
        if not re.search(r'rel\s*=\s*["\']?stylesheet', tag, re.I):
            continue
        h = re.search(r'href\s*=\s*["\']([^"\']+)["\']', tag, re.I)
        if not h:
            continue
        # UNESCAPE the href. Every `&` in a query string is written `&amp;` in
        # HTML, and requesting it literally gives the server one parameter with
        # the rest of the query inside its value -- MediaWiki replies with an
        # empty stylesheet, and the page renders unstyled for reasons that look
        # nothing like a fetch problem.
        css = get(urljoin(url, htmlmod.unescape(h.group(1))), 20)
        if not css:
            continue
        fn = "sheet%d.css" % n
        n += 1
        open(os.path.join(d, fn), "wb").write(css)
        txt = txt.replace(tag, tag.replace(h.group(1), fn), 1)

    # ...and the pictures. Capped: a news front page can carry a hundred, and
    # the point is to SEE the layout with images in it, not to mirror the site.
    imgs, kinds = 0, {}
    for m in list(re.finditer(r'<img\b[^>]*>', txt, re.I))[:24]:
        tag = m.group(0)
        h = re.search(r'\bsrc\s*=\s*["\']([^"\']+)["\']', tag, re.I)
        if not h or h.group(1).startswith("data:"):
            continue
        raw = get(urljoin(url, htmlmod.unescape(h.group(1))), 20)
        if not raw:
            continue
        kind = ("png"  if raw[:8] == b"\x89PNG\r\n\x1a\n" else
                "jpeg" if raw[:2] == b"\xff\xd8" else
                "gif"  if raw[:3] == b"GIF" else
                "webp" if raw[:4] == b"RIFF" and raw[8:12] == b"WEBP" else
                "avif" if raw[4:12] == b"ftypavif" else
                "svg"  if b"<svg" in raw[:400].lower() else "?")
        kinds[kind] = kinds.get(kind, 0) + 1
        fn = "img%d.%s" % (imgs, kind if kind != "?" else "bin")
        imgs += 1
        open(os.path.join(d, fn), "wb").write(raw)
        txt = txt.replace(tag, tag.replace(h.group(1), fn), 1)

    open(os.path.join(d, "index.html"), "w", encoding="utf-8").write(txt)
    fmt = " ".join("%s=%d" % kv for kv in sorted(kinds.items()))
    print("  %-12s %8d bytes, %d sheet(s), %d image(s) %s%s" % (name, len(html), n, imgs, fmt,
          "   [BOT CHECK, not a page]" if any(p in txt.lower() for p in BOT_CHECK) else ""))


def triage(root, shots):
    print("\n%-12s %6s %6s %5s %6s %8s  %s" %
          ("site", "bytes", "nodes", "css", "runs", "ms/frame", "notes"))
    for name in [s[0] for s in SITES]:
        p = os.path.join(root, name, "index.html")
        if not os.path.exists(p):
            continue
        src = open(p, encoding="utf-8", errors="replace").read().lower()
        if any(b in src for b in BOT_CHECK):
            print("  %-12s (bot check -- not a page, skipped)" % name)
            continue
        args = [BIN, p, "1100", "900", "0"]
        if shots:
            os.makedirs(os.path.join(root, "shots"), exist_ok=True)
            args.append(os.path.join(root, "shots", name + ".ppm"))
        env = dict(os.environ, TEXTDUMP="1")
        try:
            r = subprocess.run(args, capture_output=True, text=True,
                               errors="replace", env=env, cwd=ROOT, timeout=180)
        except subprocess.TimeoutExpired:
            print("  %-12s TIMEOUT" % name)
            continue
        out = r.stdout
        m = re.search(r"(\d+) bytes -> (\d+) nodes(\s*\(TRUNCATED\))?, root (-?\d+), "
                      r"(\d+) css rule\w*(\s*\(TRUNCATED\))?", out)
        b, nd, ntr, _root, css, ctr = m.groups() if m else ("?", "?", None, "?", "?", None)
        runs = sum(1 for l in out.splitlines() if l.startswith("RUN|"))
        ms = re.search(r"([\d.]+) ms per build\+layout", out)
        notes = []
        if ntr: notes.append("ARENA-FULL")
        if ctr: notes.append("CSS-FULL")
        if runs == 0: notes.append("NOTHING RENDERED")
        for w in re.findall(r"\*\*\* (.+?) \*\*\*", out):
            if "CHECK(S) FAILED" in w or "blit path was never" in w:
                continue
            notes.append(w)
        print("  %-12s %6s %6s %5s %6d %8s  %s" %
              (name, b, nd, css, runs, ms.group(1) if ms else "?", ", ".join(notes)))


if __name__ == "__main__":
    root  = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "webreal")
    shots = os.environ.get("SHOTS")
    os.makedirs(root, exist_ok=True)
    if not glob.glob(os.path.join(root, "*", "index.html")):
        print("fetching %d sites into %s" % (len(SITES), root))
        for nm, u in SITES:
            fetch(nm, u, root)
    else:
        print("using the pages already in %s (delete it to refetch)" % root)
    triage(root, shots)
