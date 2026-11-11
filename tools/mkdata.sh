#!/bin/sh
# Build a data disk for the VMM's own IDE driver and file systems:
#   partition 1: FAT32, about 48 MB      partition 2: FAT16, about 15 MB
#   partition 3: exFAT, 32 MB            partition 4: extended, with three logical drives:
#     ext2, 32 MB (1 KB blocks); ext2, 32 MB (8 KB blocks); ext3, 32 MB, left
#     needing recovery: two committed transactions in its journal (debugfs),
#     the second revoking a block the first logged; ext4, 32 MB (4 KB blocks,
#     64-bit, metadata_csum): extents (a depth-1 tree, an uninitialized one),
#     a hashed directory, an inode with a broken checksum, and a transaction
#     with journal checksums (csum v3) left to replay; a journal device (4 MB)
#     and ext3 using it (32 MB, data=journal), two transactions left in it
#     (the old CRC32 checksums), the second's commit checksum broken; a journal device (4 MB)
#     and ext3 using it (32 MB, data=journal), two transactions left in it
#     (the old CRC32 checksums), the second's commit checksum broken
# with a few files (long names among them) written by mtools, by
# tools/exfat.py on the exFAT one, and by debugfs on ext2.
# usage: mkdata.sh OUT.img
#        mkdata.sh --big OUT.img   a 5 GB exFAT disk with 4 KB sectors and no
#                                  partition table, a 4.5 GB file on it (sparse)
#        mkdata.sh --btrfs OUT.img three btrfs partitions: zstd, LZO, zlib
set -e
if [ "$1" = --btrfs ]; then
    # three btrfs partitions (48 MB each, mixed mode): zstd (with a subvolume),
    # LZO and zlib, the same files on each; on zlib's, one data sector of an
    # incompressible file damaged (its checksum no longer fits)
    out=$2
    rm -f "$out.tmp"
    truncate -s 150M "$out.tmp"
    sfdisk -q "$out.tmp" <<PART
label: dos
2048,98304,83
100352,98304,83
198656,98304,83
PART
    d=$(mktemp -d)
    mkdir -p "$d/root/Sub Volume Dir" "$d/root/Long Folder Name"
    printf 'Hello from btrfs.\n' > "$d/root/hello.txt"
    printf 'inline, in a subvolume\n' > "$d/root/Sub Volume Dir/small.txt"
    printf 'Inside btrfs.\n' > "$d/root/Long Folder Name/A file with a long name.txt"
    ln -s hello.txt "$d/root/link"
    python3 -c "
import sys, random
r = sys.argv[1] + '/root/'
open(r + 'big.bin', 'wb').write(bytes((i * 7 + i // 251) & 255 for i in range(300000)))
open(r + 'text.txt', 'w').write(''.join('line %d of a compressible text file\\n' % i for i in range(20000)))
random.seed(7)
open(r + 'random.bin', 'wb').write(b'UNIQUE-MARKER-0001' + bytes(random.getrandbits(8) for _ in range(16366)))" "$d"
    n=0
    for c in zstd lzo zlib; do
        img=$d/$c.img
        truncate -s 48M "$img"
        sub=""
        [ $c = zstd ] && sub="--subvol Sub Volume Dir"
        if [ $c = zstd ]; then
            mkfs.btrfs -q -f -M -L "BT$c" -r "$d/root" --subvol "Sub Volume Dir" --compress $c "$img" >/dev/null
        else
            mkfs.btrfs -q -f -M -L "BT$c" -r "$d/root" --compress $c "$img" >/dev/null
        fi
        if [ $c = zlib ]; then                          # damage random.bin's first sector
            python3 -c "
import sys
data = open(sys.argv[1], 'rb').read()
at = data.find(b'UNIQUE-MARKER-0001')
assert at > 0
with open(sys.argv[1], 'r+b') as f:
    f.seek(at + 100)
    b = f.read(1)
    f.seek(at + 100)
    f.write(bytes([b[0] ^ 0xFF]))" "$img"
        fi
        dd if="$img" of="$out.tmp" bs=512 seek=$((2048 + n * 98304)) conv=notrunc status=none
        n=$((n + 1))
    done
    rm -rf "$d"
    mv "$out.tmp" "$out"
    exit 0
fi
if [ "$1" = --big ]; then
    out=$2
    rm -f "$out.tmp"
    truncate -s 5G "$out.tmp"
    mkfs.exfat -q -L BIGEX -s 4096 -c 32K "$out.tmp" >/dev/null 2>&1 || mkfs.exfat -L BIGEX -s 4096 -c 32K "$out.tmp" >/dev/null
    python3 -I - "$out.tmp" "$(dirname "$0")" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
from exfat import ExFat
v = ExFat(sys.argv[1], 0)
size = 4 * 1024**3 + 512 * 1024**2
v.write_sparse("Huge file.bin", size, {0: b"start of the huge file", 4 * 1024**3 + 16: b"beyond 4 GB"})
v.write("small.txt", b"a small file on a big disk\r\n")
v.close()
PY
    mv "$out.tmp" "$out"
    exit 0
fi
out=$1
tmp=$out.tmp
rm -f "$tmp"
truncate -s 270M "$tmp"
sfdisk -q "$tmp" <<PART
label: dos
2048,98304,c
100352,30720,6
131072,65536,7
196608,348160,5
198656,65536,83
266240,65536,83
333824,65536,83
401408,65536,83
468992,8192,83
479232,65536,83
PART
mkfs.fat -F 32 -s 1 -n DATA32 -i 32323232 --offset 2048 "$tmp" 49152 >/dev/null
mkfs.fat -F 16 -s 4 -n DATA16 -i 16161616 --offset 100352 "$tmp" 15360 >/dev/null
p1="$tmp@@$((2048 * 512))"
p2="$tmp@@$((100352 * 512))"
export MTOOLS_SKIP_CHECK=1 MTOOLS_NO_VFAT=0
dir=$(mktemp -d)
printf 'Hello from FAT32.\r\n' > "$dir/hello.txt"
printf 'A file with a long name.\r\n' > "$dir/long"
printf 'Deep inside.\r\n' > "$dir/deep"
python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 7 + i // 251) & 255 for i in range(300000)))" > "$dir/big"
printf 'FAT16 here.\r\n' > "$dir/f16"
mcopy -i "$p1" "$dir/hello.txt" ::/HELLO.TXT
mcopy -i "$p1" "$dir/long" "::/A long file name.txt"
mmd -i "$p1" "::/Sub Folder" ::/SUB
mmd -i "$p1" "::/Sub Folder/Deeper"
mcopy -i "$p1" "$dir/deep" "::/Sub Folder/Deeper/Inside File.txt"
mcopy -i "$p1" "$dir/big" ::/BIG.BIN
mcopy -i "$p2" "$dir/f16" ::/README.TXT
rm -rf "$dir"
ex=$tmp.ex
rm -f "$ex"
truncate -s 32M "$ex"
mkfs.exfat -q -L DATAEX -c 4K "$ex" >/dev/null 2>&1 || mkfs.exfat -L DATAEX -c 4K "$ex" >/dev/null
dd if="$ex" of="$tmp" bs=512 seek=131072 conv=notrunc status=none
rm -f "$ex"
python3 -I - "$tmp" "$(dirname "$0")" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
from exfat import ExFat
v = ExFat(sys.argv[1], 131072 * 512)
v.write("Hello exFAT.txt", b"Hello from exFAT.\r\n")
v.mkdir("Long Folder Name")
v.write("Long Folder Name/A file in a folder with a long name.txt", b"Inside exFAT.\r\n")
v.write("README.TXT", b"exFAT here.\r\n")
v.write("big.bin", bytes((i * 7 + i // 251) & 255 for i in range(300000)))
v.close()
PY
e2=$tmp.e2
rm -f "$e2"
truncate -s 32M "$e2"
mke2fs -q -F -t ext2 -b 1024 -L DATAE2 -U 2e2e2e2e-0000-4000-8000-000000000002 -E root_owner=0:0 "$e2"
d2=$(mktemp -d)
printf 'Hello from ext2.\n' > "$d2/hello"
printf 'Inside ext2.\n' > "$d2/inside"
printf 'ext2 here.\r\n' > "$d2/readme"
python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 7 + i // 251) & 255 for i in range(300000)))" > "$d2/big"
cat > "$d2/cmds" <<CMDS
write $d2/hello "Hello ext2.txt"
mkdir "Long Directory Name"
write $d2/inside "Long Directory Name/A file in a directory with a long name.txt"
write $d2/readme README.TXT
write $d2/big big.bin
symlink link-to-hello "Hello ext2.txt"
CMDS
debugfs -w -f "$d2/cmds" "$e2" >/dev/null 2>&1
rm -rf "$d2"
dd if="$e2" of="$tmp" bs=512 seek=198656 conv=notrunc status=none
rm -f "$e2"
e8=$tmp.e8
rm -f "$e8"
truncate -s 32M "$e8"
mke2fs -q -F -t ext2 -b 8192 -L DATAE8 "$e8" 2>/dev/null
d8=$(mktemp -d)
printf 'ext2 with 8 KB blocks.\n' > "$d8/hello"
python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 7 + i // 251) & 255 for i in range(300000)))" > "$d8/big"
printf 'write %s/hello hello8k.txt\nwrite %s/big big.bin\n' "$d8" "$d8" > "$d8/cmds"
debugfs -w -f "$d8/cmds" "$e8" >/dev/null 2>&1
rm -rf "$d8"
dd if="$e8" of="$tmp" bs=512 seek=266240 conv=notrunc status=none
rm -f "$e8"
e3=$tmp.e3
rm -f "$e3"
truncate -s 32M "$e3"
mke2fs -q -F -t ext3 -b 1024 -J size=1 -L DATAE3 "$e3"
d3=$(mktemp -d)
printf 'old contents\n' > "$d3/v"; printf 'not replayed\n' > "$d3/r"
printf 'old second\n' > "$d3/s"; printf 'written before the crash\n' > "$d3/b"; printf 'old third\n' > "$d3/t"
printf 'write %s/v victim.txt\nwrite %s/r revoked.txt\nwrite %s/s second.txt\nwrite %s/b before.txt\nwrite %s/t third.txt\n' \
    "$d3" "$d3" "$d3" "$d3" "$d3" | debugfs -w -f - "$e3" >/dev/null 2>&1
T=$(debugfs -R "bmap third.txt 0" "$e3" 2>/dev/null)
V=$(debugfs -R "bmap victim.txt 0" "$e3" 2>/dev/null)
R=$(debugfs -R "bmap revoked.txt 0" "$e3" 2>/dev/null)
S=$(debugfs -R "bmap second.txt 0" "$e3" 2>/dev/null)
python3 -c "
import sys
def blk(s): return s.encode().ljust(1024, b'.')
open(sys.argv[1] + '/t1', 'wb').write(blk('REPLAYED!!!!\\n') + blk('WRONG WRONG!\\n'))
open(sys.argv[1] + '/t2', 'wb').write(blk('NEW SECOND\\n'))
open(sys.argv[1] + '/t3', 'wb').write(blk('BAD THIRD\\n'))" "$d3"
printf 'jo\njw -b %s,%s %s/t1\njw -r %s -b %s %s/t2\njc\n' "$V" "$R" "$d3" "$R" "$S" "$d3" | debugfs -w -f - "$e3" >/dev/null 2>&1
rm -rf "$d3"
dd if="$e3" of="$tmp" bs=512 seek=333824 conv=notrunc status=none
rm -f "$e3"
e4=$tmp.e4
rm -f "$e4"
truncate -s 32M "$e4"
mke2fs -q -F -t ext4 -b 4096 -L DATAE4 "$e4"
d4=$(mktemp -d)
printf 'Hello from ext4.\n' > "$d4/hello"
python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 7 + i // 251) & 255 for i in range(300000)))" > "$d4/big"
python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 13 + 1) & 255 for i in range(1048576)))" > "$d4/frag"
printf 'checksummed\n' > "$d4/corrupt"; printf 'not yet replayed!!!!!\n' > "$d4/jtxt"
head -c 8192 /dev/zero | tr '\0' 'P' > "$d4/pre"
{
    echo "write $d4/hello \"Hello ext4.txt\""; echo "write $d4/big big.bin"; echo "write $d4/frag frag.bin"
    echo "write $d4/corrupt corrupt.txt"; echo "write $d4/pre prealloc.bin"; echo "write $d4/jtxt journal.txt"
    echo "mkdir \"Extent Dir\""
    for i in $(seq 0 119); do echo "write $d4/hello \"Extent Dir/entry number $i in a big directory.txt\""; done
    for b in $(seq 10 10 250); do echo "punch frag.bin $b $b"; done
    echo "fallocate prealloc.bin 2 9"; echo "sif prealloc.bin size 40960"
} > "$d4/cmds"
debugfs -w -f "$d4/cmds" "$e4" >/dev/null 2>&1
e2fsck -fyD "$e4" >/dev/null 2>&1 || true              # hash the big directory
J=$(debugfs -R "bmap journal.txt 0" "$e4" 2>/dev/null)
python3 -c "
import sys
open(sys.argv[1], 'wb').write(b'replayed, checksummed\\n'.ljust(4096, b'.'))" "$d4/jw"
printf 'jo -c\njw -b %s %s/jw\njc\n' "$J" "$d4" | debugfs -w -f - "$e4" >/dev/null 2>&1
loc=$(debugfs -R "imap corrupt.txt" "$e4" 2>/dev/null | sed -n 's/.*located at block \([0-9]*\), offset 0x\([0-9a-f]*\).*/\1 \2/p')
python3 -c "
import sys
blk, off = int(sys.argv[2]), int(sys.argv[3], 16)
with open(sys.argv[1], 'r+b') as f:
    f.seek(blk * 4096 + off + 0x10)                 # i_mtime: the checksum no longer fits
    b = f.read(1)
    f.seek(blk * 4096 + off + 0x10)
    f.write(bytes([b[0] ^ 0x5A]))" "$e4" $loc
rm -rf "$d4"
dd if="$e4" of="$tmp" bs=512 seek=401408 conv=notrunc status=none
rm -f "$e4"
jd=$tmp.jd; fx=$tmp.fx
rm -f "$jd" "$fx"
truncate -s 4M "$jd"; truncate -s 32M "$fx"
JU=4a4a4a4a-0000-4000-8000-00000000000a; FU=3e3e3e3e-0000-4000-8000-00000000000b
mke2fs -q -F -O journal_dev -b 1024 -U $JU -L EXTJNL "$jd"
mke2fs -q -F -t ext2 -b 1024 -U $FU -L DATAX3 "$fx"
dx=$(mktemp -d)
printf 'old external\n' > "$dx/x"; printf 'data journalled\n' > "$dx/h"; printf 'not committed\n' > "$dx/u"
printf 'write %s/x ext.txt\nwrite %s/h hello.txt\nwrite %s/u uncommitted.txt\n' "$dx" "$dx" "$dx" \
    | debugfs -w -f - "$fx" >/dev/null 2>&1
printf 'feature has_journal\nssv journal_uuid %s\nssv journal_inum 0\nssv journal_dev 0x0803\n' $JU \
    | debugfs -w -f - "$fx" >/dev/null 2>&1
tune2fs -o journal_data "$fx" >/dev/null
python3 -c "
import sys, uuid
with open(sys.argv[1], 'r+b') as f:                  # the volume, among the device's users
    f.seek(2 * 1024 + 0x40); f.write((1).to_bytes(4, 'big'))
    f.seek(2 * 1024 + 0x100); f.write(uuid.UUID(sys.argv[2]).bytes)" "$jd" "$FU"
X=$(debugfs -R "bmap ext.txt 0" "$fx" 2>/dev/null)
U=$(debugfs -R "bmap uncommitted.txt 0" "$fx" 2>/dev/null)
python3 -c "
import sys
open(sys.argv[1] + '/xd', 'wb').write(b'external replay\\n'.ljust(1024, b'.'))
open(sys.argv[1] + '/ud', 'wb').write(b'WRONG: replayed\\n'.ljust(1024, b'.'))" "$dx"
printf 'jo -c -f %s\njw -b %s %s/xd\njw -b %s %s/ud\njc\n' "$jd" "$X" "$dx" "$U" "$dx" | debugfs -w -f - "$fx" >/dev/null 2>&1
CB=$(debugfs -R "logdump -f $jd" "$fx" 2>/dev/null | sed -n 's/.*sequence 2, type 2 (commit block) at block \([0-9]*\).*/\1/p')
python3 -c "
import sys
with open(sys.argv[1], 'r+b') as f:                  # the second commit's CRC32: wrong
    f.seek(int(sys.argv[2]) * 1024 + 16)          # (an external journal's blocks are the device's)
    b = f.read(1)
    f.seek(int(sys.argv[2]) * 1024 + 16)
    f.write(bytes([b[0] ^ 0xFF]))" "$jd" "$CB"
rm -rf "$dx"
dd if="$jd" of="$tmp" bs=512 seek=468992 conv=notrunc status=none
dd if="$fx" of="$tmp" bs=512 seek=479232 conv=notrunc status=none
rm -f "$jd" "$fx"
mv "$tmp" "$out"
