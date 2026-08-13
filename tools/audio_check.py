#!/usr/bin/env python3
"""audio_check.py -- did the OS actually make the sound it said it did?

A driver that initialises cleanly, reports success and emits silence is the
normal failure mode of audio hardware, not an exotic one: an AC'97 codec powers
up muted, bus mastering can be off, a descriptor length can be in the wrong
unit, and every register still reads back correctly through all of it. "It
printed PLAYED" is not evidence, and neither is somebody saying it sounded
about right.

So QEMU is run with `-audiodev wav,path=...`, which writes what the guest
played to a file on the BUILD MACHINE, and this reads that file back and
measures it:

  * how much of it is not silence  -- catches the muted codec and the DMA that
                                      never ran
  * the fundamental frequency      -- catches a wrong sample rate, a descriptor
                                      length in bytes where samples were meant,
                                      and channels swapped into mono
  * peak amplitude                 -- catches clipping and a volume of zero

Zero-crossing counting rather than an FFT, because the test signal is a square
wave with a known fundamental: no numpy, no dependency, and the arithmetic is
inspectable. A tone that is half the expected frequency is the classic
"samples vs frames" bug and this reports it as a number rather than as a
wrong-sounding beep.

  python3 tools/audio_check.py build/audio-out.wav --hz 440
"""
import struct
import sys


def read_wav(path):
    """Minimal RIFF/WAVE reader: enough for what QEMU writes, and explicit
    about what it refuses rather than silently misreading it."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 44 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")

    pos, fmt, frames = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8: pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _brate, _align, bits = struct.unpack_from("<HHIIHH", body, 0)
            fmt = {"tag": tag, "channels": ch, "rate": rate, "bits": bits}
        elif cid == b"data":
            frames = body
        pos += 8 + size + (size & 1)          # chunks are word-aligned

    if fmt is None or frames is None:
        raise ValueError("missing fmt or data chunk")
    if fmt["bits"] != 16 or fmt["tag"] != 1:
        raise ValueError("expected 16-bit PCM, got %d-bit tag %d"
                         % (fmt["bits"], fmt["tag"]))
    return fmt, frames


def analyse(fmt, raw):
    ch = max(1, fmt["channels"])
    n = len(raw) // 2
    samples = struct.unpack("<%dh" % n, raw[:n * 2])
    left = samples[0::ch]                     # one channel is enough to measure

    peak = max((abs(s) for s in left), default=0)
    nonzero = sum(1 for s in left if abs(s) > 64)

    # Zero crossings, with a hysteresis band so a silent or noisy stretch does
    # not manufacture a frequency out of dither.
    gate = max(200, peak // 4)
    crossings, state = 0, 0
    for s in left:
        if state <= 0 and s > gate:
            state = 1
            crossings += 1
        elif state >= 0 and s < -gate:
            state = -1
            crossings += 1
    seconds = len(left) / float(fmt["rate"]) if fmt["rate"] else 0.0
    hz = (crossings / 2.0) / seconds if seconds > 0 else 0.0
    return {
        "channels": ch, "rate": fmt["rate"], "frames": len(left),
        "seconds": seconds, "peak": peak,
        "nonsilent": nonzero / float(len(left)) if left else 0.0,
        "hz": hz,
    }


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0] if args else "build/audio-out.wav"
    want_hz = 440.0
    if "--hz" in sys.argv:
        want_hz = float(sys.argv[sys.argv.index("--hz") + 1])

    try:
        fmt, raw = read_wav(path)
    except (OSError, ValueError) as e:
        print("=== audio-check: FAIL -- %s (%s)" % (e, path))
        return 1

    a = analyse(fmt, raw)
    print("  %s: %d ch, %d Hz, %.2f s, peak %d, %.0f%% non-silent, measured %.1f Hz"
          % (path, a["channels"], a["rate"], a["seconds"], a["peak"],
             a["nonsilent"] * 100, a["hz"]))

    fails = []
    if a["seconds"] < 0.05:
        fails.append("almost nothing was played (%.3fs)" % a["seconds"])
    if a["nonsilent"] < 0.5:
        fails.append("mostly SILENCE (%.0f%% non-silent) -- muted codec, or the "
                     "DMA never ran" % (a["nonsilent"] * 100))
    if a["peak"] < 1000:
        fails.append("peak amplitude %d is inaudible" % a["peak"])
    if want_hz > 0:
        # 10%: generous enough for the zero-crossing estimate and the tail of a
        # partly-filled buffer, tight enough that half or double the frequency
        # -- the samples/frames confusion -- cannot pass.
        if abs(a["hz"] - want_hz) > want_hz * 0.10:
            fails.append("expected ~%.0f Hz, measured %.1f Hz%s"
                         % (want_hz, a["hz"],
                            "  (half: samples vs frames?)"
                            if abs(a["hz"] - want_hz / 2) < want_hz * 0.1 else
                            "  (double: stereo counted as mono?)"
                            if abs(a["hz"] - want_hz * 2) < want_hz * 0.2 else ""))

    if fails:
        for f in fails:
            print("     FAIL: %s" % f)
        print("=== audio-check: FAIL (%d)" % len(fails))
        return 1
    print("=== audio-check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
