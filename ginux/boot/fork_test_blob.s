.section .rodata
.global fork_test_elf_start
.global fork_test_elf_end

fork_test_elf_start:
.incbin "userland/fork_test.elf"
fork_test_elf_end:
