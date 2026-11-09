#!/usr/bin/env python3
"""The shell and its programs, each a process of its own.

The desktop and taskbar; the Start menu by mouse (with its Programs
submenu) and by keyboard (Ctrl+Esc); desktop icons (double-click);
Calculator by keyboard and mouse, including an error; Paint (a tool, a
colour, a rectangle, a fill); Clock (switched to digital from its menu);
taskbar buttons switching, minimizing and restoring; the Run dialog
starting a second Calculator, closed again with Alt+F4; and Shut Down,
which ends the session and the programs still running. Coordinates follow
user/apps/*.c.
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine, near, pixel, rgb  # noqa: E402

BLACK, RED, FACE, DESKTOP, ACCENT = (0, 0, 0), (52, 8, 8), (55, 55, 55), (17, 30, 40), (26, 26, 50)

check = Checker()


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui(), "the shell started (G at the VMM console)")
        a = m.screenshot("shell-desktop")
        check(near(pixel(a, 600, 300), rgb(DESKTOP)), "the desktop fills the screen")
        check(not near(pixel(a, 48, 30), rgb(DESKTOP)), "desktop icons are drawn")
        check(near(pixel(a, 300, 470), rgb(FACE)), "the taskbar runs along the bottom")

        # --- Start menu by mouse: Start > Programs > Calculator ---
        m.goto(30, 466); m.click()
        b = m.screenshot("shell-start")
        check(near(pixel(b, 2, 400), rgb(BLACK)), "the Start menu rises from the Start button")
        m.goto(60, 371)                             # hover "Programs": the submenu opens
        m.goto(220, 371); m.click()                 # "Calculator"
        check(m.wait_log("GUI: launch Calculator") and m.wait_log("USER: started CALC.BIN"),
              "Start > Programs > Calculator starts a process")
        check(m.wait_log("GUI: calculator ready"), "the Calculator runs")

        # --- Calculator ---
        m.type("12+30=")
        check(m.wait_log("GUI: calc = 42."), "keyboard: 12+30= gives 42")
        m.type("7/2=")
        check(m.wait_log("GUI: calc = 3.5"), "keyboard: 7/2= gives 3.5")
        m.type("5/0=")
        check(m.wait_log("GUI: calc error: Cannot divide by zero"), "dividing by zero is an error")
        m.key("esc")
        for x, y in ((259, 226), (385, 322), (259, 290), (427, 322)):   # 7 + 1 =
            m.goto(x, y); m.click(0.3)
        check(m.wait_log("GUI: calc = 8."), "mouse: 7 + 1 = gives 8")

        # --- Paint from its desktop icon ---
        m.goto(48, 150); m.dblclick()
        check(m.wait_log("GUI: launch Paint") and m.wait_log("GUI: paint ready"),
              "double-clicking a desktop icon starts Paint")
        m.goto(79, 140); m.click()
        check(m.wait_log("GUI: paint tool rectangle"), "picking the rectangle tool")
        m.drag(150, 120, 250, 200)
        check(m.wait_log("GUI: paint rectangle from"), "dragging draws a rectangle")
        m.goto(165, 390); m.click()
        check(m.wait_log("GUI: paint foreground colour 12"), "picking red from the palette")
        m.goto(79, 114); m.click()
        m.goto(200, 160); m.click()
        check(m.wait_log("GUI: paint fill at"), "the fill tool fills")
        c = m.screenshot("shell-paint")
        check(near(pixel(c, 220, 180), rgb(RED)), "the rectangle is filled red")
        check(near(pixel(c, 200, 120), rgb(BLACK)), "inside its black outline")

        # --- Clock by keyboard; its Settings menu ---
        for k in ("ctrl-esc", "p", "l"):
            m.key(k)
        check(m.wait_log("GUI: clock ready"), "Ctrl+Esc, P, L starts the Clock")
        m.key("alt-s"); m.key("d")
        check(m.wait_log("GUI: clock digital with seconds"), "Clock switches to digital")
        d = m.screenshot("shell-clock")

        # --- taskbar buttons: Calculator, Paint, Clock ---
        m.goto(150, 466); m.click()                 # Calculator to the front
        m.goto(150, 466); m.click()                 # ... and again: minimized
        check(m.wait_log("GUI: minimized Calculator"), "a second click on its button minimizes it")
        m.goto(150, 466); m.click()
        check(m.wait_log("GUI: restored Calculator"), "a third restores it")

        # --- Run: a second Calculator, closed with Alt+F4 ---
        for k in ("ctrl-esc", "r"):
            m.key(k)
        m.type("calc.bin\n")
        check(m.wait_log("GUI: launch calc.bin") and m.wait_log("GUI: calculator ready", 2),
              "the Run dialog starts a second Calculator")
        m.key("alt-f4")
        check(m.wait_log("USER: CALC.BIN exited with code 0"), "Alt+F4 closes it (its process ends)")

        # --- Shut Down ---
        check(m.shut_down(), "Start > Shut Down ends the shell")
        check(m.wait_log("USER: PAINT.BIN exited") and m.wait_log("USER: CLOCK.BIN exited"),
              "... and the programs still running")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
