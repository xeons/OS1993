#!/usr/bin/env python3
"""Check the absolute pointer (VMware port, QEMU vmport=on).

The host sends absolute positions through QMP, as QEMU's window does when
the pointer moves over it. The guest cursor must land on exactly that pixel
-- that is what keeps OS1993's pointer under the host's -- and clicks must
hit the control there.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine, near, pixel, rgb  # noqa: E402

DESKTOP = (17, 30, 40)
check = Checker()


def main():
    with Machine(vmport=True) as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check("absolute positions via the VMware port" in m.log(),
              "VMM switched the mouse to absolute mode")
        m.key("g")
        check(m.wait_log("GUI: mouse ready"), "GUI started")

        for x, y in ((450, 420), (600, 440)):         # empty desktop
            m.abs_move(x, y)
            s = m.screenshot(f"abs-{x}-{y}")
            # The arrow's first row is a single pixel: its tip.
            tip = not near(pixel(s, x, y), rgb(DESKTOP))
            beside = near(pixel(s, x + 1, y), rgb(DESKTOP)) and near(pixel(s, x - 1, y), rgb(DESKTOP))
            check(tip and beside, f"cursor tip exactly at {x},{y}")

        m.abs_move(280, 103); m.abs_click()
        check(m.wait_log("GUI: check Sound effects on"), "absolute click hits the check box")
        m.abs_move(460, 151); m.abs_click()
        check(m.wait_log("GUI: radio Turbo"), "absolute click hits the radio button")

        m.key("esc")
        check(m.wait_log("USER: exited with code 0"), "Esc leaves the GUI")
        m.key("x", 1)
        check(m.wait_exit(), "QEMU powered off (absolute mode switched off for DOS)")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
