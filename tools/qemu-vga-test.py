#!/usr/bin/env python3
"""Check the VDD: the GUI's 640x480x16 desktop -- custom palette included --
must come back pixel-identical (apart from the animated progress bar) after
the screen was handed to a text-mode DOS session, and the session must
really be in text mode.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine, near, pixel, rgb  # noqa: E402

DESKTOP = (17, 30, 40)
PROGRESS = (443, 265, 166, 16)      # the animated progress bar (user/gui.c)
check = Checker()


def differs(a, b):
    """Pixels that differ outside the animated progress bar."""
    if a[:2] != b[:2]:
        return -1
    w, h = a[:2]
    px, py, pw, ph = PROGRESS
    n = 0
    for y in range(h):
        row = slice(y * w * 3, (y + 1) * w * 3)
        if a[2][row] == b[2][row]:
            continue
        for x in range(w):
            if py <= y < py + ph and px <= x < px + pw:
                continue
            i = (y * w + x) * 3
            n += a[2][i:i + 3] != b[2][i:i + 3]
    return n


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui() and m.start_demo(), "GUI started (shell, then the Control Demo)")
        m.goto(60, 30)                             # activate Programs first:
        m.click()                                  # the click below won't change it
        m.goto(120, 109)                           # over "MS-DOS Prompt"
        a = m.screenshot("vga-desktop")
        check(a[:2] == (640, 480), f"desktop is 640x480 (got {a[0]}x{a[1]})")
        check(near(pixel(a, 630, 440), rgb(DESKTOP)), "desktop drawn with the GUI's palette")

        m.click()
        check(m.wait_log("Started DOS 1"), "a DOS session takes the screen")
        b = m.screenshot("vga-session")
        check(b[:2] != (640, 480), f"the session is in text mode ({b[0]}x{b[1]})")

        m.key("alt-tab")
        m.wait_log("focus -> console")
        c = m.screenshot("vga-back1")
        n = differs(a, c)
        check(n == 0, f"desktop restored pixel-identical after Alt+Tab ({n} pixels differ)")

        m.key("alt-tab")
        m.wait_log("focus -> DOS 1", 2)
        m.type("exit\n")
        m.wait_log("session in VM 1 ended")
        m.wait_log("focus -> console", 2)
        d = m.screenshot("vga-back2")
        n = differs(a, d)
        check(n == 0, f"desktop restored pixel-identical after the session ended ({n} pixels differ)")

        m.key("esc")
        check(m.wait_log("USER: DEMO.BIN exited with code 0"), "Esc ends the demo")
        check(m.shut_down(), "Start > Shut Down ends the GUI")
        e = m.screenshot("vga-text")
        check(e[:2] != (640, 480), f"back to the text console ({e[0]}x{e[1]})")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
