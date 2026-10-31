.section .rodata
.global separation_test_elf_start
.global separation_test_elf_end

separation_test_elf_start:
.incbin "userland/separation_test.elf"
separation_test_elf_end:
