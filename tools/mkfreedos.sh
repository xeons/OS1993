#!/bin/sh
# Build build/freedos.img: a bootable FreeDOS 1.3 hard disk with OS1993 on it.
#
#   tools/mkfreedos.sh [normal|emm386] [autotest|autorun]
#
#   normal    HIMEMX only (what OS1993 needs)
#   emm386    HIMEMX + JEMM386: the CPU is in V86 mode, OS1993 must refuse
#   autotest  run "OS1993 /T" at boot and power off afterwards
#   autorun   run "OS1993" at boot (driven by a script) -- or C:\OS1993\RUN.BAT
#             instead if a test put one there (OS1993 /D, say) -- then
#             C:\OS1993\AFTER.BAT if there is one, and power off
#
# The FreeDOS 1.3 Lite USB image is downloaded once into .cache/freedos and
# checked against the published SHA-256.
set -eu

variant=${1:-normal}
autotest=${2:-}
root=$(cd "$(dirname "$0")/.." && pwd)
cache=$root/.cache/freedos
build=$root/build
img=$build/freedos.img
part=32256                     # partition 1 starts at LBA 63

url=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-LiteUSB.zip
sha=64a934585087ccd91a18c55e20ee01f5f6762be712eeaa5f456be543778f9f7e

if [ ! -f "$cache/FD13LITE.img" ]; then
    mkdir -p "$cache"
    echo "Downloading FreeDOS 1.3 Lite..."
    curl -sSL -o "$cache/FD13-LiteUSB.zip" "$url"
    echo "$sha  $cache/FD13-LiteUSB.zip" | sha256sum -c --quiet
    python3 -I -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extract('FD13LITE.img', sys.argv[2])" \
        "$cache/FD13-LiteUSB.zip" "$cache"
fi

mkdir -p "$build"
cp "$cache/FD13LITE.img" "$img"
I="$img@@$part"
export MTOOLS_SKIP_CHECK=1

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

memmgr=
[ "$variant" = emm386 ] && memmgr='DEVICE=\FREEDOS\BIN\JEMM386.EXE NOEMS'

cat > "$tmp/FDCONFIG.SYS" <<EOF
!LASTDRIVE=Z
!BUFFERS=20
!FILES=40
DOS=HIGH
DEVICE=\\FREEDOS\\BIN\\HIMEMX.EXE
$memmgr
SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:1024 /P=\\FDAUTO.BAT
EOF

cat > "$tmp/FDAUTO.BAT" <<'EOF'
@echo off
SET DOSDIR=\FREEDOS
SET PATH=\FREEDOS\BIN
SET COMSPEC=C:\FREEDOS\BIN\COMMAND.COM
cd \OS1993
if exist AUTOTEST goto test
if exist AUTORUN goto run
echo.
echo OS1993 is in C:\OS1993. Type OS1993 to start it.
goto end
:test
OS1993 /T > LOADER.LOG
QEXIT
:run
if exist RUN.BAT call RUN.BAT
if not exist RUN.BAT OS1993
if exist AFTER.BAT call AFTER.BAT
QEXIT
:end
EOF

[ "$autotest" = autotest ] && : > "$tmp/AUTOTEST"
[ "$autotest" = autorun ] && : > "$tmp/AUTORUN"
# QEXIT.COM: out 0F4h, al -- QEMU's isa-debug-exit device ends the emulator
printf '\272\364\000\260\000\356\315\040' > "$tmp/QEXIT.COM"   # mov dx,0F4h / mov al,0 / out dx,al / int 20h

# DOS line endings
for f in FDCONFIG.SYS FDAUTO.BAT; do
    sed -i 's/$/\r/' "$tmp/$f"
done

mdel -i "$I" ::/SETUP.BAT ::/FDCONFIG.SYS ::/FDAUTO.BAT
mcopy -i "$I" "$tmp/FDCONFIG.SYS" "$tmp/FDAUTO.BAT" ::/
mmd   -i "$I" ::/OS1993
mcopy -i "$I" "$build/OS1993.COM" "$build"/*.DLL "$build"/*.BIN \
      "$tmp/QEXIT.COM" ::/OS1993/
[ -f "$tmp/AUTOTEST" ] && mcopy -i "$I" "$tmp/AUTOTEST" ::/OS1993/
[ -f "$tmp/AUTORUN" ] && mcopy -i "$I" "$tmp/AUTORUN" ::/OS1993/
echo "Built $img ($variant${autotest:+, $autotest})"
