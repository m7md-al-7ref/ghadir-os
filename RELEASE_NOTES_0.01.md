# Ghadir OS 0.01

First public release of Ghadir OS: a from-scratch operating system (**ginux** kernel + **gng** tools) that runs GCC on its own kernel.

## Download and run
Get `ghadir-os-0.01.iso` (attached below) and boot it:

    qemu-system-i386 -cpu max -m 512 -cdrom ghadir-os-0.01.iso

Click inside the window to capture the keyboard, then type `help`. Try `cc hello.c -o hello && ./hello`.

SHA-256 of the image: see `SHA256SUMS` (attached).

## Highlights
- Interactive shell with pipes, redirection, globbing, scripts, `undo`.
- GCC 4.7.4, GNU binutils 2.45 and TinyCC run inside the system; programs are compiled, linked and executed there.
- 83 kernel tests, 35 shell tests and 11 window/keyboard tests, all run on QEMU.

## Known limitations
No persistent storage (files are in RAM), no arrow keys / history, no real signals, no networking, VGA console is English only, tested on QEMU only.
Full list in the README.

## Third-party software
The image contains GCC, binutils, TinyCC, dietlibc and glibc-linked programs under their own licenses; versions and sources are listed in `docs/THIRD_PARTY.md`.
