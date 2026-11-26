#!/bin/bash
# tools/ios26/run.sh SYSTEM.apfs NAME [seconds]: qemu/run-ios26.sh on a Mac with Homebrew's QEMU and coreutils; the console
# without its escape sequences, from the kernel's log on, in logs/ios26/NAME.txt (the whole of it in NAME.log)
cd "$(dirname "$0")/../.."
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PWD/../hostbin:$PATH
export QEMU_EFI=${QEMU_EFI:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd} QEMU_VARS=${QEMU_VARS:-/opt/homebrew/share/qemu/edk2-arm-vars.fd}
mkdir -p logs/ios26
qemu/run-ios26.sh "$1" "${3:-120}" > "logs/ios26/$2.log" 2>&1
LC_ALL=C sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' "logs/ios26/$2.log" | LC_ALL=C tr -d '\r' | sed -n '/KERNEL LOG/,$p' > "logs/ios26/$2.txt"
wc -l "logs/ios26/$2.txt"
