#include "ginux_syscall.h"

/* تحويل رقم صحيح موجب إلى نص عشري -- لا يوجد printf هنا، bare-metal مثل باقي
   برامج userland في هذا المشروع */
static int itoa_simple(int val, char* out) {
    if (val == 0) { out[0] = '0'; return 1; }
    char tmp[12];
    int n = 0;
    while (val > 0) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

void _start(void) {
    int my_pid = ginux_getpid();

    char buf[64];
    int len = 0;
    const char p1[] = "[fork-test] pid=";
    for (int i = 0; p1[i]; i++) buf[len++] = p1[i];
    len += itoa_simple(my_pid, buf + len);
    buf[len++] = ' ';
    const char p2[] = "before fork()\n";
    for (int i = 0; p2[i]; i++) buf[len++] = p2[i];
    ginux_write(buf, (unsigned int) len);

    int fork_ret = ginux_fork();

    if (fork_ret == 0) {
        /* --- الطفل --- */
        int child_pid = ginux_getpid();
        len = 0;
        const char c1[] = "[fork-test]   CHILD running, pid=";
        for (int i = 0; c1[i]; i++) buf[len++] = c1[i];
        len += itoa_simple(child_pid, buf + len);
        buf[len++] = '\n';
        ginux_write(buf, (unsigned int) len);

        ginux_exit(42); /* status مميز حتى يتأكد الأب أنها وصلت صحيحة عبر waitpid */
        /* لا نصل إلى هنا */
    }

    if (fork_ret < 0) {
        const char err[] = "[fork-test] fork() FAILED\n";
        ginux_write(err, sizeof(err) - 1);
        for (;;) __asm__ volatile ("nop");
    }

    /* --- الأب --- */
    len = 0;
    const char p3[] = "[fork-test] PARENT: fork() returned child pid=";
    for (int i = 0; p3[i]; i++) buf[len++] = p3[i];
    len += itoa_simple(fork_ret, buf + len);
    buf[len++] = '\n';
    ginux_write(buf, (unsigned int) len);

    int status = -1;
    int reaped;
    do {
        reaped = ginux_waitpid(fork_ret, &status, 0);
    } while (reaped == 0); /* waitpid non-blocking -- ننتظر بـ polling */

    len = 0;
    const char p4[] = "[fork-test] PARENT: waitpid() reaped pid=";
    for (int i = 0; p4[i]; i++) buf[len++] = p4[i];
    len += itoa_simple(reaped, buf + len);
    const char p5[] = " status=";
    for (int i = 0; p5[i]; i++) buf[len++] = p5[i];
    len += itoa_simple(status, buf + len);
    buf[len++] = '\n';
    ginux_write(buf, (unsigned int) len);

    for (;;) __asm__ volatile ("nop");
}
