#!/usr/bin/env python3
"""The VMM's own disks: IDE driver, FAT32, FAT16 and exFAT, the DOS redirector.

A 96 MB disk on the secondary IDE master (tools/mkdata.sh: FAT32 and
FAT16 partitions, with long names written by mtools, and exFAT, written by
tools/exfat.py) is the VMM's; FreeDOS had mounted the FAT ones as D: and
E:, and keeps those letters through the redirector; exFAT, which DOS
can't read, is F:. A DOS session runs E:\\DOSTEST.BAT (reading it through the
redirector too): DIR, TYPE, COPY, MD, REN, DEL, ECHO into files. Then
FSTEST.BIN works on both volumes through KERNEL.DLL, long names and all,
and checks what DOS did. After OS1993 ends, mtools reads what both wrote
from the image, and fsck.fat checks both file systems.
"""
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import BUILD, QDIR, ROOT, Checker, Machine  # noqa: E402

check = Checker()
P1, P2 = 2048 * 512, 100352 * 512

BATCH = r"""@echo off
dir d:\ > e:\dosout.txt
type d:\hello.txt >> e:\dosout.txt
type d:\SUBFOL~1\DEEPER\INSIDE~1.TXT >> e:\dosout.txt
copy d:\big.bin e:\big2.bin > nul
md e:\dosdir
echo made by DOS> e:\dosdir\dosfile.txt
ren e:\dosdir\dosfile.txt renamed.txt
del d:\gone.txt
echo DOS wrote this> d:\SUBFOL~1\fromdos.txt
dir e:\ >> e:\dosout.txt
e:
cd \dosdir
cd >> e:\dosout.txt
c:
type f:\readme.txt >> e:\dosout.txt
copy f:\big.bin f:\bigdos.bin > nul
md f:\dosx
echo exfat from DOS> f:\dosx\dos.txt
dir f:\ >> e:\dosout.txt
type g:\readme.txt >> e:\dosout.txt
copy g:\big.bin g:\bigdos.bin > nul
md g:\dosx
echo ext2 from DOS> g:\dosx\dos.txt
dir g:\ >> e:\dosout.txt
echo batch done>> e:\dosout.txt
exit
"""

AFTER = r"""@echo off
dir d:\ > c:\after.txt
type d:\fromgui.txt >> c:\after.txt
type e:\dosdir\renamed.txt >> c:\after.txt
echo DOS again> d:\afterdos.txt
"""


def mtools(cmd, part, *args, **kw):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    return subprocess.run([cmd, "-i", f"{IMG}@@{part}", *args], env=env, capture_output=True, **kw)


def read(part, path):
    r = mtools("mtype", part, path)
    return r.stdout.decode("latin-1") if r.returncode == 0 else None


IMG = os.path.join(QDIR, "fat.img")


def main():
    os.makedirs(QDIR, exist_ok=True)
    subprocess.run([os.path.join(ROOT, "tools", "mkdata.sh"), IMG], check=True)
    with tempfile.TemporaryDirectory() as d:
        bat = os.path.join(d, "DOSTEST.BAT")
        with open(bat, "w", newline="\r\n") as f:
            f.write(BATCH)
        gone = os.path.join(d, "GONE.TXT")
        with open(gone, "w") as f:
            f.write("delete me\r\n")
        mtools("mcopy", P2, bat, "::/DOSTEST.BAT", check=True)
        mtools("mcopy", P1, gone, "::/GONE.TXT", check=True)

    with Machine(data_disk=IMG, files={"AFTER.BAT": AFTER}) as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        log = m.log()
        check("ATA: disk 2 (secondary master): QEMU HARDDISK, 128 MB, BIOS 81h, the VMM's" in log
              and "BIOS 80h, left to DOS" in log, "IDE: the boot disk stays DOS's, the second channel is the VMM's")
        check("FAT32 on disk 2 at 2048: 96760 clusters of 512 bytes, serial 3232-3232" in log
              and "FAT16 on disk 2 at 100352: 7655 clusters of 2048 bytes" in log, "FAT32 and FAT16 mounted")
        check("REDIR: D: is the VMM's now (FAT32" in log and "REDIR: E: is the VMM's now (FAT16" in log,
              "DOS's D: and E: go through the redirector now")
        check("exFAT on disk 2 at 131072: 7" in log and "REDIR: F: for the VMM's exFAT volume" in log,
              "exFAT mounted, F: for DOS")
        check("ext2 on disk 2 at 196608: 32768 blocks of 1024 bytes" in log and "REDIR: G: for the VMM's ext2 volume" in log,
              "ext2 mounted, G: for DOS")

        m.key("n")
        m.wait_log("Started DOS 1")
        m.wait_log("V86 self-test passed")
        import time
        time.sleep(1.5)
        m.type("e:\\dostest\n")
        check(m.wait_log("session in VM 1 ended", timeout=90), "a DOS session ran E:\\DOSTEST.BAT")
        time.sleep(0.5)

        check(m.start_gui(), "the shell started")
        opened = m.log().count("GUI: run dialog")
        for k in ("ctrl-esc", "r"):
            m.key(k)
        m.wait_log("GUI: run dialog", opened + 1)
        m.type("fstest.bin\n")
        m.wait_log("USER: FSTEST.BIN exited", timeout=120)
        log = m.log()
        for f in re.findall(r"FS: FAIL .*", log):
            print("        " + f)
        line = re.search(r"FS: (\d+) of (\d+) checks passed", log)
        check(line and line.group(1) == line.group(2) and int(line.group(2)) >= 115,
              line.group(0) if line else "FSTEST: no result")
        check(m.shut_down(), "the GUI shuts down")
        m.key("x", 1)
        check(m.wait_exit(30), "OS1993 returns to DOS")
        for f in re.findall(r"(?:V86|CACHE|ATA): .*(?:error|stopped|lost).*", m.log()):
            print("        " + f)

    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    r = subprocess.run(["mtype", "-i", os.path.join(BUILD, "freedos.img") + "@@32256", "::/AFTER.TXT"],
                       env=env, capture_output=True)
    after = r.stdout.decode("latin-1")
    check("Written by an OS1993 program." in after and "made by DOS" in after and "NEWLON~1" in after,
          "back in DOS, D: and E: are DOS's own again and show what the VMM wrote")
    check(read(P1, "::/AFTERDOS.TXT") == "DOS again\r\n", "... and DOS writes to D: itself")

    out = read(P2, "::/DOSOUT.TXT") or ""
    check("Hello from FAT32." in out and "Deep inside." in out, "DOS's TYPE through the redirector")
    check(re.search(r"BIG\s+BIN\s+300[ ,.]?000", out) and "HELLO" in out, "DOS's DIR of D:\\")
    check("E:\\DOSDIR" in out and "batch done" in out, "DOS's CD into E:\\DOSDIR")
    check(read(P1, "::/FROMGUI.TXT") == "Written by an OS1993 program.\r\n", "mtools reads D:\\FROMGUI.TXT")
    r = mtools("mdir", P2, "-b", "::/GUIDIR")
    names = r.stdout.decode().split()
    check(r.returncode == 0 and len([n for n in names if "rather long file name" in n or n.endswith(".txt")]) >= 1
          and len(r.stdout.decode().strip().splitlines()) == 20, "mtools lists E:\\GUIDIR: 20 long names")
    r = mtools("mdir", P1, "-b", "::/")
    check(b"New Long Name Document.txt" in r.stdout and b"GONE.TXT" not in r.stdout,
          "mtools sees the long name the VMM wrote, and not the file DOS deleted")
    check("exFAT here." in out and re.search(r"BIGDOS\s+BIN\s+300[ ,.]?000", out), "DOS reads and writes exFAT through the redirector")
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from exfat import ExFat
    try:
        x = ExFat(IMG, 131072 * 512)
        check(x.read("FROMGUI.TXT") == b"exFAT, written by an OS1993 program.\r\n", "tools/exfat.py reads F:\\FROMGUI.TXT")
        names = [e.name for e in x.listdir("GUIDIR")]
        check(len(names) == 40 and "Entry number 78 of a folder that grows.txt" in names,
              "... and F:\\GUIDIR's 40 long names (checksums and name hashes right)")
        check(x.read("frag1.bin") == bytes((i * 31 + 7) & 255 for i in range(6000)), "... and the file that was in pieces")
        x.f.close()
    except Exception as e:                              # noqa: BLE001
        check(False, f"tools/exfat.py reads the exFAT volume ({e})")
    check("ext2 here." in out and re.search(r"BIGDOS\s+BIN\s+300[ ,.]?000", out), "DOS reads and writes ext2 through the redirector")
    with open(IMG, "rb") as f:
        disk = f.read()
    with tempfile.NamedTemporaryFile(suffix=".img") as t:
        t.write(disk[196608 * 512:(196608 + 65536) * 512])
        t.flush()
        r = subprocess.run(["e2fsck", "-fn", t.name], capture_output=True, text=True)
        for l in r.stdout.splitlines():
            if not l.startswith(("Pass ", "e2fsck", "DATAE2:")):
                print("        " + l)
        check(r.returncode == 0, "e2fsck: the ext2 volume is clean")

        def dbg(cmd):
            return subprocess.run(["debugfs", "-R", cmd, t.name], capture_output=True).stdout

        check(dbg("cat FROMGUI.TXT") == b"ext2, written by an OS1993 program.\r\n", "debugfs reads G:\\FROMGUI.TXT")
        ls = dbg("ls -p GUIDIR").decode("latin-1")
        check(len(re.findall(r"Entry number \d+ of a directory that grows\.txt", ls)) == 40,
              "... and G:\\GUIDIR's 40 names")
        check(dbg('cat "A large new file.dat"') == bytes((i * 17 + 3) & 255 for i in range(5000)),
              "... and the truncated file")
    with tempfile.NamedTemporaryFile(suffix=".img") as t:
        t.write(disk[131072 * 512:(131072 + 65536) * 512])
        t.flush()
        r = subprocess.run(["fsck.exfat", "-n", t.name], capture_output=True, text=True)
        for l in (r.stdout + r.stderr).splitlines()[-6:]:
            if "clean" not in l and "version" not in l:
                print("        " + l)
        check(r.returncode == 0 and "clean" in r.stdout, "fsck.exfat: the exFAT volume is clean")
    for name, start, count in (("FAT32", 2048, 98304), ("FAT16", 100352, 30720)):
        with tempfile.NamedTemporaryFile(suffix=".img") as t:
            t.write(disk[start * 512:(start + count) * 512])
            t.flush()
            r = subprocess.run(["fsck.fat", "-n", "-v", t.name], capture_output=True, text=True)
            bad = [l for l in r.stdout.splitlines() if re.search(r"(?i)differ|lost|free cluster summary wrong|bad|invalid|wrong|orphan|reclaim", l)
                   and not l.startswith("Checking")]
            for l in bad[:8]:
                print("        " + l)
            check(r.returncode == 0 and not bad, f"fsck.fat: the {name} volume is clean")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
