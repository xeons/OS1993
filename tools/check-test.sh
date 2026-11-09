#!/bin/sh
# Evaluate a `make test` run: the kernel must have logged a clean shutdown on
# COM1, and the loader must have made it back to DOS with exit code 00.
build=${1:-build}
fail=0

expect() {  # file pattern description
    if [ -f "$1" ] && tr -d '\r' < "$1" | grep -q "$2"; then
        echo "  PASS  $3"
    else
        echo "  FAIL  $3"
        fail=1
    fi
}

echo "OS1993 test results:"
expect "$build/SERIAL.LOG" "kernel entered"                 "kernel reached kmain"
expect "$build/SERIAL.LOG" "memory self-test passed"        "memory manager self-test"
expect "$build/SERIAL.LOG" "V86 self-test passed"           "DOS calls from protected mode"
expect "$build/SERIAL.LOG" "ring-3 self-test passed"        "ring 3: syscalls; CLI, kernel and code writes refused"
expect "$build/SERIAL.LOG" "multi-VM self-test passed"      "concurrent VMs, private memory, focus"
expect "$build/SERIAL.LOG" "DOS shell test passed"          "COMMAND.COM runs inside the VM"
expect "$build/SERIAL.LOG" "DOS concurrency test passed"    "two VMs share DOS concurrently"
expect "$build/SERIAL.LOG" "returning to DOS (exit code 0)" "kernel shut down cleanly"
expect "$build/LOADER.LOG" "Exit code 00"                   "loader returned to DOS"

if [ $fail -ne 0 ]; then
    echo
    echo "--- SERIAL.LOG ---"; tr -d '\r' < "$build/SERIAL.LOG" 2>/dev/null
    echo "--- LOADER.LOG ---"; tr -d '\r' < "$build/LOADER.LOG" 2>/dev/null
fi
exit $fail
