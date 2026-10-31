#!/bin/bash
# يبني الـ shell (sh) وغلاف المترجم (cc) ساكنَين 32-bit، ويضعهما في prebuilt/shell/bin
# المتطلبات: gcc-multilib
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$HERE/../prebuilt/shell/bin"; mkdir -p "$OUT"
VER="$(cat "$HERE/../../VERSION" 2>/dev/null || echo dev)"
for prog in sh cc; do
  gcc -m32 -static -DGHADIR_VERSION="\"$VER\"" -O1 -fno-pie -no-pie -fcf-protection=none -Wall -Wextra "$HERE/$prog.c" -o "$OUT/$prog" \
      -Wl,-Ttext-segment=0x40000000 -Wl,-z,noexecstack
  strip --strip-all "$OUT/$prog"
done
echo "تم: $OUT"
