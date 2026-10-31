.section .rodata
.global fd_identity_test_elf_start
.global fd_identity_test_elf_end

fd_identity_test_elf_start:
.incbin "userland/fd_identity_test.elf"
fd_identity_test_elf_end:
