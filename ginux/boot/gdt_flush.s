.global gdt_flush
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)

    mov $0x10, %ax   /* kernel data selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    ljmp $0x08, $.flush   /* far jump يعمل reload لـ CS */
.flush:
    ret

.global tss_flush
tss_flush:
    mov $0x28, %ax   /* TSS selector */
    ltr %ax
    ret
