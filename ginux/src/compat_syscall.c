#ifndef GHADIR_VERSION
#define GHADIR_VERSION "dev"
#endif

#include "compat_abi.h"
#include "idt.h"
#include "process.h"
#include "paging.h"
#include "pmm.h"
#include "initrd.h"
#include "pipe.h"
#include "pit.h"
#include "io.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "elf.h"
#include "trust_window.h"
#include "rng.h"
#include "gdt.h"

/* ============================================================
 * طبقة syscalls للتوافق -- أرقام Linux i386 وسلوكها (ما تتوقعه glibc 2.x)
 *
 * قواعد عامة:
 *  - القيمة المرجعة: >= 0 نجاح، -errno فشل (يُكتب في eax)
 *  - كل مؤشر من ring3 يُتحقق منه عبر page tables (PRESENT+USER[+WRITE])
 *  - الحجب (blocking) بنمط "yield & retry": إذا لم يكن هناك شيء جاهز نُرجِع eip
 *    2 بايت (حتى تُعاد int $0x80) ونعطي المعالج لـ process أخرى، لأن
 *    النواة ليس فيها sleep queues. eax وباقي الـ registers تبقى محفوظة.
 * ============================================================ */

#define SYS_exit          1
#define SYS_fork          2
#define SYS_read          3
#define SYS_write         4
#define SYS_open          5
#define SYS_close         6
#define SYS_waitpid       7
#define SYS_creat         8
#define SYS_link          9
#define SYS_unlink       10
#define SYS_execve       11
#define SYS_chdir        12
#define SYS_time         13
#define SYS_chmod        15
#define SYS_chown        16
#define SYS_utime        30
#define SYS_rename       38
#define SYS_mkdir        39
#define SYS_rmdir        40
#define SYS_lseek        19
#define SYS_getpid       20
#define SYS_getuid       24
#define SYS_alarm        27
#define SYS_pause        29
#define SYS_access       33
#define SYS_sync         36
#define SYS_kill         37
#define SYS_dup          41
#define SYS_pipe         42
#define SYS_times        43
#define SYS_brk          45
#define SYS_getgid       47
#define SYS_geteuid      49
#define SYS_getegid      50
#define SYS_ioctl        54
#define SYS_fcntl        55
#define SYS_setpgid      57
#define SYS_umask        60
#define SYS_dup2         63
#define SYS_getppid      64
#define SYS_getpgrp      65
#define SYS_setsid       66
#define SYS_sigaction    67
#define SYS_setrlimit    75
#define SYS_getrlimit    76
#define SYS_gettimeofday 78
#define SYS_symlink      83
#define SYS_readlink     85
#define SYS_mmap         90
#define SYS_munmap       91
#define SYS_truncate     92
#define SYS_ftruncate    93
#define SYS_fchmod       94
#define SYS_fchown       95
#define SYS_getrusage    77
#define SYS_stat        106
#define SYS_lstat       107
#define SYS_fstat       108
#define SYS_fsync       118
#define SYS_wait4       114
#define SYS_uname       122
#define SYS_mprotect    125
#define SYS_sigprocmask 126
#define SYS_fchdir      133
#define SYS_llseek      140
#define SYS_getdents    141
#define SYS_flock       143
#define SYS_sched_yield 158
#define SYS_pread64     180
#define SYS_pwrite64    181
#define SYS_readv       145
#define SYS_writev      146
#define SYS_getsid      147
#define SYS_nanosleep   162
#define SYS_rt_sigaction   174
#define SYS_rt_sigprocmask 175
#define SYS_getcwd      183
#define SYS_vfork       190
#define SYS_ftruncate64 194
#define SYS_lchown32    198
#define SYS_fchown32    207
#define SYS_chown32     212
#define SYS_getdents64  220
#define SYS_gettid      224
#define SYS_utimes      271
#define SYS_ugetrlimit  191
#define SYS_mmap2       192
#define SYS_stat64      195
#define SYS_lstat64     196
#define SYS_fstat64     197
#define SYS_getuid32    199
#define SYS_getgid32    200
#define SYS_geteuid32   201
#define SYS_getegid32   202
#define SYS_madvise     219
#define SYS_fcntl64     221
#define SYS_set_tid_address 258
#define SYS_exit_group  252
/* نداءات خاصة بـ Ginux (خارج مدى أرقام Linux المعروفة) */
#define SYS_GINUX_UNLINK_CANCEL 400 /* ebx = handle أرجعه unlink -- نافذة الأمانة */

#define O_ACCMODE  0x3
#define O_CREAT    0x40
#define O_EXCL     0x80
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800
#define O_DIRECTORY 0x10000

#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_IFREG 0100000

#define BLOCK  0x7FFFFFFF   /* sentinel: yield & retry */
#define NOREPLY 0x7FFFFFFE  /* sentinel: لا تكتب eax (exit/execve استبدلا الـ frame) */

#define USER_LO 0x40000000u
#define USER_HI 0x80000000u

/* ---------------- تحقق وحماية مؤشرات المستخدم ---------------- */

static int uok(const process_t* cur, uint32_t ptr, uint32_t len, int write) {
    if (len == 0) return 1;
    uint32_t end = ptr + len;
    if (end < ptr || ptr < USER_LO || end > USER_HI) return 0;
    for (uint32_t va = ptr & ~0xFFFu; va < end; va += PMM_FRAME_SIZE) {
        uint64_t fl = paging_get_flags(cur->as, va);
        if (!(fl & PAGE_PRESENT) || !(fl & PAGE_USER)) return 0;
        if (write && !(fl & PAGE_WRITE)) return 0;
    }
    return 1;
}

/* ينسخ نص C من المستخدم (حتى max-1 بايت). يرجع الطول، أو -errno */
static int ustrcpy(const process_t* cur, uint32_t ptr, char* out, int max) {
    for (int i = 0; i < max; i++) {
        if (i == 0 || ((ptr + (uint32_t) i) & 0xFFF) == 0) {
            if (!uok(cur, ptr + (uint32_t) i, 1, 0)) return -EFAULT;
        }
        out[i] = *(const char*)(ptr + (uint32_t) i);
        if (out[i] == '\0') return i;
    }
    out[max - 1] = '\0';
    return -ENAMETOOLONG;
}

#define MAXP 128      /* أقصى طول مسار يُقبل من المستخدم (ثابت لكل استدعاء) */
#define PATHBUF 192   /* بفر المسار القانوني = INITRD_MAX_NAME */

/* يحوّل مسارًا من المستخدم (مطلق أو نسبي لـ cwd، مع "." و".." و"//") إلى مسار قانوني بلا "/" في البداية
   ("" = الجذر). يرجع 0 أو -errno. out بحجم PATHBUF على الأقل. */
static int resolve_path(const process_t* cur, uint32_t uptr, char* out) {
    char raw[MAXP];
    int n = ustrcpy(cur, uptr, raw, MAXP);
    if (n < 0) return n;
    if (n == 0) return -ENOENT;
    char comb[PATHBUF + MAXP + 2];
    int k = 0;
    if (raw[0] != '/') {
        for (const char* c = cur->cwd; *c && k < PATHBUF; c++) comb[k++] = *c;
        comb[k++] = '/';
    }
    for (const char* q = raw; *q; q++) comb[k++] = *q;
    comb[k] = '\0';
    int o = 0, depth = 0, starts[64];
    const char* q = comb;
    while (*q) {
        while (*q == '/') q++;
        if (!*q) break;
        const char* s = q;
        while (*q && *q != '/') q++;
        int len = (int)(q - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') { if (depth > 0) o = starts[--depth]; continue; }
        if (depth >= 63) return -ENAMETOOLONG;
        starts[depth++] = o;
        if (o > 0) { if (o >= PATHBUF - 1) return -ENAMETOOLONG; out[o++] = '/'; }
        if (o + len >= PATHBUF) return -ENAMETOOLONG;
        memcpy(out + o, s, (size_t) len);
        o += len;
    }
    out[o] = '\0';
    return 0;
}


/* ---------------- الوقت ---------------- */

static uint8_t cmos_read(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

static uint32_t rtc_unix_time(void) {
    while (cmos_read(0x0A) & 0x80) { }
    uint8_t s = cmos_read(0), m = cmos_read(2), h = cmos_read(4);
    uint8_t d = cmos_read(7), mo = cmos_read(8), y = cmos_read(9), b = cmos_read(0x0B);
    int pm = (h & 0x80) && !(b & 2);
    h &= 0x7F;
    if (!(b & 4)) { s = bcd2bin(s); m = bcd2bin(m); h = bcd2bin(h); d = bcd2bin(d); mo = bcd2bin(mo); y = bcd2bin(y); }
    if (!(b & 2)) { if (pm) h = (uint8_t)((h % 12) + 12); else if (h == 12) h = 0; }
    int32_t year = 2000 + y;
    /* days_from_civil (Howard Hinnant) */
    int32_t yy = year - (mo <= 2);
    int32_t era = (yy >= 0 ? yy : yy - 399) / 400;
    uint32_t yoe = (uint32_t)(yy - era * 400);
    uint32_t doy = (uint32_t)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int32_t days = era * 146097 + (int32_t) doe - 719468;
    return (uint32_t) days * 86400u + h * 3600u + m * 60u + s;
}

static uint32_t g_wall_base = 0, g_tick_base = 0;
static int g_clock_ready = 0;

static void clock_now(uint32_t* sec, uint32_t* usec) {
    if (!g_clock_ready) { g_wall_base = rtc_unix_time(); g_tick_base = pit_get_ticks(); g_clock_ready = 1; }
    uint32_t dt = pit_get_ticks() - g_tick_base; /* 100Hz */
    *sec = g_wall_base + dt / 100;
    *usec = (dt % 100) * 10000;
}

/* ---------------- جدول المجلدات المفتوحة (LFD_DIR) ---------------- */

#define MAX_DIR_HANDLES 64
static char g_dir_path[MAX_DIR_HANDLES][PATHBUF];
static int  g_dir_ref[MAX_DIR_HANDLES];

static int dir_handle_new(const char* path) {
    for (int i = 0; i < MAX_DIR_HANDLES; i++) {
        if (g_dir_ref[i] == 0) {
            size_t n = kstrlen(path);
            memcpy(g_dir_path[i], path, n + 1);
            g_dir_ref[i] = 1;
            return i;
        }
    }
    return -1;
}
void compat_dir_ref(int id)   { if (id >= 0 && id < MAX_DIR_HANDLES && g_dir_ref[id] > 0) g_dir_ref[id]++; }
void compat_dir_unref(int id) { if (id >= 0 && id < MAX_DIR_HANDLES && g_dir_ref[id] > 0) g_dir_ref[id]--; }

/* ---------------- fd table ---------------- */

static lfd_t* lfd_get(process_t* cur, int fd) {
    if (fd < 0 || fd >= LFD_MAX || cur->lfds[fd].type == LFD_NONE) return NULL;
    return &cur->lfds[fd];
}

static int lfd_alloc_from(process_t* cur, int minfd) {
    for (int i = minfd; i < LFD_MAX; i++) if (cur->lfds[i].type == LFD_NONE) return i;
    return -1;
}

static void lfd_release(lfd_t* e) {
    if (e->type == LFD_FILE) initrd_close_content(e->content_id);
    else if (e->type == LFD_PIPE_R) pipe_unref(e->pipe_id, 0);
    else if (e->type == LFD_PIPE_W) pipe_unref(e->pipe_id, 1);
    else if (e->type == LFD_DIR) compat_dir_unref(e->content_id);
    e->type = LFD_NONE;
}

static void lfd_dup_into(process_t* cur, int newfd, const lfd_t* src) {
    cur->lfds[newfd] = *src;
    cur->lfds[newfd].cloexec = 0;
    if (src->type == LFD_FILE) initrd_ref_content(src->content_id);
    else if (src->type == LFD_PIPE_R) pipe_ref(src->pipe_id, 0);
    else if (src->type == LFD_PIPE_W) pipe_ref(src->pipe_id, 1);
    else if (src->type == LFD_DIR) compat_dir_ref(src->content_id);
}

/* ---------------- open/read/write/lseek ---------------- */

static int path_is_dir(const char* path) {
    return path[0] == '\0' || kstrcmp(path, "dev") == 0 || initrd_is_dir(path);
}

/* 0 إذا أب المسار مجلد موجود، وإلا -ENOENT/-ENOTDIR */
static int parent_dir_ok(const char* path) {
    int k = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') k = i;
    if (k < 0) return 0;
    char parent[PATHBUF];
    memcpy(parent, path, (size_t) k);
    parent[k] = '\0';
    if (path_is_dir(parent)) return 0;
    return initrd_find(parent) >= 0 ? -ENOTDIR : -ENOENT;
}

static int32_t initrd_err(int rc) {
    switch (rc) {
    case -2: return -EEXIST;
    case -3: return -ENOENT;
    case -4: return -ENOTDIR;
    case -5: return -39; /* ENOTEMPTY */
    case -6: return -EINVAL;
    case -7: return -EISDIR;
    default: return -ENOMEM;
    }
}

static int32_t do_open(process_t* cur, uint32_t path_ptr, uint32_t flags) {
    char path[PATHBUF];
    int n = resolve_path(cur, path_ptr, path);
    if (n < 0) return n;

    int fd = lfd_alloc_from(cur, 0);
    if (fd < 0) return -EMFILE;
    lfd_t* e = &cur->lfds[fd];
    memset(e, 0, sizeof(*e));
    e->flags = flags;

    if (kstrcmp(path, "dev/tty") == 0 || kstrcmp(path, "dev/console") == 0) { e->type = LFD_CONSOLE; return fd; }
    if (kstrcmp(path, "dev/null") == 0) { e->type = LFD_NULL; return fd; }

    int want_write = (flags & O_ACCMODE) != 0;
    int idx = initrd_find(path);

    /* مجلد (صريح أو ضمني أو الجذر): نفتحه كمجلد للقراءة فقط */
    if (path_is_dir(path) && !(idx >= 0 && !initrd_is_dir(path))) {
        if (want_write || (flags & O_CREAT && flags & O_EXCL)) return (flags & O_CREAT && flags & O_EXCL) ? -EEXIST : -EISDIR;
        int h = dir_handle_new(path);
        if (h < 0) return -ENFILE;
        e->type = LFD_DIR;
        e->content_id = h;
        return fd;
    }
    if (flags & O_DIRECTORY) return idx >= 0 ? -ENOTDIR : -ENOENT;

    if (idx < 0) {
        if (!(flags & O_CREAT)) return -ENOENT;
        int pc = parent_dir_ok(path);
        if (pc < 0) return pc;
        if (kstrcmp(path, "dev") == 0 || (path[0]=='d' && path[1]=='e' && path[2]=='v' && path[3]=='/')) return -EACCES;
        idx = initrd_create(path);
        if (idx < 0) return -ENOMEM;
    } else {
        if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
        if ((flags & O_TRUNC) && want_write) initrd_truncate(idx, 0);
    }
    /* إعادة إنشاء الاسم/تفريغه تلغي أي حذف معلّق عليه (انظر trust_window.h) */
    if (flags & (O_CREAT | O_TRUNC)) trust_window_cancel_path(path);
    int cid = initrd_open_by_index(idx);
    if (cid < 0) return -EIO;
    e->type = LFD_FILE;
    e->content_id = cid;
    return fd;
}

static int32_t do_read(process_t* cur, int fd, uint32_t ptr, uint32_t len) {
    lfd_t* e = lfd_get(cur, fd);
    if (!e) return -EBADF;
    if (!uok(cur, ptr, len, 1)) return -EFAULT;
    if (len == 0) return 0;
    switch (e->type) {
    case LFD_CONSOLE: {
        int n = tty_read((char*) ptr, len);
        if (n > 0) return n;
        if (n < 0) return 0; /* EOF (Ctrl-D) */
        return (e->flags & O_NONBLOCK) ? -EAGAIN : BLOCK;
    }
    case LFD_NULL: return 0;
    case LFD_DIR: return -EISDIR;
    case LFD_FILE: {
        if ((e->flags & O_ACCMODE) == 1) return -EBADF; /* O_WRONLY */
        int32_t n = initrd_read_content(e->content_id, (void*) ptr, e->pos, len);
        if (n < 0) return -EIO;
        e->pos += (uint32_t) n;
        return n;
    }
    case LFD_PIPE_R: {
        int n = pipe_read_ex(e->pipe_id, (void*) ptr, len);
        if (n >= 0) return n;
        return (e->flags & O_NONBLOCK) ? -EAGAIN : BLOCK;
    }
    default: return -EBADF;
    }
}

static int32_t do_write(process_t* cur, int fd, uint32_t ptr, uint32_t len) {
    lfd_t* e = lfd_get(cur, fd);
    if (!e) return -EBADF;
    if (!uok(cur, ptr, len, 0)) return -EFAULT;
    if (len == 0) return 0;
    switch (e->type) {
    case LFD_CONSOLE: console_write((const char*) ptr, len); return (int32_t) len;
    case LFD_NULL: return (int32_t) len;
    case LFD_FILE: {
        if ((e->flags & O_ACCMODE) == 0) return -EBADF;
        if (e->flags & O_APPEND) { int sz = initrd_content_size_of(e->content_id); if (sz >= 0) e->pos = (uint32_t) sz; }
        int32_t n = initrd_write_content(e->content_id, (const void*) ptr, e->pos, len);
        if (n < 0) return -ENOMEM;
        e->pos += (uint32_t) n;
        return n;
    }
    case LFD_PIPE_W: {
        int n = pipe_write_ex(e->pipe_id, (const void*) ptr, len);
        if (n > 0) return n;
        if (n == -2) return -EPIPE;
        return (e->flags & O_NONBLOCK) ? -EAGAIN : BLOCK;
    }
    default: return -EBADF;
    }
}

static int64_t do_seek(process_t* cur, int fd, int64_t off, uint32_t whence) {
    lfd_t* e = lfd_get(cur, fd);
    if (!e) return -EBADF;
    if (e->type != LFD_FILE) return -ESPIPE;
    int csize = initrd_content_size_of(e->content_id);
    if (csize < 0) return -EBADF;
    int64_t base = (whence == 0) ? 0 : (whence == 1) ? (int64_t) e->pos : (whence == 2) ? (int64_t) csize : -1;
    if (base < 0) return -EINVAL;
    int64_t np = base + off;
    if (np < 0) return -EINVAL;
    e->pos = (uint32_t) np;
    return np;
}

/* ---------------- stat ---------------- */

struct kstat_old {
    uint16_t st_dev, pad1; uint32_t st_ino; uint16_t st_mode, st_nlink, st_uid, st_gid, st_rdev, pad2;
    uint32_t st_size, st_blksize, st_blocks, st_atime, u1, st_mtime, u2, st_ctime, u3, u4, u5;
} __attribute__((packed));

struct kstat64 {
    uint64_t st_dev; uint32_t pad1, st_ino32; uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev; uint32_t pad2; int64_t st_size; uint32_t st_blksize; uint64_t st_blocks;
    uint32_t st_atime, st_atime_ns, st_mtime, st_mtime_ns, st_ctime, st_ctime_ns; uint64_t st_ino;
} __attribute__((packed));

_Static_assert(sizeof(struct kstat_old) == 64, "old stat must be 64 bytes");
_Static_assert(sizeof(struct kstat64) == 96, "stat64 must be 96 bytes");

typedef struct { uint32_t mode, size, ino, rdev; } kinfo_t;

static void info_for_content(kinfo_t* k, int content_id, int idx) {
    int sz = initrd_content_size_of(content_id);
    k->mode = S_IFREG | 0644;
    k->size = sz < 0 ? 0 : (uint32_t) sz;
    k->ino = 100 + (uint32_t) (idx < 0 ? content_id : idx);
    k->rdev = 0;
}

/* رقم inode ثابت وفريد لكل مجلد (من مساره الموحَّد). مهم: cpp في GCC يقارن st_dev/st_ino لمجلدات
   -I ليحذف المكرر؛ لو أرجعنا نفس الرقم لكل المجلدات (كما كان) يظن أن /gccinc و/dietinc مجلد واحد
   فيتجاهل الثاني ويفشل في إيجاد stdio.h. */
static uint32_t dir_ino_of(const char* path) {
    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (const char* c = path; *c; c++) { h ^= (uint8_t) *c; h *= 16777619u; }
    return 0x10000000u | (h & 0x0FFFFFFFu);         /* بعيد عن أرقام الملفات (100+) */
}

static int32_t info_for_fd(process_t* cur, int fd, kinfo_t* k) {
    lfd_t* e = lfd_get(cur, fd);
    if (!e) return -EBADF;
    memset(k, 0, sizeof(*k));
    switch (e->type) {
    case LFD_CONSOLE: k->mode = S_IFCHR | 0620; k->rdev = 0x0501; k->ino = 1; break;
    case LFD_NULL:    k->mode = S_IFCHR | 0666; k->rdev = 0x0103; k->ino = 2; break;
    case LFD_PIPE_R: case LFD_PIPE_W: k->mode = S_IFIFO | 0600; k->ino = 3; break;
    case LFD_FILE:    info_for_content(k, e->content_id, -1); break;
    case LFD_DIR:     k->mode = S_IFDIR | 0755; k->ino = dir_ino_of(g_dir_path[e->content_id]); break;
    default: return -EBADF;
    }
    return 0;
}

static int32_t info_for_path(process_t* cur, uint32_t ptr, kinfo_t* k) {
    char path[PATHBUF];
    int n = resolve_path(cur, ptr, path);
    if (n < 0) return n;
    memset(k, 0, sizeof(*k));
    if (kstrcmp(path, "dev/tty") == 0 || kstrcmp(path, "dev/console") == 0) { k->mode = S_IFCHR | 0620; k->rdev = 0x0501; return 0; }
    if (kstrcmp(path, "dev/null") == 0) { k->mode = S_IFCHR | 0666; k->rdev = 0x0103; return 0; }
    int idx = initrd_find(path);
    if (idx >= 0 && initrd_content_id(idx) >= 0) { info_for_content(k, initrd_content_id(idx), idx); return 0; }
    if (path_is_dir(path)) { k->mode = S_IFDIR | 0755; k->ino = dir_ino_of(path); return 0; }
    return -ENOENT;
}

static int32_t put_stat(process_t* cur, uint32_t buf, const kinfo_t* k, int is64) {
    uint32_t now, us; clock_now(&now, &us);
    if (!is64) {
        struct kstat_old st; memset(&st, 0, sizeof(st));
        if (!uok(cur, buf, sizeof(st), 1)) return -EFAULT;
        st.st_ino = k->ino; st.st_mode = (uint16_t) k->mode; st.st_nlink = 1;
        st.st_rdev = (uint16_t) k->rdev; st.st_size = k->size; st.st_blksize = 4096;
        st.st_blocks = (k->size + 511) / 512; st.st_atime = st.st_mtime = st.st_ctime = now;
        memcpy((void*) buf, &st, sizeof(st));
    } else {
        struct kstat64 st; memset(&st, 0, sizeof(st));
        if (!uok(cur, buf, sizeof(st), 1)) return -EFAULT;
        st.st_ino32 = k->ino; st.st_ino = k->ino; st.st_mode = k->mode; st.st_nlink = 1;
        st.st_rdev = k->rdev; st.st_size = k->size; st.st_blksize = 4096;
        st.st_blocks = (k->size + 511) / 512; st.st_atime = st.st_mtime = st.st_ctime = now;
        memcpy((void*) buf, &st, sizeof(st));
    }
    return 0;
}

/* ---------------- ذاكرة: brk / mmap / munmap / mprotect ---------------- */

static int32_t do_brk(process_t* cur, uint32_t req) {
    if (req == 0 || req < cur->brk_base || req > USER_HEAP_MAX) return (int32_t) cur->brk_current;
    uint32_t old_top = (cur->brk_current + 0xFFF) & ~0xFFFu;
    uint32_t new_top = (req + 0xFFF) & ~0xFFFu;
    if (new_top > old_top) {
        uint32_t va;
        for (va = old_top; va < new_top; va += PMM_FRAME_SIZE) {
            uint32_t phys = pmm_alloc_frame();
            if (!phys) break;
            memset((void*) phys, 0, PMM_FRAME_SIZE);
            if (!paging_map(cur->as, va, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX)) { pmm_free_frame(phys); break; }
        }
        if (va < new_top) { /* فشل جزئي: أعد ما خصصناه */
            for (uint32_t u = old_top; u < va; u += PMM_FRAME_SIZE) {
                uint32_t ph = paging_get_phys(cur->as, u);
                paging_unmap(cur->as, u); if (ph) pmm_free_frame(ph);
            }
            return (int32_t) cur->brk_current;
        }
    } else if (new_top < old_top) {
        for (uint32_t va = new_top; va < old_top; va += PMM_FRAME_SIZE) {
            uint32_t ph = paging_get_phys(cur->as, va);
            if (ph) { paging_unmap(cur->as, va); pmm_free_frame(ph); }
        }
    }
    cur->brk_current = req;
    return (int32_t) req;
}

#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_FIXED  0x10
#define MAP_ANON   0x20

static uint64_t prot_to_flags(uint32_t prot) {
    uint64_t f = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_WRITE) f |= PAGE_WRITE;
    if (!(prot & PROT_EXEC)) f |= PAGE_NX;
    return f;
}

static int32_t do_mmap(process_t* cur, uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int fd, uint32_t off) {
    (void) addr;
    if (len == 0) return -EINVAL;
    if (flags & MAP_FIXED) return -EINVAL;               /* غير مدعوم بعد */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -EACCES; /* سياسة W^X */
    uint32_t size = (len + 0xFFF) & ~0xFFFu;
    if (size < len) return -ENOMEM;

    lfd_t* e = NULL;
    if (!(flags & MAP_ANON)) {
        e = lfd_get(cur, fd);
        if (!e) return -EBADF;
        if (e->type != LFD_FILE) return -ENODEV;
    }
    if (cur->mmap_cur + size > USER_MMAP_MAX || cur->mmap_cur + size < cur->mmap_cur) return -ENOMEM;

    uint32_t base = cur->mmap_cur;
    uint64_t pf = prot_to_flags(prot);
    for (uint32_t va = base; va < base + size; va += PMM_FRAME_SIZE) {
        uint32_t phys = pmm_alloc_frame();
        int ok = phys != 0;
        if (ok) { memset((void*) phys, 0, PMM_FRAME_SIZE); ok = paging_map(cur->as, va, phys, pf); if (!ok) pmm_free_frame(phys); }
        if (!ok) {
            for (uint32_t u = base; u < va; u += PMM_FRAME_SIZE) {
                uint32_t ph = paging_get_phys(cur->as, u);
                paging_unmap(cur->as, u); if (ph) pmm_free_frame(ph);
            }
            return -ENOMEM;
        }
        if (e) { /* نسخ خاص (MAP_PRIVATE) من محتوى الملف */
            uint32_t phys2 = paging_get_phys(cur->as, va);
            initrd_read_content(e->content_id, (void*) phys2, off + (va - base), PMM_FRAME_SIZE);
        }
    }
    cur->mmap_cur = base + size;
    return (int32_t) base;
}

static int32_t do_munmap(process_t* cur, uint32_t addr, uint32_t len) {
    if ((addr & 0xFFF) || len == 0) return -EINVAL;
    uint32_t end = (addr + len + 0xFFF) & ~0xFFFu;
    if (addr < cur->mmap_base || end > cur->mmap_cur || end < addr) return -EINVAL;
    for (uint32_t va = addr; va < end; va += PMM_FRAME_SIZE) {
        uint32_t ph = paging_get_phys(cur->as, va);
        if (ph) { paging_unmap(cur->as, va); pmm_free_frame(ph); }
    }
    if (end == cur->mmap_cur) cur->mmap_cur = addr;
    return 0;
}

static int32_t do_mprotect(process_t* cur, uint32_t addr, uint32_t len, uint32_t prot) {
    if (addr & 0xFFF) return -EINVAL;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -EACCES;
    uint32_t end = (addr + len + 0xFFF) & ~0xFFFu;
    if (addr < USER_LO || end > USER_HI || end < addr) return -ENOMEM;
    for (uint32_t va = addr; va < end; va += PMM_FRAME_SIZE) {
        uint32_t ph = paging_get_phys(cur->as, va);
        if (!ph) return -ENOMEM;
        paging_unmap(cur->as, va);
        if (!paging_map(cur->as, va, ph, prot_to_flags(prot))) return -ENOMEM;
    }
    return 0;
}

/* ---------------- fork / exec / wait ---------------- */

static int32_t do_execve(process_t* cur, struct registers* r, uint32_t path_ptr, uint32_t argv_ptr, uint32_t envp_ptr) {
    char np[PATHBUF];
    int n = resolve_path(cur, path_ptr, np);
    if (n < 0) return n;
    int idx = initrd_find(np);
    if (idx < 0) return path_is_dir(np) ? -EACCES : -ENOENT;
    const initrd_file_t* file = initrd_get(idx);
    if (!file) return -ENOENT;
    const void* data = file->data; uint32_t size = file->size;

    compat_args_t* a = compat_args_new();
    if (!a) return -ENOMEM;
    int rc = 0;
    char tmp[256];
    for (int pass = 0; pass < 2 && rc == 0; pass++) {
        uint32_t base = pass == 0 ? argv_ptr : envp_ptr;
        if (base == 0) continue;
        for (int i = 0; ; i++) {
            if (!uok(cur, base + (uint32_t) i * 4, 4, 0)) { rc = -EFAULT; break; }
            uint32_t sp = *(uint32_t*)(base + (uint32_t) i * 4);
            if (sp == 0) break;
            int l = ustrcpy(cur, sp, tmp, sizeof(tmp));
            if (l < 0) { rc = (l == -ENAMETOOLONG) ? -E2BIG : l; break; }
            if (!(pass == 0 ? compat_args_add_arg(a, tmp) : compat_args_add_env(a, tmp))) { rc = -E2BIG; break; }
        }
    }
    if (rc == 0 && a->argc == 0) compat_args_add_arg(a, np); /* argv[0] لا يكون فارغًا أبدًا */
    if (rc == 0) {
        if (process_execve_compat(cur, data, size, np, r, a) != 0) rc = -ENOEXEC;
    }
    compat_args_free(a);
    return rc == 0 ? NOREPLY : rc;
}

static int32_t do_wait(process_t* cur, int32_t pid, uint32_t status_ptr, uint32_t options) {
    int status = 0;
    int rc = process_waitpid(cur->pid, pid > 0 ? pid : -1, &status);
    if (rc > 0) {
        if (status_ptr) {
            if (!uok(cur, status_ptr, 4, 1)) return rc; /* الطفل حُصد فعلًا */
            uint32_t ws = (status >= 0x10000) ? (uint32_t)(status & 0x7F) : (uint32_t)((status & 0xFF) << 8);
            *(uint32_t*) status_ptr = ws;
        }
        return rc;
    }
    if (rc < 0) return -ECHILD;
    if (options & 1 /* WNOHANG */) return 0;
    return BLOCK;
}

/* ---------------- ioctl / uname / متفرقات ---------------- */

struct ktermios { uint32_t iflag, oflag, cflag, lflag; uint8_t line; uint8_t cc[19]; } __attribute__((packed));

static int32_t do_ioctl(process_t* cur, int fd, uint32_t cmd, uint32_t arg) {
    lfd_t* e = lfd_get(cur, fd);
    if (!e) return -EBADF;
    if (e->type != LFD_CONSOLE) return -ENOTTY;
    switch (cmd) {
    case 0x5401: { /* TCGETS */
        if (!uok(cur, arg, sizeof(struct ktermios), 1)) return -EFAULT;
        struct ktermios t; memset(&t, 0, sizeof(t));
        t.iflag = 0x0500; /* ICRNL|IXON */
        t.oflag = 0x0005; /* OPOST|ONLCR */
        t.cflag = 0x00BF; /* CS8|CREAD|B38400 */
        t.lflag = 0x8A3B; /* ISIG|ICANON|ECHO|ECHOE|ECHOK|ECHOCTL|ECHOKE|IEXTEN */
        t.cc[0] = 3; t.cc[1] = 28; t.cc[2] = 127; t.cc[3] = 21; t.cc[4] = 4; t.cc[6] = 1;
        memcpy((void*) arg, &t, sizeof(t));
        return 0;
    }
    case 0x5402: case 0x5403: case 0x5404: return 0; /* TCSETS* -- نتجاهلها */
    case 0x5413: { /* TIOCGWINSZ */
        if (!uok(cur, arg, 8, 1)) return -EFAULT;
        uint16_t ws[4] = { 25, 80, 0, 0 };
        memcpy((void*) arg, ws, 8);
        return 0;
    }
    case 0x540F: return tty_foreground_pid() > 0 ? tty_foreground_pid() : cur->pid;  /* TIOCGPGRP */
    case 0x5410: {  /* TIOCSPGRP: الـ shell يحدد العملية الأمامية (هدف Ctrl-C) */
        if (!uok(cur, arg, 4, 0)) return -EFAULT;
        tty_set_fg(*(int32_t*) arg);
        return 0;
    }
    case 0x541B: if (uok(cur, arg, 4, 1)) *(uint32_t*) arg = 0; return 0; /* FIONREAD */
    default: return -ENOTTY;
    }
}

static int32_t do_fcntl(process_t* cur, int fd, uint32_t cmd, uint32_t arg) {
    lfd_t* e = lfd_get(cur, fd);
    if (!e) return -EBADF;
    switch (cmd) {
    case 0: { int nf = lfd_alloc_from(cur, (int) arg); if (nf < 0) return -EMFILE; lfd_dup_into(cur, nf, e); return nf; }
    case 1: return e->cloexec;
    case 2: e->cloexec = (uint8_t)(arg & 1); return 0;
    case 3: return (int32_t) e->flags;
    case 4: e->flags = (e->flags & ~(O_NONBLOCK | O_APPEND)) | (arg & (O_NONBLOCK | O_APPEND)); return 0;
    default: return -EINVAL;
    }
}

static int32_t do_uname(process_t* cur, uint32_t buf) {
    if (!uok(cur, buf, 6 * 65, 1)) return -EFAULT;
    char* u = (char*) buf;
    memset(u, 0, 6 * 65);
    const char* f[5] = { "Ginux", "ginux", "5.4.0", "#1 Ghadir OS " GHADIR_VERSION, "i686" };
    for (int i = 0; i < 5; i++) { size_t l = kstrlen(f[i]); memcpy(u + i * 65, f[i], l); }
    return 0;
}

static int32_t do_getrlimit(process_t* cur, uint32_t res, uint32_t ptr, uint32_t inf) {
    if (!uok(cur, ptr, 8, 1)) return -EFAULT;
    uint32_t* v = (uint32_t*) ptr;
    v[0] = v[1] = inf;
    if (res == 3) v[0] = 8u * 1024 * 1024;           /* RLIMIT_STACK */
    if (res == 7) { v[0] = v[1] = LFD_MAX; }           /* RLIMIT_NOFILE */
    return 0;
}

static int is_ignored_sig(uint32_t sig) { return sig == 17 || sig == 18 || sig == 23 || sig == 28; }

/* getdents / getdents64: كل entry تُكتب كما في Linux i386.
   cookie المجلد في lfd.pos: 0 = ".", 1 = "..", >=2 = (cookie لـ initrd_list)+2 */
static int32_t do_getdents(process_t* cur, int fd, uint32_t buf, uint32_t count, int is64) {
    lfd_t* e = lfd_get(cur, fd);
    if (!e) return -EBADF;
    if (e->type != LFD_DIR) return -ENOTDIR;
    if (!uok(cur, buf, count, 1)) return -EFAULT;
    const char* dpath = g_dir_path[e->content_id];
    uint32_t written = 0;
    for (;;) {
        char name[PATHBUF];
        int isdir = 1;
        uint32_t next;
        if (e->pos == 0) { name[0] = '.'; name[1] = '\0'; next = 1; }
        else if (e->pos == 1) { name[0] = '.'; name[1] = '.'; name[2] = '\0'; next = 2; }
        else {
            int c = initrd_list(dpath, (int)(e->pos - 2), name, (int) sizeof(name), &isdir);
            if (c == 0) break;
            next = (uint32_t) c + 2;
        }
        uint32_t nl = (uint32_t) kstrlen(name);
        uint32_t reclen = is64 ? ((19 + nl + 1 + 7) & ~7u) : ((10 + nl + 1 + 1 + 3) & ~3u);
        if (written + reclen > count) { if (written == 0) return -EINVAL; break; }
        uint8_t* rec = (uint8_t*)(buf + written);
        memset(rec, 0, reclen);
        uint32_t ino = 1000 + e->pos;
        uint8_t dtype = isdir ? 4 : 8; /* DT_DIR / DT_REG */
        if (is64) {
            *(uint32_t*)(rec + 0) = ino;
            *(uint32_t*)(rec + 8) = next;
            *(uint16_t*)(rec + 16) = (uint16_t) reclen;
            rec[18] = dtype;
            memcpy(rec + 19, name, nl + 1);
        } else {
            *(uint32_t*)(rec + 0) = ino;
            *(uint32_t*)(rec + 4) = next;
            *(uint16_t*)(rec + 8) = (uint16_t) reclen;
            memcpy(rec + 10, name, nl + 1);
            rec[reclen - 1] = dtype;
        }
        written += reclen;
        e->pos = next;
    }
    return (int32_t) written;
}

/* ---------------- dispatcher ---------------- */



/* struct statx (256 بايت). glibc الحديثة تفضّلها لـ fstat/stat؛ ونعبّئ الحقول الأساسية فقط (STATX_BASIC_STATS) */
static int32_t put_statx(process_t* cur, uint32_t buf, const kinfo_t* k) {
    if (!uok(cur, buf, 256, 1)) return -EFAULT;
    uint32_t now, us; clock_now(&now, &us);
    uint8_t* b = (uint8_t*) buf; memset(b, 0, 256);
    *(uint32_t*)(b + 0)  = 0x7FF;                 /* stx_mask = STATX_BASIC_STATS */
    *(uint32_t*)(b + 4)  = 4096;                  /* stx_blksize */
    *(uint32_t*)(b + 16) = 1;                     /* stx_nlink */
    *(uint16_t*)(b + 28) = (uint16_t) k->mode;    /* stx_mode */
    *(uint64_t*)(b + 32) = k->ino;                /* stx_ino */
    *(uint64_t*)(b + 40) = k->size;               /* stx_size */
    *(uint64_t*)(b + 48) = (k->size + 511) / 512; /* stx_blocks */
    for (int off = 64; off <= 112; off += 16) { *(uint32_t*)(b + off) = now; } /* atime/btime/ctime/mtime (sec) */
    *(uint32_t*)(b + 128) = (k->rdev >> 8) & 0xFF; /* stx_rdev_major */
    *(uint32_t*)(b + 132) = k->rdev & 0xFF;        /* stx_rdev_minor */
    return 0;
}

/* ---------------- نداءات المكتبات الحديثة (TLS، الساعات، *at، clone) ---------------- */

#define AT_FDCWD_V (-100)
#define SYS_openat       295
#define SYS_mkdirat      296
#define SYS_fchownat     298
#define SYS_fstatat64    300
#define SYS_unlinkat     301
#define SYS_renameat     302
#define SYS_linkat       303
#define SYS_symlinkat    304
#define SYS_readlinkat   305
#define SYS_fchmodat     306
#define SYS_faccessat    307
#define SYS_clock_nanosleep 267
#define SYS_tkill        238
#define SYS_tgkill       270
#define SYS_set_thread_area 243
#define SYS_get_thread_area 244
#define SYS_clock_gettime 265
#define SYS_clock_getres  266
#define SYS_clock_gettime64 403
#define SYS_getrandom    355
#define SYS_prlimit64    340
#define SYS_set_robust_list 311
#define SYS_futex        240
#define SYS_sysinfo      116
#define SYS_sched_getaffinity 242
#define SYS_sigaltstack  186
#define SYS_rt_sigpending 176
#define SYS_setitimer    104
#define SYS_getitimer    105
#define SYS_getpriority  96
#define SYS_setpriority  97
#define SYS_nice         34
#define SYS_fdatasync    148
#define SYS_fadvise64_64 272
#define SYS_getpgid      132
#define SYS_mremap       163
#define SYS_clone        120
#define SYS_pipe2        331
#define SYS_dup3         330
#define SYS_statx        383

/* *at(): ندعم AT_FDCWD أو مسارًا مطلقًا فقط (dirfd حقيقي لمجلد مفتوح: غير مدعوم بعد) */
static int at_dir_ok(const process_t* cur, int32_t dirfd, uint32_t path_ptr) {
    if (dirfd == AT_FDCWD_V) return 1;
    return uok(cur, path_ptr, 1, 0) && *(const char*) path_ptr == '/';
}

/* الساعات: 0=REALTIME، والباقي (MONOTONIC، BOOTTIME، CPUTIME…) نقيسها بـ ticks منذ الإقلاع */
static void clk_get(uint32_t clk, uint32_t* sec, uint32_t* nsec) {
    if (clk == 0 || clk == 5 || clk == 8) { uint32_t us; clock_now(sec, &us); *nsec = us * 1000u; }
    else { uint32_t t = pit_get_ticks(); *sec = t / 100u; *nsec = (t % 100u) * 10000000u; }
}

static int32_t do_clock_gettime(process_t* cur, uint32_t clk, uint32_t ptr, int is64) {
    if (clk > 11) return -EINVAL;
    uint32_t s, ns; clk_get(clk, &s, &ns);
    if (is64) { if (!uok(cur, ptr, 16, 1)) return -EFAULT; uint32_t* o = (uint32_t*) ptr; o[0] = s; o[1] = 0; o[2] = ns; o[3] = 0; }
    else      { if (!uok(cur, ptr, 8, 1))  return -EFAULT; uint32_t* o = (uint32_t*) ptr; o[0] = s; o[1] = ns; }
    return 0;
}

static int32_t do_sysinfo(process_t* cur, uint32_t ptr) {
    if (!uok(cur, ptr, 64, 1)) return -EFAULT;
    uint32_t* o = (uint32_t*) ptr; memset(o, 0, 64);
    o[0] = pit_get_ticks() / 100u;                         /* uptime */
    o[4] = pmm_total_frame_count() * PMM_FRAME_SIZE;       /* totalram */
    o[5] = pmm_free_frame_count() * PMM_FRAME_SIZE;        /* freeram */
    ((uint16_t*) o)[(10 * 4) / 2] = 1;                     /* procs (تقريبي) */
    o[13] = 1;                                             /* mem_unit */
    return 0;
}

static int32_t do_prlimit64(process_t* cur, uint32_t res, uint32_t old) {
    if (!old) return 0;
    if (!uok(cur, old, 16, 1)) return -EFAULT;
    uint64_t* v = (uint64_t*) old;
    v[0] = v[1] = ~0ULL;
    if (res == 3) v[0] = 8ULL * 1024 * 1024;   /* RLIMIT_STACK */
    if (res == 7) v[0] = v[1] = LFD_MAX;       /* RLIMIT_NOFILE */
    return 0;
}

/* clone بنمط fork فقط: threads (CLONE_THREAD/FILES/SIGHAND…) غير مدعومة. CLONE_VM|CLONE_VFORK
   (posix_spawn/vfork في المكتبات الحديثة) تُنفَّذ كـ fork كامل (نسخة)، وهو سلوك صحيح للطفل الذي
   ينفّذ exec أو exit مباشرة. */
static int32_t do_clone(process_t* cur, struct registers* r, uint32_t flags, uint32_t newsp,
                        uint32_t ptid, uint32_t tls, uint32_t ctid) {
    const uint32_t ALLOWED = 0xFF | 0x100 | 0x4000 | 0x80000 | 0x100000 | 0x200000 | 0x1000000;
    if (flags & ~ALLOWED) return -ENOSYS;
    if ((flags & 0x100) && !(flags & 0x4000)) return -ENOSYS;   /* CLONE_VM وحدها = thread */
    int c = process_fork(cur, r);
    if (c < 0) return -ENOMEM;
    process_t* ch = process_by_pid(c);
    if (ch && newsp) ch->regs.useresp = newsp;
    if ((flags & 0x100000) && uok(cur, ptid, 4, 1)) *(int32_t*) ptid = c;
    if (ch && (flags & 0x1000000) && uok(cur, ctid, 4, 1)) {
        uint32_t ph = paging_get_phys(ch->as, ctid & ~0xFFFu);
        if (ph) *(int32_t*)(ph + (ctid & 0xFFF)) = c;
    }
    if (ch && (flags & 0x80000) && uok(cur, tls, 16, 0)) { ch->tls_base = ((uint32_t*) tls)[1]; ch->tls_valid = 1; }
    return c;
}

/* آخر مقبض حذف طلبته كل عملية (نافذة الأمانة): -1 = لا يوجد */
static int g_last_unlink[MAX_TASKS] = { [0 ... MAX_TASKS - 1] = -1 };

void compat_syscall(struct registers* r) {
    process_t* cur = process_current();
    int32_t ret = -ENOSYS;
    uint32_t a1 = r->ebx, a2 = r->ecx, a3 = r->edx, a4 = r->esi, a5 = r->edi, a6 = r->ebp;
    uint32_t nr = r->eax;   /* r->eax نفسها لا تُمَس: إعادة المحاولة (BLOCK) تحتاج رقم النداء الأصلي */

    /* ترجمة عائلة *at() والنداءات المكافئة إلى النداء الأساسي (مع إزاحة الوسائط) */
    {
        int bad = 0;
        switch (nr) {
        case SYS_openat:     bad = !at_dir_ok(cur, (int32_t) a1, a2); nr = SYS_open;  a1 = a2; a2 = a3; a3 = a4; break;
        case SYS_mkdirat:    bad = !at_dir_ok(cur, (int32_t) a1, a2); nr = SYS_mkdir; a1 = a2; a2 = a3; break;
        case SYS_fchownat:   bad = !at_dir_ok(cur, (int32_t) a1, a2); nr = SYS_chown; a1 = a2; a2 = a3; a3 = a4; break;
        case SYS_fstatat64:
            if ((a4 & 0x1000 /* AT_EMPTY_PATH */) && uok(cur, a2, 1, 0) && *(const char*) a2 == '\0') { nr = SYS_fstat64; a2 = a3; break; }
            bad = !at_dir_ok(cur, (int32_t) a1, a2); nr = (a4 & 0x100) ? SYS_lstat64 : SYS_stat64; a1 = a2; a2 = a3; break;
        case SYS_unlinkat:   bad = !at_dir_ok(cur, (int32_t) a1, a2); nr = (a3 & 0x200) ? SYS_rmdir : SYS_unlink; a1 = a2; break;
        case SYS_renameat:   bad = !(at_dir_ok(cur, (int32_t) a1, a2) && at_dir_ok(cur, (int32_t) a3, a4)); nr = SYS_rename; a1 = a2; a2 = a4; break;
        case SYS_linkat:     bad = !(at_dir_ok(cur, (int32_t) a1, a2) && at_dir_ok(cur, (int32_t) a3, a4)); nr = SYS_link; a1 = a2; a2 = a4; break;
        case SYS_symlinkat:  bad = !at_dir_ok(cur, (int32_t) a2, a3); nr = SYS_symlink; a2 = a3; break;
        case SYS_readlinkat: bad = !at_dir_ok(cur, (int32_t) a1, a2); nr = SYS_readlink; a1 = a2; a2 = a3; a3 = a4; break;
        case SYS_fchmodat:   bad = !at_dir_ok(cur, (int32_t) a1, a2); nr = SYS_chmod; a1 = a2; a2 = a3; break;
        case SYS_faccessat:  bad = !at_dir_ok(cur, (int32_t) a1, a2); nr = SYS_access; a1 = a2; a2 = a3; break;
        case SYS_clock_nanosleep: nr = SYS_nanosleep; a1 = a3; a2 = a4; break;
        case SYS_tgkill:     nr = SYS_kill; a1 = a2; a2 = a3; break;
        case SYS_tkill:      nr = SYS_kill; break;
        default: break;
        }
        if (bad) { r->eax = (uint32_t) -EBADF; return; }
    }

    switch (nr) {
    case SYS_exit: case SYS_exit_group:
        cur->exit_status = (int) (a1 & 0xFF);
        process_kill_current_and_reschedule(r);
        return;
    case SYS_fork: case SYS_vfork: {
        int c = process_fork(cur, r);
        ret = c < 0 ? -ENOMEM : c;
        break;
    }
    case SYS_read:  ret = do_read(cur, (int) a1, a2, a3); break;
    case SYS_write: ret = do_write(cur, (int) a1, a2, a3); break;
    case SYS_open:  ret = do_open(cur, a1, a2); break;
    case SYS_close: {
        lfd_t* e = lfd_get(cur, (int) a1);
        if (!e) { ret = -EBADF; break; }
        lfd_release(e); ret = 0; break;
    }
    case SYS_waitpid: ret = do_wait(cur, (int32_t) a1, a2, a3); break;
    case SYS_wait4:   ret = do_wait(cur, (int32_t) a1, a2, a3); break;
    case SYS_link: {
        char op[PATHBUF], np2[PATHBUF];
        int x = resolve_path(cur, a1, op), y = resolve_path(cur, a2, np2);
        if (x < 0) { ret = x; break; }
        if (y < 0) { ret = y; break; }
        int idx = initrd_find(op);
        if (idx < 0 || initrd_content_id(idx) < 0) { ret = idx < 0 ? -ENOENT : -EPERM; break; }
        int pc = parent_dir_ok(np2);
        if (pc < 0) { ret = pc; break; }
        ret = initrd_link(np2, idx) >= 0 ? 0 : -EEXIST;
        break;
    }
    case SYS_unlink: {
        /* نافذة الأمانة: الحذف يتأجل (TRUST_WINDOW_TICKS) ويمكن إلغاؤه عبر
           نداء Ginux 400. من ناحية التوافق: النداء "نجح" (0). */
        char p[PATHBUF];
        int x = resolve_path(cur, a1, p);
        if (x < 0) { ret = x; break; }
        int ui = initrd_find(p);
        if (ui < 0 || initrd_content_id(ui) < 0) { ret = path_is_dir(p) ? -EISDIR : -ENOENT; break; }
        int h = trust_window_request_delete(p, (uint32_t) cur->pid);
        if (h >= 0 && cur->pid >= 0 && cur->pid < MAX_TASKS) g_last_unlink[cur->pid] = h;   /* لـ undo */
        ret = h >= 0 ? 0 : -EBUSY;
        break;
    }
    case SYS_mkdir: {
        char p[PATHBUF];
        int x = resolve_path(cur, a1, p);
        if (x < 0) { ret = x; break; }
        if (kstrcmp(p, "dev") == 0) { ret = -EEXIST; break; }
        int rc = initrd_mkdir(p);
        ret = rc >= 0 ? 0 : initrd_err(rc);
        break;
    }
    case SYS_rmdir: {
        char p[PATHBUF];
        int x = resolve_path(cur, a1, p);
        if (x < 0) { ret = x; break; }
        if (kstrcmp(p, "dev") == 0) { ret = -EBUSY; break; }
        int rc = initrd_rmdir(p);
        ret = rc >= 0 ? 0 : initrd_err(rc);
        break;
    }
    case SYS_rename: {
        char op[PATHBUF], np2[PATHBUF];
        int x = resolve_path(cur, a1, op), y = resolve_path(cur, a2, np2);
        if (x < 0) { ret = x; break; }
        if (y < 0) { ret = y; break; }
        int rc = initrd_rename(op, np2);
        if (rc >= 0) trust_window_cancel_path(np2); /* الاسم الهدف صار ملفًا جديدًا */
        ret = rc >= 0 ? 0 : initrd_err(rc);
        break;
    }
    case SYS_symlink: ret = -EPERM; break; /* لا روابط رمزية بعد */
    case SYS_truncate: case SYS_ftruncate: case SYS_ftruncate64: {
        int cid = -1;
        if (nr == SYS_truncate) {
            char p[PATHBUF]; int x = resolve_path(cur, a1, p);
            if (x < 0) { ret = x; break; }
            int ti = initrd_find(p);
            if (ti < 0) { ret = path_is_dir(p) ? -EISDIR : -ENOENT; break; }
            cid = initrd_content_id(ti);
        } else {
            lfd_t* e = lfd_get(cur, (int) a1);
            if (!e) { ret = -EBADF; break; }
            if (e->type != LFD_FILE) { ret = -EINVAL; break; }
            if ((e->flags & O_ACCMODE) == 0) { ret = -EINVAL; break; }
            cid = e->content_id;
        }
        ret = initrd_truncate_content(cid, nr == SYS_truncate ? a2 : a2) == 0 ? 0 : -ENOMEM;
        break;
    }
    case SYS_creat: ret = do_open(cur, a1, O_CREAT | 1 /* O_WRONLY */ | O_TRUNC); break;
    case SYS_getdents: case SYS_getdents64: ret = do_getdents(cur, (int) a1, a2, a3, nr == SYS_getdents64); break;
    case SYS_fchdir: {
        lfd_t* e = lfd_get(cur, (int) a1);
        if (!e) { ret = -EBADF; break; }
        if (e->type != LFD_DIR) { ret = -ENOTDIR; break; }
        size_t pl = kstrlen(g_dir_path[e->content_id]);
        memcpy(cur->cwd, g_dir_path[e->content_id], pl < sizeof(cur->cwd) ? pl + 1 : 0);
        ret = pl < sizeof(cur->cwd) ? 0 : -ENAMETOOLONG;
        break;
    }
    case SYS_chown: case SYS_chown32: case SYS_lchown32: case SYS_fchown: case SYS_fchown32:
    case SYS_flock: case SYS_utime: case SYS_utimes: ret = 0; break; /* لا نموذج مستخدمين/أزمنة ملفات بعد */
    case SYS_sched_yield: r->eax = 0; schedule(r); return;
    case SYS_gettid: ret = cur->pid; break;
    case SYS_getrusage: {
        if (!uok(cur, a2, 72, 1)) { ret = -EFAULT; break; }
        memset((void*) a2, 0, 72); ret = 0; break;
    }
    case SYS_pread64: case SYS_pwrite64: {
        lfd_t* e = lfd_get(cur, (int) a1);
        if (!e) { ret = -EBADF; break; }
        if (e->type != LFD_FILE) { ret = -ESPIPE; break; }
        uint32_t saved = e->pos;
        e->pos = a4;                      /* offset_low (offset_high يُهمل: الملفات < 4GB) */
        ret = nr == SYS_pread64 ? do_read(cur, (int) a1, a2, a3) : do_write(cur, (int) a1, a2, a3);
        e->pos = saved;
        break;
    }
    case SYS_GINUX_UNLINK_CANCEL: {
        /* a1 = handle أرجعه unlink، أو -1 = آخر حذف طلبته هذه العملية (يستعمله الأمر undo في الـ shell) */
        int h = (int) a1;
        if (a1 == 0xFFFFFFFFu && cur->pid >= 0 && cur->pid < MAX_TASKS) { h = g_last_unlink[cur->pid]; g_last_unlink[cur->pid] = -1; }
        ret = (h >= 0 && trust_window_cancel(h, (uint32_t) cur->pid) == 0) ? 0 : -EINVAL;
        break;
    }
    case SYS_execve: ret = do_execve(cur, r, a1, a2, a3); break;
    case SYS_chdir: {
        char p[PATHBUF];
        int x = resolve_path(cur, a1, p);
        if (x < 0) { ret = x; break; }
        if (!path_is_dir(p)) { ret = initrd_find(p) >= 0 ? -ENOTDIR : -ENOENT; break; }
        size_t pl = kstrlen(p);
        if (pl >= sizeof(cur->cwd)) { ret = -ENAMETOOLONG; break; }
        memcpy(cur->cwd, p, pl + 1);
        ret = 0;
        break;
    }
    case SYS_time: {
        uint32_t s, u; clock_now(&s, &u);
        if (a1) { if (!uok(cur, a1, 4, 1)) { ret = -EFAULT; break; } *(uint32_t*) a1 = s; }
        ret = (int32_t) s; break;
    }
    case SYS_gettimeofday: {
        uint32_t s, u; clock_now(&s, &u);
        if (a1) { if (!uok(cur, a1, 8, 1)) { ret = -EFAULT; break; } ((uint32_t*) a1)[0] = s; ((uint32_t*) a1)[1] = u; }
        ret = 0; break;
    }
    case SYS_times: {
        if (a1) { if (!uok(cur, a1, 16, 1)) { ret = -EFAULT; break; } memset((void*) a1, 0, 16); }
        ret = (int32_t) pit_get_ticks(); break;
    }
    case SYS_nanosleep: {
        if (!uok(cur, a1, 8, 0)) { ret = -EFAULT; break; }
        uint32_t now = pit_get_ticks();
        if (cur->wake_tick == 0) {
            const uint32_t* ts = (const uint32_t*) a1;
            uint32_t t = ts[0] * 100 + ts[1] / 10000000u;
            cur->wake_tick = now + (t ? t : 1);
        }
        if ((int32_t)(now - cur->wake_tick) >= 0) { cur->wake_tick = 0; ret = 0; }
        else ret = BLOCK;
        break;
    }
    case SYS_lseek: {
        int64_t v = do_seek(cur, (int) a1, (int32_t) a2, a3);
        ret = v < 0 ? (int32_t) v : (int32_t) v; break;
    }
    case SYS_llseek: {
        int64_t off = ((int64_t) a2 << 32) | a3;
        int64_t v = do_seek(cur, (int) a1, off, a5);
        if (v < 0) { ret = (int32_t) v; break; }
        if (!uok(cur, a4, 8, 1)) { ret = -EFAULT; break; }
        *(int64_t*) a4 = v; ret = 0; break;
    }
    case SYS_getpid: ret = cur->pid; break;
    case SYS_getppid: ret = cur->parent_pid < 0 ? 0 : cur->parent_pid; break;
    case SYS_getuid: case SYS_getgid: case SYS_geteuid: case SYS_getegid:
    case SYS_getuid32: case SYS_getgid32: case SYS_geteuid32: case SYS_getegid32:
        ret = 0; break; /* كل شيء root، لا يوجد نموذج مستخدمين بعد */
    case SYS_getpgrp: case SYS_getsid: case SYS_setsid: case SYS_setpgid: ret = cur->pid; break;
    case SYS_umask: ret = 022; break;
    case SYS_alarm: ret = 0; break;
    case SYS_pause: ret = BLOCK; break;
    case SYS_sync: case SYS_fsync: case SYS_chmod: case SYS_fchmod: case SYS_madvise: case SYS_setrlimit: ret = 0; break;
    case SYS_set_tid_address: ret = cur->pid; break;
    case SYS_access: {
        kinfo_t k;
        ret = info_for_path(cur, a1, &k);
        break;
    }
    case SYS_readlink: ret = -EINVAL; break;
    case SYS_getcwd: {
        size_t cl = kstrlen(cur->cwd);
        if (a2 < cl + 2) { ret = -ERANGE; break; }
        if (!uok(cur, a1, (uint32_t)(cl + 2), 1)) { ret = -EFAULT; break; }
        ((char*) a1)[0] = '/';
        memcpy((char*) a1 + 1, cur->cwd, cl + 1);
        ret = (int32_t)(cl + 2);
        break;
    }
    case SYS_brk: ret = do_brk(cur, a1); break;
    case SYS_mmap: { /* old_mmap: ebx -> struct { addr,len,prot,flags,fd,offset } */
        if (!uok(cur, a1, 24, 0)) { ret = -EFAULT; break; }
        const uint32_t* m = (const uint32_t*) a1;
        ret = do_mmap(cur, m[0], m[1], m[2], m[3], (int32_t) m[4], m[5]); break;
    }
    case SYS_mmap2: ret = do_mmap(cur, a1, a2, a3, a4, (int32_t) a5, a6 * 4096u); break;
    case SYS_munmap: ret = do_munmap(cur, a1, a2); break;
    case SYS_mprotect: ret = do_mprotect(cur, a1, a2, a3); break;
    case SYS_stat: case SYS_lstat: case SYS_stat64: case SYS_lstat64: {
        kinfo_t k; int32_t x = info_for_path(cur, a1, &k);
        ret = x < 0 ? x : put_stat(cur, a2, &k, nr == SYS_stat64 || nr == SYS_lstat64);
        break;
    }
    case SYS_fstat: case SYS_fstat64: {
        kinfo_t k; int32_t x = info_for_fd(cur, (int) a1, &k);
        ret = x < 0 ? x : put_stat(cur, a2, &k, nr == SYS_fstat64);
        break;
    }
    case SYS_ioctl: ret = do_ioctl(cur, (int) a1, a2, a3); break;
    case SYS_fcntl: case SYS_fcntl64: ret = do_fcntl(cur, (int) a1, a2, a3); break;
    case SYS_dup: {
        lfd_t* e = lfd_get(cur, (int) a1);
        if (!e) { ret = -EBADF; break; }
        int nf = lfd_alloc_from(cur, 0);
        if (nf < 0) { ret = -EMFILE; break; }
        lfd_dup_into(cur, nf, e); ret = nf; break;
    }
    case SYS_dup3: {
        lfd_t* e = lfd_get(cur, (int) a1);
        if (!e) { ret = -EBADF; break; }
        if (a2 >= LFD_MAX) { ret = -EBADF; break; }
        if (a1 == a2) { ret = -EINVAL; break; }                    /* dup3 يرفض old==new (بخلاف dup2) */
        if (a3 & ~0x2000000u) { ret = -EINVAL; break; }
        if (cur->lfds[a2].type != LFD_NONE) lfd_release(&cur->lfds[a2]);
        lfd_dup_into(cur, (int) a2, e);
        if (a3 & 0x2000000u) cur->lfds[a2].cloexec = 1;
        ret = (int32_t) a2; break;
    }
    case SYS_dup2: {
        lfd_t* e = lfd_get(cur, (int) a1);
        if (!e) { ret = -EBADF; break; }
        if (a2 >= LFD_MAX) { ret = -EBADF; break; }
        if (a1 == a2) { ret = (int32_t) a2; break; }
        if (cur->lfds[a2].type != LFD_NONE) lfd_release(&cur->lfds[a2]);
        lfd_dup_into(cur, (int) a2, e); ret = (int32_t) a2; break;
    }
    case SYS_pipe: case SYS_pipe2: {
        if (!uok(cur, a1, 8, 1)) { ret = -EFAULT; break; }
        int rfd = lfd_alloc_from(cur, 0);
        if (rfd < 0) { ret = -EMFILE; break; }
        cur->lfds[rfd].type = LFD_PIPE_R; /* نحجز مؤقتًا حتى لا يعطينا alloc نفس الرقم */
        int wfd = lfd_alloc_from(cur, 0);
        if (wfd < 0) { cur->lfds[rfd].type = LFD_NONE; ret = -EMFILE; break; }
        int id = pipe_create();
        if (id < 0) { cur->lfds[rfd].type = LFD_NONE; ret = -ENFILE; break; }
        memset(&cur->lfds[rfd], 0, sizeof(lfd_t)); memset(&cur->lfds[wfd], 0, sizeof(lfd_t));
        cur->lfds[rfd].type = LFD_PIPE_R; cur->lfds[rfd].pipe_id = id;
        cur->lfds[wfd].type = LFD_PIPE_W; cur->lfds[wfd].pipe_id = id; cur->lfds[wfd].flags = 1;
        if (nr == SYS_pipe2) {
            if (a2 & ~(0x2000000u | O_NONBLOCK)) { /* أعلام غير مدعومة */
                lfd_release(&cur->lfds[rfd]); lfd_release(&cur->lfds[wfd]); ret = -EINVAL; break;
            }
            if (a2 & 0x2000000u) { cur->lfds[rfd].cloexec = 1; cur->lfds[wfd].cloexec = 1; }
            if (a2 & O_NONBLOCK) { cur->lfds[rfd].flags |= O_NONBLOCK; cur->lfds[wfd].flags |= O_NONBLOCK; }
        }
        ((int32_t*) a1)[0] = rfd; ((int32_t*) a1)[1] = wfd;
        ret = 0; break;
    }
    case SYS_readv: case SYS_writev: {
        if (a3 > 16 || !uok(cur, a2, a3 * 8, 0)) { ret = a3 > 16 ? -EINVAL : -EFAULT; break; }
        const uint32_t* iov = (const uint32_t*) a2;
        int32_t total = 0; ret = 0;
        for (uint32_t i = 0; i < a3; i++) {
            int32_t x = (nr == SYS_readv) ? do_read(cur, (int) a1, iov[i * 2], iov[i * 2 + 1])
                                              : do_write(cur, (int) a1, iov[i * 2], iov[i * 2 + 1]);
            if (x == BLOCK) { ret = total > 0 ? total : BLOCK; goto iov_done; }
            if (x < 0) { ret = total > 0 ? total : x; goto iov_done; }
            total += x;
            if ((uint32_t) x < iov[i * 2 + 1]) break;
        }
        ret = total;
iov_done:
        break;
    }
    case SYS_uname: ret = do_uname(cur, a1); break;
    case SYS_getrlimit: ret = do_getrlimit(cur, a1, a2, 0x7FFFFFFFu); break;
    case SYS_ugetrlimit: ret = do_getrlimit(cur, a1, a2, 0xFFFFFFFFu); break;
    case SYS_sigaction: case SYS_rt_sigaction: {
        /* لا يوجد توصيل signals بعد: نسجّل "نجاحًا" ونعيد الـ action القديمة صفرية */
        uint32_t oact = (nr == SYS_sigaction) ? a3 : a3, sz = (nr == SYS_sigaction) ? 16 : 20;
        if (oact) { if (!uok(cur, oact, sz, 1)) { ret = -EFAULT; break; } memset((void*) oact, 0, sz); }
        ret = 0; break;
    }
    case SYS_sigprocmask: case SYS_rt_sigprocmask: {
        uint32_t sz = (nr == SYS_sigprocmask) ? 4 : 8;
        if (a3) { if (!uok(cur, a3, sz, 1)) { ret = -EFAULT; break; } memset((void*) a3, 0, sz); }
        ret = 0; break;
    }
    case SYS_kill: {
        int32_t pid = (int32_t) a1; uint32_t sig = a2;
        if (sig > 64) { ret = -EINVAL; break; }
        if (pid == cur->pid || pid == 0) {
            if (sig == 0 || is_ignored_sig(sig)) { ret = 0; break; }
            cur->exit_status = 0x10000 | (int) sig; /* قتل ذاتي: يظهر للأب كـ signal */
            process_kill_current_and_reschedule(r);
            return;
        }
        process_t* t = process_by_pid(pid);
        if (!t) { ret = -ESRCH; break; }
        ret = sig == 0 ? 0 : -EPERM; /* إرسال signals لعمليات أخرى غير مدعوم بعد */
        break;
    }
    case SYS_set_thread_area: {
        if (!uok(cur, a1, 16, 1)) { ret = -EFAULT; break; }
        uint32_t* ud = (uint32_t*) a1;
        int32_t en = (int32_t) ud[0];
        if (en != -1 && en != 6) { ret = -EINVAL; break; }   /* مدخل TLS واحد فقط (رقم 6) */
        process_set_tls(cur, ud[1]);
        ud[0] = 6; ret = 0; break;
    }
    case SYS_get_thread_area: {
        if (!uok(cur, a1, 16, 1)) { ret = -EFAULT; break; }
        if (!cur->tls_valid) { ret = -EINVAL; break; }
        uint32_t* ud = (uint32_t*) a1; ud[0] = 6; ud[1] = cur->tls_base; ud[2] = 0xFFFFF; ud[3] = 0x51;
        ret = 0; break;
    }
    case SYS_clock_gettime:   ret = do_clock_gettime(cur, a1, a2, 0); break;
    case SYS_clock_gettime64: ret = do_clock_gettime(cur, a1, a2, 1); break;
    case SYS_clock_getres: {
        if (a2) { if (!uok(cur, a2, 8, 1)) { ret = -EFAULT; break; } ((uint32_t*) a2)[0] = 0; ((uint32_t*) a2)[1] = 10000000; }
        ret = 0; break;
    }
    case SYS_getrandom: {
        if (!uok(cur, a1, a2, 1)) { ret = -EFAULT; break; }
        uint8_t* o = (uint8_t*) a1;
        for (uint32_t i = 0; i < a2; ) { uint32_t v = rng_next_u32(); for (int k = 0; k < 4 && i < a2; k++, i++) o[i] = (uint8_t)(v >> (8 * k)); }
        ret = (int32_t) a2; break;
    }
    case SYS_prlimit64: ret = do_prlimit64(cur, a2, a4); break;
    case SYS_set_robust_list: ret = 0; break;
    case SYS_futex: {
        /* عملية واحدة بدون threads: WAKE لا يوقظ أحدًا، وWAIT لا يمكن أن يُوقَظ أبدًا */
        uint32_t op = a2 & 0x7F;
        if (op == 1 || op == 10) { ret = 0; break; }
        if (op == 0 || op == 9) {
            if (!uok(cur, a1, 4, 0)) { ret = -EFAULT; break; }
            ret = (*(volatile uint32_t*) a1 != a3) ? -EAGAIN : (a4 ? -110 /* ETIMEDOUT */ : -EAGAIN);
            break;
        }
        ret = -ENOSYS; break;
    }
    case SYS_sysinfo: ret = do_sysinfo(cur, a1); break;
    case SYS_sched_getaffinity:
        if (a2 < 4) { ret = -EINVAL; break; }
        if (!uok(cur, a3, 4, 1)) { ret = -EFAULT; break; }
        *(uint32_t*) a3 = 1; ret = 4; break;
    case SYS_sigaltstack: if (a2) { if (!uok(cur, a2, 12, 1)) { ret = -EFAULT; break; } memset((void*) a2, 0, 12); } ret = 0; break;
    case SYS_rt_sigpending: if (!uok(cur, a1, 8, 1)) { ret = -EFAULT; break; } memset((void*) a1, 0, 8); ret = 0; break;
    case SYS_setitimer: if (a3) { if (!uok(cur, a3, 16, 1)) { ret = -EFAULT; break; } memset((void*) a3, 0, 16); } ret = 0; break;
    case SYS_getitimer: if (!uok(cur, a2, 16, 1)) { ret = -EFAULT; break; } memset((void*) a2, 0, 16); ret = 0; break;
    case SYS_getpriority: ret = 20; break;
    case SYS_setpriority: case SYS_nice: case SYS_fdatasync: case SYS_fadvise64_64: ret = 0; break;
    case SYS_getpgid: ret = cur->pid; break;
    case SYS_mremap: ret = -ENOMEM; break;   /* المكتبات تعود لـ malloc+copy+free عند الفشل */
    case SYS_clone: ret = do_clone(cur, r, a1, a2, a3, a4, a5); break;
    case SYS_statx: {
        kinfo_t k; int32_t x;
        if ((a3 & 0x1000) && uok(cur, a2, 1, 0) && *(const char*) a2 == '\0') x = info_for_fd(cur, (int) a1, &k);
        else if (!at_dir_ok(cur, (int32_t) a1, a2)) x = -EBADF;
        else x = info_for_path(cur, a2, &k);
        ret = x < 0 ? x : put_statx(cur, a5, &k);
        break;
    }
    default:
        kprintf("[compat] pid=%d unimplemented syscall %d (a1=%x a2=%x a3=%x) -> ENOSYS\n",
                cur->pid, (int) nr, a1, a2, a3);
        ret = -ENOSYS;
        break;
    }

    if (ret == NOREPLY) return;
    if (ret == BLOCK) {
        cur->yielding = 1;
        r->eip -= 2;       /* int $0x80 = 2 بايت: تُعاد عند الجدولة القادمة */
        schedule(r);
        return;
    }
    r->eax = (uint32_t) ret;
}
