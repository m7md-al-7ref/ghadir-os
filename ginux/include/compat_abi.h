#ifndef COMPAT_ABI_H
#define COMPAT_ABI_H

/* ============================================================
 * طبقة التوافق مع واجهة syscalls لمكتبات C القديمة (glibc 2.x، سنة 2000)
 *
 * القرار المعماري: بدلًا من إعادة ترقيم الـ ABI الأصلي للنواة (وهذا يكسر
 * gng-hello الجاهز مسبقًا وكل اختبارات userland)، لكل process علم
 * `abi_compat`. أي process تُنشأ عبر process_create_compat (أو تستدعي
 * execve من process تابعة لهذه الطبقة) تستعمل أرقام syscalls المعروفة في
 * i386 مع errno سالب (-1..-4095 في eax، كما تتوقع glibc: انظر
 * sysdeps/unix/sysv/linux/i386/sysdep.h، الشرط `resultvar >= 0xfffff001`).
 * الـ ABI الأصلي للنواة يبقى كما هو بالضبط للـ processes القديمة.
 * هذه الطبقة ليست كودًا منقولًا من أي نواة أخرى؛ هي فقط تلتزم بأرقام
 * النداءات وطريقة إرجاع الخطأ التي كُتبت المكتبة على أساسها.
 * ============================================================ */

#include <stdint.h>

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define ESPIPE  29
#define EPIPE   32
#define ERANGE  34
#define ENAMETOOLONG 36
#define ENOSYS  38

/* حدود عمليات execve/create لعمليات طبقة التوافق */
#define COMPAT_MAX_ARGS   32
#define COMPAT_MAX_ENV    32
#define COMPAT_ARGS_BYTES 16384

typedef struct {
    int   argc, envc;
    char* argv[COMPAT_MAX_ARGS + 1];
    char* envp[COMPAT_MAX_ENV + 1];
    char* data;      /* بفر kmalloc'd (COMPAT_ARGS_BYTES) تشير له argv/envp */
    uint32_t used;
} compat_args_t;

compat_args_t* compat_args_new(void);
void          compat_args_free(compat_args_t* a);
int           compat_args_add_arg(compat_args_t* a, const char* s);
int           compat_args_add_env(compat_args_t* a, const char* s);

struct registers;
void compat_syscall(struct registers* r);

/* جدول المجلدات المفتوحة (LFD_DIR): عدّاد مرجع لكل handle، تستدعيه process.c عند fork/exit */
void compat_dir_ref(int id);
void compat_dir_unref(int id);

/* console/tty (tty.c) */
void console_write(const char* buf, uint32_t len);
int  tty_read(char* buf, uint32_t len);
void tty_enable_irq(int on);        /* الاستقبال من مقاطعة المؤقت (وضع الـ shell) */
void tty_irq_poll(void);
int  tty_take_interrupt(void);      /* 1 إذا ضُغط Ctrl-C منذ آخر استدعاء */
void tty_set_fg(int pid);
int  tty_foreground_pid(void); /* >0 بايتات، 0 = لا يوجد سطر جاهز بعد (would block) */

#endif
