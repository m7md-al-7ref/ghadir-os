#!/bin/bash
# build-glibc22.sh -- يبني glibc 2.2 ساكنة + برنامج hello لتشغيله على Ginux.
#
# *** غير مجرَّب في بيئة التطوير (لا يوجد gcc 2.95/3.x هناك) ***
# التحقق من النواة نفسها تم باستعمال dietlibc (انظر HANDOFF.md في جذر المشروع، §8 و§17).
# شغّله داخل بيئة بمترجم من تلك الحقبة، مثلًا:
#     docker run --rm -it -v "$PWD":/w debian/eol:woody bash    (gcc 2.95/3.0، binutils 2.12)
# المصدر: https://github.com/bminor/glibc  tag glibc-2.2  (يحتاج توليد configure fragments
# بـ autoconf 2.13 إن لم تكن موجودة، وحذف placeholders في bits/ العليا -- انظر الملاحظات).
set -e
SRC=${1:?usage: $0 /path/to/glibc-2.2-src}
OUT=${2:-$PWD/out}
mkdir -p "$OUT/build" && cd "$OUT/build"
# i486 وليس i686: QEMU qemu32/pentium3 لا يدعمان cmov. لا SSE: النواة لا تحفظ حالة SSE.
CC="gcc -march=i486" CFLAGS="-O2 -g" \
"$SRC/configure" --host=i486-pc-linux-gnu --prefix="$OUT/root" \
    --disable-shared --disable-sanity-checks --with-headers=/usr/include
make -j4
make install
cat > "$OUT/hello.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc, char **argv) {
    printf("hello from glibc 2.2 on Ginux! argc=%d\n", argc);
    char *p = malloc(200000); sprintf(p, "malloc ok");
    puts(p);
    pid_t c = fork();
    if (c == 0) { puts("child"); _exit(5); }
    int st; waitpid(c, &st, 0);
    printf("child exit=%d\n", WEXITSTATUS(st));
    return 0;
}
C
# -Ttext-segment (binutils >= 2.20) أو -Ttext (القديمة): النواة تقبل فقط 0x40000000..0x80000000
# (حدود elf.c). الـ ELF لازم يكون segments منفصلة R-X و RW- (سياسة W^X).
gcc -march=i486 -static -nostdlib -I"$OUT/root/include" -O1 -c "$OUT/hello.c" -o "$OUT/hello.o"
ld -static -Ttext 0x40001000 -o "$OUT/hello" \
   "$OUT/root/lib/crt1.o" "$OUT/root/lib/crti.o" "$OUT/hello.o" \
   "$OUT/root/lib/libc.a" "$(gcc -print-libgcc-file-name)" "$OUT/root/lib/libc.a" "$OUT/root/lib/crtn.o"
echo "الناتج: $OUT/hello  -> لتجربته على النواة: ginux/tools/make-iso-with-init.sh $OUT/hello out.iso"
