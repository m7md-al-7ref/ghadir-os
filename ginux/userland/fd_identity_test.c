#include "ginux_syscall.h"

/* fd_identity_test.c -- إثبات حي لإصلاح bug حقيقي (مراجعة خارجية،
   2026): قبل الإصلاح، fd مفتوحة على ملف محذوف (unlink) كانت تتحول
   بصمت لتقرأ ملفًا جديدًا لو خلق بنفس الاسم بعدها (لأن fd كانت تخزن
   index الاسم، وindex الاسم يتعاد استعماله). هذا الاختبار يثبت أن fd
   القديمة تبقى مرتبطة بمحتواها الأصلي، بصرف النظر عمّا يقع للاسم. */

static void write_str(const char* s) {
    unsigned int len = 0;
    while (s[len]) len++;
    ginux_write(s, len);
}

void _start(void) {
    write_str("[fd-identity-test] creating 'shared_name.txt' with OLD content...\n");
    int fd1 = ginux_open("shared_name.txt", AL_O_WRONLY | AL_O_CREAT, 0);
    if (fd1 < 0) { write_str("[fd-identity-test] create FAILED\n"); ginux_exit(1); }
    const char old_content[] = "OLD-CONTENT-BEFORE-UNLINK";
    ginux_write_fd(fd1, old_content, sizeof(old_content) - 1);

    /* لا نغلق fd1 -- نتركها مفتوحة عمدًا، مثل process حقيقية تحتفظ
       بملف مفتوح بينما يحذفه شخص آخر وينشئ واحدًا جديدًا بنفس الاسم. */

    write_str("[fd-identity-test] unlinking 'shared_name.txt' (fd1 STAYS open)...\n");
    ginux_unlink("shared_name.txt");

    /* unlink() تمر عبر نافذة الأمانة (تأخير ~3 ثوانٍ) -- الاسم ما زال
       موجودًا حتى تنتهي. يجب أن ننتظر فعليًا انتهاءها قبل أن ننشئ ملفًا
       جديدًا بنفس الاسم، وإلا وجد open(O_CREAT) الملف القديم ما زال
       موجودًا وفتحه هو نفسه (وليس ملفًا جديدًا)، فيقيس الاختبار شيئًا آخر.
       نفس نمط الانتظار المستعمل في trust_window_test.c. */
    write_str("[fd-identity-test] waiting for trust window to expire...\n");
    int expired = 0;
    for (int attempt = 0; attempt < 60; attempt++) {
        for (volatile int i = 0; i < 2000000; i++) __asm__ volatile ("pause" ::: "memory");
        int probe = ginux_open("shared_name.txt", 0, 0); /* بلا O_CREAT -- يفشل إذا حُذف الاسم فعلًا */
        if (probe < 0) { expired = 1; break; }
        ginux_close(probe);
    }
    if (!expired) { write_str("[fd-identity-test] trust window never expired -- FAIL\n"); ginux_exit(1); }
    write_str("[fd-identity-test] name is gone (window expired), fd1 still open\n");

    write_str("[fd-identity-test] creating a DIFFERENT 'shared_name.txt' with NEW content...\n");
    int fd2 = ginux_open("shared_name.txt", AL_O_WRONLY | AL_O_CREAT, 0);
    if (fd2 < 0) { write_str("[fd-identity-test] second create FAILED\n"); ginux_exit(1); }
    const char new_content[] = "NEW-CONTENT-DIFFERENT-FILE";
    ginux_write_fd(fd2, new_content, sizeof(new_content) - 1);
    ginux_close(fd2);

    /* الاختبار الحاسم: نرجع نقرأ من fd1 (القديمة، ما زالت مفتوحة) --
       يجب أن ترجع OLD-CONTENT، وليس NEW-CONTENT، رغم أن اسمها
       حُذف وأُعيد استعماله لملف جديد كليًا في الوسط. */
    write_str("[fd-identity-test] seeking fd1 to start, reading back via OLD fd...\n");
    ginux_lseek(fd1, 0, 0 /* SEEK_SET */);
    char buf[64];
    int n = ginux_read(fd1, buf, sizeof(buf));
    ginux_close(fd1);

    write_str("[fd-identity-test] fd1 read back: ");
    ginux_write(buf, (unsigned int) n);
    write_str("\n");

    int matches_old = (n == (int)(sizeof(old_content) - 1));
    if (matches_old) {
        for (int i = 0; i < n; i++) if (buf[i] != old_content[i]) { matches_old = 0; break; }
    }

    write_str(matches_old
        ? "[fd-identity-test] fd1 correctly kept OLD content -- PASS "
          "(fd identity survived unlink+name-reuse)\n"
        : "[fd-identity-test] fd1 got WRONG content (leaked into new file) -- FAIL\n");

    ginux_exit(0);
}
