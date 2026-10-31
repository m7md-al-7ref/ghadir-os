#include "ginux_syscall.h"

void _start(void) {
    const char msg[] = "[ring3-ELF] hello from a REAL ELF32 executable loaded by the ginux ELF loader!\n";
    ginux_write(msg, sizeof(msg) - 1);

    for (;;) {
        __asm__ volatile ("nop");
    }
}
