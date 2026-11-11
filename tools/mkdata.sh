#!/bin/sh
# Build a data disk for the VMM's own IDE driver and file systems:
#   partition 1: FAT32, about 48 MB      partition 2: FAT16, about 15 MB
#   partition 3: exFAT, 32 MB            partition 4: extended, with three logical drives:
#     ext2, 32 MB (1 KB blocks); ext2, 32 MB (8 KB blocks); ext3, 32 MB, left
#     needing recovery: two committed transactions in its journal (debugfs),
#     the second revoking a block the first logged
# with a few files (long names among them) written by mtools, by
# tools/exfat.py on the exFAT one, and by debugfs on ext2.
# usage: mkdata.sh OUT.img
#        mkdata.sh --big OUT.img   a 5 GB exFAT disk with 4 KB sectors and no
#                                  partition table, a 4.5 GB file on it (sparse)
set -e
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
truncate -s 200M "$tmp"
sfdisk -q "$tmp" <<PART
label: dos
2048,98304,c
100352,30720,6
131072,65536,7
196608,202752,5
198656,65536,83
266240,65536,83
333824,65536,83
PART
mkfs.fat -F 32 -n DATA32 -i 32323232 --offset 2048 "$tmp" 49152 >/dev/null
mkfs.fat -F 16 -n DATA16 -i 16161616 --offset 100352 "$tmp" 15360 >/dev/null
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
printf 'old second\n' > "$d3/s"; printf 'written before the crash\n' > "$d3/b"
printf 'write %s/v victim.txt\nwrite %s/r revoked.txt\nwrite %s/s second.txt\nwrite %s/b before.txt\n' \
    "$d3" "$d3" "$d3" "$d3" | debugfs -w -f - "$e3" >/dev/null 2>&1
V=$(debugfs -R "bmap victim.txt 0" "$e3" 2>/dev/null)
R=$(debugfs -R "bmap revoked.txt 0" "$e3" 2>/dev/null)
S=$(debugfs -R "bmap second.txt 0" "$e3" 2>/dev/null)
python3 -c "
import sys
def blk(s): return s.encode().ljust(1024, b'.')
open(sys.argv[1] + '/t1', 'wb').write(blk('REPLAYED!!!!\\n') + blk('WRONG WRONG!\\n'))
open(sys.argv[1] + '/t2', 'wb').write(blk('NEW SECOND\\n'))" "$d3"
printf 'jo\njw -b %s,%s %s/t1\njw -r %s -b %s %s/t2\njc\n' "$V" "$R" "$d3" "$R" "$S" "$d3" | debugfs -w -f - "$e3" >/dev/null 2>&1
rm -rf "$d3"
dd if="$e3" of="$tmp" bs=512 seek=333824 conv=notrunc status=none
rm -f "$e3"
mv "$tmp" "$out"
