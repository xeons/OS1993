#!/usr/bin/env python3
"""Check the VGA driver from the outside, with QEMU screendumps.

    G            the 640x480 desktop: check its pixels and palette
    N            a DOS session: the screen must be text mode
    Alt+Tab      back to the desktop: must be pixel-identical
    Alt+Tab, exit  the session ends: the desktop again, identical
    Esc, X       text console, leave OS1993
"""
import os
import socket
import subprocess
import sys
import time

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = os.path.join(root, "build")
qdir = os.path.join(build, "qemu")
img = os.path.join(build, "freedos.img")

WIN_X, WIN_Y, WIN_W, WIN_H, TITLE_H = 120, 90, 400, 260, 20   # desktop.c
EGA = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170), (170, 0, 0), (170, 0, 170),
       (170, 85, 0), (170, 170, 170), (85, 85, 85), (85, 85, 255), (85, 255, 85),
       (85, 255, 255), (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255)]

failures = 0


def check(ok, what):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {what}")
    failures += not ok


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:                       # P6 width height maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:]


def pixel(shot, x, y):
    w, _, px = shot
    i = (y * w + x) * 3
    return tuple(px[i:i + 3])


def near(rgb, want):
    """VGA DACs are 6-bit; emulators widen them to 8 bits slightly differently."""
    return all(abs(a - b) <= 4 for a, b in zip(rgb, want))


def main():
    os.makedirs(qdir, exist_ok=True)
    os.chdir(qdir)                               # short monitor socket path
    subprocess.run([os.path.join(root, "tools", "mkfreedos.sh"), "normal", "autorun"],
                   check=True, stdout=subprocess.DEVNULL)
    for f in ("monitor.sock", "SERIAL.LOG"):
        if os.path.exists(f):
            os.unlink(f)
    qemu = subprocess.Popen([
        "qemu-system-i386", "-M", "isapc", "-cpu", "486", "-m", "16",
        "-drive", f"file={img},format=raw,if=ide",
        "-serial", "file:SERIAL.LOG",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=4",
        "-display", "none", "-monitor", "unix:monitor.sock,server,nowait"])
    for _ in range(50):
        if os.path.exists("monitor.sock"):
            break
        time.sleep(0.1)
    mon = socket.socket(socket.AF_UNIX)
    mon.connect("monitor.sock")
    mon.setblocking(False)

    def cmd(c):
        mon.sendall((c + "\n").encode())
        try:
            while mon.recv(4096):
                pass
        except BlockingIOError:
            pass

    def key(k, pause=0.25):
        cmd(f"sendkey {k}")
        time.sleep(pause)

    def type_(text):
        for ch in text:
            key({" ": "spc", "\n": "ret"}.get(ch, ch))

    def wait_log(marker, count=1, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            try:
                with open("SERIAL.LOG", errors="replace") as f:
                    if f.read().count(marker) >= count:
                        return
            except FileNotFoundError:
                pass
            time.sleep(0.2)
        qemu.kill()
        raise SystemExit(f"  FAIL  timed out waiting for '{marker}' x{count}")

    def shot(name):
        path = os.path.join(qdir, name + ".ppm")
        if os.path.exists(path):
            os.unlink(path)
        time.sleep(0.7)                          # let the display settle
        cmd(f"screendump {path}")
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 100:
                time.sleep(0.2)
                return read_ppm(path)
            time.sleep(0.1)
        raise SystemExit(f"  FAIL  no screendump {name}")

    wait_log("VGA driver (text")
    time.sleep(0.5)

    key("g"); wait_log("GUI: desktop drawn")
    a = shot("desktop")
    check(a[:2] == (640, 480), f"desktop is 640x480 (got {a[0]}x{a[1]})")
    check(near(pixel(a, 5, 5), EGA[3]), "desktop background is cyan")
    check(near(pixel(a, WIN_X + 25, WIN_Y + 10), EGA[1]), "title bar is blue")
    check(near(pixel(a, WIN_X + WIN_W - 20, WIN_Y + 60), EGA[15]), "window client area is white")
    check(near(pixel(a, WIN_X + WIN_W + 2, WIN_Y + 100), EGA[0]), "window drop shadow")
    palette_ok = all(near(pixel(a, WIN_X + 16 + c * 23 + 10, WIN_Y + WIN_H - 40 + 10), EGA[c])
                     for c in range(16))
    check(palette_ok, "all 16 palette swatches have the VGA default colours")
    ty = WIN_Y + TITLE_H + 14
    ink = sum(near(pixel(a, x, y), EGA[0]) for y in range(ty, ty + 16) for x in range(WIN_X + 16, WIN_X + 280))
    check(ink > 200, f"text was drawn ({ink} black pixels in the first line)")

    key("n"); wait_log("Started DOS 1"); time.sleep(2)
    b = shot("session")
    check(b[:2] != (640, 480), f"DOS session is in text mode ({b[0]}x{b[1]})")

    key("alt-tab"); wait_log("focus -> console", 1)
    c = shot("back1")
    check(c == a, "desktop restored pixel-identical after Alt+Tab")

    key("alt-tab"); wait_log("focus -> DOS 1", 2); time.sleep(0.5)
    type_("exit\n"); wait_log("session in VM 1 ended"); wait_log("focus -> console", 2)
    d = shot("back2")
    check(d == a, "desktop restored pixel-identical after the session ended")

    key("esc"); time.sleep(1)
    e = shot("text")
    check(e[:2] != (640, 480), f"Esc returns to the text console ({e[0]}x{e[1]})")
    key("x", 1)
    try:
        qemu.wait(timeout=20)
    except subprocess.TimeoutExpired:
        qemu.kill()
        check(False, "QEMU powered off")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
