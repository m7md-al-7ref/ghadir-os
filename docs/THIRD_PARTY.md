# Third-party components

The repository itself contains **no third-party source code**. The build scripts under `gng/` download
upstream sources and build them. The release disk images (`ghadir-os-*.iso`) contain the resulting
**binaries**, so the licenses below apply to those binaries. *This file is a good-faith summary, not legal advice;
if you redistribute the images, check the obligations of each license yourself.*

| Component | Version | Used for | License | Source (what the build script fetches) |
|---|---|---|---|---|
| GCC | 4.7.4 (C only) | C compiler inside Ghadir OS (`gcc`, `cc1`, `collect2`) | GPLv3 + GCC Runtime Library Exception | https://github.com/gcc-mirror/gcc  tag `releases/gcc-4.7.4` |
| GNU binutils | 2.45 | `as`, `ld`, `objdump`, `ar` | GPLv3 | http://archive.ubuntu.com/ubuntu/pool/main/b/binutils/binutils_2.45.orig.tar.xz |
| TinyCC | commit `e23dabed154a8495f3ac1ff3cc9d04fa1c207893` | small C compiler (`tcc`) | LGPLv2.1 | https://github.com/TinyCC/tinycc |
| dietlibc | 0.34~cvs20160606 | C library for programs compiled inside Ghadir OS | GPLv2 | http://archive.ubuntu.com/ubuntu/pool/universe/d/dietlibc/ |
| glibc | 2.39 (from the build host) | statically linked into `sh` and `cc` and the test programs | LGPLv2.1 | https://www.gnu.org/software/libc/ |
| libgcc | from the build host's GCC 13 | `libgcc.a` for linking | GPLv3 + GCC Runtime Library Exception | https://gcc.gnu.org/ |
| GRUB 2 | from the build host | boot loader embedded in the ISO (`grub-mkrescue`) | GPLv3 | https://www.gnu.org/software/grub/ |

Notes
- `sh` and `cc` (in `gng/shell/`) are original code (MIT) that is **statically linked** with glibc (LGPL). Their full source is in
  this repository, so anyone can relink them against a modified glibc.
- Used during development only, not shipped: QEMU, flex, bison, autoconf, binutils/GCC of the build host.
- `ginux/boot/prebuilt/gng-hello.elf` is a small prebuilt test program of the project's own C library (GNG); its source is not part of this repository yet.
