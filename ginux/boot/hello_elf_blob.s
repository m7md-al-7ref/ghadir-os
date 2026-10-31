.section .rodata
.global hello_elf_start
.global hello_elf_end

hello_elf_start:
.incbin "userland/hello.elf"
hello_elf_end:
