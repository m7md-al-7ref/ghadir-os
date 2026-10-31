.section .rodata
.global cat_motd_elf_start
.global cat_motd_elf_end

cat_motd_elf_start:
.incbin "userland/cat_motd.elf"
cat_motd_elf_end:
