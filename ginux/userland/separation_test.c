#include "ginux_syscall.h"

/* separation_test.c -- إثبات حي: طبقة الحقائق منفصلة فعليًا عن طبقة
   الاعتباريات (كتاب الفلسفة، الباب الرابع). نخلق ملفًا، نكتب فيه،
   نربط اسمًا ثانيًا به (SYS_LINK)، نتحقق أن القراءة من الاسم الثاني
   ترجع نفس المحتوى، ثم نحذف الاسم الأول ونتحقق أن الاسم الثاني ما
   زال يعمل (المحتوى نجا من حذف اسمه الأصلي -- مستحيل في تصميم مدمج). */

static void write_str(const char* s) {
    unsigned int len = 0;
    while (s[len]) len++;
    ginux_write(s, len);
}

void _start(void) {
    write_str("[separation-test] creating 'original.txt'...\n");
    int fd = ginux_open("original.txt", AL_O_WRONLY | AL_O_CREAT, 0);
    if (fd < 0) { write_str("[separation-test] create FAILED\n"); ginux_exit(1); }

    const char msg[] = "content that outlives its original name";
    ginux_write_fd(fd, msg, sizeof(msg) - 1);
    ginux_close(fd);
    write_str("[separation-test] wrote content, closed.\n");

    write_str("[separation-test] linking 'alias.txt' -> same content as 'original.txt'...\n");
    int rc = ginux_link("alias.txt", "original.txt");
    write_str(rc == 0 ? "[separation-test] link() OK\n" : "[separation-test] link() FAILED\n");

    /* نقرأ من الاسم الثاني، ونتأكد أن المحتوى مطابق */
    int fd2 = ginux_open("alias.txt", 0, 0);
    char buf[64];
    int n = ginux_read(fd2, buf, sizeof(buf));
    ginux_close(fd2);
    write_str("[separation-test] read via alias: ");
    ginux_write(buf, (unsigned int) n);
    write_str("\n");

    int matches = (n == (int)(sizeof(msg) - 1));
    if (matches) { for (int i = 0; i < n; i++) if (buf[i] != msg[i]) { matches = 0; break; } }
    write_str(matches ? "[separation-test] alias content MATCHES original -- PASS\n"
                       : "[separation-test] alias content MISMATCH -- FAIL\n");

    /* الاختبار الحاسم: نحذف الاسم الأصلي، ونتأكد أن الاسم الثاني ما زال يعمل */
    write_str("[separation-test] unlinking 'original.txt' (the ORIGINAL name)...\n");
    ginux_unlink("original.txt");

    int fd3 = ginux_open("alias.txt", 0, 0);
    if (fd3 < 0) {
        write_str("[separation-test] alias.txt GONE after original unlinked -- FAIL "
                   "(layers were not really separated)\n");
    } else {
        char buf2[64];
        int n2 = ginux_read(fd3, buf2, sizeof(buf2));
        ginux_close(fd3);
        int still_matches = (n2 == (int)(sizeof(msg) - 1));
        if (still_matches) { for (int i = 0; i < n2; i++) if (buf2[i] != msg[i]) { still_matches = 0; break; } }
        write_str(still_matches
            ? "[separation-test] alias.txt SURVIVED original's deletion, content intact -- PASS "
              "(this proves content and naming are genuinely separate layers)\n"
            : "[separation-test] alias.txt survived but content changed -- FAIL\n");
    }

    ginux_exit(0);
}
