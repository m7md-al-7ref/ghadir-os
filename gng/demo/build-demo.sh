#!/bin/bash
# build-demo.sh [out.iso]
# يجمّع أدوات gng الجاهزة (gng/prebuilt) في initrd ويبني ISO واحدًا فيه النواة ginux ومشغّل drv_all.
# المتطلبات (على Linux): gcc-multilib، make، grub-pc-bin، xorriso، mtools.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
GNG="$(cd "$HERE/.." && pwd)"
GINUX="$(cd "$GNG/../ginux" && pwd)"
OUT="${1:-$GNG/../ghadir-os-demo.iso}"
P="$GNG/prebuilt"

make -C "$GINUX" >/dev/null                      # النواة: ginux.bin
W="$(mktemp -d)"; S="$W/sys"
mkdir -p "$S"/{bin,gcclib,gccinc,tccinc,dietinc,lib,src,out,tmp}
cp "$P"/gcc/bin/gcc "$P"/binutils/bin/{as,ld,objdump,ar} "$P"/tcc/bin/tcc "$S/bin/"
cp "$P"/gcc/lib/{cc1,collect2} "$S/gcclib/"
cp "$P"/gcc/include/*.h "$S/gccinc/"
cp "$P"/tcc/include/*.h "$S/tccinc/"
cp -r "$P"/dietlibc/include/. "$S/dietinc/"
cp "$P"/dietlibc/lib/{start.o,dietlibc.a} "$P"/libgcc/libgcc.a "$P"/tcc/lib/libtcc1.a "$S/lib/"
cp "$HERE"/hello.s "$HERE"/hello_gcc.c "$HERE"/hello_tcc.c "$S/src/"
# المشغّل: glibc ساكنة، 32-bit، مربوطة عند 0x40000000 (حدود محمّل النواة)، وsegments منفصلة (W^X)
gcc -m32 -static -O1 -fno-pie -no-pie -fcf-protection=none "$HERE/drv_all.c" -o "$W/drv_all.elf" \
    -Wl,-Ttext-segment=0x40000000 -Wl,-z,noexecstack
"$GINUX/tools/make-iso-with-init.sh" "$W/drv_all.elf" "$OUT" "$S"/bin "$S"/gcclib "$S"/gccinc "$S"/tccinc "$S"/dietinc "$S"/lib "$S"/src "$S"/out "$S"/tmp
rm -rf "$W"
echo "تم: $OUT"
echo "للتشغيل:  qemu-system-i386 -cpu max -m 512 -cdrom \"$OUT\" -serial stdio"
