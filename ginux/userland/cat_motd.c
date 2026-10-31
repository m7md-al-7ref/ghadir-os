#include "ginux_syscall.h"

/* helper بسيط: يطبع رقم decimal بلا مكتبة (لا يوجد printf في هذا المستوى) */
static void write_uint(unsigned int v) {
    char buf[12];
    int i = 11;
    buf[i--] = '\0';
    if (v == 0) buf[i--] = '0';
    while (v > 0 && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    ginux_write(&buf[i + 1], 11 - (i + 1));
}

void _start(void) {
    const char pre[] = "[ring3-cat] opening 'motd.txt' from initrd...\n";
    ginux_write(pre, sizeof(pre) - 1);

    int fd = ginux_open("motd.txt", 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        const char err[] = "[ring3-cat] open() FAILED\n";
        ginux_write(err, sizeof(err) - 1);
        for (;;) __asm__ volatile ("nop");
    }

    char buf[128];
    int n;
    while ((n = ginux_read(fd, buf, sizeof(buf))) > 0) {
        ginux_write(buf, (unsigned int) n);
    }

    ginux_close(fd);

    const char post[] = "[ring3-cat] done, fd closed.\n";
    ginux_write(post, sizeof(post) - 1);

    /* اختبار حي: process ring3 عادية (بلا أي صلاحية خاصة) تقرأ مباشرة
       من السجل اللارجعي في النواة عبر SYS_LEDGER_READ -- إثبات أن
       القراءة فعلا مفتوحة (كتاب الفلسفة، الفصل السابع: "الشفافية جزء
       من المبدأ"). entry #0 يجب أن تكون دائمًا CREATE_KERNEL لـ pid=0 (kidle). */
    unsigned char entry[32];
    int rc = ginux_ledger_read(0, entry);
    const char lp[] = "[ring3-cat] ledger_read(0) rc=";
    ginux_write(lp, sizeof(lp) - 1);
    write_uint((unsigned int)(rc == 0 ? 1 : 0));
    if (rc == 0) {
        /* offsets ledger_entry_t: seq=0 tick=4 event_type=8 pid=12 */
        unsigned int event_type = *(unsigned int*)(entry + 8);
        unsigned int pid        = *(unsigned int*)(entry + 12);
        const char et[] = " event_type=";
        ginux_write(et, sizeof(et) - 1);
        write_uint(event_type);
        const char pp[] = " pid=";
        ginux_write(pp, sizeof(pp) - 1);
        write_uint(pid);
    }
    const char nl[] = "\n";
    ginux_write(nl, 1);

    /* آخر اختبار: execve() -- هذه الـ process (كانت "cat-motd") ستتبدل
       بالكامل إلى برنامج آخر تمامًا (execve_target.elf) محمّل من initrd
       وقت التشغيل الفعلي، دون إنشاء process جديدة. الـ pid يبقى
       نفسه -- execve تبدّل *صورة* process، وليس الـ process نفسها. */
    const char ex[] = "[ring3-cat] about to execve('execve_target.elf')...\n";
    ginux_write(ex, sizeof(ex) - 1);
    char* const argv[] = { "execve_target.elf", "hello-argv", "second-arg", 0 };
    ginux_execve("execve_target.elf", argv);

    /* لم نصل إلى هنا إلا إذا فشلت execve() */
    const char fail[] = "[ring3-cat] execve() FAILED!\n";
    ginux_write(fail, sizeof(fail) - 1);

    for (;;) __asm__ volatile ("nop");
}
