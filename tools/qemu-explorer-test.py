#!/usr/bin/env python3
"""Explorer, the tree view and the list view.

Explorer from the Start menu at My Computer (the drives in both panes);
the address field; the four views (large icons, small icons, list,
details); a new folder named in place (F2-style label editing), opened,
another made inside it, Backspace up (the folder we came from selected),
deleted with its contents after the question; Tab to the tree and the
arrow keys walking it (each selection shows that folder); a letter jumping
to the next name with it; Enter opening a file in Notepad; sorting from the
menu and by clicking a column header; double-clicking a folder; and the
image afterwards (the folders really gone, the rename really done).
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import IMG, Checker, Machine, pixel, to_png, QDIR  # noqa: E402

check = Checker()


def busy(shot, x0, y0, x1, y1):
    """How many pixels in the box aren't white (something is drawn there)."""
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            if pixel(shot, x, y) != (255, 255, 255):
                n += 1
    return n


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui(), "the shell started")
        for k in ("ctrl-esc", "p", "x"):
            m.key(k)
        check(m.wait_log("GUI: explorer ready") and m.wait_log("GUI: explorer at My Computer ("),
              "Explorer starts from the Start menu, at My Computer")
        a = m.screenshot("explorer-computer")
        to_png(a, os.path.join(QDIR, "explorer-computer.png"))

        m.key("alt-d")
        m.type("c:\\os1993\n")
        check(m.wait_log("GUI: explorer at C:\\os1993 ("), "the address field goes to a folder")
        for k, name in (("g", "large icons"), ("m", "small icons"), ("l", "list"), ("d", "details")):
            m.key("alt-v")
            m.key(k)
            ok = m.wait_log(f"GUI: explorer view {name}")
            shot = m.screenshot("explorer-" + name.replace(" ", "-"))
            to_png(shot, os.path.join(QDIR, "explorer-" + name.replace(" ", "-") + ".png"))
            check(ok and busy(shot, 300, 120, 600, 380) > 400, f"the {name} view shows the folder")

        # --- a new folder, named in place, opened, filled, deleted ---
        m.key("alt-f")
        m.key("f")
        check(m.wait_log("GUI: explorer made C:\\os1993\\New Folder"), "File > New Folder makes one")
        to_png(m.screenshot("explorer-rename"), os.path.join(QDIR, "explorer-rename.png"))
        m.type("Tests\n")
        check(m.wait_log("GUI: explorer renamed New Folder to Tests"), "... and its name is edited in place")
        m.key("ret")
        check(m.wait_log("GUI: explorer at C:\\os1993\\Tests (0 items)"), "Enter opens the folder")
        m.key("alt-f")
        m.key("f")
        m.type("Inner\n")
        check(m.wait_log("GUI: explorer renamed New Folder to Inner"), "a folder inside it")
        m.key("backspace")
        check(m.wait_log("GUI: explorer at C:\\os1993 (", 2), "Backspace goes up")
        m.key("delete")
        m.key("y")
        check(m.wait_log("GUI: explorer deleted 1"), "Delete asks, then deletes the folder we came from, with its contents")

        # --- the tree ---
        m.key("tab")
        m.key("up")
        check(m.wait_log("GUI: explorer at C:\\freedos ("), "Tab to the tree; Up selects the folder above, and shows it")
        m.key("left")
        check(m.wait_log("GUI: explorer at C:\\ ("), "Left goes to its parent, the drive")
        tree = m.screenshot("explorer-tree")
        to_png(tree, os.path.join(QDIR, "explorer-tree.png"))

        # --- a letter, Enter on a file ---
        m.key("tab")
        m.key("f")
        m.key("f")
        m.key("ret")
        check(m.wait_log("GUI: explorer opened C:\\FDCONFIG.SYS") and m.wait_log("GUI: notepad ready"),
              "letters jump to names; Enter opens a file in Notepad")
        m.wait_log("GUI: notepad opened")
        m.key("alt-f4")
        check(m.wait_log("USER: NOTEPAD.BIN exited"), "Notepad closed again")

        # --- sorting ---
        m.key("alt-v")
        m.key("i")
        m.key("s")
        check(m.wait_log("GUI: explorer sorted by size"), "View > Arrange Icons > by Size")
        m.goto(450, 98)                                 # the Size column's header
        m.click()
        check(m.wait_log("GUI: explorer sorted by size, descending"), "a click on the header sorts the other way")

        # --- the mouse: double-click a folder in the list ---
        m.goto(300, 134)                                # the first row: a folder
        m.dblclick()
        check(m.wait_log("GUI: explorer at C:\\", 2) and len(m.log().split("GUI: explorer at C:\\")) > 3,
              "double-clicking a folder opens it")
        m.key("alt-f4")
        check(m.wait_log("USER: EXPLORER.BIN exited with code 0"), "Alt+F4 closes Explorer")
        m.shut_down()
        m.key("x", 1)
        m.wait_exit(30)

    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    r = subprocess.run(["mdir", "-i", IMG + "@@32256", "::/OS1993"], env=env, capture_output=True, text=True)
    check("Tests" not in r.stdout and "NEWFOL" not in r.stdout.upper(), "the folders are gone from the disk")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
