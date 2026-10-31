# Contributing

Thanks for looking at Ghadir OS. It is a small from-scratch OS (a hobby / research project), so contributions of every size are welcome:
bug reports, tests, docs, small fixes, ports of tools.

## Ground rules
- **Test on QEMU before sending anything**: `make test` must pass. For shell changes also run `gng/shell/test-shell.py <iso>`.
- Add a test for a bug you fix (see `ginux/userland_compat/compattest.c` for kernel/syscall tests).
- Keep the kernel small and readable. Prefer a clear explanation in a comment over a clever trick; many comments are in Arabic, English is welcome too.
- Do not commit binaries or build outputs (`gng/prebuilt/` is ignored on purpose).
- Honesty matters here: if something is untested or known to be broken, say so in the code or in `docs/HANDOFF.md`.

## Getting started
    make kernel           # build the kernel
    make test             # kernel test suites on QEMU
    make toolchain-lite   # dietlibc + tinycc + shell (~10 min)
    make run-shell        # build the image and boot it

See `docs/HANDOFF.md` (Arabic) for the full development notes: history, every bug found and how it was fixed, known limits.
