#!/usr/bin/env python3
"""The VBE display driver: higher resolutions and colour depths.

Display Properties lists the adapter's modes; a switch shows the desktop at
the new size (the taskbar along its bottom, a gradient in high and true
colour) and asks whether to keep it -- No, or 15 seconds without an
answer, goes back. A DOS session in front and back again restores the
mode; the kept mode comes back the next time the GUI starts (OS1993.INI);
and 640x480 in 16 colours is VGA mode 12h again.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine, near, pixel  # noqa: E402

check = Checker()
SIZES = [(640, 480), (800, 600), (1024, 768), (1280, 1024)]
DEPTHS = [4, 8, 16, 32]


def choose(m, size, bpp):
    """Display Properties: pick a size and a depth, OK."""
    for k in ("ctrl-esc", "d"):
        m.key(k)
    m.wait_log("GUI: display properties", choose.opened + 1)
    choose.opened += 1
    for _ in range(len(SIZES)):
        m.key("up")
    for _ in range(SIZES.index(size)):
        m.key("down")
    m.key("tab")
    for _ in range(len(DEPTHS)):
        m.key("up")
    for _ in range(DEPTHS.index(bpp)):
        m.key("down")
    m.key("ret")


choose.opened = 0


def shot_ok(m, name, size, what):
    s = m.screenshot(name)
    check(s[:2] == size, f"{what} ({s[0]}x{s[1]})")
    return s


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        log = m.log()
        check("DISPLAY: Bochs/QEMU VBE adapter" in log, "the VBE adapter is found")
        check(m.start_gui(), "the shell started (VGA, 640x480)")
        vga = shot_ok(m, "vesa-vga", (640, 480), "the GUI starts in VGA mode 12h")
        desk16 = pixel(vga, 300, 300)

        # --- 1024x768 in high colour, then No ---
        choose(m, (1024, 768), 16)
        check(m.wait_log("DISPLAY: 1024x768, 16 bits"), "the driver switches to 1024x768, 16 bits")
        s = shot_ok(m, "vesa-1024", (1024, 768), "the screen is 1024x768")
        top, bottom = pixel(s, 500, 10), pixel(s, 500, 730)
        check(top != bottom and not near(top, desk16, 2), "high colour: the desktop is a gradient")
        face = pixel(s, 500, 768 - 14)
        check(near(face, pixel(vga, 300, 480 - 14), 8), "the taskbar runs along the new bottom edge")
        m.key("esc")                                    # "Keep this setting?" No
        check(m.wait_log("GUI: display reverted"), "No goes back")
        shot_ok(m, "vesa-back", (640, 480), "... to 640x480")

        # --- 800x600 in 256 colours: no answer reverts ---
        choose(m, (800, 600), 8)
        check(m.wait_log("DISPLAY: 800x600, 8 bits"), "800x600 in 256 colours")
        s = shot_ok(m, "vesa-800x8", (800, 600), "the screen is 800x600")
        check(near(pixel(s, 400, 300), desk16, 4), "256 colours: the theme's colours (the DAC)")
        check(m.wait_log("GUI: display reverted", 2, timeout=25), "15 seconds without an answer go back")

        # --- 800x600 in true colour, kept ---
        choose(m, (800, 600), 32)
        check(m.wait_log("DISPLAY: 800x600, 32 bits"), "800x600 in true colour")
        m.key("ret")
        check(m.wait_log("GUI: display kept"), "Yes keeps it")
        for k in ("ctrl-esc", "p", "n"):
            m.key(k)
        check(m.wait_log("GUI: notepad ready"), "a program starts in the new mode")
        shot_ok(m, "vesa-800x32", (800, 600), "... on the 800x600 screen")

        # --- a DOS session and back ---
        for k in ("ctrl-esc", "p", "m"):
            m.key(k)
        check(m.wait_log("focus -> DOS 1"), "a DOS session comes to the front")
        shot_ok(m, "vesa-dos", (720, 400), "it has the text screen")
        m.key("alt-tab")
        m.wait_log("focus -> console")
        s = shot_ok(m, "vesa-return", (800, 600), "Alt+Tab back restores 800x600")
        check(not near(pixel(s, 700, 10), (0, 0, 0), 4), "... and the GUI repaints it")
        m.key("alt-tab")
        m.wait_log("focus -> DOS 1", 2)
        m.type("exit\n")
        m.wait_log("session in VM 1 ended")

        # --- the next session starts in the kept mode ---
        m.wait_log("focus -> console", 2)
        check(m.shut_down(), "the GUI shuts down")
        check(m.start_gui() and m.wait_log("GUI: display 800x600x32 from OS1993.INI"),
              "the next session starts in the kept mode (OS1993.INI)")
        shot_ok(m, "vesa-ini", (800, 600), "... at 800x600")

        # --- back to VGA ---
        choose.opened = 0
        choose(m, (640, 480), 4)
        check(m.wait_log("DISPLAY: 640x480, 4 bits"), "640x480 in 16 colours")
        m.key("ret")
        m.wait_log("GUI: display kept", 2)
        s = shot_ok(m, "vesa-planar", (640, 480), "it is VGA mode 12h again")
        check(near(pixel(s, 300, 300), desk16, 2), "... with the 16-colour desktop")
        check(m.shut_down(), "the GUI shuts down")
    sys.exit(1 if check.failures else 0)


main()
