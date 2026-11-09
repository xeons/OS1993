#!/usr/bin/env python3
"""File access from ring-3 programs, through Paint and the file dialogs.

Paint saves a picture (Save As dialog), starts over, opens it again (Open
dialog), replaces it after the overwrite question, refuses a missing file,
browses to the parent folder and back; a second Paint process opens the
file the first one wrote. After QEMU powers off, the BMP is read straight
from the FreeDOS disk image with mtools and checked: header, palette, and
the red rectangle's pixels -- so the bytes really reached DOS's disk.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import IMG, Checker, Machine, near, pixel, rgb  # noqa: E402

RED, WHITE = (52, 8, 8), (63, 63, 63)
check = Checker()


def start_paint(m, n):
    for k in ("ctrl-esc", "p", "p"):
        m.key(k)
    return m.wait_log("GUI: paint ready", n)


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui() and start_paint(m, 1), "the shell and Paint started")

        # a red filled rectangle: tool, colour, drag (canvas origin 97,76)
        m.goto(53, 166); m.click()                      # filled rectangle
        m.goto(165, 390); m.click()                     # red
        m.drag(150, 120, 250, 200)
        check(m.wait_log("GUI: paint filled rectangle from"), "drew a red rectangle")

        m.key("ctrl-s")                                 # untitled: Save As
        check(m.wait_log("GUI: folder C:\\OS1993"), "Save As opens in the OS1993 folder")
        m.type("pic1\n")
        check(m.wait_log("GUI: paint saved C:\\OS1993\\PIC1.BMP"), "Save As writes PIC1.BMP")

        m.key("ctrl-n")
        check(m.wait_log("GUI: paint new"), "File > New starts over")
        a = m.screenshot("files-new")
        check(near(pixel(a, 220, 180), rgb(WHITE)), "the canvas is blank again")

        m.key("ctrl-o")
        m.type("pic1.bmp\n")
        check(m.wait_log("GUI: paint opened C:\\OS1993\\PIC1.BMP (400x280)"), "Open reads it back")
        b = m.screenshot("files-open")
        check(near(pixel(b, 220, 180), rgb(RED)), "the rectangle is back")

        m.key("alt-f"); m.key("a")                      # Save As over it
        m.key("ret")
        m.key("y")                                      # "already exists. Replace it?"
        check(m.wait_log("GUI: paint saved C:\\OS1993\\PIC1.BMP", 2), "replacing it after the question")

        m.key("ctrl-o")
        m.type("nosuch\n")
        m.key("ret")                                    # "File not found" box: OK
        m.key("esc")                                    # cancel the dialog
        check("NOSUCH" not in m.log() and "paint opened C:\\OS1993\\NOSUCH" not in m.log(),
              "a missing file is refused")

        m.key("ctrl-o")
        m.type("..\n")
        check(m.wait_log("GUI: folder C:\\"), "typing .. goes to the parent folder")
        m.type("os1993\n")
        check(m.wait_log("GUI: folder C:\\OS1993", 3), "typing a folder name goes into it")
        m.key("esc")

        start_paint(m, 2)                               # another process
        m.key("ctrl-o")
        m.type("pic1.bmp\n")
        check(m.wait_log("GUI: paint opened C:\\OS1993\\PIC1.BMP (400x280)", 2),
              "a second Paint process opens the file the first one wrote")

        check(m.shut_down(), "Start > Shut Down ends the GUI")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")

    out = os.path.join(os.path.dirname(IMG), "qemu", "PIC1.BMP")
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")      # partition 1 starts at LBA 63
    rc = subprocess.run(["mcopy", "-n", "-o", "-i", IMG + "@@32256", "::/OS1993/PIC1.BMP", out],
                        capture_output=True, env=env).returncode
    data = open(out, "rb").read() if rc == 0 else b""
    check(len(data) == 118 + 200 * 280 and data[:2] == b"BM", "PIC1.BMP is on the disk, 56118 bytes")
    if data:
        bpp = int.from_bytes(data[28:30], "little")
        check(bpp == 4 and data[54 + 12 * 4 + 2] == 52 * 255 // 63, "a 16-colour BMP with the theme palette")
        row = 280 - 1 - (160 - 76)                      # screen y 160: canvas row 84, stored bottom up
        byte = data[118 + row * 200 + (200 - 97) // 2]
        check(byte == 0xCC, "the rectangle's pixels are red (colour 12) in the file")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
