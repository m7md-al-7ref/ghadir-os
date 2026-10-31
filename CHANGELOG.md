# Changelog

## [0.01] - 2026-10-21 : first public release

Ghadir OS is the ginux kernel plus the gng tools. This is the first release that can compile and run C programs on its own kernel.

### Kernel (ginux)
- Boots with GRUB on x86 (PAE + NX), up to 512 MB RAM; preemptive scheduler; isolated address spaces with W^X and ASLR; user faults do not bring the kernel down.
- `fork`, `execve`, `waitpid`, pipes, TLS (`set_thread_area`), x87/SSE state saving.
- Writable in-memory file system with directories; tty with PS/2 keyboard and serial; VGA text console with cursor and screen clearing.
- Syscall compatibility layer (i386 numbers, `-errno`) with 100+ calls: files, directories, `mmap`, `statx`, `clone` (fork-style), `clock_gettime`, `getrandom`, ...
- Append-only ledger, monopoly detection for locks, trust window for `unlink`, facts/conventions file-system layers.

### Tools (gng)
- Interactive shell: pipes, redirection, `; && ||`, quoting, variables, globbing, scripts, builtins, `undo`, Ctrl-C / Ctrl-D.
- Build scripts for GCC 4.7.4, GNU binutils 2.45, TinyCC (pinned commit) and dietlibc; `cc` wrapper.
- GCC, binutils and TinyCC run inside the system and compile, link and run programs.

### Fixed during this release cycle (kernel bugs)
- Interrupt frames from ring 0 overwrote two live words of the interrupted stack (the long-standing intermittent crash with `eip=...FF0`).
- 2 MB identity-map entries used a 4 MB mask: every odd 2 MB region aliased the previous one, so the kernel wrote to the wrong physical memory above 6 MB.
- The identity map was rounded down, so running out of memory could crash the kernel instead of returning `ENOMEM`.
- All directories reported the same inode (GCC's preprocessor then dropped include directories).
- `es/fs/gs` were not restored when returning to ring 3.

### Known limitations
See the *Limitations* section of the README.
