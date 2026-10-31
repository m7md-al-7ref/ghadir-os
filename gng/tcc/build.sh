#!/bin/bash
# TinyCC (مترجم C صغير) ساكن 32-bit يعمل داخل ginux. الناتج: prebuilt/tcc/{bin/tcc,include/,lib/libtcc1.a}
# المتطلبات: gcc-multilib، curl، make.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; P="$HERE/../prebuilt/tcc"
W="${1:-/tmp/gng-build/tcc}"; mkdir -p "$W"; cd "$W"
# نثبّت commit محددًا (فرع mob يتحرك ولا يضمن بناءً قابلًا للتكرار). آخر ما جرّبته: 2026-10-21
SHA=e23dabed154a8495f3ac1ff3cc9d04fa1c207893
[ -f tcc-$SHA.tgz ] || curl -sLo tcc-$SHA.tgz https://codeload.github.com/TinyCC/tinycc/tar.gz/$SHA
[ -d tinycc-$SHA ] || tar xzf tcc-$SHA.tgz
cd tinycc-$SHA
# الربط عند 0x40000000 مطلوب لأن محمّل ginux يرفض أي PT_LOAD خارج [0x40000000, 0x80000000)
./configure --cc="gcc -m32 -fno-pie -fcf-protection=none" \
  --extra-ldflags="-static -no-pie -Wl,-Ttext-segment=0x40000000 -Wl,-z,noexecstack" \
  --cpu=i386 --enable-static --prefix=/tcc --sysincludepaths=/dietinc:/tccinc --libpaths=/lib --crtprefix=/lib \
  --elfinterp=/lib/ld.so --triplet= >/dev/null
make tcc >/dev/null
# libtcc1.a: يفشل البناء الرسمي لأن مسار headers غير موجود وقت البناء، فنبنيها يدويًا بمترجم الجهاز
( cd lib && make alloca.o atomic.o builtin.o stdatomic.o >/dev/null 2>&1 || true
  gcc -m32 -O1 -fno-pie -fno-stack-protector -fcf-protection=none -fno-builtin -I.. -c libtcc1.c -o libtcc1.o
  rm -f ../libtcc1.a; ar rcs ../libtcc1.a libtcc1.o alloca.o atomic.o builtin.o stdatomic.o )
mkdir -p "$P/bin" "$P/include" "$P/lib"
strip --strip-all -o "$P/bin/tcc" tcc
cp include/*.h "$P/include/"; cp libtcc1.a "$P/lib/"
echo "تم: $P"
