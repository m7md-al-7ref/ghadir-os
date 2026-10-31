/* ginux_syscall.h -- واجهة syscall بسيطة جدًا لبرامج userland الخاصة بـ Al-Ghadir.
   ABI: eax=رقم النداء, ebx=fd, ecx=ptr, edx=len (مثل write(2) الحقيقية) */

#define AL_STDOUT_FILENO 1

#define AL_SYS_OPEN    9
#define AL_SYS_CLOSE   10
#define AL_SYS_READ    11
#define AL_SYS_EXIT    4
#define AL_SYS_FORK    5
#define AL_SYS_EXECVE  6
#define AL_SYS_WAITPID 7
#define AL_SYS_GETPID  8
#define AL_SYS_LEDGER_READ 30
#define AL_SYS_UNLINK  20

#define AL_O_WRONLY 0x0001
#define AL_O_RDWR   0x0002
#define AL_O_CREAT  0x0040
#define AL_O_TRUNC  0x0200

static inline int ginux_unlink(const char* path) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_UNLINK), "b"(path) : "memory");
    return ret; /* الآن ترجع "handle" (>=0) إذا قُبل الطلب، وليس 0/-1 مباشرة --
                    نافذة الأمانة، انظر trust_window.h. استعمل ginux_unlink_cancel(handle) للإلغاء. */
}

#define AL_SYS_UNLINK_CANCEL 31
#define AL_SYS_LINK 32

static inline int ginux_link(const char* new_path, const char* existing_path) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_LINK), "b"(new_path), "c"(existing_path) : "memory");
    return ret;
}
static inline int ginux_unlink_cancel(int handle) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_UNLINK_CANCEL), "b"(handle) : "memory");
    return ret;
}

static inline int ginux_ledger_read(unsigned int index, void* out /* ledger_entry_t*, 32 bytes */) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_LEDGER_READ), "b"(index), "c"(out) : "memory");
    return ret;
}

static inline int ginux_write(const char* buf, unsigned int len) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(0), "b"(AL_STDOUT_FILENO), "c"(buf), "d"(len) : "memory");
    return ret;
}

static inline int ginux_write_fd(int fd, const char* buf, unsigned int len) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(0), "b"(fd), "c"(buf), "d"(len) : "memory");
    return ret;
}

static inline int ginux_open(const char* path, int flags, int mode) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_OPEN), "b"(path), "c"(flags), "d"(mode) : "memory");
    return ret;
}

static inline int ginux_read(int fd, char* buf, unsigned int len) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_READ), "b"(fd), "c"(buf), "d"(len) : "memory");
    return ret;
}

#define AL_SYS_LSEEK 12
static inline int ginux_lseek(int fd, int offset, int whence) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_LSEEK), "b"(fd), "c"(offset), "d"(whence) : "memory");
    return ret;
}

static inline int ginux_close(int fd) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_CLOSE), "b"(fd) : "memory");
    return ret;
}

static inline int ginux_fork(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_FORK) : "memory");
    return ret;
}

static inline int ginux_execve(const char* path, char* const argv[]) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_EXECVE), "b"(path), "c"(argv) : "memory");
    return ret; /* لا ترجع أبدًا إلا إذا فشلت (مثل execve الحقيقية) */
}

static inline int ginux_waitpid(int pid, int* wstatus, int options) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_WAITPID), "b"(pid), "c"(wstatus), "d"(options) : "memory");
    return ret;
}

static inline void ginux_exit(int status) {
    __asm__ volatile ("int $0x80" :: "a"(AL_SYS_EXIT), "b"(status) : "memory");
    for (;;) __asm__ volatile ("nop"); /* لا يجب أن نصل إلى هنا أبدًا -- hlt محظورة في ring3 */
}

static inline int ginux_getpid(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(AL_SYS_GETPID) : "memory");
    return ret;
}
