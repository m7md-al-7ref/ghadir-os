#!/bin/bash
# مكتبة C صغيرة (dietlibc 0.34) تُستعمل لربط البرامج التي يترجمها gcc وtcc داخل ginux.
# الناتج: prebuilt/dietlibc/{lib/start.o,lib/dietlibc.a,include/} + prebuilt/libgcc/libgcc.a
# المتطلبات: gcc-multilib (يوفّر libgcc 32-bit)، curl، make.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; P="$HERE/../prebuilt"
W="${1:-/tmp/gng-build/dietlibc}"; mkdir -p "$W"; cd "$W"
V=dietlibc-0.34~cvs20160606
[ -f $V.tar.xz ] || curl -sLo $V.tar.xz "http://archive.ubuntu.com/ubuntu/pool/universe/d/dietlibc/dietlibc_0.34~cvs20160606.orig.tar.xz"
[ -d $V ] || tar xf $V.tar.xz
# TLS و thread-safe معطّلان: مكتبات تلك الحقبة لا تحتاجهما، وهذا الإصدار يتجنب gs:
sed -i 's|^#define WANT_THREAD_SAFE|// #define WANT_THREAD_SAFE|; s|^#define WANT_TLS|// #define WANT_TLS|' $V/dietfeatures.h
( cd $V && rm -rf bin-i386 && make -j1 ARCH=i386 CC="gcc -m32 -fno-stack-protector -fcf-protection=none -fno-pie" i386 >/dev/null 2>&1 || true )
mkdir -p "$P/dietlibc/lib" "$P/dietlibc/include" "$P/libgcc"
cp $V/bin-i386/start.o $V/bin-i386/dietlibc.a "$P/dietlibc/lib/"
cp -r $V/include/. "$P/dietlibc/include/"
cp "$(gcc -m32 -print-libgcc-file-name)" "$P/libgcc/libgcc.a"
echo "تم: $P/dietlibc و $P/libgcc"
