#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "idt.h"
#include "paging.h"

#define MAX_TASKS 32
#define KERNEL_STACK_SIZE 32768
#define USER_STACK_SIZE   4096

/* --- مناطق ذاكرة عمليات طبقة التوافق --- */
#define COMPAT_STACK_PAGES  64            /* 256KB stack (glibc stdio/printf تحتاج أكثر من صفحة) */
#define USER_MMAP_BASE     0x60000000    /* mmap anonymous: bump-allocator صاعد */
#define USER_MMAP_MAX      0x7F000000
#define LFD_MAX 32

/* fd table لطبقة التوافق: لكل fd نوع (console / ملف initrd / null / pipe) */
#define LFD_NONE    0
#define LFD_CONSOLE 1
#define LFD_FILE    2
#define LFD_NULL    3
#define LFD_PIPE_R  4
#define LFD_PIPE_W  5
#define LFD_DIR     6  /* content_id = رقم handle في جدول المجلدات المفتوحة (compat_syscall.c) */
typedef struct {
    uint8_t  type;
    uint8_t  cloexec;
    uint16_t pad;
    int      content_id; /* LFD_FILE */
    uint32_t pos;
    uint32_t flags;      /* O_* كما مرّرها المستخدم (للـ fcntl F_GETFL) */
    int      pipe_id;    /* LFD_PIPE_* */
} lfd_t;

/* fd 0/1/2 محجوزة لـ stdin/stdout/stderr (مثل Unix) -- لا يوجد entry
   لها في هذا الجدول، وتُعالَج مباشرة في syscall.c. الملفات الحقيقية
   (initrd) تبدأ من fd=3. */
#define MAX_FDS 8

typedef struct {
    int in_use;
    int content_id; /* إصلاح استقرار الهوية (2026، مراجعة خارجية): نخزّن
                        content_id (طبقة الحقائق) لا index الاسم --
                        يُحسم *مرة واحدة* وقت open()، ومستقل تمامًا عن
                        أي unlink/إعادة استعمال اسم يقع بعد ذلك. انظر
                        initrd.h لشرح المشكلة الأصلية والحل. */
    uint32_t pos;    /* موقع القراءة/الكتابة الحالي (lseek) -- نفس الموقع للاثنين، مثل POSIX */
    int writable;    /* 1 إذا فُتح الملف بـ O_WRONLY أو O_RDWR */
} fd_entry_t;

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE   /* خرجت (exit) لكن الأب ما زال حيًا ولم يستدعِ waitpid() بعد */
} task_state_t;

typedef struct {
    int pid;
    char name[16];
    task_state_t state;
    struct registers regs;   /* آخر حالة محفوظة (trap frame) */
    address_space_t* as;
    uint32_t kernel_stack_top; /* لـ TSS.esp0 -- غير قابل للوصول من ring3 (عزل) */
    int is_user;

    /* حدود الذاكرة الخاصة بمساحة المستخدم (حتى تستطيع الـ syscalls التحقق من
       صحة أي مؤشر يأتيها من ring3 قبل استعماله -- خط دفاع أساسي) */
    uint32_t user_code_base, user_code_size;
    uint32_t user_stack_base, user_stack_size;

    /* program break (لـ syscall brk/sbrk -- أساس malloc في mlibc) */
    uint32_t brk_base;     /* بداية منطقة الـ heap الخاصة بهذه العملية */
    uint32_t brk_current;  /* الحد الحالي (يكبر بـ brk()) */

    fd_entry_t fds[MAX_FDS]; /* ملفات initrd المفتوحة (fd 3..3+MAX_FDS-1) */

    /* --- compat ABI (انظر compat_abi.h) --- */
    int abi_compat;           /* 1 = أرقام syscall طبقة التوافق + errno سالب */
    lfd_t lfds[LFD_MAX];
    uint32_t mmap_base, mmap_cur;
    void* kstack_base;       /* لتحرير kernel stack بعد الموت (مؤجل) -- NULL = لا يُحرَّر */
    uint32_t wake_tick;      /* nanosleep */
    uint8_t  yielding;       /* انتظرت للتو (BLOCK): لا تُجدوَل ثانيةً قبل tick المؤقت القادم */
    uint32_t tls_base;       /* قاعدة مدخل TLS (set_thread_area)؛ 0 = لا يوجد */
    uint8_t  tls_valid;
    char     cwd[128];       /* المجلد الحالي (مسار قانوني بلا "/" في البداية؛ "" = الجذر) */
    uint8_t  fpu[512] __attribute__((aligned(16))); /* حالة x87+SSE (fxsave، أو fnsave على معالج بلا FXSR) -- تُحفظ/تُستعاد عند تبديل السياق */

    int parent_pid;   /* -1 = ليس لها أب (أنشأتها kernel_main مباشرة، وليس fork) */
    int exit_status;  /* آخر قيمة exit(status) -- صالحة فقط عندما state==TASK_ZOMBIE */
} process_t;

void process_init(void);
int  process_create_kernel(void (*entry)(void), const char* name);
int  process_create_user(const void* code, uint32_t code_len, const char* name);
int  process_create_elf(const void* elf_data, uint32_t elf_size, const char* name);
void scheduler_start(void); /* لا ترجع */
process_t* process_current(void);
void process_dump(void);

/* --- عمليات طبقة التوافق (compat_abi.h) --- */
int process_create_compat(const void* elf_data, uint32_t elf_size, const char* name, const void* args);
int process_execve_compat(process_t* p, const void* elf_data, uint32_t elf_size, const char* name,
                         struct registers* r, const void* args);
void process_free_compat_image(process_t* p);
process_t* process_by_pid(int pid);
void process_set_tls(process_t* p, uint32_t base); /* set_thread_area */
void process_set_idle_last(int on);      /* وضع الـ shell: kidle لا يأخذ وقتًا إذا كان غيره جاهزًا */
void process_clear_yielding(void);       /* يُستدعى كل tick */
void process_interrupt_foreground(struct registers* r);  /* Ctrl-C: يقتل العملية الأمامية وكل فروعها */
void schedule(struct registers* r);
void process_kill_current_and_reschedule(struct registers* r);

/* fork(): تنسخ address space كامل (كود+stack+heap) الخاص بالـ parent إلى process
   جديدة. r هو trap frame الحقيقي الحالي (وليس parent->regs المخزنة، لأنها
   قديمة منذ آخر context switch) -- سيبدأ الطفل من نفس المكان بالضبط
   بعد الـ syscall، بـ eax=0. ترجع pid الطفل (للأب) أو -1 (فشل،
   مثل no free slots أو out of memory). */
int process_fork(process_t* parent, const struct registers* r);

/* execve(): تستبدل صورة process الحالية (كود، stack، heap) ببرنامج ELF
   جديد كليًا، دون إنشاء process جديدة (pid يبقى نفسه). r هو trap
   frame الحقيقي الحالي، يُعدَّل مباشرة حتى يكون الرجوع إلى ring3 مباشرة
   إلى بداية البرنامج الجديد. argc/argv_data (2026-08-17): argv حقيقية
   الآن -- يبدأ البرنامج الجديد بمكدس أولي [esp]=argc [esp+4]=argv
   [esp+8]=envp (envp فارغة حاليًا)، كما في ABI يونكس القياسي المبسط.
   ترجع 0 عند النجاح، -1 عند الفشل (تبقى process القديمة كاملة بدون
   مساس، مثل execve الحقيقية). انظر process.c للشرح الكامل. */
#define EXECVE_MAX_ARGV    8
#define EXECVE_MAX_ARG_LEN 64
int process_execve(process_t* p, const void* elf_data, uint32_t elf_size,
                    const char* name, struct registers* r,
                    int argc, char argv_data[][EXECVE_MAX_ARG_LEN]);

/* مثل waitpid(2) لكن non-blocking دائمًا (لا يوجد sleep queue حاليًا):
   >0 = pid طفل جرى حصده (وكُتب *out_status)، 0 = لديه أطفال أحياء لكن
   ليس أيٌّ منهم zombie بعد، -1 = ليس لديه أي طفل يطابق target_pid أصلًا. */
int process_waitpid(int parent_pid, int target_pid, int* out_status);

uint32_t process_userland_demo_size(void);
const void* process_userland_demo_ptr(void);

/* حدود منطقة heap المستخدم (لـ syscall brk) -- مستعملة من process.c وsyscall.c */
#define USER_HEAP_BASE 0x44000000
#define USER_HEAP_MAX  0x48000000  /* سقف 64MB لـ heap كل process */

#endif
