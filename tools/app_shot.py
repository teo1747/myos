#!/usr/bin/env python3
"""app_shot.py -- boot the OS, launch an app, photograph the screen, judge it.

A GUI app cannot report on itself. Its printf goes to the framebuffer, not the
serial line, so the usual headless trick -- run it and read what it said -- does
not work; and "I looked at it and it seemed fine" is the kind of evidence that
has been wrong twice in this repo already (a browser that rendered every page
at the wrong scale, a beep that was the right note and a fifth of the length).

So: boot headless with a QMP socket, drive the shell over serial to launch the
app, ask QEMU for a screendump, and MEASURE the result. The screenshot is kept
so a person can look, but the pass/fail does not depend on anyone looking.

  python3 tools/app_shot.py /data/apps/photos/photos.elf --name photos
  python3 tools/app_shot.py /data/apps/photos/photos.elf --name photos --keys 1,+,+

What it checks, in rising order of how much it tells you:
  * the app was spawned and did not fault (serial says so)
  * the screen is not a single flat colour  -- catches a window that never drew
  * a REGION changed against the bare desktop -- catches an app that drew
    nothing of its own, which a whole-screen check cannot see because the
    desktop already fills it with detail
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def sock_path(name, kind):
    return os.path.join(BUILD, "shot-%s-%s.sock" % (name, kind))


def boot(name, scratch):
    ser, qmp = sock_path(name, "ser"), sock_path(name, "qmp")
    for p in (ser, qmp):
        if os.path.exists(p):
            os.remove(p)
    # A COPY of the data image: these runs write to it, and a test that mutates
    # the tree it was given is a test you can only run once.
    subprocess.run(["cp", "-f", os.path.join(ROOT, "embkfs.img"), scratch],
                   check=True, cwd=ROOT)
    argv = [
        "qemu-system-x86_64", "-cpu", "max",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
        "-drive", "format=raw,file=%s,if=ide,index=1" % scratch,
        "-vga", "none", "-device", "virtio-vga,xres=1024,yres=768",
        "-display", "none",
        "-serial", "unix:%s,server,nowait" % ser,
        "-qmp", "unix:%s,server,nowait" % qmp,
        "-no-reboot", "-no-shutdown", "-m", "1024m", "-smp", "2",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(BUILD, "shot-%s-qemu.log" % name), "wb")
    return subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log), ser, qmp


def connect(path, tries=120):
    for _ in range(tries):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(path)
            return s
        except OSError:
            time.sleep(1)
    raise SystemExit("app-shot: the guest never opened %s" % path)


class Qmp:
    def __init__(self, path):
        self.s = connect(path)
        self.f = self.s.makefile("rwb")
        self.f.readline()                       # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:                             # skip asynchronous events
            line = self.f.readline()
            if not line:
                raise SystemExit("app-shot: QMP closed")
            reply = json.loads(line)
            if "event" not in reply:
                return reply

    def screendump(self, path):
        if os.path.exists(path):
            os.remove(path)
        self.cmd("screendump", filename=path)
        # screendump returns before the file is complete on some builds.
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return
            time.sleep(0.2)


def read_ppm(path):
    """P6 only -- what QEMU writes."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 PPM")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, _maxv = fields
    return w, h, data[pos:pos + w * h * 3]


def stats(w, h, px):
    """Distinct colours and the most common one -- a window that never drew
    leaves the screen at one flat colour, which this reports as 1."""
    seen = {}
    for i in range(0, len(px) - 2, 3 * 97):      # sample: exact counts are not the point
        c = px[i:i + 3]
        seen[c] = seen.get(c, 0) + 1
    top = max(seen.values()) if seen else 0
    return len(seen), top / float(sum(seen.values()) or 1)


def changed_fraction(a, b):
    """Fraction of sampled pixels that differ between two screendumps."""
    n = min(len(a), len(b))
    diff = tot = 0
    for i in range(0, n - 2, 3 * 31):
        tot += 1
        if abs(a[i] - b[i]) > 8 or abs(a[i + 1] - b[i + 1]) > 8 or abs(a[i + 2] - b[i + 2]) > 8:
            diff += 1
    return diff / float(tot or 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app")
    ap.add_argument("--name", default="app")
    ap.add_argument("--args", default="")
    ap.add_argument("--boot-wait", type=int, default=80)
    ap.add_argument("--settle", type=int, default=45)
    ap.add_argument("--keys", default="", help="comma-separated chars to type after launch")
    args = ap.parse_args()

    scratch = os.path.join(BUILD, "shot-%s.img" % args.name)
    print("=== app-shot: booting for %s" % args.app)
    q, ser_path, qmp_path = boot(args.name, scratch)
    before = os.path.join(BUILD, "shot-%s-desktop.ppm" % args.name)
    after = os.path.join(BUILD, "shot-%s.ppm" % args.name)
    seen = b""

    try:
        ser = connect(ser_path)
        ser.settimeout(1.0)

        def pump(seconds):
            nonlocal seen
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = ser.recv(4096)
                except socket.timeout:
                    continue
                if not b:
                    break
                seen += b

        pump(args.boot_wait)
        qmp = Qmp(qmp_path)
        qmp.screendump(before)                   # the desktop, for comparison

        cmd = ("run %s %s" % (args.app, args.args)).strip() + "\n"
        ser.sendall(cmd.encode())
        pump(args.settle)

        for k in [k for k in args.keys.split(",") if k]:
            ser.sendall(k.encode())
            pump(3)

        qmp.screendump(after)
    finally:
        q.terminate()
        try:
            q.wait(timeout=10)
        except subprocess.TimeoutExpired:
            q.kill()

    text = seen.decode("utf-8", "replace")
    for line in text.splitlines():
        low = line.lower()
        if any(w in low for w in ("page fault", "panic", "denied", "failed", args.name)):
            print("  guest: %s" % line.strip())

    fails = []
    # Specific tokens, not the substring "fault": the boot log says "default"
    # more than once, and a check that matches it reports a crash on every
    # successful run -- which is how a harness teaches you to ignore it.
    low = text.lower()
    if "page fault" in low or "kernel panic:" in low or "#pf" in low:
        fails.append("the guest faulted -- see the serial log above")
    if "failed to start" in low:
        fails.append("the app did not start -- wrong path, or it is not on the image")

    try:
        w, h, ap_px = read_ppm(after)
        _, _, de_px = read_ppm(before)
    except (OSError, ValueError) as e:
        print("=== app-shot: FAIL -- no usable screenshot (%s)" % e)
        return 1

    colours, dominant = stats(w, h, ap_px)
    moved = changed_fraction(de_px, ap_px)
    print("  %s: %dx%d, %d distinct colours, %.0f%% one colour, %.0f%% of the "
          "screen changed" % (os.path.basename(after), w, h, colours,
                              dominant * 100, moved * 100))

    if colours < 8:
        fails.append("the screen is nearly a flat colour -- nothing drew")
    if moved < 0.02:
        fails.append("the screen is unchanged from the bare desktop -- the app "
                     "drew nothing of its own")

    if fails:
        for f in fails:
            print("     FAIL: %s" % f)
        print("=== app-shot: FAIL (%d)" % len(fails))
        return 1
    print("=== app-shot: OK  (%s)" % after)
    return 0


if __name__ == "__main__":
    sys.exit(main())
