#!/bin/sh
# Build a data disk for the VMM's own IDE driver and file systems:
#   partition 1: FAT32, about 48 MB      partition 2: FAT16, about 15 MB
# with a few files (long names among them) written by mtools.
# usage: mkdata.sh OUT.img
set -e
out=$1
tmp=$out.tmp
rm -f "$tmp"
truncate -s 64M "$tmp"
sfdisk -q "$tmp" <<PART
label: dos
2048,98304,c
100352,30720,6
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
mv "$tmp" "$out"
