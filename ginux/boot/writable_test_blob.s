.section .rodata
.global writable_test_elf_start
.global writable_test_elf_end

writable_test_elf_start:
.incbin "userland/writable_test.elf"
writable_test_elf_end:
