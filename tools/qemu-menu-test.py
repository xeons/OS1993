#!/usr/bin/env python3
"""Drive the menus: the Programs menu bar by mouse and keyboard (Alt+letter,
F10 and arrows), the system menu by mouse (greyed items do nothing) and
keyboard (Alt+Space: Restore, Move with the arrow keys), and a right-click
context menu with a submenu, by mouse and keyboard. Coordinates follow
user/apps/gui.c and the metrics in user/ui/menu.c.
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine, near, pixel, rgb  # noqa: E402

BLACK, ACCENT = (0, 0, 0), (26, 26, 50)

check = Checker()


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui() and m.start_demo(), "GUI started (shell, then the Control Demo)")

        # --- menu bar, mouse: File at 27..75 on the bar (y 42..60) ---
        m.goto(45, 50); m.click()
        check(m.wait_log("GUI: activate Programs"), "clicking the bar activates the window")
        m.goto(60, 73)                          # hover "MS-DOS Prompt"
        a = m.screenshot("menu-file")
        check(near(pixel(a, 27, 90), rgb(BLACK)), "File menu drops down below its title")
        check(near(pixel(a, 30, 66), rgb(ACCENT)), "hovered item is highlighted")
        m.goto(60, 91); m.click()               # "About OS1993..."
        check(m.wait_log("GUI: menu About OS1993"), "clicking an item sends its command")

        # --- menu bar, keyboard ---
        m.goto(100, 25); m.click()              # Programs again
        m.key("alt-h"); m.key("c")
        check(m.wait_log("GUI: menu Contents"), "Alt+H then a mnemonic chooses Help > Contents")
        m.key("ret")                            # dismiss its message box
        check(m.wait_log("GUI: box Contents -> 1"), "Contents message box closed with Enter")
        for k in ("f10", "right", "ret", "down", "ret"):
            m.key(k)
        check(m.wait_log("GUI: menu About OS1993", 2), "F10, arrows and Enter walk the menu bar")

        # --- system menu, mouse: box at 24..37, menu from y 43 ---
        m.goto(100, 25); m.click()
        m.goto(30, 30); m.click()
        b = m.screenshot("menu-system")
        check(near(pixel(b, 24, 100), rgb(BLACK)), "system box opens the system menu")
        m.goto(50, 52); m.click()               # Restore: greyed while normal
        m.goto(50, 124); m.click()              # Maximize
        check(m.wait_log("GUI: maximized Programs"), "system menu Maximize")
        check("restored Programs" not in m.log(), "greyed Restore did nothing")

        # --- system menu, keyboard ---
        m.key("alt-spc"); m.key("r")
        check(m.wait_log("GUI: restored Programs"), "Alt+Space, R restores")
        m.key("alt-spc"); m.key("m")
        for k in ("right", "right", "ret"):
            m.key(k)
        check(m.wait_log("GUI: moved Programs to 32,16"), "Alt+Space, M moves with the arrow keys")

        # --- context menu with a submenu (Controls, over "Colour scheme:") ---
        m.goto(280, 192); m.rclick()
        m.goto(300, 204)                        # hover "Speed": submenu opens
        m.goto(200, 204); m.click()             # "Slow": the submenu flipped left (no room)
        check(m.wait_log("GUI: speed Slow"), "right-click menu: hover opens a submenu, click chooses")
        m.goto(280, 192); m.rclick()
        for k in ("down", "right", "down", "down", "ret"):
            m.key(k)
        check(m.wait_log("GUI: speed Turbo"), "context menu by keyboard")

        m.key("esc")
        check(m.wait_log("USER: DEMO.BIN exited with code 0"), "Esc ends the demo")
        check(m.shut_down(), "Start > Shut Down ends the GUI")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
