#!/bin/bash
# Boot a Darwin 25 (userland 26) system under QEMU: the open kernel, the small image of tools/ios26/boot-image.sh as
# its RAM disk, and the system as a disk of the host (virtio-blk, /dev/rvblk0), which userland/src/init26.c makes the root.
# usage: run-ios26.sh SYSTEM.apfs [seconds] [extra qemu args...]
# env: CMDLINE=<extra boot-args>   K=<kernel, default hakios-kernel.open>   QEMU_SMP, QEMU_MEM (default 6G): qemu/run.sh
#      DISK_RW=1  the disk writable (virtio_blk=2)
# Needs the loader built with the identity of the IPSW's iPhone (xnuloader/mkidentity.sh), as tools/lower/dk-test.sh does.
set -e
cd "$(dirname "$0")/.."
IMG=$1; SECS=${2:-120}; shift; shift || true
[ -f "$IMG" ] || { sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
K=${K:-hakios-kernel.open}
[ -f userland/boot26.apfs ] || tools/ios26/boot-image.sh
RO=on BLK=1; [ -n "${DISK_RW:-}" ] && RO=off BLK=2
xnuloader/mkpayload.py -o qemu/payload.bin kernel=$K ramdisk=userland/boot26.apfs \
	--cmdline "cpus=${QEMU_SMP:-1} virtio_blk=$BLK ${CMDLINE:-}" > /dev/null
exec qemu/run.sh "$SECS" -global virtio-mmio.force-legacy=false \
	-drive "file=$IMG,format=raw,if=none,id=sys,readonly=$RO" -device virtio-blk-device,drive=sys "$@"
