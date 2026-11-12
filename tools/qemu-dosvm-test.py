#!/usr/bin/env python3
"""DOS sessions' virtual devices, through DOSVM.COM (test/dos/dosvm.asm).

Each test runs in a DOS session with its output redirected to a file in
C:\\OS1993; the files are read off the disk image afterwards, and the
kernel's log is checked as it goes.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import BUILD, Checker, Machine  # noqa: E402

check = Checker()
DOSVM = os.path.join(BUILD, "DOSVM.COM")


def results(m, name, want, fails=0):
    text = m.fetch(name)
    lines = [l.strip() for l in text.splitlines()]
    for l in lines:
        if l and not l.startswith(("PASS", "FAIL")):
            print("        " + name + ": " + l)
    passed = sum(l.startswith("PASS") for l in lines)
    failed = [l for l in lines if l.startswith("FAIL")]
    if len(failed) != fails:
        for l in failed:
            print("        " + name + ": " + l)
    check(passed >= want and len(failed) == fails,
          f"{name}: {passed} of {want} checks" + (f", {len(failed)} refused as expected" if fails else ""))


def main():
    with Machine(binaries={"DOSVM.COM": DOSVM}, vmport=True,
                 extra=("-serial", "file:COM2.LOG", "-parallel", "file:LPT1.LOG")) as m:
        if not m.wait_log("V86 self-test passed"):
            raise SystemExit("  FAIL  VMM did not boot")
        time.sleep(0.5)
        m.key("n")
        check(m.wait_log("Started DOS 1"), "a DOS session")
        time.sleep(1.5)

        # --- the virtual timer ---
        m.type("dosvm p >pit.txt\n")
        check(m.wait_log("VPIT: VM 1 timer at 1000 Hz"), "the VM sets its timer to 1 kHz")
        check(m.wait_log("VPIT: real timer at 1000 Hz"), "... so the real timer runs at 1 kHz")
        check(m.wait_log("VPIT: real timer at 18 Hz", timeout=20), "... and back to 18.2 Hz with the VM's")
        time.sleep(1)

        # --- idle detection ---
        m.type("dosvm i >idle.txt\n")
        time.sleep(3)
        m.key("q")
        time.sleep(1)

        # --- the INT 33h mouse ---
        m.type("dosvm m >mouse.txt\n")
        time.sleep(2)
        m.abs_move(320, 240)                            # the middle: 320,100 in text mode's 640x200
        m.abs_click()
        time.sleep(1.5)

        # --- XMS and INT 15h ---
        m.type("dosvm x >xms.txt\n")
        time.sleep(1.5)
        m.type("dosvm e >e15.txt\n")
        time.sleep(1.5)

        # --- serial and printer ports ---
        m.type("dosvm c 2 >c2a.txt\n")
        check(m.wait_log("VDEV: COM2 (IRQ 3) now belongs to VM 1"), "COM2 goes to the first VM that uses it")
        time.sleep(1.5)
        m.type("dosvm c 1 >c1.txt\n")
        check(m.wait_log("VDEV: VM 1 may not use COM1 (the VMM's debug line)"), "COM1 stays the VMM's")
        time.sleep(0.5)
        m.type("dosvm l >lpt.txt\n")
        check(m.wait_log("VDEV: LPT1 (IRQ 7) now belongs to VM 1"), "LPT1 too")
        time.sleep(0.5)
        m.key("alt-tab", 1)
        m.key("n")
        check(m.wait_log("Started DOS 2"), "a second session")
        time.sleep(1.5)
        m.type("dosvm c 2 >c2b.txt\n")
        check(m.wait_log("VDEV: VM 2 wants COM2, which VM 1 owns"), "... which can't have COM2 while VM 1 has it")
        time.sleep(0.5)
        m.type("exit\n")
        m.wait_log("session in VM 2 ended")
        time.sleep(0.5)
        m.key("alt-tab", 1)

        m.type("exit\n")
        check(m.wait_log("session in VM 1 ended"), "the session ended")
        check(m.wait_log("VDEV: COM2 is free again"), "... and its ports are free again")
        log = m.log()
        idled = 0
        for l in log.splitlines():
            if "session in VM 1 ended" in l and "idled " in l:
                idled = int(l.split("idled ")[1].split()[0])
        check(idled > 10, f"a VM polling the keyboard was idled ({idled} times)")

        time.sleep(0.5)
        m.key("n")
        check(m.wait_log("Started DOS 3"), "a third session")
        time.sleep(1.5)
        m.type("dosvm c 2 >c2c.txt\n")
        check(m.wait_log("VDEV: COM2 (IRQ 3) now belongs to VM 1", 2), "... gets COM2 now")
        time.sleep(1.5)
        m.type("dosvm k >memall.txt\n")             # what a session gets by default
        time.sleep(1)
        # --- per-session settings: OS1993.INI [DOS], read for each new session ---
        for line in ("echo [DOS]>os1993.ini", "echo Memory=400>>os1993.ini", "echo XMS=0>>os1993.ini",
                     "echo Devices=LPT1>>os1993.ini", "echo Mouse=0>>os1993.ini"):
            m.type(line + "\n")
            time.sleep(0.3)
        m.type("exit\n")
        m.wait_log("session in VM 1 ended", 2)
        time.sleep(0.5)
        m.key("n")
        check(m.wait_log("Started DOS 4"), "a session with OS1993.INI's settings")
        check(m.wait_log("VM 1 has 400 KB of conventional memory"), "Memory=400")
        time.sleep(1.5)
        m.type("dosvm k >mem400.txt\n")
        time.sleep(1)
        m.type("dosvm c 2 >c2d.txt\n")
        check(m.wait_log("VDEV: VM 1 may not use COM2 (its settings)"), "Devices=LPT1: no COM2")
        time.sleep(1)
        m.type("dosvm x >xms0.txt\n")
        time.sleep(1)
        m.type("dosvm m >mouse0.txt\n")
        time.sleep(1)
        m.type("exit\n")
        m.wait_log("session in VM 1 ended", 3)
        time.sleep(0.5)
        m.key("x", 1)
        m.wait_exit(30)
        results(m, "C2A.TXT", 2)
        results(m, "C1.TXT", 0, 1)
        results(m, "LPT.TXT", 1)
        results(m, "C2B.TXT", 0, 1)
        results(m, "C2C.TXT", 2)
        results(m, "MOUSE.TXT", 4)
        text = m.fetch("MOUSE.TXT")
        at = text.split("clicked at ")[1].split()[0] if "clicked at " in text else "?"
        x, y = (int(v) for v in at.split(",")) if "," in at else (-1, -1)
        check(abs(x - 320) <= 8 and abs(y - 100) <= 8, f"the handler got the click where it was ({at})")
        results(m, "XMS.TXT", 8)
        results(m, "E15.TXT", 1)
        def kb(name, what):
            t = m.fetch(name)
            for l in t.splitlines():
                if l.startswith(what):
                    return int(l.split(": ")[1].split()[0])
            return -1
        bios_all, free_all = kb("MEMALL.TXT", "INT 12h"), kb("MEMALL.TXT", "largest")
        bios400, free400 = kb("MEM400.TXT", "INT 12h"), kb("MEM400.TXT", "largest")
        print(f"        default: INT 12h {bios_all} KB, largest block {free_all} KB; "
              f"Memory=400: {bios400} KB, {free400} KB")
        check(bios400 == 400 and 0 < free400 < 400 and free_all > free400 + 100,
              "the session with Memory=400 has 400 KB")
        results(m, "C2D.TXT", 0, 1)
        results(m, "XMS0.TXT", 0, 1)
        results(m, "MOUSE0.TXT", 0, 1)
        results(m, "PIT.TXT", 2)
        results(m, "IDLE.TXT", 1)
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
