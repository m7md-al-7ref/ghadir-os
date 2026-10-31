/* userland_demo.s -- برنامج ring3 تجريبي صغير جدا.
   مستقل عن موقع التحميل (Position Independent) عبر تقنية call/pop حتى نتمكن
   من وضعه في أي عنوان ظاهري عشوائي (ASLR) دون الحاجة إلى relocation.
   يستعمل syscall واحدًا فقط (write) -- مثال على "أقل صلاحيات ممكنة" لبرنامج مستخدم. */

.section .text
.global userland_demo_start
.global userland_demo_end

userland_demo_start:
    call get_str
msg:
    .ascii "[ring3] hello from an isolated usermode process (PID demo)\n"
.set msg_len, . - msg

get_str:
    pop %esi              /* esi = عنوان msg الحقيقي وقت التشغيل (بدون حاجة لـ absolute address) */

    /* ABI: eax=رقم النداء, ebx=fd, ecx=ptr, edx=len */
    mov $1, %ebx             /* fd = STDOUT */
    mov %esi, %ecx
    mov $msg_len, %edx
    mov $0, %eax              /* 0 = sys_write */
    int $0x80

spin:
    /* برنامج المستخدم ليس لديه الحق في تنفيذ hlt (تعليمة محجوزة لـ ring0) --
       لذلك نستعمل busy loop بسيطة ليبقى موجودًا ويشارك المعالج مع باقي العمليات */
    jmp spin

userland_demo_end:
