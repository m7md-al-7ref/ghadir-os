#include "ginux_syscall.h"

/* writable_test.c -- إثبات حي: initrd قابلة للكتابة فعليًا (2026-08-11).
   ينشئ ملفًا جديدًا، يكتب فيه، يغلقه، ثم يعيد فتحه، يقرأ، يتأكد أن المحتوى
   مطابق، ثم يمسحه (unlink) ويتأكد أنه لم يعد موجودًا. */

static void write_str(const char* s) {
    unsigned int len = 0;
    while (s[len]) len++;
    ginux_write(s, len);
}

void _start(void) {
    write_str("[writable-test] creating 'test_output.txt'...\n");
    int fd = ginux_open("test_output.txt", AL_O_WRONLY | AL_O_CREAT, 0);
    if (fd < 0) {
        write_str("[writable-test] open(O_CREAT) FAILED\n");
        ginux_exit(1);
    }

    const char msg[] = "Hello from a REAL writable initrd file!";
    int n = ginux_write_fd(fd, msg, sizeof(msg) - 1);
    write_str("[writable-test] wrote bytes, n=");
    {
        char buf[12]; int i = 11; buf[i--] = '\0';
        unsigned int v = (unsigned int) n;
        if (v == 0) buf[i--] = '0';
        while (v > 0 && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
        write_str(&buf[i + 1]);
    }
    write_str("\n");
    ginux_close(fd);

    /* نعيد فتح الملف نفسه، لكن للقراءة، ونتأكد أن المحتوى مطابق */
    write_str("[writable-test] re-opening for read...\n");
    int fd2 = ginux_open("test_output.txt", 0, 0);
    if (fd2 < 0) {
        write_str("[writable-test] re-open FAILED\n");
        ginux_exit(1);
    }
    char readbuf[64];
    int rn = ginux_read(fd2, readbuf, sizeof(readbuf));
    ginux_close(fd2);

    write_str("[writable-test] read back: ");
    ginux_write(readbuf, (unsigned int) rn);
    write_str("\n");

    int matches = (rn == (int)(sizeof(msg) - 1));
    if (matches) {
        for (int i = 0; i < rn; i++) {
            if (readbuf[i] != msg[i]) { matches = 0; break; }
        }
    }
    write_str(matches ? "[writable-test] content MATCHES -- PASS\n"
                       : "[writable-test] content MISMATCH -- FAIL\n");

    /* الآن نمسحه (unlink) ونتأكد أن initrd_find لم يعد يجده */
    write_str("[writable-test] unlinking...\n");
    int rc = ginux_unlink("test_output.txt");
    write_str(rc == 0 ? "[writable-test] unlink OK\n" : "[writable-test] unlink FAILED\n");

    int fd3 = ginux_open("test_output.txt", 0, 0);
    write_str(fd3 < 0 ? "[writable-test] confirmed gone after unlink -- PASS\n"
                       : "[writable-test] STILL EXISTS after unlink -- FAIL\n");

    ginux_exit(0);
}
