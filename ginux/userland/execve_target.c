#include "ginux_syscall.h"

/* execve_target.c -- هذا البرنامج ليس مضمّن compile-time مثل باقي
   برامج الاختبار. يوضع في initrd (rootfs/execve_target.elf) ويُحمَّل
   فقط عبر execve() وقت التشغيل الفعلي -- إثبات حقيقي أن execve() تستطيع
   تحميل برنامج من initrd، وليس نظريًا فقط.
 *
 * (2026-08-17) الآن يقرأ argc/argv فعليًا من المكدس الذي بناه الكيرنال
 * (process_execve -> build_initial_stack، انظر process.c). _start
 * يجب أن تكون naked (بدون prologue تلقائي) لأن القيم فوق esp مباشرة
 * وقت البداية ليست مثل معاملات دالة عادية (لا يوجد return address
 * محقون -- execve قفزت مباشرة إلى هنا، ولا يوجد نداء call عادي). */

static void write_uint(unsigned int v) {
    char buf[12];
    int i = 11;
    buf[i--] = '\0';
    if (v == 0) buf[i--] = '0';
    while (v > 0 && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    ginux_write(&buf[i + 1], 11 - (i + 1));
}

/* __attribute__((used)): مستدعاة فعليًا من داخل inline asm في _start
   (السطر "call real_start")، لكن المترجم لا يستطيع تتبع هذا الاستدعاء
   عبر نص asm خام، فيعتبرها "غير مستخدمة" ويطلق تحذيرًا خاطئًا بدون هذا
   الـ attribute. */
__attribute__((used))
static void real_start(int argc, char** argv) {
    const char msg[] = "[execve-target] I am a DIFFERENT program, loaded via execve() from initrd!\n";
    ginux_write(msg, sizeof(msg) - 1);

    int pid = ginux_getpid();
    const char p[] = "[execve-target] my pid=";
    ginux_write(p, sizeof(p) - 1);
    write_uint((unsigned int) pid);
    const char nl[] = " (should be SAME pid as the caller -- execve replaces image, not process)\n";
    ginux_write(nl, sizeof(nl) - 1);

    /* الإثبات الجديد: argv حقيقية، مقروءة من المكدس الذي بناه execve() */
    const char al[] = "[execve-target] argc=";
    ginux_write(al, sizeof(al) - 1);
    write_uint((unsigned int) argc);
    ginux_write("\n", 1);

    for (int i = 0; i < argc; i++) {
        const char ai[] = "[execve-target]   argv[";
        ginux_write(ai, sizeof(ai) - 1);
        write_uint((unsigned int) i);
        const char eq[] = "] = \"";
        ginux_write(eq, sizeof(eq) - 1);
        unsigned int len = 0;
        while (argv[i][len]) len++;
        ginux_write(argv[i], len);
        ginux_write("\"\n", 2);
    }

    ginux_exit(0);
}

__attribute__((naked)) void _start(void) {
    __asm__ volatile (
        "mov (%esp), %eax\n"   /* argc */
        "mov 4(%esp), %ebx\n"  /* argv */
        "push %ebx\n"
        "push %eax\n"
        "call real_start\n"
    );
}
