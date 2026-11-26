#!/bin/bash
# tools/ios26/boot-image.sh: the small image a Darwin 25 system starts from under QEMU: userland/src/init26.c as its
# /sbin/launchd, and the directories it mounts on.  Written to userland/boot26.apfs (the payload's RAM disk, md0).
set -e
cd "$(dirname "$0")/../../userland"
export PATH="$PWD/../xnu-deps/host/bin:$PATH"
CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
mkdir -p obj
$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
$CC -c libmini/start.S -o obj/start.o
$CC -Ilibmini $LD -o obj/init26 src/init26.c obj/mini.o obj/start.o
rm -rf boot26 && mkdir -p boot26/sbin boot26/dev boot26/flash
cp obj/init26 boot26/sbin/launchd
python3 ../tools/apfs/mkapfs.py -o boot26.apfs Boot=boot26 > /dev/null
ls -l boot26.apfs
