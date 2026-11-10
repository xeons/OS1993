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


def vga_rom_dir():
    """QEMU's ISA VGA (with the Bochs VBE registers) loads vgabios.bin, which
    distributions ship as vgabios-isavga.bin: a directory that has it."""
    d = os.path.join(BUILD, "qemu-rom")
    os.makedirs(d, exist_ok=True)
    link = os.path.join(d, "vgabios.bin")
    if not os.path.exists(link):
        for base in ("/usr/share/qemu", "/usr/share/qemu-firmware", "/usr/share/seabios"):
            src = os.path.join(base, "vgabios-isavga.bin")
            if os.path.exists(src):
                os.symlink(src, link)
                break
    return d
IMG = os.path.join(BUILD, "freedos.img")

KEYS = {" ": "spc", "\n": "ret", ".": "dot", ",": "comma", "-": "minus", "/": "slash",
        "\t": "tab", "=": "equal", "+": "shift-equal", "*": "shift-8", "%": "shift-5", "@": "shift-2"}


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
    def __init__(self, variant="normal", mode="autorun", vmport=False):
        self.variant, self.mode, self.vmport = variant, mode, vmport

    def __enter__(self):
        os.makedirs(QDIR, exist_ok=True)
        os.chdir(QDIR)                              # short monitor socket path
        subprocess.run([os.path.join(ROOT, "tools", "mkfreedos.sh"), self.variant, self.mode],
                       check=True, stdout=subprocess.DEVNULL)
        for f in ("monitor.sock", "qmp.sock", "SERIAL.LOG"):
            if os.path.exists(f):
                os.unlink(f)
        rom = vga_rom_dir()
        self.qemu = subprocess.Popen([
            # vmport is on by default for isapc; off keeps the plain relative
            # PS/2 path that mouse_move can drive.
            "qemu-system-i386", "-M", "isapc,vmport=on" if self.vmport else "isapc,vmport=off",
            "-cpu", "486", "-m", "16", "-vga", "std", "-L", rom, "-L", "/usr/share/qemu",
            "-drive", f"file={IMG},format=raw,if=ide", "-serial", "file:SERIAL.LOG",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=4",
            "-display", "none", "-monitor", "unix:monitor.sock,server,nowait",
            "-qmp", "unix:qmp.sock,server,nowait"])
        for _ in range(50):
            if os.path.exists("monitor.sock"):
                break
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX)
        self.mon.connect("monitor.sock")
        self.mon.setblocking(False)
        self.qmp = socket.socket(socket.AF_UNIX)
        for _ in range(50):
            try:
                self.qmp.connect("qmp.sock")
                break
            except OSError:
                time.sleep(0.1)
        self.qmp_file = self.qmp.makefile("rw")
        self.qmp_file.readline()                    # greeting
        self.qmp_cmd({"execute": "qmp_capabilities"})
        return self

    def qmp_cmd(self, msg):
        import json
        self.qmp_file.write(json.dumps(msg) + "\n")
        self.qmp_file.flush()
        while True:                                 # skip async events
            reply = json.loads(self.qmp_file.readline())
            if "return" in reply or "error" in reply:
                return reply

    # An absolute pointer position (needs vmport=True: the VMware mouse).
    def abs_move(self, x, y):
        self.qmp_cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": x * 0x7FFF // 639}},
            {"type": "abs", "data": {"axis": "y", "value": y * 0x7FFF // 479}}]}})
        time.sleep(0.4)

    def abs_click(self, pause=0.6):
        for down in (True, False):
            self.qmp_cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": down, "button": "left"}}]}})
            time.sleep(0.3 if down else pause)

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

    # The GUI session: the shell, the Control Demo from its Start menu, and
    # Start > Shut Down at the end.
    def start_gui(self):
        self.key("g")
        return self.wait_log("GUI: shell ready")

    def start_demo(self, n=1):
        for k in ("ctrl-esc", "p", "d"):
            self.key(k)
        return self.wait_log("GUI: desktop ready", n) and self.wait_log("GUI: mouse ready", n)

    def shut_down(self):
        for k in ("ctrl-esc", "u", "y"):
            self.key(k)
        return self.wait_log("USER: SHELL.BIN exited with code 0")

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

    def rclick(self, pause=0.6):
        self.cmd("mouse_button 2", 0.3)
        self.cmd("mouse_button 0", pause)

    def dblclick(self, pause=0.6):
        for b in ("1", "0", "1"):
            self.cmd(f"mouse_button {b}", 0.08)
        self.cmd("mouse_button 0", pause)

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
