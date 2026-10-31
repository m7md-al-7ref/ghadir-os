#!/bin/bash
# GCC 4.7.4 (لغة C فقط) ساكن 32-bit. الناتج: prebuilt/gcc/{bin/gcc,lib/cc1,lib/collect2,include/}
# لماذا 4.7.4؟ مكتوب بلغة C (لا C++) فيبنى أسرع بكثير، ولا يزال قابلًا للبناء بمترجم حديث.
# المتطلبات: gcc-multilib، flex، bison، texinfo، libgmp-dev:i386 libmpfr-dev:i386 libmpc-dev:i386
#   (sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install ...)
# ساعات على معالج واحد. هذا السكربت مُجمَّع من الأوامر التي نفّذتُها يدويًا ولم أعد تشغيله كاملًا.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; P="$HERE/../prebuilt/gcc"
W="${1:-/tmp/gng-build/gcc}"; mkdir -p "$W/tools" "$W/build"; cd "$W"
[ -f gcc.tgz ] || curl -sLo gcc.tgz https://codeload.github.com/gcc-mirror/gcc/tar.gz/refs/tags/releases/gcc-4.7.4
[ -d gcc-releases-gcc-4.7.4 ] || tar xzf gcc.tgz
for t in ar ranlib nm strip objdump objcopy readelf as ld gcc; do ln -sf "$(which $t)" "$W/tools/i686-linux-gnu-$t"; done
export PATH="$W/tools:$PATH"
OLD="-std=gnu89 -fgnu89-inline -w"           # GCC 4.7 يحتاج لهجة C القديمة وتجاهل التحذيرات مع مترجم حديث
cd build
CC="gcc -m32 -fno-pie -fcf-protection=none $OLD -O1" CFLAGS="-O1 -fno-pie -fcf-protection=none $OLD" \
../gcc-releases-gcc-4.7.4/configure --host=i686-linux-gnu --build=x86_64-linux-gnu --target=i686-linux-gnu --prefix=/gcc \
  --enable-languages=c --disable-bootstrap --disable-multilib --disable-nls --disable-shared --disable-libssp \
  --disable-libgomp --disable-libquadmath --disable-libmudflap --disable-libitm --disable-libatomic --disable-lto \
  --disable-plugin --without-ppl --without-cloog \
  --with-gmp-include=/usr/include/i386-linux-gnu --with-gmp-lib=/usr/lib/i386-linux-gnu \
  --with-mpfr-include=/usr/include/i386-linux-gnu --with-mpfr-lib=/usr/lib/i386-linux-gnu \
  --with-mpc-include=/usr/include/i386-linux-gnu --with-mpc-lib=/usr/lib/i386-linux-gnu >/dev/null
LD_ARGS="-static -no-pie -Wl,-Ttext-segment=0x40000000 -Wl,-z,noexecstack"
make -j1 all-gcc FLEX=flex BISON=bison LDFLAGS="$LD_ARGS" >/dev/null 2>&1 || true   # الجولة الأولى قد تتعثر بـ gengtype-lex الفارغ
rm -f gcc/gengtype-lex.c gcc/build/gengtype-lex.o                                   # ملف فارغ ولّده make قبل أن يوجد flex
make -j1 all-gcc FLEX=flex BISON=bison LDFLAGS="$LD_ARGS" >/dev/null
# الـ driver: بنيناه بـ -O0 (النسخة المرفقة كذلك)
( cd gcc && rm -f gcc.o xgcc && make xgcc FLEX=flex BISON=bison LDFLAGS="$LD_ARGS" \
    CFLAGS="-O0 -fno-pie -fcf-protection=none $OLD -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks" >/dev/null )
# مهم: الـ Makefile يولّد gcc/specs بمترجم المضيف (gcc حديث) فيكسر الـ driver. نحذفه (الـ driver يملك specs مدمجة)
rm -f gcc/specs
mkdir -p "$P/bin" "$P/lib" "$P/include"
strip --strip-all -o "$P/bin/gcc" gcc/xgcc
strip --strip-all -o "$P/lib/cc1" gcc/cc1; strip --strip-all -o "$P/lib/collect2" gcc/collect2
cp gcc/include/*.h "$P/include/"
echo "تم: $P"
