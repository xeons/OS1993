#!/usr/bin/env python3
"""Threads, and the process, memory and file APIs.

THRTEST.BIN runs its checks: threads joined with their exit codes,
critical sections, events, semaphores, mutexes, waits with timeouts,
TLS, errno and GetLastError per thread, suspended and terminated threads,
the 16-thread limit, the C heap and stdio shared by threads, virtual
memory, private heaps, a child process waited for, and the new file calls.
Then: a thread running into its guard page, a write to a read-only page,
the first thread ending while another goes on, and exit() from a second
thread.
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


def ended(m, n):
    return m.wait_log("USER: THRTEST.BIN exited", n, timeout=90)


def main():
    with Machine(files={"OS1993.INI": "[386Enh]\nSwapFile=12288\n"}) as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.start_gui(), "the shell started")

        run(m, "thrtest.bin")
        check(ended(m, 2), "THRTEST.BIN ran (and its child)")   # the child exits first
        log = m.log()
        for f in re.findall(r"THR: FAIL .*", log):
            print("        " + f)
        line = re.search(r"THR: (\d+) of (\d+) checks passed", log)
        check(line and line.group(1) == line.group(2) and int(line.group(2)) >= 60,
              line.group(0) if line else "no result")
        check(re.search(r"USER: THRTEST.BIN exited with code 7", log) is not None, "the child exited with 7")
        lines = re.findall(r"THR: line from thread (\d), number (\d+) of 20", log)
        check(len(lines) == 60 and all(len([l for l in lines if l[0] == t]) == 20 for t in "123"),
              "printf from three threads: 60 whole lines")

        n = m.log().count("USER: THRTEST.BIN exited")
        run(m, "thrtest.bin stack")
        ended(m, n + 1)
        log = m.log()
        check(re.search(r"USER: THRTEST.BIN: thread \d+ overflowed its stack", log) and
              "THRTEST.BIN killed by exception 0E" in log, "a thread's stack overflow hits its guard page")

        before = len(m.log())
        run(m, "thrtest.bin readonly")
        ended(m, n + 2)
        check("THRTEST.BIN killed by exception 0E (page fault, write to" in m.log()[before:],
              "a write to a read-only page faults")

        run(m, "thrtest.bin exitthread")
        ended(m, n + 3)
        log = m.log()
        check("THR: the second thread outlived the first" in log and
              "USER: THRTEST.BIN exited with code 5" in log,
              "the first thread ends; the last one's code is the process's")

        run(m, "thrtest.bin exitprocess")
        ended(m, n + 4)
        log = m.log()
        check("USER: THRTEST.BIN exited with code 9" in log and "THR: the first thread went on" not in log,
              "exit() in a second thread ends the whole process")

        # demand paging: 16 MB committed on a 16 MB machine, through a 12 MB swap file
        check("PAGER: swap file C:\\OS1993\\WIN386.SWP" in m.log(), "a swap file next to VMM.BIN")
        run(m, "thrtest.bin swap 16")
        ended(m, n + 5)
        log = m.log()
        check("THR: swap: 16 MB written and read back twice, 0 bad pages" in log and
              "USER: THRTEST.BIN exited with code 0" in log, "16 MB committed on a 16 MB machine, written and read back twice")
        check("PAGER: memory is short: writing pages to the swap file" in log and
              "PAGER: reading pages back from the swap file" in log, "... through the swap file")

        for k in ("ctrl-esc", "p", "c"):
            m.key(k)
        check(m.wait_log("GUI: calculator ready"), "the shell carries on (Calculator starts)")
        check(m.shut_down(), "the GUI shuts down")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
