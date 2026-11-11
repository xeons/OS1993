#!/usr/bin/env python3
"""The C runtime: CRT.DLL and libcrt.a, and its protections.

CRTTEST.BIN (importing CRT.DLL) and CRTSTAT.BIN (linked with libcrt.a)
run the runtime's checks -- formatting, the bounds-checked functions,
numbers, the heap, setjmp, files, time. Then each protection is set off
on purpose and must end the program at once, with the reason in the log:
the stack canary, the heap's guards and double-free check,
_FORTIFY_SOURCE, Annex K's constraint handler, printf's %n and assert.
Each process gets a stack canary of its own. The shell carries on.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine  # noqa: E402

check = Checker()


def run(m, command):
    opened = m.log().count("GUI: run dialog")
    for k in ("ctrl-esc", "r"):
        m.key(k)
    m.wait_log("GUI: run dialog", opened + 1)
    m.type(command + "\n")


def main():
    with Machine() as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check("RANDOM: seeded" in m.log(), "the kernel's random numbers are seeded")
        check(m.start_gui(), "the shell started")

        for prog in ("CRTTEST.BIN", "CRTSTAT.BIN"):
            run(m, prog.lower())
            ok = m.wait_log(f"USER: {prog} exited", timeout=60)
            line = re.search(rf"CRT: {prog}: (\d+) of (\d+) checks passed", m.log(), re.I)
            fails = re.findall(r"CRT: FAIL .*", m.log())
            for f in fails:
                print("        " + f)
            check(ok and line and line.group(1) == line.group(2) and int(line.group(2)) > 60,
                  f"{prog}: {line.group(0) if line else 'no result'}")
            check(f"USER: {prog} exited with code 0" in m.log(), f"{prog} exits with code 0")
        log = m.log()
        dyn = re.search(r"started CRTTEST.BIN as process \d+ \((\d+) modules", log)
        stat = re.search(r"started CRTSTAT.BIN as process \d+ \((\d+) modules", log)
        check(dyn and stat and int(dyn.group(1)) > int(stat.group(1)),
              "CRTTEST.BIN loads CRT.DLL; CRTSTAT.BIN doesn't (the runtime is linked in)")

        cases = [
            ("smash", "stack buffer overrun"),
            ("heap", "heap corruption: a write ran past the end of a block"),
            ("double", "heap corruption: double free"),
            ("fortify", "buffer overrun: memcpy"),
            ("constraint", "runtime-constraint violation: strcpy_s: s doesn't fit"),
            ("percentn", "runtime-constraint violation: printf: %n is not supported"),
            ("assert", "assertion failed: deliberate"),
        ]
        for prog in ("CRTTEST.BIN", "CRTSTAT.BIN"):
            for n, (arg, reason) in enumerate(cases):
                before = m.log().count(f"USER: {prog} exited")
                run(m, f"{prog.lower()} {arg}")
                m.wait_log(f"USER: {prog} exited", before + 1)
                log = m.log()
                check(f"USER: {prog}: {reason}" in log and f"CRT: {arg} survived" not in log,
                      f"{prog} {arg}: ended -- {reason}")
            check(m.log().count(f"USER: {prog} exited with code -1033") == len(cases),
                  f"{prog}: each with the fail-fast exit code")

        canaries = []
        for prog in ("crttest.bin", "crttest.bin", "crtstat.bin"):
            before = len(re.findall(r"CRT: canary ([0-9a-f]{8})", m.log()))
            run(m, f"{prog} canary")
            m.wait_log("CRT: canary", before + 1)
            canaries = re.findall(r"CRT: canary ([0-9a-f]{8})", m.log())
        check(len(canaries) == 3 and len(set(canaries)) == 3, f"every process has its own canary {canaries}")
        check(all(c.endswith("00") and c != "00000000" for c in canaries), "... random, with a zero low byte")

        for k in ("ctrl-esc", "p", "c"):
            m.key(k)
        check(m.wait_log("GUI: calculator ready"), "the shell carries on (Calculator starts)")
        check(m.shut_down(), "the GUI shuts down")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
