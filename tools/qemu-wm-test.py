#!/usr/bin/env python3
"""Drive the ring-3 window manager and controls with QEMU's mouse/keyboard.

Every control is clicked or typed into and must report the right value in
the GUI's log; windows are activated, moved, maximized, minimized,
restored, resized and closed; and the custom palette must survive a trip
to a DOS session. Coordinates follow the layout in user/gui.c.
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine, near, pixel, rgb  # noqa: E402

GRAY4, WHITE, FACE, DESKTOP = (38, 38, 38), (63, 63, 63), (55, 55, 55), (17, 30, 40)

check = Checker()


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check("MOUSE: PS/2 mouse on IRQ 12" in m.log(), "VMM found the PS/2 mouse")
        m.key("g")
        check(m.wait_log("GUI: desktop ready") and m.wait_log("GUI: mouse ready"),
              "GUI started with the window manager")

        a = m.screenshot("wm-start")
        check(near(pixel(a, 630, 470), rgb(DESKTOP)), "desktop uses the custom palette")
        check(near(pixel(a, 275, 47), rgb(GRAY4)) and near(pixel(a, 275, 48), rgb(WHITE)),
              "active title bar (Controls) has pinstripes")
        check(near(pixel(a, 40, 25), rgb(FACE)) and near(pixel(a, 40, 26), rgb(FACE)),
              "inactive title bar (Programs) is plain")

        # --- controls in the Controls window (client origin 251,65) ---
        m.goto(280, 103); m.click()
        check(m.wait_log("GUI: check Sound effects on"), "check box toggles")
        m.goto(460, 151); m.click()
        check(m.wait_log("GUI: radio Turbo"), "radio button selects")
        m.goto(420, 168); m.click()
        m.type("abc")
        m.goto(300, 266); m.click()
        check(m.wait_log("GUI: list Teal"), "list box selects an item")
        m.goto(588, 220); m.click()
        check(m.wait_log("GUI: slider 90"), "slider follows a click")
        m.goto(601, 301); m.click()
        check(m.wait_log("GUI: scroll 11"), "scroll bar arrow steps")
        m.goto(485, 338); m.click()
        check(m.wait_log("GUI: OK: sound=1 clock=1 speed=Turbo name=Helloabc colour=Teal volume=90"),
              "OK reports every value (typed text included)")

        # --- windows ---
        m.goto(60, 30); m.click()
        check(m.wait_log("GUI: activate Programs"), "clicking an inactive window activates it")
        b = m.screenshot("wm-activate")
        check(near(pixel(b, 40, 23), rgb(GRAY4)) and near(pixel(b, 40, 24), rgb(WHITE)),
              "Programs now has the striped title")
        m.drag(60, 30, 60, 130)
        check(m.wait_log("GUI: moved Programs to 16,116"), "title-bar drag moves the window")

        m.goto(400, 52); m.click()
        check(m.wait_log("GUI: activate Controls"), "Controls activated again")
        m.goto(595, 54); m.click()
        check(m.wait_log("GUI: maximized Controls"), "maximize box")
        c = m.screenshot("wm-max")
        check(not near(pixel(c, 630, 470), rgb(DESKTOP)), "maximized window covers the screen")
        m.goto(609, 14); m.click()
        check(m.wait_log("GUI: restored Controls"), "restore box")

        m.drag(615, 360, 575, 330)
        check(m.wait_log("GUI: resized Controls to 340x300"), "size grip resizes")

        m.goto(150, 312); m.click()
        check(m.wait_log("GUI: activate About OS1993"), "About activated")
        m.goto(304, 314); m.click()
        check(m.wait_log("GUI: minimized About OS1993"), "minimize box")
        d = m.screenshot("wm-min")
        check(not near(pixel(d, 10, 460), rgb(DESKTOP)), "minimized window shows as a tile")
        m.goto(60, 460); m.click()
        check(m.wait_log("GUI: restored About OS1993"), "clicking the tile restores it")
        m.goto(285, 412); m.click()
        check(m.wait_log("GUI: closed About OS1993"), "About's OK button closes it")

        # --- a DOS session from the Programs window, and back ---
        m.goto(120, 188); m.click()
        check(m.wait_log("Started DOS 1"), "MS-DOS Prompt button starts a DOS session")
        m.key("alt-tab")
        m.wait_log("focus -> console")
        e = m.screenshot("wm-back")
        check(near(pixel(e, 630, 470), rgb(DESKTOP)), "custom palette restored after the session")
        m.key("alt-tab")
        m.wait_log("focus -> DOS 1", 2)
        m.type("exit\n")
        check(m.wait_log("session in VM 1 ended"), "DOS session ended")

        m.key("esc")
        check(m.wait_log("USER: exited with code 0"), "Esc leaves the GUI")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
