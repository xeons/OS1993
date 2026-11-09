"""Shared helpers for driving OS1993 under QEMU + FreeDOS from tests.

    with Machine() as m:          # boots build/freedos.img (autorun)
        m.wait_log("PS/2 mouse")
        m.key("g"); m.goto(100, 50); m.click()
        shot = m.screenshot("name")   # (width, height, rgb bytes)
"""
import os
import socket
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
QDIR = os.path.join(BUILD, "qemu")
IMG = os.path.join(BUILD, "freedos.img")

KEYS = {" ": "spc", "\n": "ret", ".": "dot", ",": "comma", "-": "minus", "/": "slash"}


def near(rgb, want, tol=4):
    """VGA DACs are 6-bit; emulators widen them to 8 bits slightly differently."""
    return all(abs(a - b) <= tol for a, b in zip(rgb, want))


def dac(v6):
    """Nominal 8-bit value of a 6-bit DAC component."""
    return round(v6 * 255 / 63)


def rgb(c):
    return tuple(dac(v) for v in c)


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


def to_png(shot, path):
    import struct
    import zlib
    w, h, px = shot
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Checker:
    def __init__(self):
        self.failures = 0

    def __call__(self, ok, what):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}")
        self.failures += not ok
        return ok


class Machine:
    def __init__(self, variant="normal", mode="autorun"):
        self.variant, self.mode = variant, mode

    def __enter__(self):
        os.makedirs(QDIR, exist_ok=True)
        os.chdir(QDIR)                              # short monitor socket path
        subprocess.run([os.path.join(ROOT, "tools", "mkfreedos.sh"), self.variant, self.mode],
                       check=True, stdout=subprocess.DEVNULL)
        for f in ("monitor.sock", "SERIAL.LOG"):
            if os.path.exists(f):
                os.unlink(f)
        self.qemu = subprocess.Popen([
            "qemu-system-i386", "-M", "isapc", "-cpu", "486", "-m", "16",
            "-drive", f"file={IMG},format=raw,if=ide", "-serial", "file:SERIAL.LOG",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=4",
            "-display", "none", "-monitor", "unix:monitor.sock,server,nowait"])
        for _ in range(50):
            if os.path.exists("monitor.sock"):
                break
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX)
        self.mon.connect("monitor.sock")
        self.mon.setblocking(False)
        return self

    def __exit__(self, *exc):
        if self.qemu.poll() is None:
            self.qemu.kill()
        return False

    def cmd(self, c, pause=0.25):
        self.mon.sendall((c + "\n").encode())
        time.sleep(pause)
        try:
            while self.mon.recv(4096):
                pass
        except BlockingIOError:
            pass

    def key(self, k, pause=0.25):
        self.cmd(f"sendkey {k}", pause)

    def type(self, text):
        for ch in text:
            self.key(KEYS.get(ch, ch))

    def log(self):
        try:
            with open(os.path.join(QDIR, "SERIAL.LOG"), errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def wait_log(self, marker, count=1, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            if self.log().count(marker) >= count:
                return True
            time.sleep(0.2)
        return False

    # QEMU's PS/2 model accumulates relative motion and drains it in small
    # packets; a new move is added to whatever hasn't been sent yet. So move
    # in modest steps and let each one drain.
    def move(self, dx, dy):
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            self.cmd(f"mouse_move {sx} {sy}", 0.15)
            dx -= sx
            dy -= sy
        time.sleep(0.3)

    # The kernel clamps the pointer to the screen, so pushing it into the
    # top-left corner gives an absolute reference.
    def goto(self, x, y):
        self.move(-700, -500)
        self.move(x, y)

    def click(self, pause=0.6):
        self.cmd("mouse_button 1", 0.3)
        self.cmd("mouse_button 0", pause)

    def drag(self, x0, y0, x1, y1):
        self.goto(x0, y0)
        self.cmd("mouse_button 1", 0.4)
        self.move(x1 - x0, y1 - y0)
        self.cmd("mouse_button 0", 0.8)

    def screenshot(self, name):
        path = os.path.join(QDIR, name + ".ppm")
        if os.path.exists(path):
            os.unlink(path)
        time.sleep(0.7)
        self.cmd(f"screendump {path}")
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 100:
                time.sleep(0.2)
                return read_ppm(path)
            time.sleep(0.1)
        raise RuntimeError(f"no screendump {name}")

    def wait_exit(self, timeout=20):
        try:
            self.qemu.wait(timeout=timeout)
            return True
        except subprocess.TimeoutExpired:
            self.qemu.kill()
            return False
