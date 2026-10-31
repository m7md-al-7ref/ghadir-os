/* مثل ما ينتجه ld عادةً لبرنامج ساكن: segment للكود (R-X) يشمل ELF header
   + program headers (حتى يعمل AT_PHDR)، وsegment للبيانات (RW-) في صفحات منفصلة
   -- يحترم سياسة W^X في elf.c. العنوان الأساسي 0x40000000 (نفس ما سنمرره
   لـ glibc عبر -Wl,-Ttext-segment=0x40000000). */
ENTRY(_start)
PHDRS {
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x40000000 + SIZEOF_HEADERS;
    .text   : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    . = ALIGN(4096);
    .data   : { *(.data .data.*) } :data
    .bss    : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
