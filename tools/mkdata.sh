#!/bin/sh
# Build a data disk for the VMM's own IDE driver and file systems:
#   partition 1: FAT32, about 48 MB      partition 2: FAT16, about 15 MB
#   partition 3: exFAT, 32 MB            partition 4: ext2, 32 MB (1 KB blocks)
# with a few files (long names among them) written by mtools, by
# tools/exfat.py on the exFAT one, and by debugfs on ext2.
# usage: mkdata.sh OUT.img
set -e
out=$1
tmp=$out.tmp
rm -f "$tmp"
truncate -s 128M "$tmp"
sfdisk -q "$tmp" <<PART
label: dos
2048,98304,c
100352,30720,6
131072,65536,7
196608,65536,83
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
dd if="$e2" of="$tmp" bs=512 seek=196608 conv=notrunc status=none
rm -f "$e2"
mv "$tmp" "$out"
