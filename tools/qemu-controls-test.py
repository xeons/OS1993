#!/usr/bin/env python3
"""Drive the newer standard controls in the demo's More Controls window
(Programs > File > More Controls...): a drop-down list combo box (mouse,
the list's scroll bar, keys, Esc and a click outside), an editable combo
box whose list opens upwards (typing, choosing, keys), the tab control
(clicks, Left/Right) and up-downs (a buddy edit with auto-repeat and
arrow keys, a horizontal one that wraps), then OK reads every value back.
Coordinates follow more_create() in user/apps/demo.c and the metrics in
user/ui/combo.c, tab.c and updown.c.
"""
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine, near, pixel, rgb  # noqa: E402

BLACK, WHITE, FACE, ACCENT = (0, 0, 0), (63, 63, 63), (55, 55, 55), (26, 26, 50)
GRAY2, GRAY4, GRAY5 = (21, 21, 21), (38, 38, 38), (46, 46, 46)

# More Controls: window at 120,100 (400x280); client origin 125,125
TAB_Y = 145                     # the tab strip (tabs at y 136..155)
TAB_ADVANCED, TAB_INFO = 255, 323
COLOUR = (350, 178)             # the drop-down list combo's box (271..431, 167..189)
LIST_X, LIST_Y = 271, 189       # ... its list drops below it, 160 wide
ROW = 16
NAME_EDIT, NAME_ARROW = (250, 346), (341, 346)
NAME_LIST_Y = 203               # the editable combo's list: above it (no room below)
SPIN_UP, SPIN_DOWN = (330, 172), (330, 184)
LEVEL_INC = (295, 216)
OK = (463, 346)

check = Checker()


def row_y(top, i):
    return top + 2 + ROW * i + ROW // 2


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui() and m.start_demo(), "GUI started (shell, then the Control Demo)")

        # --- open the window from the Programs menu ---
        m.goto(45, 50); m.click()               # File
        m.goto(60, 109); m.click()              # More Controls...
        check(m.wait_log("GUI: menu More Controls"), "File > More Controls... opens the window")
        a = m.screenshot("controls-open")
        check(near(pixel(a, 140, 140), rgb(FACE)) and near(pixel(a, 217, 147), rgb(GRAY5)),
              "tab control: the selected tab is raised, the others darker")
        check(near(pixel(a, 271 + 30, 167), rgb(BLACK)), "drop-down list combo box drawn")

        # --- drop-down list combo: mouse ---
        m.goto(*COLOUR); m.click()
        check(m.wait_log("GUI: combo dropped"), "clicking the box drops the list (CBN_DROPDOWN)")
        m.goto(320, row_y(LIST_Y, 3))           # hover "Teal"
        b = m.screenshot("controls-combo-list")
        check(near(pixel(b, LIST_X, LIST_Y + 60), rgb(BLACK)), "the list is below the box")
        check(near(pixel(b, LIST_X + 5, row_y(LIST_Y, 3)), rgb(ACCENT)), "the hovered item is highlighted")
        check(near(pixel(b, LIST_X + 143, LIST_Y + 60), rgb(GRAY2)), "12 items: the list has a scroll bar")
        m.click()
        check(m.wait_log("GUI: combo Teal") and m.wait_log("GUI: combo closed"),
              "clicking an item chooses it (CBN_SELCHANGE, CBN_CLOSEUP)")

        m.goto(*COLOUR); m.click()              # scroll the list one line, then choose
        m.goto(LIST_X + 151, LIST_Y + 1 + 130 - 8); m.click()
        m.goto(320, row_y(LIST_Y, 7)); m.click()
        check(m.wait_log("GUI: combo Midnight"), "the list's scroll bar arrow scrolls it")

        m.goto(*COLOUR); m.click()              # a click outside dismisses
        m.goto(200, 380); m.click()
        check(m.wait_log("GUI: combo closed", 3), "a click outside closes the list")
        n = m.log().count("GUI: combo ")

        # --- drop-down list combo: keys (it has the focus now) ---
        m.key("up")
        check(m.wait_log("GUI: combo Sand"), "Up selects the previous item while closed")
        m.key("alt-down")
        check(m.wait_log("GUI: combo dropped", 4), "Alt+Down drops the list")
        m.key("down"); m.key("ret")
        check(m.wait_log("GUI: combo Midnight", 2), "arrows and Enter choose in the list")
        m.key("f4")
        check(m.wait_log("GUI: combo dropped", 5), "F4 drops the list")
        m.key("home"); m.key("esc")
        check(m.wait_log("GUI: combo closed", 5), "Esc closes it")
        check(m.log().count("GUI: combo ") == n + 6 and "USER: DEMO.BIN exited" not in m.log(),
              "... without choosing (and the demo did not see the Esc)")

        # --- editable combo: typing, then the list above it ---
        m.goto(*NAME_EDIT); m.click()
        m.key("ctrl-a"); m.type("zed")
        check(m.wait_log("GUI: name typed zed"), "typing in the editable combo (CBN_EDITCHANGE)")
        m.goto(*NAME_ARROW); m.click()
        check(m.wait_log("GUI: name dropped"), "its arrow drops the list")
        m.goto(250, row_y(NAME_LIST_Y, 5))      # hover "Ken"
        c = m.screenshot("controls-name-list")
        check(near(pixel(c, 189, NAME_LIST_Y + 40), rgb(BLACK)) and
              near(pixel(c, 250, NAME_LIST_Y + 1), rgb(WHITE)),
              "no room below: the list opens above the box")
        check(near(pixel(c, 195, row_y(NAME_LIST_Y, 5)), rgb(ACCENT)), "hover highlight in the upper list")
        m.click()
        check(m.wait_log("GUI: name Ken"), "clicking a name chooses it")
        m.key("down")
        check(m.wait_log("GUI: name Linus"), "Down in the field selects the next name")
        m.key("alt-down"); m.key("esc")
        check(m.wait_log("GUI: name closed", 2), "Alt+Down / Esc from the field")

        # --- tabs and up-downs ---
        m.goto(TAB_ADVANCED, TAB_Y); m.click()
        check(m.wait_log("GUI: tab Advanced"), "clicking a tab selects it (TCN_SELCHANGE)")
        d = m.screenshot("controls-advanced")
        check(near(pixel(d, 271, 175), rgb(GRAY4)) and near(pixel(d, 300, 175), rgb(WHITE)),
              "the General page is hidden, Advanced's edit shown")
        m.goto(*SPIN_UP); m.click()
        check(m.wait_log("GUI: spin 2"), "up-down: the up arrow steps (WM_VSCROLL)")
        m.click()
        check(m.wait_log("GUI: spin 3"), "... again")
        m.cmd("mouse_button 1", 1.6)            # held: it repeats
        m.cmd("mouse_button 0", 0.6)
        held = [int(v) for v in re.findall(r"GUI: spin (\d+)", m.log())]
        check(max(held) >= 6, f"holding the arrow repeats (reached {max(held)})")
        n8, n7 = m.log().count("GUI: spin 8\n"), m.log().count("GUI: spin 7\n")
        m.key("ctrl-a"); m.type("7"); m.key("up")
        check(m.wait_log("GUI: spin 8\n", n8 + 1), "a typed number is read back; Up in the buddy steps it")
        m.goto(*SPIN_DOWN); m.click()
        check(m.wait_log("GUI: spin 7\n", n7 + 1), "the down arrow steps back")
        for _ in range(4):
            m.goto(*LEVEL_INC); m.click(0.3)
        check(m.wait_log("GUI: level 3") and m.wait_log("GUI: level 0"),
              "horizontal up-down steps and wraps (UDS_WRAP)")

        m.goto(TAB_ADVANCED, TAB_Y); m.click()  # focus the tabs
        m.key("right")
        check(m.wait_log("GUI: tab Info"), "Right moves to the next tab")
        m.key("left")
        check(m.wait_log("GUI: tab Advanced", 2), "Left moves back")

        m.goto(*OK); m.click()
        check(m.wait_log("GUI: more OK: colour=Midnight name=Linus known=6 copies=7 level=0 tab=1"),
              "OK reads every value back (CB_GETLBTEXT, UDM_GETPOS, TCM_GETCURSEL)")

        m.key("esc")
        check(m.wait_log("USER: DEMO.BIN exited with code 0"), "Esc ends the demo")
        check(m.shut_down(), "Start > Shut Down ends the GUI")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
