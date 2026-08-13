#!/usr/bin/env python3
"""audio_test.py -- boot the OS, make it play a tone, measure what came out.

The whole loop in one command, because a test that takes three manual steps is
a test that stops being run. It boots QEMU headless with an AC'97 attached and
`-audiodev wav`, drives the kernel's serial console to issue `test audio`, and
hands the resulting file to audio_check.py.

The serial is a UNIX SOCKET rather than a file: the harness has to WRITE the
command, and `-serial file:` is output only -- which is the small thing that
makes the difference between a test you can run and a screenshot you have to
interpret.

  make test-audio
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "audio-out.wav")
SOCK = os.path.join(ROOT, "build", "audio-ser.sock")
SCRATCH = os.path.join(ROOT, "build", "audio-persist.img")
HZ = 440


def boot():
    for p in (OUT, SOCK):
        if os.path.exists(p):
            os.remove(p)
    subprocess.run(["cp", "-f", os.path.join(ROOT, "embkfs.img"), SCRATCH],
                   check=True, cwd=ROOT)
    argv = [
        "qemu-system-x86_64", "-cpu", "max",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
        "-drive", "format=raw,file=%s,if=ide,index=1" % SCRATCH,
        "-device", "AC97,audiodev=snd0",
        "-audiodev", "wav,id=snd0,path=%s" % OUT,
        "-vga", "none", "-device", "virtio-vga,xres=800,yres=600", "-display", "none",
        "-serial", "unix:%s,server,nowait" % SOCK,
        "-no-reboot", "-no-shutdown", "-m", "521m", "-smp", "1",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(ROOT, "build", "audio-qemu.log"), "wb")
    return subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)


def drive(boot_wait, play_wait):
    s = None
    for _ in range(90):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(SOCK)
            break
        except OSError:
            time.sleep(1)
    if s is None:
        raise SystemExit("audio-test: the guest never opened its serial socket")

    s.settimeout(1.0)
    seen = b""

    def pump(seconds):
        nonlocal seen
        end = time.time() + seconds
        while time.time() < end:
            try:
                b = s.recv(4096)
            except socket.timeout:
                continue
            if not b:
                break
            seen += b

    pump(boot_wait)
    s.sendall(b"test audio\n")
    pump(play_wait)
    return seen.decode("utf-8", "replace")


def main():
    print("=== test-audio: booting with an AC'97 and a WAV sink")
    q = boot()
    try:
        out = drive(75, 120)
    finally:
        q.terminate()
        try:
            q.wait(timeout=10)
        except subprocess.TimeoutExpired:
            q.kill()

    for line in out.splitlines():
        if "audio" in line.lower() or "[cmd]" in line:
            print("  guest: %s" % line.strip())

    if "test audio: PLAYED" not in out:
        print("=== test-audio: FAIL -- the guest did not report playing")
        return 1

    # The guest saying PLAYED is NOT the result. This is.
    return subprocess.call([sys.executable,
                            os.path.join(ROOT, "tools", "audio_check.py"),
                            OUT, "--hz", str(HZ)], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
