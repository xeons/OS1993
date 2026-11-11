#!/bin/sh
# Build a data disk for the VMM's own IDE driver and file systems:
#   partition 1: FAT32, about 48 MB      partition 2: FAT16, about 15 MB
#   partition 3: exFAT, 32 MB
# with a few files (long names among them) written by mtools, and by
# tools/exfat.py on the exFAT one.
# usage: mkdata.sh OUT.img
set -e
out=$1
tmp=$out.tmp
rm -f "$tmp"
truncate -s 96M "$tmp"
sfdisk -q "$tmp" <<PART
label: dos
2048,98304,c
100352,30720,6
131072,65536,7
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
mv "$tmp" "$out"
