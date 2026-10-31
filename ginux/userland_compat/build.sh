#!/bin/sh
# يبني برنامج اختبار طبقة التوافق (بدون أي libc) ويضعه كـ init في rootfs_compat/
set -e
cd "$(dirname "$0")/.."
gcc -m32 -march=i486 -mno-sse -mno-mmx -O1 -fomit-frame-pointer -ffreestanding -fno-pie \
    -fno-stack-protector -fno-builtin -Wall -Wextra -c userland_compat/compattest.c -o userland_compat/compattest.o
ld -m elf_i386 -z noexecstack -T userland_compat/compat.ld -nostdlib -o rootfs_compat/init userland_compat/compattest.o
