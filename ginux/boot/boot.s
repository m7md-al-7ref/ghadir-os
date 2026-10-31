/* boot.s — نقطة انطلاق ginux Kernel
   يحتوي على هيدر Multiboot حتى يتمكن GRUB من إقلاع النواة،
   ثم يهيئ الـ stack وينادي kernel_main المكتوبة بلغة C */

.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bss
.align 16
stack_bottom:
.skip 16384   /* 16 KB مخصصة للـ stack */
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp   /* تهيئة الـ stack pointer */

    push %ebx               /* مؤشر multiboot info (يضعه GRUB في ebx) */
    push %eax               /* multiboot magic number */
    call kernel_main        /* الانتقال للنواة المكتوبة بـ C */

    cli
hang:
    hlt
    jmp hang

.size _start, . - _start
