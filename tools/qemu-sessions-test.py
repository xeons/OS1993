#!/usr/bin/env python3
"""Drive two interactive DOS sessions in OS1993 under QEMU + FreeDOS.

Keys go in through QEMU's monitor (sendkey), like a user typing:

    N  mkdir one          session 1, left sitting at its prompt
    Alt+Tab, N  mkdir two  exit      session 2 while session 1 is idle in DOS
    Alt+Tab  exit         back to session 1
    X                     leave OS1993 (FDAUTO then powers QEMU off)

Pass if both directories exist on the disk afterwards.
"""
import os
import socket
import subprocess
import sys
import time

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = os.path.join(root, "build")
img = os.path.join(build, "freedos.img")
sock_path = os.path.join(build, "qemu", "monitor.sock")

KEYS = {" ": "spc", "\n": "ret"}


def main():
    os.makedirs(os.path.dirname(sock_path), exist_ok=True)
    subprocess.run([os.path.join(root, "tools", "mkfreedos.sh"), "normal", "autorun"], check=True)
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    qemu = subprocess.Popen([
        "qemu-system-i386", "-M", "isapc", "-cpu", "486", "-m", "16",
        "-drive", f"file={img},format=raw,if=ide",
        "-serial", f"file:{build}/qemu/SERIAL.LOG",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=4",
        "-display", "none", "-monitor", f"unix:{sock_path},server,nowait"])

    for _ in range(50):
        if os.path.exists(sock_path):
            break
        time.sleep(0.1)
    mon = socket.socket(socket.AF_UNIX)
    mon.connect(sock_path)

    def key(k, pause=0.25):
        mon.sendall(f"sendkey {k}\n".encode())
        time.sleep(pause)

    def type_(text):
        for ch in text:
            key(KEYS.get(ch, ch))

    def wait_log(marker, timeout=30):
        """Wait until the kernel's COM1 log shows marker."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                with open(f"{build}/qemu/SERIAL.LOG", errors="replace") as f:
                    if marker in f.read():
                        return
            except FileNotFoundError:
                pass
            time.sleep(0.2)
        qemu.kill()
        raise SystemExit(f"  FAIL  timed out waiting for '{marker}'")

    wait_log("V86 self-test passed")     # FreeDOS booted, VMM up
    time.sleep(0.5)
    key("n"); wait_log("Started DOS 1"); time.sleep(1.5)
    type_("mkdir one\n"); time.sleep(1.5)
    key("alt-tab", 1); key("n"); wait_log("Started DOS 2"); time.sleep(1.5)
    type_("mkdir two\n"); time.sleep(1.5)
    type_("exit\n"); wait_log("session in VM 2 ended")
    time.sleep(0.5)
    key("alt-tab", 1)
    type_("exit\n"); wait_log("session in VM 1 ended")
    time.sleep(0.5)
    key("x", 1)

    try:
        qemu.wait(timeout=20)
    except subprocess.TimeoutExpired:
        qemu.kill()
        print("  FAIL  QEMU did not power off (OS1993 hung?)")
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    listing = subprocess.run(["mdir", "-b", "-i", img + "@@32256", "::/OS1993"],
                             capture_output=True, text=True, env=env).stdout.upper()
    ok = True
    for d in ("ONE", "TWO"):
        found = f"::/OS1993/{d}" in listing
        print(f"  {'PASS' if found else 'FAIL'}  directory {d} created in its session")
        ok &= found
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
