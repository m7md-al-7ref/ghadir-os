#!/bin/bash
# make-iso-with-init.sh <init.elf> <out.iso> [ملفات إضافية...]
# يبني ISO بطبقة التوافق: initrd فيه الملف "init" (يُشغَّل وحده) + motd.txt + أي ملفات إضافية.
set -e
INIT=${1:?usage: $0 init.elf out.iso [extra files]}; OUT=${2:?}; shift 2
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W=$(mktemp -d); mkdir -p "$W/rd" "$W/iso/boot/grub"
cp "$INIT" "$W/rd/init"; cp "$ROOT/rootfs/motd.txt" "$W/rd/"
for f in "$@"; do cp -r "$f" "$W/rd/"; done
tar --format=ustar -cf "$W/iso/boot/initrd.tar" -C "$W/rd" .
cp "$ROOT/ginux.bin" "$W/iso/boot/"; cp "$ROOT/grub/grub.cfg" "$W/iso/boot/grub/"
grub-mkrescue -o "$OUT" "$W/iso" 2>/dev/null
rm -rf "$W"
