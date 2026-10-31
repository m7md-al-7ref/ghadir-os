/* pipe_reader.s -- يعيد المحاولة (polling) للقراءة من pipe رقم 0 حتى تصل
   بيانات، ثم يطبعها عبر console. يثبت أن البيانات وصلت فعليًا من
   process معزولة أخرى (pipe_writer) عبر buffer تديره النواة فقط. */

.section .text
.global pipe_reader_start
.global pipe_reader_end

pipe_reader_start:
    sub $80, %esp            /* مساحة محلية في الـ stack الخاص بنا لتخزين المقروء */

poll_loop:
    mov $0, %ebx               /* pipe id = 0 */
    mov %esp, %ecx              /* buffer = esp (داخل الـ stack المحلي) */
    mov $64, %edx                 /* حاول القراءة حتى 64 بايت */
    mov $3, %eax                   /* SYS_PIPE_READ */
    int $0x80

    cmp $0, %eax
    jle poll_loop                  /* 0 أو خطأ -- أعد المحاولة */

    /* وصلت بيانات -- eax = عدد البايتات، نطبعها (SYS_WRITE: ebx=fd, ecx=ptr, edx=len) */
    mov %eax, %edx
    mov %esp, %ecx
    mov $1, %ebx
    mov $0, %eax                    /* SYS_WRITE */
    int $0x80

spin_reader:
    jmp spin_reader

pipe_reader_end:
