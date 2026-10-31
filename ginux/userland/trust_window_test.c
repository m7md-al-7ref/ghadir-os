#include "ginux_syscall.h"

/* trust_window_test.c -- إثبات حي لنافذة الأمانة (2026-08-11):
   1. ننشئ ملفًا، نطلب حذفه، ثم نلغيه فورًا -- نتأكد أنه ما زال موجودًا (الإلغاء يعمل).
   2. ننشئ ملفًا ثانيًا، نطلب حذفه، ولا نلغيه -- ننتظر (busy-wait مبني على
      محاولات open() متكررة، بلا اعتماد على معايرة دقيقة لعدد cycles)
      حتى تنتهي النافذة فعليًا (300 tick = ~3 ثوانٍ حقيقية عبر PIT)،
      نتأكد أنه اختفى فعلًا. */

static void ws(const char* s) {
    unsigned int len = 0;
    while (s[len]) len++;
    ginux_write(s, len);
}

static void wnum(int v) {
    char buf[12]; int i = 11; buf[i--] = '\0';
    unsigned int u = (unsigned int) v;
    if (u == 0) buf[i--] = '0';
    while (u > 0 && i >= 0) { buf[i--] = '0' + (u % 10); u /= 10; }
    ws(&buf[i + 1]);
}

static void busy_pause(void) {
    for (volatile int i = 0; i < 2000000; i++) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

void _start(void) {
    /* --- اختبار 1: الإلغاء --- */
    ws("[trust-test] === TEST 1: request delete then cancel immediately ===\n");
    int fd1 = ginux_open("tw_cancel_me.txt", AL_O_WRONLY | AL_O_CREAT, 0);
    ginux_write_fd(fd1, "keep me", 7);
    ginux_close(fd1);

    int h1 = ginux_unlink("tw_cancel_me.txt");
    ws("[trust-test] unlink() returned handle="); wnum(h1); ws("\n");

    int c1 = ginux_unlink_cancel(h1);
    ws("[trust-test] cancel() returned="); wnum(c1); ws("\n");

    int check1 = ginux_open("tw_cancel_me.txt", 0, 0);
    if (check1 >= 0) {
        ws("[trust-test] TEST 1 PASS: file still exists after cancel\n");
        ginux_close(check1);
    } else {
        ws("[trust-test] TEST 1 FAIL: file gone despite cancel!\n");
    }

    /* --- اختبار 2: التنفيذ الفعلي بعد انتهاء النافذة --- */
    ws("[trust-test] === TEST 2: request delete, DO NOT cancel, wait for expiry ===\n");
    int fd2 = ginux_open("tw_let_expire.txt", AL_O_WRONLY | AL_O_CREAT, 0);
    ginux_write_fd(fd2, "delete me", 9);
    ginux_close(fd2);

    int h2 = ginux_unlink("tw_let_expire.txt");
    ws("[trust-test] unlink() returned handle="); wnum(h2); ws(" (not cancelling this one)\n");

    int gone = 0;
    for (int attempt = 0; attempt < 40; attempt++) {
        busy_pause();
        int check2 = ginux_open("tw_let_expire.txt", 0, 0);
        if (check2 < 0) {
            ws("[trust-test] TEST 2 PASS: file gone after window expired (attempt=");
            wnum(attempt);
            ws(")\n");
            gone = 1;
            break;
        }
        ginux_close(check2);
    }
    if (!gone) {
        ws("[trust-test] TEST 2 FAIL: file still exists after max wait!\n");
    }

    ginux_exit(0);
}
