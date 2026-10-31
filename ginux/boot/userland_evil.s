.section .text
.global userland_evil_start
.global userland_evil_end

userland_evil_start:
    /* محاولة 1: نعطيه مؤشرًا خارج منطقة الكود/الستاك الخاصة بنا (مثلًا عنوان النواة
       نفسها في 0x00100000) -- يجب أن يُرفض */
    mov $1, %ebx
    mov $0x00100000, %ecx
    mov $50, %edx
    mov $0, %eax
    int $0x80

    /* محاولة 2: syscall برقم غير موجود (غير write مسموح) -- يجب أن يُرفض */
    mov $99, %eax
    int $0x80

    /* محاولة 3: overflow في ptr+len (integer wraparound) */
    mov $1, %ebx
    mov $0xFFFFFFF0, %ecx
    mov $0xFFFFFFFF, %edx
    mov $0, %eax
    int $0x80

spin_evil:
    jmp spin_evil

userland_evil_end:
