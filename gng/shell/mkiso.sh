#!/bin/bash
# mkiso.sh [out.iso] -- يبني ISO تفاعليًا: النواة ginux + shell كـ init + أدوات gng الموجودة في prebuilt/.
# يعمل بنسخة "خفيفة" (tcc فقط) أو كاملة (gcc + binutils + tcc). المتطلبات (Linux): gcc-multilib، make، grub-pc-bin، xorriso، mtools.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; GNG="$(cd "$HERE/.." && pwd)"; GINUX="$(cd "$GNG/../ginux" && pwd)"
OUT="${1:-$GNG/../ghadir-os-shell.iso}"; P="$GNG/prebuilt"
"$HERE/build.sh" >/dev/null     # دائمًا: يستغرق ثوانٍ، ويمنع بقاء ثنائيات قديمة بعد تعديل المصدر
for f in "$P/dietlibc/lib/dietlibc.a" "$P/dietlibc/lib/start.o" "$P/libgcc/libgcc.a"; do
  [ -f "$f" ] || { echo "مفقود: $f -- شغّل:  make toolchain-lite  (أو make toolchain للنسخة الكاملة)"; exit 1; }
done
HAVE_GCC=0; HAVE_TCC=0; HAVE_BU=0
[ -x "$P/gcc/bin/gcc" ] && [ -f "$P/gcc/lib/cc1" ] && HAVE_GCC=1
[ -x "$P/tcc/bin/tcc" ] && HAVE_TCC=1
[ -x "$P/binutils/bin/as" ] && HAVE_BU=1
if [ $HAVE_GCC = 0 ] && [ $HAVE_TCC = 0 ]; then echo "لا يوجد مترجم: شغّل make toolchain-lite أو make toolchain"; exit 1; fi
echo "المكوّنات: gcc=$HAVE_GCC tcc=$HAVE_TCC binutils=$HAVE_BU"
make -C "$GINUX" >/dev/null
W="$(mktemp -d)"; S="$W/sys"
mkdir -p "$S"/{bin,gcclib,gccinc,tccinc,dietinc,lib,tmp,etc,home,src}
cp "$P"/shell/bin/{sh,cc} "$S/bin/"
cp -r "$P"/dietlibc/include/. "$S/dietinc/"
cp "$P"/dietlibc/lib/{start.o,dietlibc.a} "$P"/libgcc/libgcc.a "$S/lib/"
if [ $HAVE_GCC = 1 ]; then cp "$P"/gcc/bin/gcc "$S/bin/"; cp "$P"/gcc/lib/{cc1,collect2} "$S/gcclib/"; cp "$P"/gcc/include/*.h "$S/gccinc/"; fi
if [ $HAVE_BU = 1 ]; then cp "$P"/binutils/bin/{as,ld,objdump,ar} "$S/bin/"; fi
if [ $HAVE_TCC = 1 ]; then cp "$P"/tcc/bin/tcc "$S/bin/"; cp "$P"/tcc/include/*.h "$S/tccinc/"; cp "$P"/tcc/lib/libtcc1.a "$S/lib/"; fi
cp "$HERE"/etc/profile "$HERE"/etc/NOTICE "$S/etc/"; cp "$HERE"/home/hello.c "$S/home/"
cp "$GNG"/demo/hello_gcc.c "$GNG"/demo/hello.s "$S/src/" 2>/dev/null || true
# big.c: اختبار ضغط للمترجم (3000 دالة)، يُولَّد وقت البناء ولا يُخزَّن في المصدر. جرّب: cc big.c -o big
awk 'BEGIN{for(i=0;i<3000;i++) printf "int f%d(int x){int k;int s=0;for(k=0;k<x;k++)s+=k*%d+(s>>1);return s+%d;}\n",i,i%97+1,i; print "int main(void){return f1(3)&1;}"}' > "$S/home/big.c"
"$GINUX/tools/make-iso-with-init.sh" "$P/shell/bin/sh" "$OUT" "$S"/bin "$S"/gcclib "$S"/gccinc "$S"/tccinc "$S"/dietinc "$S"/lib "$S"/tmp "$S"/etc "$S"/home "$S"/src
rm -rf "$W"
echo "تم: $OUT"
echo "للتشغيل:  qemu-system-i386 -cpu max -m 512 -cdrom \"$OUT\""
