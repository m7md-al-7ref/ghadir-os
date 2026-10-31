.macro ISR_NOERR num
.global isr\num
isr\num:
    cli
    push $0
    push $\num
    jmp isr_common_stub
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    cli
    push $\num
    jmp isr_common_stub
.endm

.macro IRQ num, mapped
.global irq\num
irq\num:
    cli
    push $0
    push $\mapped
    jmp irq_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31
/* 128 = 0x80 مستعملة لـ syscalls */
ISR_NOERR 128

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

.extern isr_handler
.extern irq_handler

/* ============================================================
 * إطار مقاطعة موحّد (16 dword = struct registers كاملة) لـ ring0 و ring3
 *
 * الجذر الحقيقي للبق الرابع (ظهر بعد التحقيق باستخدام QEMU):
 *   مقاطعة same-privilege (ring0->ring0) تدفع المعالج 3 قيم فقط
 *   (eip,cs,eflags) بدل 5، فحجم الـ frame 56 بايت، بينما struct registers
 *   حجمها 64 بايت. أي كود يكتب useresp/ss (offset 56/60) كان يكتب فوق
 *   أول كلمتين *حيّتين* من stack المهمة المقاطَعة (أي esp الأصلي بالضبط):
 *     - الـ stub القديم كان يكتب `movl $0, 56(%esp)` عند كل مقاطعة ring0
 *     - schedule() كانت تنفّذ `*r = regs` (64 بايت) فوق frame حجمه 56
 *   النتيجة: ret/pop لاحق في kidle يقرأ قيمة جرت الكتابة فوقها (غالبًا useresp
 *   الخاص بـ user stack = النمط ...FF0 المشهور) = كرش.
 *
 * الإصلاح: عند ring0 نعيد تموضع الـ 14 dword للأسفل بمقدار 8 بايت، فيصبح
 * لدينا frame كامل من 16 dword *تحت* esp الأصلي دون المساس بأي بايت حيّ،
 * ونكتب useresp = esp الأصلي الحقيقي و ss = 0x10. وبذلك يتعامل كل الكود
 * (schedule/fork/execve/idt) مع بنية واحدة موحّدة بدون sentinel
 * ولا حيل عناوين.
 * ============================================================ */
.macro BUILD_FRAME
    push %ds
    pusha

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    cmpl $0x08, 48(%esp)            /* cs == kernel code ؟ */
    jne 9f
    sub $8, %esp
    cld
    mov %esp, %edi                  /* edi/esi/ecx/eax محفوظة أصلًا في الـ frame (pusha) */
    lea 8(%esp), %esi
    mov $14, %ecx
    rep movsl                       /* نسخ للأسفل (dst < src): آمن حتى مع التداخل */
    lea 64(%esp), %eax
    mov %eax, 56(%esp)              /* useresp = esp الأصلي الحقيقي */
    movl $0x10, 60(%esp)            /* ss */
9:
.endm

isr_common_stub:
    BUILD_FRAME
    push %esp
    call isr_handler
    add $4, %esp
    jmp common_return

irq_common_stub:
    BUILD_FRAME
    push %esp
    call irq_handler
    add $4, %esp
    jmp common_return

/* --- common_return: رجوع مشترك --------------------------------------
 *  ring3: iret عادية (5 قيم، المعالج يبدّل ss:esp). يجب أن نعيد es/fs/gs
 *         إلى user data selector، وإلا صفّرتها iret (DPL0 < CPL3) وتسبب أي
 *         rep movs/stos في كود المستخدم (glibc memcpy) استثناء #GP.
 *  ring0: iret لا تبدّل esp (same-privilege)، فنبني iret-frame (3 قيم)
 *         يدويًا تحت useresp الهدف ونقفز إلى هناك. نمرّر القيم عبر .bss دون
 *         الحاجة إلى أي register حر. المقاطعات معطّلة (interrupt gate) فلا
 *         race على هذه المتغيرات (معالج واحد). */
.global common_return
common_return:
    cmpl $0x08, 48(%esp)
    je .Lcommon_ring0

    mov user_data_sel, %es
    mov user_data_sel, %fs
    mov user_gs_sel, %gs        /* 0x23، أو 0x33 إذا ضبطت العملية TLS (set_thread_area) */
    popa
    pop %ds
    add $8, %esp
    iret

.Lcommon_ring0:
    movl 44(%esp), %eax
    movl %eax, saved_eip
    movl 52(%esp), %eax
    movl %eax, saved_eflags
    movl 56(%esp), %eax
    movl %eax, saved_target_esp
    popa
    pop %ds
    mov saved_target_esp, %esp
    pushl saved_eflags
    pushl $0x08
    pushl saved_eip
    iret

.section .data
.align 4
user_data_sel:
    .long 0x23
.global user_gs_sel
user_gs_sel:
    .long 0x23

.section .bss
.align 4
saved_target_esp:
    .skip 4
saved_eip:
    .skip 4
saved_eflags:
    .skip 4
