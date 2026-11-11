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
from exfat import ExFat  # noqa: E402

check = Checker()
P1, P2 = 2048 * 512, 100352 * 512

BATCH = r"""@echo off
e:\lfntest.com
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
BIG = os.path.join(QDIR, "big.img")
E1, E8, E3 = 198656, 266240, 333824                    # the logical drives: ext2 (1 KB, 8 KB blocks), ext3


def main():
    slave = "--primary-slave" in sys.argv                # beside the boot disk: INT 13h is the VMM's
    pos, dnum, drive = ("primary slave", 1, "81h") if slave else ("secondary master", 2, "81h")
    print(f"  (the data disk is the {pos})")
    os.makedirs(QDIR, exist_ok=True)
    subprocess.run([os.path.join(ROOT, "tools", "mkdata.sh"), IMG], check=True)
    subprocess.run([os.path.join(ROOT, "tools", "mkdata.sh"), "--big", BIG], check=True)
    with tempfile.TemporaryDirectory() as d:
        bat = os.path.join(d, "DOSTEST.BAT")
        with open(bat, "w", newline="\r\n") as f:
            f.write(BATCH)
        gone = os.path.join(d, "GONE.TXT")
        with open(gone, "w") as f:
            f.write("delete me\r\n")
        mtools("mcopy", P2, bat, "::/DOSTEST.BAT", check=True)
        mtools("mcopy", P2, os.path.join(BUILD, "LFNTEST.COM"), "::/LFNTEST.COM", check=True)
        mtools("mcopy", P1, gone, "::/GONE.TXT", check=True)

    with Machine(data_disk=IMG, files={"AFTER.BAT": AFTER}, data_index=1 if slave else 2, big_disk=BIG) as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        log = m.log()
        check(re.search(rf"ATA: disk {dnum} \({pos}\): QEMU HARDDISK, 200 MB, LBA48, BIOS {drive} \(\d+/\d+/\d+\), the VMM's", log)
              and re.search(r"ATA: disk 0 \(primary master\).*BIOS 80h.*the VMM's", log)
              and "REDIR: C: stays DOS's (its boot drive)" in log,
              "IDE: both disks are the VMM's; DOS's C: goes through its INT 13h")
        check(f"FAT32 on disk {dnum} at 2048: 96760 clusters of 512 bytes, serial 3232-3232" in log
              and f"FAT16 on disk {dnum} at 100352: 7655 clusters of 2048 bytes" in log, "FAT32 and FAT16 mounted")
        check("REDIR: D: is the VMM's now (FAT32" in log and "REDIR: E: is the VMM's now (FAT16" in log,
              "DOS's D: and E: go through the redirector now")
        check(f"exFAT on disk {dnum} at 131072: 7" in log and "REDIR: F: for the VMM's exFAT volume" in log,
              "exFAT mounted, F: for DOS")
        check(f"ext2 on disk {dnum} at {E1}: 32768 blocks of 1024 bytes" in log and "REDIR: G: for the VMM's ext2 volume" in log,
              "ext2 (a logical drive) mounted, G: for DOS")
        check(f"ext2 on disk {dnum} at {E8}: 4096 blocks of 8192 bytes" in log and "REDIR: H: for the VMM's ext2 volume" in log,
              "ext2 with 8 KB blocks (the next logical drive), H:")
        check("EXT2: replayed 2 transaction(s) from the journal: 2 block(s), 1 revoked" in log
              and f"ext3 on disk {dnum} at {E3}: " in log and "read-only" in log
              and "REDIR: I: for the VMM's ext3 volume" in log,
              "ext3 needing recovery: its journal replayed (2 transactions, a revoke), I:")
        check("exFAT on disk 3 at 0: " in log and "REDIR: J: for the VMM's exFAT volume" in log,
              "a 5 GB exFAT disk with 4 KB sectors and no partition table, J:")

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
        check(line and line.group(1) == line.group(2) and int(line.group(2)) >= 156,
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

    lfn = read(P2, "::/LFNOUT.TXT") or ""
    for l in lfn.splitlines():
        if not l.startswith("PASS") and l != "DONE":
            print("        LFNTEST: " + l)
    check(lfn.count("PASS ") == 17 and "FAIL" not in lfn and "DONE" in lfn,
          f"LFNTEST.COM: the long-name calls, share modes and locks from DOS ({lfn.count('PASS ')} of 17)")
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
    try:
        x = ExFat(IMG, 131072 * 512)
        check(x.read("FROMGUI.TXT") == b"exFAT, written by an OS1993 program.\r\n", "tools/exfat.py reads F:\\FROMGUI.TXT")
        names = [e.name for e in x.listdir("GUIDIR")]
        check(len(names) == 40 and "Entry number 78 of a folder that grows.txt" in names,
              "... and F:\\GUIDIR's 40 long names (checksums and name hashes right)")
        check(x.read("frag1.bin") == bytes((i * 31 + 7) & 255 for i in range(6000)), "... and the file that was in pieces")
        ent = x.entry_raw("tz.txt")
        check(ent is not None and ent[23] == 0x84, "exFAT records the UTC offset (+1 hour)")
        x.f.close()
    except Exception as e:                              # noqa: BLE001
        check(False, f"tools/exfat.py reads the exFAT volume ({e})")
    check("ext2 here." in out and re.search(r"BIGDOS\s+BIN\s+300[ ,.]?000", out), "DOS reads and writes ext2 through the redirector")
    with open(IMG, "rb") as f:
        disk = f.read()
    with tempfile.NamedTemporaryFile(suffix=".img") as t:
        t.write(disk[E1 * 512:(E1 + 65536) * 512])
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
        ls = dbg("ls -l").decode("latin-1")
        check(re.search(r"120777.*sym-short", ls) and re.search(r"120777.*sym-long", ls) and
              re.search(r"Links: 2", dbg("stat README.TXT").decode("latin-1")),
              "debugfs: the symbolic links, and README.TXT with two names")
        st = dbg("stat sparse.bin").decode("latin-1")
        size = re.search(r"Size: (\d+)", st)
        blk = re.search(r"^(\d+)", dbg(f"bmap sparse.bin {5 * 1024 * 1024}").decode().strip())
        tail = disk[E1 * 512 + int(blk.group(1)) * 1024:][:4] if blk else b""
        check(size and int(size.group(1)) == 5 * 1024**3 + 4 and tail == b"tail",
              "... and the 5 GB sparse file (its size, and its last block)")
        tz = dbg("cat tz.txt").decode()
        mt = re.search(r"mtime: (0x[0-9a-f]+)", dbg("stat tz.txt").decode())
        import calendar
        import time as _t
        if tz and mt:
            local = calendar.timegm(_t.strptime(tz, "%Y-%m-%d %H:%M:%S"))
            diff = int(mt.group(1), 16) - local
        check(tz and mt and abs(diff + 3600) <= 2, "ext2 keeps UTC: written at UTC+1, an hour earlier than the clock")
    with tempfile.NamedTemporaryFile(suffix=".img") as t:
        t.write(disk[E8 * 512:(E8 + 65536) * 512])
        t.flush()
        r = subprocess.run(["e2fsck", "-fn", t.name], capture_output=True, text=True)
        check(r.returncode == 0, "e2fsck: the 8 KB-block ext2 volume is clean")
        w = subprocess.run(["debugfs", "-R", "cat written.bin", t.name], capture_output=True).stdout
        check(w == bytes((i * 3 + 1) & 255 for i in range(200000)), "debugfs reads what the VMM wrote with 8 KB blocks")
    with tempfile.NamedTemporaryFile(suffix=".img") as t:
        t.write(disk[E3 * 512:(E3 + 65536) * 512])
        t.flush()
        r = subprocess.run(["e2fsck", "-fn", t.name], capture_output=True, text=True)
        h = subprocess.run(["dumpe2fs", "-h", t.name], capture_output=True, text=True).stdout
        check(r.returncode == 0 and "needs_recovery" not in h and re.search(r"Journal start:\s+0", h),
              "e2fsck: the ext3 volume is clean, its journal empty, needs_recovery gone")
    r = subprocess.run(["fsck.exfat", "-n", BIG], capture_output=True, text=True)
    check(r.returncode == 0 and "clean" in r.stdout, "fsck.exfat: the 5 GB disk is clean")
    xb = ExFat(BIG, 0)
    check(xb.read_at("Huge file.bin", 4 * 1024**3 + 1024, 17) == b"written past 4 GB",
          "tools/exfat.py finds the bytes written past 4 GB")
    xb.f.close()
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
            bad = [l for l in r.stdout.splitlines() if re.search(r"(?i)differ|lost|free cluster summary wrong|bad|invalid|wrong|orphan|reclaim|dirty", l)
                   and not l.startswith("Checking")]
            for l in bad[:8]:
                print("        " + l)
            check(r.returncode == 0 and not bad, f"fsck.fat: the {name} volume is clean")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
