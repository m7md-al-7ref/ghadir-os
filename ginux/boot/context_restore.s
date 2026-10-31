.global context_restore
.extern common_return

/* context_restore(struct registers* r) __attribute__((noreturn))
 *
 * بوابة الدخول الأولى للـ scheduler (نداء وحيد، من scheduler_start()).
 * تضع %esp = r مباشرة وتفوّض الباقي إلى common_return (في isr.s) التي
 * فيها منطق ring0/ring3 المشترك -- انظر الشرح المفصل هناك. لم نعد
 * بحاجة إلى وسيط esp منفصل: r->regs.useresp الخاص بـ kidle معبّأ أصلًا
 * بـ kernel_stack_top من process_create_kernel، وcommon_return
 * تقرؤه مباشرة من البنية مثل أي إعادة جدولة عادية. */
context_restore:
    mov 4(%esp), %eax        /* arg1: struct registers* r */
    mov %eax, %esp
    jmp common_return
