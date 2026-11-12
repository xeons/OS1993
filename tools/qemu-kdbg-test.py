#!/usr/bin/env python3
"""The kernel debugger on COM1 (OS1993 /D) and the double-fault task.

Ctrl+C on the line breaks in; R, .T, .VM, .M, DB on a VM's video memory,
.PT; an execution breakpoint (the debug registers) on dos_new_session hit
by pressing N, K's stack, a single step, a data watch hit by the session's
own screen; then .CRASH: a kernel thread runs off its stack into the guard
page, the double fault arrives in its own task, and the debugger and the
blue screen both name the thread.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import BUILD, Checker, Machine  # noqa: E402

check = Checker()


def sym(name):
    out = subprocess.run(["i686-elf-nm", os.path.join(BUILD, "vmm.elf")], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return parts[0]
    raise SystemExit(f"no symbol {name}")


def main():
    with Machine(serial_sock=True, files={"RUN.BAT": "OS1993 /D\n"}) as m:
        def cmd(text, expect, what, n=1):
            m.serial_send(text + "\r")
            return check(m.wait_log(expect, n, 10), what)

        check(m.wait_log("KDBG: kernel debugger on COM1"), "OS1993 /D: the debugger is on")
        if not m.wait_log("V86 self-test passed"):
            raise SystemExit("  FAIL  VMM did not boot")
        m.serial_send("\x03")
        check(m.wait_log("*** KDBG: break (Ctrl+C)", 1, 10), "Ctrl+C breaks in")
        cmd("r", "CR2=", "R shows the registers")
        cmd(".t", "System VM", ".T lists the threads")
        cmd(".vm", "System VM", ".VM lists the VMs", 2)
        cmd(".m", "frames:", ".M: memory")
        cmd("db 0:46C 4", "0000046C  ", "DB at seg:off (the BIOS tick count)")
        cmd(".pt 80000000", "PTE = ", ".PT walks the page tables")
        bp = sym("dos_new_session")
        cmd("bp " + bp, "breakpoint 0 at " + bp.upper(), "BP sets an execution breakpoint")
        m.serial_send("g\r")
        m.key("n")
        check(m.wait_log("*** KDBG: breakpoint 0 at " + bp.upper(), 1, 10), "... and N hits it")
        cmd("k", "(frame ", "K follows the EBP chain")
        cmd("t", "*** KDBG: step", "T steps one instruction")
        cmd("bc *", "#", "BC * clears it")
        m.serial_send("g\r")
        check(m.wait_log("Started DOS 1"), "G: the session starts")
        cmd_ok = m.wait_log("VMM: focus -> DOS 1")
        m.serial_send("\x03")
        m.wait_log("*** KDBG: break (Ctrl+C)", 2, 10)
        cmd("bw 0:41C 2", "breakpoint 0 at 0000041C", "BW: a write watch on the BIOS keyboard buffer tail")
        m.serial_send("g\r")
        m.key("a")
        check(cmd_ok and m.wait_log("*** KDBG: breakpoint 0 at 0000041C", 1, 10),
              "... a key typed in the session hits it (in V86 mode)")
        cmd("r", "V86 mode", "the frame is the VM's", 2)
        cmd("bc 0", "#", "BC 0")
        m.serial_send("g\r")
        m.key("backspace")
        m.type("exit\n")
        check(m.wait_log("session in VM 1 ended"), "the session goes on and ends")

        m.serial_send("\x03")
        m.wait_log("*** KDBG: break (Ctrl+C)", 3, 10)
        cmd(".crash", "a thread will overflow its stack", ".CRASH")
        m.serial_send("g\r")
        check(m.wait_log("*** double fault at", 1, 10), "the overflow double-faults into its own task")
        check(m.wait_log("Kernel stack overflow in thread crash test", 1, 10),
              "the debugger names the thread that overflowed")
        cmd("k", "(frame ", "K on the faulting frame", 2)
        m.serial_send("q\r")
        m.wait_log("*** FATAL", 1, 10)
        m.key("spc")                                    # Press any key to return to DOS
        check(m.wait_exit(30), "back to DOS from the blue screen")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
