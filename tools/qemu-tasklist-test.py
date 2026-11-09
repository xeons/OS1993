#!/usr/bin/env python3
"""Ctrl+Alt+Del: the shell's Close Program dialog and the VMM's own screen.

The dialog lists the running programs and ends one; Ctrl+Alt+Del in a DOS
session brings the GUI forward with the dialog (and never reaches DOS);
a program that hangs holding the window system makes the shell unable to
answer, so the VMM shows its text-mode screen, which ends the program and
gives the GUI back working; Esc returns to the hung GUI, and Ctrl+Alt+Del
again on that screen ends the whole session.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine  # noqa: E402

check = Checker()
STUCK = "VMM: Ctrl+Alt+Del: the window system is not responding"


def start(m, key, ready, n=1):
    for k in ("ctrl-esc", "p", key):
        m.key(k)
    return m.wait_log(ready, n)


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui() and start(m, "c", "GUI: calculator ready") and
              start(m, "n", "GUI: notepad ready"), "the shell, Calculator and Notepad started")

        # --- the Close Program dialog ---
        m.key("ctrl-alt-delete")
        check(m.wait_log("GUI: close program"), "Ctrl+Alt+Del opens Close Program")
        log = m.log()
        check("task SHELL.BIN: Shell" in log and "task CALC.BIN: Calculator" in log and
              "task NOTEPAD.BIN: Untitled - Notepad" in log, "it lists the programs by window title")
        m.key("down"); m.key("ret")                     # Calculator, End Task
        check(m.wait_log("GUI: end task CALC.BIN") and m.wait_log("USER: CALC.BIN exited with code -1"),
              "End Task ends the chosen program")
        m.key("esc")
        check(m.wait_log("GUI: closed Close Program"), "Esc closes the dialog")
        check(STUCK not in m.log(), "the shell answered, so the VMM didn't step in")

        # --- from a DOS session ---
        start(m, "m", "Started DOS 1")
        m.wait_log("focus -> DOS 1")
        m.key("ctrl-alt-delete")
        check(m.wait_log("focus -> console") and m.wait_log("GUI: close program", 2),
              "in a DOS session it brings the GUI forward with the dialog")
        m.key("esc")
        m.key("alt-tab")
        m.wait_log("focus -> DOS 1", 2)
        m.type("exit\n")
        check(m.wait_log("session in VM 1 ended"), "... and DOS never saw it (the session carries on)")

        # --- a program that hangs holding the window system ---
        for k in ("ctrl-esc", "r"):
            m.key(k)
        m.type("hang.bin\n")
        check(m.wait_log("GUI: hang ready"), "HANG.BIN started from Run")
        m.key("h")
        check(m.wait_log("GUI: hanging"), "it stops responding")
        m.key("ctrl-alt-delete")
        check(m.wait_log(STUCK), "the shell can't answer: the VMM steps in")
        s = m.screenshot("tasklist-stuck")
        check(s[:2] == (720, 400), f"its screen is text mode (got {s[0]}x{s[1]})")
        m.key("ret")                                    # the program holding the lock
        check(m.wait_log("VMM: ending HANG.BIN") and m.wait_log("USER: HANG.BIN exited"),
              "Enter ends the program holding the window system")
        m.wait_log("focus", 1)
        s = m.screenshot("tasklist-back")
        check(s[:2] == (640, 480), "the GUI comes back")
        check(start(m, "l", "GUI: clock ready"), "... and responds again")

        # --- Esc, then Ctrl+Alt+Del twice: everything ends ---
        for k in ("ctrl-esc", "r"):
            m.key(k)
        m.type("hang.bin\n")
        m.wait_log("GUI: hang ready", 2)
        m.key("h")
        m.wait_log("GUI: hanging", 2)
        m.key("ctrl-alt-delete")
        m.wait_log(STUCK, 2)
        m.key("esc")
        check(m.screenshot("tasklist-esc")[:2] == (640, 480), "Esc goes back to the (still hung) GUI")
        m.key("ctrl-alt-delete")
        check(m.wait_log(STUCK, 3), "Ctrl+Alt+Del again: the VMM's screen")
        m.key("ctrl-alt-delete")
        check(m.wait_log("VMM: ending the GUI session") and
              m.wait_log("USER: SHELL.BIN exited"), "pressed there, it ends every program and the GUI")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
