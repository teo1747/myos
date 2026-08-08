#!/usr/bin/env python3
"""Render every page in the corpus and check what it claims about itself.

A page states its expectations in HTML comments, so the assertion lives beside
the markup it is about instead of in a table somewhere else:

    <!-- EXPECT-TEXT: DEEPLINK-A -->            a run containing this must exist
    <!-- EXPECT-NO-TEXT: lorem -->              ...and this one must not
    <!-- EXPECT-LINE: STORED dark count=2 -->   a whole LINE contains this
    <!-- EXPECT-COLOR: HEADING #e0604a -->      the run containing HEADING is that colour
    <!-- EXPECT-ORDER: FIRST SECOND -->         FIRST's run comes before SECOND's
    <!-- EXPECT-LEFT-OF: RANK TITLE -->         RANK's run starts left of TITLE's
    <!-- EXPECT-BELOW: LASTMARKER Item -->      first arg's run sits below the last of the second
    <!-- EXPECT-ZOOM: BIGTEXT 2.0 -->           at ZOOM=2 that run is ~2x taller and 2x further right

Every check is against the RESOLVED render -- position and computed colour --
not against the source, which is the only way to test a cascade.
"""
import gzip, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN  = os.path.join(ROOT, "build", "browser_render")


def runs_for(path, w=940, h=620):
    env = dict(os.environ, TEXTDUMP="1")
    r = subprocess.run([BIN, path, str(w), str(h), "0"],
                       capture_output=True, text=True, env=env, cwd=ROOT)
    runs, checks = [], 0
    for line in r.stdout.splitlines():
        if line.startswith("RUN|"):
            _, x, y, rw, rh, col, bg, text = line.split("|", 7)
            runs.append(dict(x=float(x), y=float(y), w=float(rw), h=float(rh),
                             color=col, bg=bg, text=text))
        elif line.startswith("*** "):
            checks += 1
            print("      " + line.strip())
    return runs, checks, r.returncode


def find(runs, needle):
    return [r for r in runs if needle in r["text"]]


def runs_at_zoom(path, zoom, w=940, h=620):
    env = dict(os.environ, TEXTDUMP="1", ZOOM=str(zoom))
    r = subprocess.run([BIN, path, str(w), str(h), "0"],
                       capture_output=True, text=True, env=env, cwd=ROOT)
    runs = []
    for line in r.stdout.splitlines():
        if line.startswith("RUN|"):
            _, x, y, rw, rh, col, bg, text = line.split("|", 7)
            runs.append(dict(x=float(x), y=float(y), w=float(rw), h=float(rh),
                             color=col, bg=bg, text=text))
    return runs


def find_count(path, needle, w=940, h=620):
    env = dict(os.environ, FIND=needle)
    r = subprocess.run([BIN, path, str(w), str(h), "0"],
                       capture_output=True, text=True, env=env, cwd=ROOT)
    for line in r.stdout.splitlines():
        if line.startswith("FIND|"):
            return int(line.rsplit("|", 1)[1])
    return -1


# --- the visual check ------------------------------------------------------
#
# Every expectation above is about words and where they are, and NONE of them
# caught the five defects that turned up the first time a real page was
# rendered to an image: centred table cells, gappy link text, welded words, an
# unpainted header, light-on-light text. Each changed how the page LOOKED
# without changing which words existed or their order.
#
# So the render is compared against a stored reference, pixel for pixel. A
# reference is a promise that the page looked right when someone last looked
# at it -- which is the only thing that can be checked automatically, and the
# reason `bless` prints a warning rather than being a silent flag.
REFS = os.path.join(ROOT, "tests", "web", "refs")
SHOT_W, SHOT_H = 940, 700


def render_ppm(path):
    out = os.path.join(ROOT, "build", "corpus_shot.ppm")
    if os.path.exists(out):
        os.remove(out)
    subprocess.run([BIN, path, str(SHOT_W), str(SHOT_H), "0", out],
                   capture_output=True, text=True, errors="replace", cwd=ROOT)
    return open(out, "rb").read() if os.path.exists(out) else None


def check_visual(path, bless):
    name = os.path.splitext(os.path.basename(path))[0]
    ref  = os.path.join(REFS, name + ".ppm.gz")
    got  = render_ppm(path)
    if got is None:
        print("      FAIL VISUAL: nothing rendered"); return 1
    if bless or not os.path.exists(ref):
        os.makedirs(REFS, exist_ok=True)
        with gzip.open(ref, "wb", compresslevel=9) as f:
            f.write(got)
        print("      (visual reference %s -- LOOK AT IT before trusting it)"
              % ("re-blessed" if bless else "created"))
        return 0
    with gzip.open(ref, "rb") as f:
        want = f.read()
    if want == got:
        return 0
    # Say WHERE, not just that. A count alone cannot tell a moved paragraph
    # from a changed colour.
    diff, top, bot = 0, None, None
    hdr_g = got.index(b"255\n") + 4
    hdr_w = want.index(b"255\n") + 4
    for y in range(SHOT_H):
        row = y * SHOT_W * 3
        a = want[hdr_w + row: hdr_w + row + SHOT_W * 3]
        b = got[hdr_g + row: hdr_g + row + SHOT_W * 3]
        if a != b:
            d = sum(1 for i in range(0, len(a), 3) if a[i:i+3] != b[i:i+3])
            diff += d
            if top is None: top = y
            bot = y
    bad = os.path.join(ROOT, "build", name + "-actual.ppm")
    open(bad, "wb").write(got)
    print("      FAIL VISUAL: %d px differ, rows %s-%s; actual written to %s"
          % (diff, top, bot, os.path.relpath(bad, ROOT)))
    print("      (if the change is intended: make web-corpus BLESS=1)")
    return 1


def check_page(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    expects = re.findall(r"<!--\s*(EXPECT-[A-Z-]+):\s*(.*?)\s*-->", src)
    m = re.search(r"<!--\s*CORPUS-WIDTH:\s*(\d+)\s*-->", src)
    width = int(m.group(1)) if m else 940
    runs, harness_fails, rc = runs_for(path, w=width)
    fails = harness_fails

    for kind, arg in expects:
        parts = arg.split()
        if kind == "EXPECT-TEXT":
            if not find(runs, arg):
                print("      FAIL %s: no run contains %r" % (kind, arg)); fails += 1
        elif kind == "EXPECT-NO-TEXT":
            if find(runs, arg):
                print("      FAIL %s: a run contains %r" % (kind, arg)); fails += 1
        elif kind == "EXPECT-LINE":
            # Runs are WORDS, so a sentence spans several. Join them by row --
            # which is also the only way to assert on text a script composed,
            # since the words it produced were never adjacent in the source.
            rows = {}
            for r in runs:
                rows.setdefault(round(r["y"], 1), []).append((r["x"], r["text"]))
            joined = ["".join(t for _, t in sorted(v)) for v in rows.values()]
            if not any(arg in " ".join(line.split()) for line in joined):
                print("      FAIL %s: no line contains %r" % (kind, arg)); fails += 1
        elif kind == "EXPECT-COLOR":
            hit = find(runs, parts[0])
            if not hit:
                print("      FAIL %s: no run contains %r" % (kind, parts[0])); fails += 1
            elif hit[0]["color"].lower() != parts[1].lower():
                print("      FAIL %s: %s is %s, expected %s"
                      % (kind, parts[0], hit[0]["color"], parts[1])); fails += 1
        elif kind == "EXPECT-ORDER":
            a, b = find(runs, parts[0]), find(runs, parts[1])
            if not a or not b:
                print("      FAIL %s: missing %r or %r" % (kind, parts[0], parts[1])); fails += 1
            elif (a[0]["y"], a[0]["x"]) >= (b[0]["y"], b[0]["x"]):
                print("      FAIL %s: %s does not precede %s" % (kind, parts[0], parts[1])); fails += 1
        elif kind == "EXPECT-LEFT-OF":
            a, b = find(runs, parts[0]), find(runs, parts[1])
            if not a or not b:
                print("      FAIL %s: missing %r or %r" % (kind, parts[0], parts[1])); fails += 1
            elif a[0]["x"] >= b[0]["x"]:
                print("      FAIL %s: %s (x=%.0f) is not left of %s (x=%.0f)"
                      % (kind, parts[0], a[0]["x"], parts[1], b[0]["x"])); fails += 1
        elif kind == "EXPECT-FIND":
            # The needle may contain spaces -- "operating system" is the whole
            # point of the feature -- so the COUNT is the last token and
            # everything before it is what to look for.
            want = int(parts[-1])
            needle = " ".join(parts[:-1])
            got = find_count(path, needle, w=width)
            if got != want:
                print("      FAIL %s: %r found %d times, expected %d"
                      % (kind, needle, got, want)); fails += 1
        elif kind == "EXPECT-X":
            hit = find(runs, parts[0])
            lo, hi = float(parts[1]), float(parts[2])
            if not hit:
                print("      FAIL %s: no run contains %r" % (kind, parts[0])); fails += 1
            elif not (lo <= hit[0]["x"] <= hi):
                print("      FAIL %s: %s at x=%.1f, expected %.0f..%.0f"
                      % (kind, parts[0], hit[0]["x"], lo, hi)); fails += 1
        elif kind == "EXPECT-BELOW":
            a, b = find(runs, parts[0]), find(runs, parts[1])
            if not a or not b:
                print("      FAIL %s: missing %r or %r" % (kind, parts[0], parts[1])); fails += 1
            else:
                lowest = max(r["y"] for r in b)
                if a[0]["y"] <= lowest:
                    print("      FAIL %s: %s at y=%.0f is not below %s (lowest y=%.0f)"
                          % (kind, parts[0], a[0]["y"], parts[1], lowest)); fails += 1
        elif kind == "EXPECT-ZOOM":
            # Render the SAME page twice at different zooms and compare. A
            # single render can say nothing about a scale factor -- whatever
            # zoom did, the numbers at 1.0 would look identical.
            z = float(parts[1])
            zr = runs_at_zoom(path, z, w=width)
            a, b = find(runs, parts[0]), find(zr, parts[0])
            if not a or not b:
                print("      FAIL %s: no run contains %r at one of the zooms"
                      % (kind, parts[0])); fails += 1
            else:
                got = b[0]["h"] / a[0]["h"] if a[0]["h"] else 0.0
                # Glyph heights land on integers, so the ratio is close but not
                # exact -- 10%% is tight enough to catch "zoom did nothing"
                # (ratio 1.0) and loose enough to survive rounding.
                if abs(got - z) > z * 0.10:
                    print("      FAIL %s: %s scaled %.2fx, expected %.2fx"
                          % (kind, parts[0], got, z)); fails += 1
                # ...and the box around it grew too, or the page is big type
                # in boxes that stayed small.
                elif b[0]["x"] <= a[0]["x"] and a[0]["x"] > 2:
                    print("      FAIL %s: %s at x=%.0f did not move right (was %.0f)"
                          % (kind, parts[0], b[0]["x"], a[0]["x"])); fails += 1
        else:
            print("      FAIL: unknown expectation %s" % kind); fails += 1

    fails += check_visual(path, os.environ.get("BLESS"))

    print("  %-22s %4dpx %3d runs, %2d expectations, %s"
          % (os.path.basename(path), width, len(runs), len(expects),
             "OK" if fails == 0 else "%d FAILED" % fails))
    return fails


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "web")
    pages = sorted(p for p in os.listdir(d) if p.endswith(".html"))
    if not pages:
        print("no pages in %s" % d); return 1
    print("=== web corpus: %s (%d pages) ===" % (d, len(pages)))
    total = sum(check_page(os.path.join(d, p)) for p in pages)
    print("=== web-corpus: %s ===" % ("OK (0 failures)" if total == 0 else "%d FAILURE(S)" % total))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
