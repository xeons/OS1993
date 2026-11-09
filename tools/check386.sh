#!/bin/sh
# Fail if the kernel contains instructions a 386 (without a 387) can't run.
# The loader needs no check: nasm's `cpu 386` directive enforces it.
objdump=$1
elf=$2

bad='^(cmov|bswap|cmpxchg|xadd|invlpg|invd|wbinvd|cpuid|rdtsc|rdmsr|wrmsr|rdpmc|sysenter|sysexit|syscall|ud2|endbr|movbe|popcnt|lzcnt|tzcnt|f[a-z]|(v?p?(add|sub|mul|mov|xor|and|or)[a-z]*(ps|pd|ss|sd)$)|emms|movq|movd)'

# Ring-3 modules start .text with the 48-byte MOD3 header: data, not code.
start=
text=$("$objdump" -h "$elf" | awk '$2 == ".text" { print $4; exit }')
if [ -n "$text" ] && "$objdump" -s -j .text --stop-address=$((0x$text + 4)) "$elf" | grep -q 'MOD3'; then
    start="--start-address=$((0x$text + 48))"
fi

hits=$("$objdump" -d --no-show-raw-insn $start "$elf" |
       awk -F'\t' 'NF >= 2 && $1 ~ /:$/ { split($2, w, " "); print w[1] "\t" $0 }' |
       grep -E "$bad")

if [ -n "$hits" ]; then
    echo "check386: post-386 / FPU instructions found:"
    echo "$hits" | cut -f2- | head -20
    exit 1
fi
echo "check386: OK -- $(basename "$elf") is clean 386 code"
