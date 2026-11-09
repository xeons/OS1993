#!/usr/bin/env python3
"""Notepad and the multi-line edit control.

Typing with Enter and Tab, saving (Save As, CR LF line ends), the
clipboard between two Notepad processes, the "has changed" question,
opening a file, selecting with Shift+End and deleting, undo, closing with
unsaved changes (Cancel keeps it open, No discards), Find (repeatedly, to
the end; whole words), Replace All and its undo, Word Wrap on and off, and
the file's exact bytes on the disk image afterwards.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import IMG, Checker, Machine  # noqa: E402

TEXT = "dear diary,\n\ttoday os1993 got an editor.\n"
ON_DISK = TEXT.replace("\n", "\r\n").encode()
check = Checker()


def start_notepad(m, n):
    for k in ("ctrl-esc", "p", "n"):
        m.key(k)
    return m.wait_log("GUI: notepad ready", n)


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui() and start_notepad(m, 1), "the shell and Notepad started")

        m.type(TEXT)
        m.key("ctrl-s")
        m.type("diary\n")
        check(m.wait_log(f"GUI: notepad saved C:\\OS1993\\DIARY.TXT ({len(ON_DISK)} bytes)"),
              "Save As writes DIARY.TXT with CR LF line ends")

        for k in ("ctrl-a", "ctrl-c"):                  # to the clipboard ...
            m.key(k)
        check(start_notepad(m, 2), "a second Notepad process")
        m.key("ctrl-v")                                 # ... and out in another process
        m.key("ctrl-s")
        m.type("copy\n")
        check(m.wait_log(f"GUI: notepad saved C:\\OS1993\\COPY.TXT ({len(ON_DISK)} bytes)"),
              "the clipboard carries the text between processes")

        m.type("more")
        m.key("ctrl-n")                                 # "has changed": No
        m.key("n")
        check(m.wait_log("GUI: notepad new"), "New asks about unsaved changes, then starts over")

        m.key("ctrl-o")
        m.type("diary.txt\n")
        check(m.wait_log("GUI: notepad opened C:\\OS1993\\DIARY.TXT (3 lines)"), "Open reads it back")

        for k in ("ctrl-home", "shift-end", "delete"):  # the first line's text goes
            m.key(k)
        m.key("ctrl-s")
        check(m.wait_log(f"GUI: notepad saved C:\\OS1993\\DIARY.TXT ({len(ON_DISK) - 11} bytes)"),
              "Shift+End selects to the end of the line; Delete deletes it")
        m.key("ctrl-z")
        m.key("ctrl-s")
        check(m.wait_log(f"GUI: notepad saved C:\\OS1993\\DIARY.TXT ({len(ON_DISK)} bytes)", 2),
              "Undo puts it back")

        m.type("x")
        m.key("alt-f4")                                 # "has changed": Cancel
        m.key("esc")
        check("USER: NOTEPAD.BIN exited" not in m.log(), "Cancel keeps Notepad open")
        m.key("alt-f4")
        m.key("n")
        check(m.wait_log("USER: NOTEPAD.BIN exited with code 0"), "No closes it without saving")

        # --- Find, Replace, Word Wrap (a fresh Notepad; the other is still open) ---
        check(start_notepad(m, 3), "a third Notepad for searching")
        m.type("the cat sat on the mat\nthe end\n")
        m.key("ctrl-home")
        m.key("ctrl-f")
        m.type("the\n")                                # Enter: Find Next
        check(m.wait_log("GUI: notepad found the at 0"), "Find selects the first match")
        m.key("ret")
        check(m.wait_log("GUI: notepad found the at 15"), "Find Next: the second")
        m.key("ret")
        check(m.wait_log("GUI: notepad found the at 23"), "... the third, on the next line")
        m.key("ret")
        check(m.wait_log("GUI: notepad cannot find the"), "... then it says it can't find more")
        m.key("ret")                                    # the message box
        m.key("esc")                                    # the dialog
        m.key("ctrl-home")
        m.key("ctrl-f")
        m.type("at")
        m.key("tab"); m.key("tab"); m.key("spc")        # Whole words
        m.key("ret")
        check(m.wait_log("GUI: notepad cannot find at"), "whole words: 'at' isn't in cat, sat, mat")
        m.key("ret")
        m.key("esc")

        m.key("ctrl-h")
        m.type("the")
        m.key("tab")
        m.type("a")
        m.goto(435, 182); m.click()                     # Replace All
        check(m.wait_log("GUI: notepad replaced 3"), "Replace All replaces every match")
        m.key("esc")
        m.key("ctrl-s")
        m.type("search\n")
        check(m.wait_log("GUI: notepad saved C:\\OS1993\\SEARCH.TXT (27 bytes)"),
              "the text reads 'a cat sat on a mat / a end'")
        m.key("ctrl-z")
        m.key("ctrl-s")
        check(m.wait_log("GUI: notepad saved C:\\OS1993\\SEARCH.TXT (33 bytes)"),
              "one Undo takes the whole Replace All back")

        m.key("ctrl-n")
        m.type("lorem ipsum " * 8)                      # 96 columns: wider than the window
        m.key("alt-e"); m.key("w")
        check(m.wait_log("GUI: notepad word wrap on (2 lines)"), "Word Wrap folds the long line")
        m.key("alt-e"); m.key("w")
        check(m.wait_log("GUI: notepad word wrap off (1 lines)"), "... and unfolds it")

        check(m.shut_down(), "Start > Shut Down ends the GUI")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")

    out = os.path.join(os.path.dirname(IMG), "qemu", "DIARY.TXT")
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    subprocess.run(["mcopy", "-n", "-o", "-i", IMG + "@@32256", "::/OS1993/DIARY.TXT", out],
                   capture_output=True, env=env)
    data = open(out, "rb").read() if os.path.exists(out) else b""
    check(data == ON_DISK, "DIARY.TXT on the disk holds exactly the text, CR LF line ends")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
