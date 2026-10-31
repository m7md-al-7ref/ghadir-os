#include "syscall.h"
#include "idt.h"
#include "process.h"
#include "serial.h"
#include "pipe.h"
#include "pmm.h"
#include "paging.h"
#include "initrd.h"
#include "kstring.h"
#include "ledger.h"
#include "trust_window.h"
#include "compat_abi.h"

/* ملاحظة تزامن: هذه الأرقام يجب أن تبقى مطابقة تمامًا لـ
   mlibc/include/al/syscall.h (AL_SYS_*) -- مكتوبة هنا محليًا لأن النواة
   وmlibc بناءان منفصلان (النواة لا تضمّن headers الخاصة بـ mlibc). */
#define SYS_WRITE        0
#define SYS_PIPE_CREATE  1
#define SYS_PIPE_WRITE   2
#define SYS_PIPE_READ    3
#define SYS_EXIT         4
#define SYS_FORK         5
#define SYS_EXECVE       6
#define SYS_WAITPID      7
#define SYS_GETPID       8
#define SYS_OPEN         9
#define SYS_CLOSE        10
#define SYS_READ         11
#define SYS_LSEEK        12
#define SYS_BRK          15
#define SYS_STAT         21
#define SYS_FSTAT        22
#define SYS_UNLINK       20
#define SYS_LEDGER_READ  30 /* قراءة entry من السجل اللارجعي -- انظر ledger.h.
                                لا يوجد SYS_LEDGER_WRITE ولا SYS_LEDGER_DELETE
                                أصلا -- هذا الأمر مقصود، ليس نسيانًا (كتاب الفلسفة،
                                الفصل السابع: عدم توثيق الذات). */
#define SYS_UNLINK_CANCEL 31 /* إلغاء طلب حذف معلق (نافذة الأمانة) -- انظر trust_window.h */
#define SYS_LINK 32 /* ربط اسم جديد بمحتوى موجود -- إثبات فصل طبقة الحقائق عن الاعتباريات، انظر initrd.h */

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* fd 0..2 محجوزة لـ std*، والملفات الحقيقية (initrd) تبدأ من هنا.
   يجب أن يطابق بالضبط المنطق في process.h (fds[MAX_FDS] يمثل fd = FD_BASE..FD_BASE+MAX_FDS-1) */
#define FD_BASE 3

#define MAX_PATH 64

/* مثل struct stat الخاصة بـ mlibc (sys/stat.h) -- يجب أن تطابق بالضبط ترتيب
   وأحجام الحقول (long/unsigned long = 4 بايت في i386، مثل uint32_t/int32_t) */
struct al_stat {
    uint32_t st_dev;
    uint32_t st_ino;
    int32_t  st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    int32_t  st_size;
    int32_t  st_blksize;
    int32_t  st_blocks;
    int32_t  st_atime;
    int32_t  st_mtime;
    int32_t  st_ctime;
};
#define AL_S_IFREG 0100000

/* مبدأ "أقل صلاحيات ممكنة": عدد محدود جدًا من الـ syscalls، وكل واحد منها
   يتحقق بدقة قبل التنفيذ. أي مؤشر يأتي من ring3 يُعامل كـ "غير
   موثوق" حتى نتأكد أنه بالكامل داخل منطقة الذاكرة المخصصة لهذه الـ process. */

static int ptr_in_bounds(const process_t* cur, uint32_t ptr, uint32_t len, int require_write) {
    if (!cur) return 0;
    int overflow = (ptr + len) < ptr;
    if (overflow || len > 4096) return 0;

    int in_stack = ptr >= cur->user_stack_base && (ptr + len) <= (cur->user_stack_base + cur->user_stack_size);
    if (in_stack) return 1; /* الـ stack دائمًا RW، صالح للقراءة والكتابة */

    int in_heap = ptr >= cur->brk_base && (ptr + len) <= cur->brk_current;
    if (in_heap) return 1; /* heap الخاص بـ malloc/brk -- RW دائمًا */

    if (!require_write) {
        int in_code = ptr >= cur->user_code_base && (ptr + len) <= (cur->user_code_base + cur->user_code_size);
        if (in_code) return 1; /* الكود قابل للقراءة فقط (R-X)، صالح كمصدر لا كوجهة */
    }
    return 0;
}

/* تنسخ مسار (نص منتهٍ بـ NUL) من ذاكرة المستخدم (ring3) إلى بفر في النواة،
   بأمان. نطلب أن تكون منطقة MAX_PATH كاملة صالحة (in_bounds) حتى قبل أن
   نعرف الطول الحقيقي للنص -- حماية من القراءة خارج حدود الـ process. */
static int copy_path_from_user(const process_t* cur, uint32_t user_ptr, char* out) {
    if (!ptr_in_bounds(cur, user_ptr, MAX_PATH, 0)) return 0;
    const char* src = (const char*) user_ptr;
    int i = 0;
    for (; i < MAX_PATH - 1; i++) {
        out[i] = src[i];
        if (out[i] == '\0') return 1;
    }
    out[MAX_PATH - 1] = '\0'; /* المسار طويل جدًا/غير منتهٍ -- نقصّه بدل أن نسقط */
    return 1;
}

/* تجد fd_entry فارغة في جدول process، وترجع رقم fd الكامل (FD_BASE-relative) */
static int alloc_fd_slot(const process_t* cur) {
    for (int i = 0; i < MAX_FDS; i++)
        if (!cur->fds[i].in_use) return i;
    return -1;
}

/* تحوّل رقم fd (من ring3) إلى fd_entry في الجدول، مع تحقق كامل من الحدود */
static fd_entry_t* fd_lookup(process_t* cur, uint32_t fd) {
    if (fd < FD_BASE) return NULL; /* std* ليس لها entry هنا */
    uint32_t i = fd - FD_BASE;
    if (i >= MAX_FDS) return NULL;
    if (!cur->fds[i].in_use) return NULL;
    return &cur->fds[i];
}

static void fill_stat_for_index(struct al_stat* st, int idx) {
    memset(st, 0, sizeof(*st));
    const initrd_file_t* f = initrd_get(idx);
    st->st_mode = AL_S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = f ? (int32_t) f->size : 0;
    st->st_blksize = 512;
    st->st_blocks = f ? (int32_t)((f->size + 511) / 512) : 0;
}

/* fstat عبر content_id (fd مفتوحة) -- إصلاح استقرار الهوية، لا نمر عبر
   اسم قد يكون حُذف/تبدّل. انظر initrd.h. */
static void fill_stat_for_content(struct al_stat* st, int content_id) {
    memset(st, 0, sizeof(*st));
    int size = initrd_content_size_of(content_id);
    st->st_mode = AL_S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = (size >= 0) ? size : 0;
    st->st_blksize = 512;
    st->st_blocks = (size >= 0) ? (int32_t)((size + 511) / 512) : 0;
}

static void syscall_handler(struct registers* r) {
    uint32_t syscall_no = r->eax;
    process_t* cur = process_current();

    /* العمليات ذات طبقة التوافق (أرقام Linux + errno سالب) تُوجَّه إلى طبقتها الخاصة */
    if (cur && cur->abi_compat) { compat_syscall(r); return; }

    switch (syscall_no) {
        case SYS_WRITE: {
            /* ABI: ebx=fd, ecx=ptr, edx=len (مثل write(2) الحقيقية) */
            uint32_t fd = r->ebx, ptr = r->ecx, len = r->edx;
            if (!cur || !cur->is_user || !ptr_in_bounds(cur, ptr, len, 0)) {
                kprintf("[syscall] SECURITY: sys_write rejected (ptr=%x len=%d pid=%d)\n",
                        ptr, len, cur ? cur->pid : -1);
                r->eax = (uint32_t) -1;
                return;
            }

            if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                kprintf("[pid %d says] ", cur->pid);
                const char* s = (const char*) ptr;
                for (uint32_t i = 0; i < len; i++) serial_putc(s[i]);
                r->eax = len;
                break;
            }

            /* كتابة لملف initrd حقيقي (2026-08-11 -- initrd قابلة للكتابة الآن) */
            fd_entry_t* e = fd_lookup(cur, fd);
            if (!e || !e->writable) {
                kprintf("[syscall] SECURITY: write rejected, fd=%d not open for writing (pid=%d)\n",
                        (int) fd, cur->pid);
                r->eax = (uint32_t) -1;
                return;
            }
            int32_t n = initrd_write_content(e->content_id, (const void*) ptr, e->pos, len);
            if (n < 0) { r->eax = (uint32_t) -1; break; }
            e->pos += (uint32_t) n;
            r->eax = (uint32_t) n;
            break;
        }

        case SYS_EXIT: {
            int status = (int) r->ebx;
            if (cur) cur->exit_status = status; /* لازم قبل process_kill -- waitpid() تحتاجها */
            kprintf("[syscall] pid=%d exit(%d)\n", cur ? cur->pid : -1, status);
            process_kill_current_and_reschedule(r);
            return; /* r الآن فيه trap frame الخاص بـ process أخرى */
        }

        case SYS_FORK: {
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }
            int child_pid = process_fork(cur, r);
            r->eax = (uint32_t) child_pid; /* -1 = فشل، وإلا pid الطفل (بالنسبة للأب فقط -- الطفل يبدأ بـ eax=0 تضعه process_fork نفسها) */
            break;
        }

        case SYS_EXECVE: {
            /* ABI: ebx=pathname ptr، ecx=argv ptr (مصفوفة مؤشرات
               NULL-terminated في ذاكرة المستدعي، أو 0 لعدم تمرير argv).
               initrd فقط حاليًا -- بدون صدفة حقيقية بعد تستطيع بناء PATH
               search، مسار مباشر فقط. الحد الأقصى EXECVE_MAX_ARGV عناصر،
               EXECVE_MAX_ARG_LEN بايت لكل واحد (process.h). argv[0]
               بالعرف اسم البرنامج، مثل يونكس -- لا يوجد إجبار هنا. */
            uint32_t path_ptr = r->ebx, argv_ptr = r->ecx;
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }

            char path[MAX_PATH];
            if (!copy_path_from_user(cur, path_ptr, path)) { r->eax = (uint32_t) -1; return; }

            int idx = initrd_find(path);
            if (idx < 0) {
                kprintf("[syscall] execve: '%s' not found in initrd (pid=%d)\n", path, cur->pid);
                r->eax = (uint32_t) -1;
                return;
            }
            const initrd_file_t* file = initrd_get(idx);
            if (!file) { r->eax = (uint32_t) -1; return; }

            /* يجب أن نستخرج argv من ذاكرة المستدعي *قبل* process_execve --
               بعد النجاح، تُدمَّر مساحة العناوين القديمة بالكامل، فيجب أن
               ننسخ كل شيء إلى بفر في النواة قبل أن نبدأ الاستبدال. */
            char argv_data[EXECVE_MAX_ARGV][EXECVE_MAX_ARG_LEN];
            int argc = 0;
            if (argv_ptr != 0) {
                for (; argc < EXECVE_MAX_ARGV; argc++) {
                    uint32_t entry_ptr = argv_ptr + (uint32_t) argc * sizeof(uint32_t);
                    if (!ptr_in_bounds(cur, entry_ptr, sizeof(uint32_t), 0)) break;
                    uint32_t str_ptr = *(uint32_t*) entry_ptr;
                    if (str_ptr == 0) break; /* NULL terminator الخاص بـ argv[] */
                    if (!ptr_in_bounds(cur, str_ptr, EXECVE_MAX_ARG_LEN, 0)) break;
                    const char* src = (const char*) str_ptr;
                    int i = 0;
                    for (; i < EXECVE_MAX_ARG_LEN - 1 && src[i]; i++) argv_data[argc][i] = src[i];
                    argv_data[argc][i] = '\0';
                }
            }

            /* execve تعدّل r مباشرة عند النجاح (انظر process_execve في
               process.c) -- بدون "return -1" هنا، لا يوجد رجوع عادي للمستدعي
               أصلا (مثل execve الحقيقية: النجاح يبدل البرنامج بالكامل). */
            int rc = process_execve(cur, file->data, file->size, path, r, argc, argv_data);
            if (rc != 0) { r->eax = (uint32_t) -1; return; }
            return; /* r الآن فيه trap frame الخاص بالبرنامج الجديد */
        }

        case SYS_WAITPID: {
            /* ABI: ebx=pid (-1=أي طفل), ecx=wstatus ptr (0=تجاهل), edx=options (لا تُقرأ حاليًا) */
            uint32_t target_pid = r->ebx, wstatus_ptr = r->ecx;
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }

            int status = 0;
            int reaped = process_waitpid(cur->pid, (int) target_pid, &status);
            if (reaped <= 0) { r->eax = (uint32_t) reaped; break; } /* -1=لا يوجد طفل، 0=حي ما زال (non-blocking) */

            if (wstatus_ptr != 0) {
                if (!ptr_in_bounds(cur, wstatus_ptr, sizeof(int), 1)) {
                    kprintf("[syscall] SECURITY: waitpid wstatus ptr rejected (ptr=%x pid=%d)\n",
                            wstatus_ptr, cur->pid);
                    /* الطفل حُصد فعلًا (slot تحرر) -- نكمل ونرجع pid الخاص به، فقط بدون كتابة status */
                } else {
                    memcpy((void*) wstatus_ptr, &status, sizeof(status));
                }
            }
            r->eax = (uint32_t) reaped;
            break;
        }

        case SYS_GETPID: {
            if (!cur) { r->eax = (uint32_t) -1; return; }
            r->eax = (uint32_t) cur->pid;
            break;
        }

        case SYS_BRK: {
            /* ABI: ebx = العنوان المطلوب (0 = استعلام عن الحد الحالي فقط) */
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }
            uint32_t requested = r->ebx;

            if (requested == 0) { r->eax = cur->brk_current; break; }

            if (requested < cur->brk_base || requested > USER_HEAP_MAX) {
                kprintf("[syscall] SECURITY: brk rejected out-of-range request %x (pid=%d)\n",
                        requested, cur->pid);
                r->eax = cur->brk_current; /* فشل: أرجع القيمة الحالية (مثل Linux) */
                break;
            }

            uint32_t old_top = (cur->brk_current + 0xFFF) & ~0xFFF;
            uint32_t new_top = (requested + 0xFFF) & ~0xFFF;

            if (new_top > old_top) {
                uint32_t va;
                for (va = old_top; va < new_top; va += PMM_FRAME_SIZE) {
                    uint32_t phys = pmm_alloc_frame();
                    if (!phys) {
                        kprintf("[syscall] brk: out of memory (pid=%d)\n", cur->pid);
                        break;
                    }
                    if (!paging_map(cur->as, va, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX)) {
                        kprintf("[syscall] brk: paging_map failed (pid=%d)\n", cur->pid);
                        pmm_free_frame(phys);
                        break;
                    }
                }
                cur->brk_current = va; /* توقفنا عند آخر صفحة نجحت فعليًا */
            } else {
                /* تقليص: لا نحرر frames الآن (leak موثق، مثل process kill) */
                cur->brk_current = requested;
            }

            r->eax = cur->brk_current;
            break;
        }

        case SYS_PIPE_CREATE: {
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }
            int id = pipe_create();
            kprintf("[syscall] pid=%d pipe_create -> id=%d\n", cur->pid, id);
            r->eax = (uint32_t) id;
            break;
        }

        case SYS_PIPE_WRITE: {
            uint32_t id = r->ebx, ptr = r->ecx, len = r->edx;
            if (!cur || !cur->is_user || !ptr_in_bounds(cur, ptr, len, 0)) {
                kprintf("[syscall] SECURITY: pipe_write rejected (ptr=%x len=%d pid=%d)\n",
                        ptr, len, cur ? cur->pid : -1);
                r->eax = (uint32_t) -1;
                return;
            }
            int n = pipe_write((int) id, (const void*) ptr, len);
            r->eax = (uint32_t) n;
            break;
        }

        case SYS_PIPE_READ: {
            uint32_t id = r->ebx, ptr = r->ecx, len = r->edx;
            /* هنا يجب "require_write=1" لأننا سنكتب في هذا المؤشر (وجهة
               القراءة) -- يجب أن يكون في stack المستخدم (RW)، وليس في منطقة
               الكود (R-X، غير قابلة للكتابة) */
            if (!cur || !cur->is_user || !ptr_in_bounds(cur, ptr, len, 1)) {
                kprintf("[syscall] SECURITY: pipe_read rejected (ptr=%x len=%d pid=%d)\n",
                        ptr, len, cur ? cur->pid : -1);
                r->eax = (uint32_t) -1;
                return;
            }
            int n = pipe_read((int) id, (void*) ptr, len);
            r->eax = (uint32_t) n;
            break;
        }

        case SYS_OPEN: {
            /* ABI: ebx=pathname ptr, ecx=flags, edx=mode (مثل open(2)).
               initrd الآن قابلة للكتابة (2026-08-11) -- انظر initrd.h. */
            uint32_t path_ptr = r->ebx, flags = r->ecx;
            (void) r->edx; /* mode (صلاحيات unix، ليس لدينا نموذج صلاحيات حقيقي بعد، نتجاهله) */
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }

            char path[MAX_PATH];
            if (!copy_path_from_user(cur, path_ptr, path)) {
                kprintf("[syscall] SECURITY: open rejected, bad path ptr=%x (pid=%d)\n", path_ptr, cur->pid);
                r->eax = (uint32_t) -1;
                return;
            }

            int want_write = (flags & 0x3) != 0;      /* O_WRONLY|O_RDWR */
            int want_create = (flags & 0x40) != 0;    /* O_CREAT */
            int want_trunc  = (flags & 0x200) != 0;   /* O_TRUNC */

            int idx = initrd_find(path);
            if (idx < 0) {
                if (!want_create) {
                    kprintf("[syscall] open('%s') -> ENOENT (pid=%d)\n", path, cur->pid);
                    r->eax = (uint32_t) -1;
                    break;
                }
                idx = initrd_create(path);
                if (idx < 0) {
                    kprintf("[syscall] open('%s', O_CREAT) -> failed (table full?) (pid=%d)\n", path, cur->pid);
                    r->eax = (uint32_t) -1;
                    break;
                }
                kprintf("[syscall] pid=%d created '%s' (initrd_idx=%d)\n", cur->pid, path, idx);
            } else if (want_trunc && want_write) {
                initrd_truncate(idx, 0);
            }

            int slot = alloc_fd_slot(cur);
            if (slot < 0) {
                kprintf("[syscall] open('%s') -> EMFILE, no free fd (pid=%d)\n", path, cur->pid);
                r->eax = (uint32_t) -1;
                break;
            }

            /* إصلاح استقرار الهوية: نحسم content_id *مرة واحدة* هنا (وقت
               open فقط)، ونخزنه في fd -- وليس index الاسم. انظر
               initrd.h لشرح المشكلة الأصلية (fd تتحول لملف آخر بعد
               unlink+إعادة استعمال الاسم). */
            int cid = initrd_open_by_index(idx);
            if (cid < 0) {
                kprintf("[syscall] open('%s') -> internal error resolving content (pid=%d)\n", path, cur->pid);
                r->eax = (uint32_t) -1;
                break;
            }

            cur->fds[slot].in_use = 1;
            cur->fds[slot].content_id = cid;
            cur->fds[slot].pos = 0;
            cur->fds[slot].writable = want_write;
            kprintf("[syscall] pid=%d open('%s') -> fd=%d (writable=%d, content_id=%d)\n",
                    cur->pid, path, FD_BASE + slot, want_write, cid);
            r->eax = (uint32_t)(FD_BASE + slot);
            break;
        }

        case SYS_CLOSE: {
            uint32_t fd = r->ebx;
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }
            if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) { r->eax = 0; break; }
            fd_entry_t* e = fd_lookup(cur, fd);
            if (!e) { r->eax = (uint32_t) -1; break; }
            initrd_close_content(e->content_id); /* fix: يحرر المحتوى فعليًا إذا كان آخر fd + الاسم محذوف مسبقًا */
            e->in_use = 0;
            r->eax = 0;
            break;
        }

        case SYS_READ: {
            /* ABI: ebx=fd, ecx=ptr (وجهة، يجب أن تكون RW), edx=len */
            uint32_t fd = r->ebx, ptr = r->ecx, len = r->edx;
            if (!cur || !cur->is_user || !ptr_in_bounds(cur, ptr, len, 1)) {
                kprintf("[syscall] SECURITY: read rejected (ptr=%x len=%d pid=%d)\n",
                        ptr, len, cur ? cur->pid : -1);
                r->eax = (uint32_t) -1;
                return;
            }
            if (fd == STDIN_FILENO) {
                /* لا يوجد driver يربط keyboard بالـ syscall بعد */
                r->eax = 0;
                break;
            }
            fd_entry_t* e = fd_lookup(cur, fd);
            if (!e) { r->eax = (uint32_t) -1; break; }

            int32_t n = initrd_read_content(e->content_id, (void*) ptr, e->pos, len);
            if (n < 0) { r->eax = (uint32_t) -1; break; }
            e->pos += (uint32_t) n;
            r->eax = (uint32_t) n;
            break;
        }

        case SYS_LSEEK: {
            /* ABI: ebx=fd, ecx=offset (signed), edx=whence (0=SET,1=CUR,2=END) */
            uint32_t fd = r->ebx;
            int32_t offset = (int32_t) r->ecx;
            uint32_t whence = r->edx;
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }
            fd_entry_t* e = fd_lookup(cur, fd);
            if (!e) { r->eax = (uint32_t) -1; break; }
            int csize = initrd_content_size_of(e->content_id);
            if (csize < 0) { r->eax = (uint32_t) -1; break; }

            int64_t base = (whence == 0) ? 0 : (whence == 1) ? (int64_t) e->pos :
                            (whence == 2) ? (int64_t) csize : -1;
            if (base < 0) { r->eax = (uint32_t) -1; break; } /* whence غير صالح */

            int64_t new_pos = base + offset;
            if (new_pos < 0) { r->eax = (uint32_t) -1; break; }
            /* lseek(2) الحقيقية لا ترفض الانتقال بعد نهاية الملف (هذا
               أمر عادي، يُحدث ثقبًا/hole عند الكتابة بعدها) -- لكن فقط للملفات
               المفتوحة للكتابة. للقراءة فقط، السلوك القديم (رفض تجاوز
               النهاية) ما زال منطقيًا (لا يوجد ما نقرؤه بعد النهاية). */
            if (new_pos > (int64_t) csize && !e->writable) { r->eax = (uint32_t) -1; break; }

            e->pos = (uint32_t) new_pos;
            r->eax = e->pos;
            break;
        }

        case SYS_UNLINK: {
            /* ABI: ebx=pathname ptr. الآن تمر عبر نافذة الأمانة (كتاب
               الفلسفة، الفصل السادس) -- لا يُنفَّذ الحذف فورًا، يدخل
               تأخير قابل للإلغاء. ترجع >= 0 (handle، يُستعمل مع
               SYS_UNLINK_CANCEL) عند القبول، -1 عند الفشل (الملف
               غير موجود، أو جدول الطلبات المعلقة ممتلئ). */
            uint32_t path_ptr = r->ebx;
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }
            char path[MAX_PATH];
            if (!copy_path_from_user(cur, path_ptr, path)) { r->eax = (uint32_t) -1; return; }
            int handle = trust_window_request_delete(path, (uint32_t) cur->pid);
            r->eax = (uint32_t) handle;
            break;
        }

        case SYS_UNLINK_CANCEL: {
            /* ABI: ebx=handle (أرجعه SYS_UNLINK) */
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }
            int rc = trust_window_cancel((int) r->ebx, (uint32_t) cur->pid);
            r->eax = (uint32_t) rc;
            break;
        }

        case SYS_LINK: {
            /* ABI: ebx=new_path ptr, ecx=existing_path ptr. إثبات فصل
               طبقة الحقائق عن الاعتباريات (initrd.h) -- بلا نسخ بايتات،
               فقط رابط جديد في طبقة الأسماء لنفس المحتوى الموجود. */
            uint32_t new_ptr = r->ebx, existing_ptr = r->ecx;
            if (!cur || !cur->is_user) { r->eax = (uint32_t) -1; return; }
            char new_path[MAX_PATH], existing_path[MAX_PATH];
            if (!copy_path_from_user(cur, new_ptr, new_path) ||
                !copy_path_from_user(cur, existing_ptr, existing_path)) {
                r->eax = (uint32_t) -1;
                return;
            }
            int existing_idx = initrd_find(existing_path);
            if (existing_idx < 0) { r->eax = (uint32_t) -1; break; }
            int rc = initrd_link(new_path, existing_idx);
            kprintf("[syscall] pid=%d link('%s' -> '%s') -> %s\n",
                    cur->pid, new_path, existing_path, rc >= 0 ? "OK" : "FAILED");
            r->eax = (uint32_t)(rc >= 0 ? 0 : -1);
            break;
        }

        case SYS_STAT: {
            /* ABI: ebx=pathname ptr, ecx=statbuf ptr (RW) */
            uint32_t path_ptr = r->ebx, buf_ptr = r->ecx;
            if (!cur || !cur->is_user || !ptr_in_bounds(cur, buf_ptr, sizeof(struct al_stat), 1)) {
                r->eax = (uint32_t) -1;
                return;
            }
            char path[MAX_PATH];
            if (!copy_path_from_user(cur, path_ptr, path)) { r->eax = (uint32_t) -1; return; }

            int idx = initrd_find(path);
            if (idx < 0) { r->eax = (uint32_t) -1; break; }

            struct al_stat st;
            fill_stat_for_index(&st, idx);
            memcpy((void*) buf_ptr, &st, sizeof(st));
            r->eax = 0;
            break;
        }

        case SYS_FSTAT: {
            /* ABI: ebx=fd, ecx=statbuf ptr (RW) */
            uint32_t fd = r->ebx, buf_ptr = r->ecx;
            if (!cur || !cur->is_user || !ptr_in_bounds(cur, buf_ptr, sizeof(struct al_stat), 1)) {
                r->eax = (uint32_t) -1;
                return;
            }
            fd_entry_t* e = fd_lookup(cur, fd);
            if (!e) { r->eax = (uint32_t) -1; break; }

            struct al_stat st;
            fill_stat_for_content(&st, e->content_id);
            memcpy((void*) buf_ptr, &st, sizeof(st));
            r->eax = 0;
            break;
        }

        case SYS_LEDGER_READ: {
            /* ABI: ebx=index، ecx=out ptr (RW، حجم ledger_entry_t).
               لا يوجد شرط صلاحية زائد عن صلاحية process عادية -- القراءة
               مفتوحة عمدًا لأي process (انظر ledger.h: "الشفافية جزء من
               المبدأ، وليست استثناءً"). لا توجد حتى case واحدة في هذا الملف
               كله تكتب أو تمسح من السجل -- الكتابة الوحيدة تحدث
               مباشرة من process.c عند أحداث دورة الحياة، وليس عبر أي
               استدعاء نظام يستطيع أي process مناداته. */
            uint32_t index = r->ebx, buf_ptr = r->ecx;
            if (!cur || !cur->is_user || !ptr_in_bounds(cur, buf_ptr, sizeof(ledger_entry_t), 1)) {
                r->eax = (uint32_t) -1;
                return;
            }
            ledger_entry_t e;
            if (ledger_read(index, &e) != 0) { r->eax = (uint32_t) -1; break; }
            memcpy((void*) buf_ptr, &e, sizeof(e));
            r->eax = 0;
            break;
        }

        default:
            kprintf("[syscall] SECURITY: unknown/forbidden syscall %d from pid=%d rejected\n",
                    (int) syscall_no, cur ? cur->pid : -1);
            r->eax = (uint32_t) -1;
    }
}

void syscall_init(void) {
    register_interrupt_handler(128, syscall_handler);
    kprintf("[syscall] int 0x80 gate armed (DPL=3): write, exit, fork, waitpid, getpid, brk, "
            "pipe_create/write/read, open/close/read/lseek/stat/fstat (initrd, read-only)\n");
}
