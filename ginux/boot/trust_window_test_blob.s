.section .rodata
.global trust_window_test_elf_start
.global trust_window_test_elf_end

trust_window_test_elf_start:
.incbin "userland/trust_window_test.elf"
trust_window_test_elf_end:
