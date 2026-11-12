#!/usr/bin/env python3
"""The DPMI host: protected-mode programs in a DOS session.

DOSVM D (test/dos/dosvm.asm) talks to the host directly: the mode switch,
descriptors, memory, a real-mode interrupt through 0300h, interrupt vectors,
and the end through INT 21h. Then DPMITEST.EXE (test/dos/dpmitest.c, Open
Watcom), a DOS/4GW program: 32-bit flat C with malloc, files and the screen
-- DOS/4GW finds the host and runs on it. Then DOS32A, another extender,
runs the same program. The session must carry on after each.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import BUILD, Checker, Machine  # noqa: E402

check = Checker()
W = "/opt/watcom/binw"


def results(m, name, want):
    text = m.fetch(name)
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    for l in lines:
        if not l.startswith("PASS"):
            print("        " + name + ": " + l)
    passed = sum(l.startswith("PASS") for l in lines)
    failed = sum(l.startswith("FAIL") for l in lines)
    check(passed >= want and not failed, f"{name}: {passed} of {want} checks")


def main():
    bins = {"DOSVM.COM": os.path.join(BUILD, "DOSVM.COM"),
            "DPMITEST.EXE": os.path.join(BUILD, "watcom", "DPMITEST.EXE"),
            "DOS4GW.EXE": os.path.join(W, "dos4gw.exe"),
            "DOS32A.EXE": os.path.join(W, "dos32a.exe")}
    with Machine(binaries=bins) as m:
        if not m.wait_log("V86 self-test passed"):
            raise SystemExit("  FAIL  VMM did not boot")
        time.sleep(0.5)
        m.key("n")
        check(m.wait_log("Started DOS 1"), "a DOS session")
        time.sleep(1.5)

        m.type("dosvm d >dpmi.txt\n")
        check(m.wait_log("DPMI: VM 1: a 32-bit client"), "DOSVM D enters protected mode")
        check(m.wait_log("DPMI: VM 1's client ended"), "... and ends from there")
        time.sleep(1)

        m.type("dpmitest >dos4gw.txt\n")
        check(m.wait_log("DPMI: VM 1: a 32-bit client", 2, timeout=40), "DOS/4GW uses the host")
        check(m.wait_log("DPMI: VM 1's client ended", 2, timeout=60), "... and ends")
        time.sleep(1)

        m.type("dos32a dpmitest >dos32a.txt\n")
        check(m.wait_log("DPMI: VM 1: a 32-bit client", 3, timeout=40), "DOS32A uses the host")
        check(m.wait_log("DPMI: VM 1's client ended", 3, timeout=60), "... and ends")
        time.sleep(1)

        m.type("exit\n")
        check(m.wait_log("session in VM 1 ended"), "the session carries on and ends")
        m.key("x", 1)
        m.wait_exit(30)
        print("\n".join("        " + l for l in m.log().splitlines() if l.startswith("DPMI:")))
        results(m, "DPMI.TXT", 10)
        results(m, "DOS4GW.TXT", 8)
        results(m, "DOS32A.TXT", 8)
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
