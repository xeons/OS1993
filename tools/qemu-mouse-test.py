#!/usr/bin/env python3
"""Check the PS/2 mouse driver and the GUI's use of it, through QEMU.

QEMU's monitor moves the emulated PS/2 mouse (mouse_move / mouse_button),
which produces real IRQ 12 packets for the VMM. Screendumps and the GUI's
debug log show where the cursor went and whether the window was dragged.
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

WIN_X, WIN_Y, WIN_W, WIN_H, TITLE_H = 120, 90, 400, 260, 20   # user/gui.c
CYAN, BLUE, WHITE, BLACK = (0, 168, 168), (0, 0, 168), (255, 255, 255), (0, 0, 0)
failures = 0


def check(ok, what):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {what}")
    failures += not ok


def near(rgb, want):
    return all(abs(a - b) <= 4 for a, b in zip(rgb, want))


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    return int(fields[1]), int(fields[2]), data[pos + 1:]


def pixel(shot, x, y):
    w, _, px = shot
    i = (y * w + x) * 3
    return tuple(px[i:i + 3])


def main():
    os.makedirs(qdir, exist_ok=True)
    os.chdir(qdir)
    subprocess.run([os.path.join(root, "tools", "mkfreedos.sh"), "normal", "autorun"],
                   check=True, stdout=subprocess.DEVNULL)
    for f in ("monitor.sock", "SERIAL.LOG"):
        if os.path.exists(f):
            os.unlink(f)
    qemu = subprocess.Popen([
        "qemu-system-i386", "-M", "isapc", "-cpu", "486", "-m", "16",
        "-drive", f"file={img},format=raw,if=ide", "-serial", "file:SERIAL.LOG",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=4",
        "-display", "none", "-monitor", "unix:monitor.sock,server,nowait"])
    for _ in range(50):
        if os.path.exists("monitor.sock"):
            break
        time.sleep(0.1)
    mon = socket.socket(socket.AF_UNIX)
    mon.connect("monitor.sock")
    mon.setblocking(False)

    def cmd(c, pause=0.3):
        mon.sendall((c + "\n").encode())
        time.sleep(pause)
        try:
            while mon.recv(4096):
                pass
        except BlockingIOError:
            pass

    def log():
        try:
            with open("SERIAL.LOG", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def wait_log(marker, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            if marker in log():
                return True
            time.sleep(0.2)
        return False

    def shot(name):
        path = os.path.join(qdir, name + ".ppm")
        if os.path.exists(path):
            os.unlink(path)
        time.sleep(0.7)
        cmd(f"screendump {path}")
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 100:
                time.sleep(0.2)
                return read_ppm(path)
            time.sleep(0.1)
        raise SystemExit(f"  FAIL  no screendump {name}")

    if not wait_log("PS/2 mouse"):
        raise SystemExit("  FAIL  VMM did not boot")
    check("MOUSE: PS/2 mouse on IRQ 12" in log(), "VMM found the PS/2 mouse")
    time.sleep(0.5)
    cmd("sendkey g")
    check(wait_log("GUI: mouse ready"), "GUI sees the mouse")

    # The cursor starts at (320,240), over the window's white client area:
    # the XOR arrow turns its tip black.
    a = shot("mouse0")
    check(near(pixel(a, 320, 240), BLACK) and near(pixel(a, 320, 250), BLACK),
          "XOR cursor drawn at its start position")

    cmd("mouse_move 100 0", 1)
    b = shot("mouse1")
    check(near(pixel(b, 320, 240), WHITE), "old cursor position erased")
    check(near(pixel(b, 420, 240), BLACK), "cursor moved right by 100 pixels")

    # Grab the title bar at (420,100), drag by (-60,-40), release.
    cmd("mouse_move 0 -140", 1)
    cmd("mouse_button 1", 0.5)
    check(wait_log("GUI: click at 420,100"), "click reported at 420,100")
    cmd("mouse_move -60 -40", 1)
    cmd("mouse_button 0", 0.5)
    moved = wait_log("GUI: window moved to 60,50")
    check(moved, "window dragged to 60,50")
    c = shot("mouse2")
    check(near(pixel(c, 60 + 30, 50 + 10), BLUE), "title bar drawn at the new position")
    check(near(pixel(c, WIN_X + WIN_W - 10, WIN_Y + 200), CYAN), "old window area repainted")

    cmd("sendkey esc", 1)
    cmd("sendkey x", 1)
    try:
        qemu.wait(timeout=20)
    except subprocess.TimeoutExpired:
        qemu.kill()
        check(False, "QEMU powered off (mouse released for DOS)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
