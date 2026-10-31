# Architecture (ginux 0.01)

A short map of how the kernel works. For the full history, every bug found and the reasoning behind decisions, read `HANDOFF.md` (Arabic).

## Boot
GRUB (Multiboot 1) loads `ginux.bin` and one module, the **initrd** (a USTAR archive). `kernel_main` then initialises, in order:
serial and VGA output, GDT/TSS, IDT and PIC, the physical memory manager (a bitmap of 4 KB frames, up to 512 MB), the initrd reservation,
paging (PAE + NX), the kernel heap, keyboard, RNG and stack-protector canary, the ledger, pipes and the process table; it creates the idle task (`kidle`)
and either the legacy demo programs or, if the initrd contains a file named `init`, that program; finally it starts the 100 Hz timer and the scheduler.

## Memory layout
| Range | Use |
|---|---|
| physical 0 – 512 MB | identity-mapped for the kernel (2 MB pages, NX; the first 4 MB use 4 KB pages so kernel sections get W^X) |
| `0x30000000` – | kernel heap (grows on demand) |
| `0x40000000` – `0x80000000` | user space; every process has its **own** page tables (separate PDPT entries), the kernel is shared and supervisor-only |
| `0x40000000` – | program image (ELF `PT_LOAD` segments must lie inside the user range) |
| `0x44000000` – `0x48000000` | heap (`brk`) |
| `0x50000000` + random | stack (256 KB for compat processes, ASLR) |
| `0x60000000` – `0x7F000000` | anonymous `mmap` |

## Processes and scheduling
Up to 32 tasks. Each has a kernel stack and a saved interrupt frame. **Every** interrupt frame is normalised to the same 16-dword layout (`boot/isr.s`,
macro `BUILD_FRAME`): a ring-0 interrupt pushes only 3 words, so the stub relocates the frame down to make room for `useresp/ss`. This removes any special
case from the scheduler.

The timer (100 Hz) calls `schedule()`, a round-robin over READY tasks. In *shell mode* (`init` present) idle gets the CPU only when nothing else can run, and
tasks that just had to wait are skipped until the next tick. There are no sleep queues: a blocking syscall rewinds `eip` by 2 bytes (so `int $0x80` re-executes)
and yields; this "yield and retry" is how `read`, `waitpid`, pipes and `nanosleep` block.

`fork` copies the whole address space (no copy-on-write). `execve` builds the new address space first and only then drops the old one, so a failed `execve` leaves the process intact.

## System calls: two ABIs
1. The kernel's **original ABI**: ~20 small calls (`write`, `fork`, `open`, `unlink`, `ledger_read`, ...), used by the built-in demo programs.
2. A **compatibility layer** (`ginux/src/compat_syscall.c`): the i386 syscall numbers and the `-errno` return convention, selected per process (`abi_compat`). It is not code from
   another kernel; it only follows the numbers and conventions that glibc and dietlibc were written against, which is why unmodified static programs run.
Both are reached through `int 0x80`. Every user pointer is validated against the process's page tables before the kernel touches it.

## Program loading (`src/elf.c`)
Static ELF32 only. Rejected: segments outside the user range, any segment that is both writable and executable (W^X), an entry point outside an executable segment, malformed headers.
The initial stack contains `argc/argv/envp` and an `auxv` (`AT_PHDR`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM`, `AT_HWCAP`, ...).
Thread-local storage is supported through one GDT entry (selector `0x33`) whose base is switched per process; x87/SSE state is saved with `fxsave` on context switches.

## File system (`src/initrd.c`)
Two layers, on purpose: a **facts** layer (file contents, reference counted by open files and by names) and a **conventions** layer (names, directories, hard links pointing at facts).
Deleting a name never invalidates an open file. The boot image is read-only until first write (copy-on-write to heap). `unlink` goes through the **trust window**
(`src/trust_window.c`): the real deletion happens ~3 s later and can be cancelled.

## Console and input (`src/tty.c`, `src/vga.c`, `src/keyboard.c`)
A canonical tty (line editing: Backspace, Ctrl-U, Ctrl-D, Ctrl-C) fed from the PS/2 keyboard and the serial port by the timer interrupt, so typing is never lost.
Output goes to VGA text mode and to serial; kernel logs go to serial only once the shell owns the screen. Ctrl-C kills the foreground job and all of its descendants.

## Other kernel mechanisms
- **Ledger** (`src/ledger.c`): append-only array of process life-cycle events; there is deliberately no function that edits or deletes an entry.
- **Monopoly detection** (`src/spinlock.c`): a lock held far longer than a threshold (measured in TSC cycles) is reported.
- **Fault isolation**: a ring-3 exception kills only that process (the parent sees it as a signal-style status).
