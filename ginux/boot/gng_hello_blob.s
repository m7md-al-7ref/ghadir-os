.section .rodata
.global gng_hello_start
.global gng_hello_end

gng_hello_start:
.incbin "boot/prebuilt/gng-hello.elf"
gng_hello_end:
