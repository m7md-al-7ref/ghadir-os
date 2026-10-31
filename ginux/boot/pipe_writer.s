/* pipe_writer.s -- يكتب رسالة في pipe رقم 0 (أنشأتها النواة مسبقًا عند
   الإقلاع)، ثم يعلن عبر console أنه أنجز ذلك. مثال بسيط على IPC بين
   عمليتين معزولتين تمامًا (page directories منفصلة) عبر buffer تديره
   النواة، بدون أي ذاكرة مشتركة مباشرة بينهما. */

.section .text
.global pipe_writer_start
.global pipe_writer_end

pipe_writer_start:
    call get_msg
msg:
    .ascii "secret handoff: process A -> kernel pipe -> process B"
.set msg_len, . - msg

get_msg:
    pop %esi                /* esi = عنوان msg الحقيقي وقت التشغيل */

    /* sys_pipe_write(id=0, ptr=esi, len=msg_len) -- eax=2, ebx=id, ecx=ptr, edx=len */
    mov $0, %ebx
    mov %esi, %ecx
    mov $msg_len, %edx
    mov $2, %eax
    int $0x80

    /* إعلام عبر console (sys_write: eax=0, ebx=fd, ecx=ptr, edx=len) */
    mov $1, %ebx
    mov %esi, %ecx
    mov $msg_len, %edx
    mov $0, %eax
    int $0x80

spin_writer:
    jmp spin_writer

pipe_writer_end:
