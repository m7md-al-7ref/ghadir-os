#!/bin/bash
# GNU binutils 2.45 (as, ld, objdump, ar) ساكنة 32-bit. الناتج: prebuilt/binutils/bin/
# المتطلبات: gcc-multilib، curl، make، xz. (ساعة تقريبًا على معالج واحد)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; P="$HERE/../prebuilt/binutils/bin"
W="${1:-/tmp/gng-build/binutils}"; mkdir -p "$W/tools" "$W/build"; cd "$W"
[ -f binutils.tar.xz ] || curl -sLo binutils.tar.xz http://archive.ubuntu.com/ubuntu/pool/main/b/binutils/binutils_2.45.orig.tar.xz
[ -d binutils-2.45 ] || tar xf binutils.tar.xz
# البناء يُعدّ "cross" شكليًا (host مختلف عن build) فيطلب أدوات ببادئة i686-linux-gnu-: نوفّرها بروابط
for t in ar ranlib nm strip objdump objcopy readelf as ld gcc; do ln -sf "$(which $t)" "$W/tools/i686-linux-gnu-$t"; done
export PATH="$W/tools:$PATH"
cd build
CC="gcc -m32 -fno-pie -fcf-protection=none -O1" ../binutils-2.45/configure \
  --host=i686-linux-gnu --build=x86_64-linux-gnu --target=i686-linux-gnu --prefix=/bu \
  --disable-nls --disable-gdb --disable-gdbserver --disable-sim --disable-werror --disable-gprofng \
  --disable-plugins --disable-shared --disable-multilib --disable-libctf --disable-gold --without-zstd \
  --without-debuginfod LDFLAGS="-static -no-pie" >/dev/null
make -j1 all-gas all-ld all-binutils >/dev/null 2>&1 || true
# libtool يتجاهل -static، فنعيد الربط بـ -all-static وبالعنوان الصحيح
rm -f gas/as-new ld/ld-new binutils/ar binutils/objdump
make -j1 all-gas all-ld all-binutils LDFLAGS="-all-static -no-pie -Wl,-Ttext-segment=0x40000000 -Wl,-z,noexecstack" >/dev/null
mkdir -p "$P"
strip --strip-all -o "$P/as" gas/as-new; strip --strip-all -o "$P/ld" ld/ld-new
strip --strip-all -o "$P/objdump" binutils/objdump; strip --strip-all -o "$P/ar" binutils/ar
echo "تم: $P"
