#!/bin/bash
# glibc الحديثة (الموجودة على جهازك) ساكنة 32-bit: برنامج يختبر printf/malloc/fork/ملفات/أعداد عشرية.
# الناتج: prebuilt/glibc/hello-modern.elf
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
gcc -m32 -static -O1 -fno-pie -no-pie -fcf-protection=none "$HERE/hello.c" -o "$HERE/../../prebuilt/glibc/hello-modern.elf" \
    -Wl,-Ttext-segment=0x40000000 -Wl,-z,noexecstack
echo "تم. شغّله كـ init: ginux/tools/make-iso-with-init.sh gng/prebuilt/glibc/hello-modern.elf out.iso"
