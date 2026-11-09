#!/usr/bin/env python3
"""Keyboard accelerators and message boxes.

Accelerators: F1 and Ctrl+Q in Programs, Ctrl+R and Ctrl+3 in Controls
(each table works only while its window is active). Message boxes: modal
to their owner (clicking the owner leaves the box in front), Enter takes
the default button (also when it's Cancel), Tab moves between buttons,
letters answer Yes/No, Esc does nothing in a Yes/No box, Alt+F4 means
Cancel, and Yes on the exit confirmation ends the demo.
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine  # noqa: E402

check = Checker()


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui() and m.start_demo(), "GUI started (shell, then the Control Demo)")

        # --- Programs: F1 opens the Contents box; the owner is disabled ---
        m.goto(120, 25); m.click()
        check(m.wait_log("GUI: activate Programs"), "Programs active")
        m.key("f1")
        check(m.wait_log("GUI: accel Contents"), "F1 accelerator sends its command")
        m.screenshot("msgbox-info")
        m.goto(120, 25); m.click()              # the disabled owner's title
        m.key("ret")
        check(m.wait_log("GUI: box Contents -> 1"), "Enter answers OK (box stayed in front)")
        check(m.log().count("activate Programs") == 1, "clicking the disabled owner didn't activate it")

        # --- Ctrl+Q: Yes/No question; N answers, Esc can't cancel ---
        m.key("ctrl-q")
        check(m.wait_log("GUI: accel Exit"), "Ctrl+Q accelerator")
        m.screenshot("msgbox-question")
        m.key("esc")
        m.key("n")
        check(m.wait_log("GUI: box Exit -> 7"), "N answers No; Esc did nothing")
        check(m.log().count("box Exit") == 1, "only one answer was given")

        # --- Controls: its own accelerators ---
        m.goto(400, 52); m.click()
        check(m.wait_log("GUI: activate Controls"), "Controls active")
        m.key("ctrl-r")
        check(m.wait_log("GUI: volume reset"), "Ctrl+R accelerator (Controls table)")
        m.key("ctrl-3")
        check(m.wait_log("GUI: speed Turbo"), "Ctrl+3 accelerator")
        m.key("f1")
        check("accel Contents" in m.log() and m.log().count("accel Contents") == 1,
              "F1 does nothing while Controls is active")

        # --- the Cancel button's warning: Cancel is the default ---
        m.goto(570, 338); m.click()
        check(m.wait_log("GUI: button Cancel"), "Cancel button")
        m.screenshot("msgbox-warning")
        m.key("ret")
        check(m.wait_log("GUI: box Cancel -> 2"), "Enter takes the default (Cancel)")
        m.goto(570, 338); m.click()
        m.key("tab"); m.key("ret")
        check(m.wait_log("GUI: box Cancel -> 1"), "Tab moves to OK, Enter presses it")
        m.goto(570, 338); m.click()
        m.key("alt-f4")
        check(m.wait_log("GUI: box Cancel -> 2", 2), "Alt+F4 in a box means Cancel")

        # --- exit through the confirmation ---
        m.goto(120, 25); m.click()
        m.key("ctrl-q"); m.key("y")
        check(m.wait_log("GUI: box Exit -> 6"), "Y answers Yes")
        check(m.wait_log("USER: DEMO.BIN exited with code 0"), "Yes on the confirmation ends the demo")
        check(m.shut_down(), "Start > Shut Down ends the GUI")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
