#!/usr/bin/env python3
"""The GUI's newer parts, through the programs that use them.

Calculator's scientific view (sin, x^y); Notepad's Print to a .PRN file
through a dialog template (DialogBox) and the Save As dialog; Paint's
Attributes dialog (an up-down buddy), a selection through the picture
clipboard, zoom and undo; Explorer's toolbar tooltip, the files clipboard
(copy and paste into a new folder), Back and Forward; a taskbar button's
right-button system menu; and on the disk afterwards the printed pages and
the folder gone again.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import IMG, Checker, Machine, QDIR  # noqa: E402

check = Checker()


def start(m, key, ready, n=1):
    for k in ("ctrl-esc", "p", key):
        m.key(k)
    return m.wait_log(ready, n)


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui(), "the shell started")

        # --- Calculator, scientific ---
        check(start(m, "c", "GUI: calculator ready"), "Calculator started")
        m.key("alt-v")
        m.key("c")
        check(m.wait_log("GUI: calc scientific view"), "View > Scientific")
        m.type("30s=")
        check(m.wait_log("GUI: calc = 0.5"), "sin 30 degrees = 0.5")
        m.type("2y10=")
        check(m.wait_log("GUI: calc = 1024."), "2 x^y 10 = 1024")
        m.key("alt-f4")
        m.wait_log("USER: CALC.BIN exited")

        # --- Notepad: Print to a file ---
        check(start(m, "n", "GUI: notepad ready"), "Notepad started")
        m.type("printed page\n")
        m.key("ctrl-p")
        m.key("ret")                                    # Print: OK (a dialog template)
        check(m.wait_log("GUI: folder C:\\OS1993"), "... then the Print to File dialog")
        m.key("ret")                                    # untitled.prn
        check(m.wait_log("GUI: notepad printed 1 page(s) x 1 to C:\\OS1993\\UNTITLED.PRN"),
              "Print lays out the page and writes it to UNTITLED.PRN")
        m.key("alt-f4")
        m.key("n")                                      # don't save
        m.wait_log("USER: NOTEPAD.BIN exited")

        # --- Paint: Attributes, selection, clipboard, zoom, undo ---
        check(start(m, "p", "GUI: paint ready"), "Paint started")
        m.key("ctrl-e")
        m.type("200\t100\n")
        check(m.wait_log("GUI: paint canvas 200x100"), "Image > Attributes makes the canvas 200x100")
        m.key("ctrl-a")
        m.key("ctrl-c")
        check(m.wait_log("GUI: paint copied 200x100"), "Select All, Copy: a picture on the clipboard")
        m.key("ctrl-v")
        check(m.wait_log("GUI: paint pasted 200x100"), "Paste brings it back as a selection")
        m.key("ctrl-pgdn")
        check(m.wait_log("GUI: paint zoom 2x"), "Ctrl+PgDn zooms in")
        m.key("ctrl-z")
        check(m.wait_log("GUI: paint undo"), "Undo")
        m.key("alt-f4")
        m.key("n")
        m.wait_log("USER: PAINT.BIN exited")

        # --- Explorer: tooltip, files clipboard, Back and Forward ---
        check(start(m, "x", "GUI: explorer ready"), "Explorer started")
        m.goto(300, 300)
        m.goto(227, 71)                                 # the Properties button
        check(m.wait_log("GUI: tooltip Properties"), "a toolbar button's tooltip")
        m.goto(300, 300)
        m.key("alt-d")
        m.type("c:\\os1993\n")
        check(m.wait_log("GUI: explorer at C:\\os1993 ("), "the address field goes to a folder")
        m.key("alt-f")
        m.key("f")
        m.wait_log("GUI: explorer made C:\\os1993\\New Folder")
        m.type("Box\n")
        check(m.wait_log("GUI: explorer renamed New Folder to Box"), "a folder named Box")
        m.type("p")                                     # PROGRAMS.INI
        m.key("ctrl-c")
        check(m.wait_log("GUI: explorer copied to the clipboard 1"), "Copy puts a file on the clipboard")
        m.type("b")
        m.key("ret")
        check(m.wait_log("GUI: explorer at C:\\os1993\\Box (0 items)"), "into Box")
        m.key("ctrl-v")
        check(m.wait_log("GUI: explorer copied 1 to C:\\os1993\\Box"), "Paste copies it here")
        m.key("alt-left")
        check(m.wait_log("GUI: explorer back to C:\\os1993"), "Alt+Left goes back")
        m.key("alt-right")
        check(m.wait_log("GUI: explorer forward to C:\\os1993\\Box"), "Alt+Right goes forward again")

        # --- the taskbar button's system menu ---
        m.goto(110, 466)
        m.rclick()
        m.key("n")                                      # Minimize
        check(m.wait_log("GUI: taskbar menu f020"), "a taskbar button's right button: the system menu")
        m.goto(110, 466)
        m.click()                                       # back to the front

        m.key("backspace")
        m.wait_log("GUI: explorer at C:\\os1993 (", 3)
        m.key("delete")
        m.key("y")
        check(m.wait_log("GUI: explorer deleted 1"), "Box deleted with what it held")
        m.key("alt-f4")
        m.wait_log("USER: EXPLORER.BIN exited")
        m.shut_down()
        m.key("x", 1)
        m.wait_exit(30)

    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    r = subprocess.run(["mdir", "-i", IMG + "@@32256", "::/OS1993"], env=env, capture_output=True, text=True)
    check("BOX" not in r.stdout.upper().split(), "Box is gone from the disk")
    out = os.path.join(QDIR, "UNTITLED.PRN")
    subprocess.run(["mcopy", "-n", "-o", "-i", IMG + "@@32256", "::/OS1993/UNTITLED.PRN", out], env=env,
                   capture_output=True)
    data = open(out, "rb").read() if os.path.exists(out) else b""
    check(b"printed page" in data and b"Page 1" in data and data.endswith(b"\f"),
          "the .PRN file holds the page: the text, its number, a form feed")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
