/* compattest.c -- يختبر طبقة التوافق بأرقام Linux الحقيقية، بدون أي libc.
   يمثل بالضبط ما ستفعله glibc 2.2 الساكنة عند البدء ثم أثناء printf/malloc/fork. */
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;

static inline i32 sc(i32 n, u32 a, u32 b, u32 c, u32 d, u32 e) {
    i32 r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory");
    return r;
}
/* 6 معاملات (mmap2): لا نستطيع تمرير ebp كـ operand، فنحفظه يدويًا. يتطلب -fomit-frame-pointer */
static inline i32 sc6(i32 n, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) {
    i32 r; u32 sv;
    __asm__ volatile ("movl %%ebp, %[sv]; movl %[f], %%ebp; int $0x80; movl %[sv], %%ebp"
                      : "=a"(r), [sv] "=&m"(sv) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), [f] "m"(f) : "memory");
    return r;
}
#define sys0(n)           sc(n,0,0,0,0,0)
#define sys1(n,a)         sc(n,(u32)(a),0,0,0,0)
#define sys2(n,a,b)       sc(n,(u32)(a),(u32)(b),0,0,0)
#define sys3(n,a,b,c)     sc(n,(u32)(a),(u32)(b),(u32)(c),0,0)

void* memcpy(void* d, const void* s, unsigned n) { char* a = d; const char* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned n) { char* a = d; while (n--) *a++ = (char)c; return d; }

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sys3(4, 1, s, slen(s)); }
static void num(i32 v) {
    char b[16]; int i = 15; b[i] = 0; int neg = v < 0; u32 u = neg ? (u32)-v : (u32)v;
    if (!u) b[--i] = '0';
    while (u) { b[--i] = '0' + (u % 10); u /= 10; }
    if (neg) b[--i] = '-';
    out(b + i);
}
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int fails = 0, passes = 0;
static void check(const char* name, int ok) {
    out(ok ? "[PASS] " : "[FAIL] "); out(name); out("\n");
    if (ok) passes++; else fails++;
}

#define SYS_exit 1
#define SYS_fork 2
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_unlink 10
#define SYS_execve 11
#define SYS_time 13
#define SYS_lseek 19
#define SYS_getpid 20
#define SYS_kill 37
#define SYS_pipe 42
#define SYS_brk 45
#define SYS_ioctl 54
#define SYS_munmap 91
#define SYS_wait4 114
#define SYS_uname 122
#define SYS_mprotect 125
#define SYS_nanosleep 162
#define SYS_mmap2 192
#define SYS_fstat64 197
#define SYS_clone 120
#define SYS_sysinfo 116
#define SYS_chdir 12
#define SYS_rename 38
#define SYS_mkdir 39
#define SYS_rmdir 40
#define SYS_getcwd 183
#define SYS_stat64 195
#define SYS_getdents64 220

static char** g_envp;

static void child_mode(int argc, char** argv, u32* auxv) {
    out("[execd] argc="); num(argc); out(" argv[1]="); out(argv[1]);
    out(" argv[2]="); out(argc > 2 ? argv[2] : "(none)"); out("\n");
    out("[execd] env0="); out(g_envp[0] ? g_envp[0] : "(none)"); out("\n");
    u32 pagesz = 0, entry = 0;
    for (; auxv[0]; auxv += 2) { if (auxv[0] == 6) pagesz = auxv[1]; if (auxv[0] == 9) entry = auxv[1]; }
    out("[execd] AT_PAGESZ="); num((i32)pagesz); out(" AT_ENTRY_ok="); num(entry != 0); out("\n");
    sys1(SYS_exit, 42);
}

void cmain(u32* sp) {
    int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    char** envp = argv + argc + 1;
    g_envp = envp;
    char** e = envp; while (*e) e++;
    u32* auxv = (u32*)(e + 1);

    if (argc >= 2 && seq(argv[1], "execd")) child_mode(argc, argv, auxv);
    if (argc >= 2 && seq(argv[1], "bigmem")) {
        /* يحجز 300MB ويلمس كل صفحة (يختبر وصول النواة لإطارات فوق 128MB عبر identity map) */
        u32 mb = 300;
        if (argc >= 3) { mb = 0; for (const char* c = argv[2]; *c; c++) mb = mb * 10 + (u32)(*c - '0'); }
        u32 sz = mb * 1024u * 1024u;
        out("[bigmem] mapping "); num((i32)mb); out("MB...\n");
        u32 m = (u32)sc6(192, 0, sz, 3, 0x22, (u32)-1, 0);
        out("[bigmem] mmap returned\n");
        if (m >= 0xFFFFF000u) { out("[bigmem] mmap failed: "); num((i32)m); out("\n"); sys1(SYS_exit, 1); }
        for (u32 off = 0; off < sz; off += 4096) *(volatile char*)(m + off) = (char)(off >> 12);
        int bad = 0;
        for (u32 off = 0; off < sz; off += 4096) if (*(volatile char*)(m + off) != (char)(off >> 12)) bad++;
        out("[bigmem] mapped, touched, verified; mismatches="); num(bad); out("\n");
        sys1(SYS_exit, bad ? 2 : 0);
    }

    out("=== compattest: compat ABI on Ginux ===\n");
    check("argc>=1 and argv[0] present", argc >= 1 && argv[0]);
    check("envp has PATH", envp[0] && envp[0][0] == 'P');
    { int pg = 0; for (u32* a = auxv; a[0]; a += 2) if (a[0] == 6) pg = (int)a[1]; check("auxv AT_PAGESZ=4096", pg == 4096); }

    /* ---- errno سالب (ما تفحصه glibc بـ >= 0xfffff001) ---- */
    check("open(nonexistent) == -ENOENT", sys3(SYS_open, "/nope.txt", 0, 0) == -2);
    check("write(bad fd) == -EBADF", sys3(SYS_write, 99, "x", 1) == -9);
    check("write(bad ptr) == -EFAULT", sys3(SYS_write, 1, 0x10, 4) == -14);
    check("unknown syscall == -ENOSYS", sys0(9999) == -38);

    /* ---- ما تفعله glibc عند البدء ---- */
    check("getuid/geteuid/getgid/getegid all 0", sys0(24) == 0 && sys0(49) == 0 && sys0(47) == 0 && sys0(50) == 0);
    { char u[390]; check("uname ok, sysname=Ginux", sys1(SYS_uname, u) == 0 && seq(u, "Ginux")); }
    { char st[96]; int r = sys2(SYS_fstat64, 1, st);
      unsigned mode = *(unsigned*)(st + 16);
      check("fstat64(1): S_IFCHR", r == 0 && (mode & 0170000) == 0020000); }
    { char t[36]; check("ioctl(1,TCGETS)==0 (isatty)", sys3(SYS_ioctl, 1, 0x5401, t) == 0); }
    { u32 now = (u32)sys1(SYS_time, 0); check("time() looks like a real date (>2020)", now > 1577836800u); }

    /* ---- FPU (glibc: __setfpucw/fldcw فور البدء) ---- */
    { volatile double a = 1.5, b = 2.25; volatile double r = a * b; check("x87 FPU works (1.5*2.25=3.375)", (int)(r * 1000) == 3375); }

    /* ---- brk ---- */
    { u32 b0 = (u32)sys1(SYS_brk, 0); u32 b1 = (u32)sys1(SYS_brk, b0 + 3 * 4096 + 17);
      check("brk grows", b1 == b0 + 3 * 4096 + 17);
      char* p = (char*)b0; p[0] = 'A'; p[3 * 4096] = 'Z';
      check("heap memory usable", p[0] == 'A' && p[3 * 4096] == 'Z');
      u32 b2 = (u32)sys1(SYS_brk, b0);
      check("brk shrinks back", b2 == b0); }

    /* ---- mmap anonymous (glibc malloc للطلبات الكبيرة) ---- */
    { u32 m = (u32)sc6(SYS_mmap2, 0, 8192, 3 /*RW*/, 0x22 /*PRIVATE|ANON*/, (u32)-1, 0);
      check("mmap2 anon returns user address", m >= 0x60000000u && m < 0x80000000u);
      char* p = (char*)m; p[0] = 1; p[8191] = 2;
      check("mmap memory is zero-filled & writable", p[1] == 0 && p[0] == 1 && p[8191] == 2);
      check("mprotect RO ok", sys3(SYS_mprotect, m, 4096, 1) == 0);
      check("munmap ok", sys2(SYS_munmap, m, 8192) == 0);
      u32 bad = (u32)sc6(SYS_mmap2, 0, 4096, 7 /*RWX*/, 0x22, (u32)-1, 0);
      check("mmap RWX refused (W^X) == -EACCES", (i32)bad == -13); }

    /* ---- ملفات initrd ---- */
    { int fd = sys3(SYS_open, "/motd.txt", 0, 0);
      check("open(/motd.txt) >= 3", fd >= 3);
      char buf[64]; int n = sys3(SYS_read, fd, buf, 63);
      check("read(motd) > 0", n > 0);
      check("lseek(SEEK_END)>0", sys3(SYS_lseek, fd, 0, 2) > 0);
      check("lseek(SEEK_SET,0)==0", sys3(SYS_lseek, fd, 0, 0) == 0);
      check("close ok", sys1(SYS_close, fd) == 0);
      check("close again == -EBADF", sys1(SYS_close, fd) == -9); }
    { int fd = sys3(SYS_open, "/scratch.txt", 0x242 /*RDWR|CREAT|TRUNC*/, 0644);
      check("create scratch.txt", fd >= 3);
      check("write file", sys3(SYS_write, fd, "ginux!", 6) == 6);
      sys3(SYS_lseek, fd, 0, 0);
      char b[8]; memset(b, 0, 8);
      check("read back file", sys3(SYS_read, fd, b, 6) == 6 && seq(b, "ginux!"));
      sys1(SYS_close, fd); }

    /* ---- مجلدات ---- */
    { check("mkdir(/d1)==0", sys2(SYS_mkdir, "/d1", 0755) == 0);
      check("mkdir(/d1) again == -EEXIST", sys2(SYS_mkdir, "/d1", 0755) == -17);
      check("mkdir(/nope/x) == -ENOENT", sys2(SYS_mkdir, "/nope/x", 0755) == -2);
      check("chdir(/d1)==0", sys1(SYS_chdir, "/d1") == 0);
      { char cwd[64]; int n = sys2(SYS_getcwd, cwd, 64); check("getcwd == /d1 (len incl NUL = 4)", n == 4 && seq(cwd, "/d1")); }
      int fd = sys3(SYS_open, "f.txt", 0x242, 0644);      /* نسبي لـ cwd */
      check("create relative f.txt in cwd", fd >= 3);
      sys3(SYS_write, fd, "hi", 2); sys1(SYS_close, fd);
      check("mkdir(sub) relative", sys2(SYS_mkdir, "sub", 0755) == 0);
      check("stat64(/d1/f.txt) works via absolute path", ({ char st[96]; sys2(SYS_stat64, "/d1/f.txt", st) == 0 && *(u32*)(st + 44) == 2; }));
      check("chdir(sub/..) lands in /d1", sys1(SYS_chdir, "sub/..") == 0 && ({ char c[16]; sys2(SYS_getcwd, c, 16); seq(c, "/d1"); }));
      check("chdir(..)", sys1(SYS_chdir, "..") == 0 && ({ char c[16]; sys2(SYS_getcwd, c, 16); seq(c, "/"); }));
      check("chdir(/nope) == -ENOENT", sys1(SYS_chdir, "/nope") == -2);
      check("chdir(/d1/f.txt) == -ENOTDIR", sys1(SYS_chdir, "/d1/f.txt") == -20);
      { int d = sys3(SYS_open, "/d1", 0, 0);
        check("open(/d1) as directory", d >= 3);
        char buf[512]; int n = sys3(SYS_getdents64, d, buf, 512);
        int seen_dot = 0, seen_dd = 0, seen_f = 0, seen_sub = 0, subdir = 0;
        for (int off = 0; off < n; ) {
            char* nm = buf + off + 19; unsigned short rl = *(unsigned short*)(buf + off + 16); char ty = buf[off + 18];
            if (seq(nm, ".")) seen_dot = 1; else if (seq(nm, "..")) seen_dd = 1;
            else if (seq(nm, "f.txt")) seen_f = (ty == 8); else if (seq(nm, "sub")) { seen_sub = 1; subdir = (ty == 4); }
            off += rl;
        }
        check("getdents64 lists . .. f.txt(file) sub(dir)", n > 0 && seen_dot && seen_dd && seen_f && seen_sub && subdir);
        check("getdents64 returns 0 at end", sys3(SYS_getdents64, d, buf, 512) == 0);
        check("read(dir) == -EISDIR", sys3(SYS_read, d, buf, 4) == -21);
        sys1(SYS_close, d); }
      check("open(/d1,O_WRONLY) == -EISDIR", sys3(SYS_open, "/d1", 1, 0) == -21);
      check("rmdir(/d1) non-empty == -ENOTEMPTY", sys1(SYS_rmdir, "/d1") == -39);
      check("rename(/d1/f.txt,/d1/sub/g.txt)", sys2(SYS_rename, "/d1/f.txt", "/d1/sub/g.txt") == 0);
      check("old name gone (-ENOENT)", sys3(SYS_open, "/d1/f.txt", 0, 0) == -2);
      check("rename directory /d1 -> /d2", sys2(SYS_rename, "/d1", "/d2") == 0);
      { int g = sys3(SYS_open, "/d2/sub/g.txt", 0, 0); char b[4]; int n = sys3(SYS_read, g, b, 4);
        check("file moved with renamed dir, content intact", g >= 3 && n == 2 && b[0] == 'h'); sys1(SYS_close, g); }
      check("unlink(directory) == -EISDIR", sys1(SYS_unlink, "/d2") == -21);
      check("rmdir(/d2/sub) non-empty then ok after file gone", sys1(SYS_rmdir, "/d2/sub") == -39); }

    /* ---- unlink ثم إعادة إنشاء نفس الاسم (نمط ld): نافذة الأمانة يجب ألا تحذف الملف الجديد ---- */
    { int fd = sys3(SYS_open, "/relink.txt", 0x242, 0644); sys3(SYS_write, fd, "AAA", 3); sys1(SYS_close, fd);
      check("unlink(relink.txt) ok", sys1(SYS_unlink, "/relink.txt") == 0);
      fd = sys3(SYS_open, "/relink.txt", 0x242, 0644); sys3(SYS_write, fd, "BBB", 3); sys1(SYS_close, fd);
      u32 ts[2] = { 4, 0 }; sys2(SYS_nanosleep, ts, 0);    /* أطول من نافذة الأمانة (3 ثوانٍ) */
      fd = sys3(SYS_open, "/relink.txt", 0, 0); char b[4] = {0};
      int n = fd >= 3 ? sys3(SYS_read, fd, b, 3) : -1;
      check("re-created file survives the trust window with NEW content", n == 3 && b[0] == 'B');
      sys1(SYS_close, fd); }

    /* ---- fork + wait4 + status ---- */
    { int pid = sys0(SYS_fork);
      if (pid == 0) sys1(SYS_exit, 7);
      int st = 0; int w = sys3(SYS_wait4, pid, &st, 0);
      check("fork+wait4: pid matches", w == pid && pid > 0);
      check("wait4 status == exit(7)<<8", st == (7 << 8)); }
    { int pid = sys0(SYS_fork);
      if (pid == 0) { volatile int* np = (int*)0; *np = 1; sys1(SYS_exit, 0); }
      int st = 0; sys3(SYS_wait4, pid, &st, 0);
      check("child NULL deref killed by SIGSEGV (status==11)", st == 11); }
    { int pid = sys0(SYS_fork);
      if (pid == 0) { sys2(SYS_kill, sys0(SYS_getpid), 6); sys1(SYS_exit, 0); }
      int st = 0; sys3(SYS_wait4, pid, &st, 0);
      check("raise(SIGABRT) terminates (status==6)", st == 6); }
    check("wait4 with no children == -ECHILD", sys3(SYS_wait4, -1, 0, 0) == -10);

    /* ---- pipe بين أب وطفل مع EOF ---- */
    { int fds[2]; check("pipe() ok", sys1(SYS_pipe, fds) == 0 && fds[0] >= 3 && fds[1] >= 3);
      int pid = sys0(SYS_fork);
      if (pid == 0) {
          sys1(SYS_close, fds[0]);
          sys3(SYS_write, fds[1], "hello via pipe", 14);
          sys1(SYS_exit, 0);
      }
      sys1(SYS_close, fds[1]);
      char b[32]; memset(b, 0, 32); int tot = 0, n;
      while ((n = sys3(SYS_read, fds[0], b + tot, 31 - tot)) > 0) tot += n;
      check("pipe data arrives", tot == 14 && seq(b, "hello via pipe"));
      check("pipe EOF (read==0) after writers closed", n == 0);
      sys3(SYS_wait4, pid, 0, 0); sys1(SYS_close, fds[0]); }

    /* ---- nanosleep (blocking بدون spin) ---- */
    { u32 ts[2] = { 0, 50000000 }; u32 t0 = (u32)sys1(SYS_time, 0);
      check("nanosleep(50ms) returns 0", sys2(SYS_nanosleep, ts, 0) == 0); (void)t0; }

    /* ---- stress: 60 fork/wait (يكشف تسرب slots/ذاكرة/kernel stacks) ---- */
    { int okc = 0;
      for (int i = 0; i < 60; i++) {
          int pid = sys0(SYS_fork);
          if (pid == 0) sys1(SYS_exit, i & 0x7f);
          int st = 0;
          if (pid > 0 && sys3(SYS_wait4, pid, &st, 0) == pid && st == ((i & 0x7f) << 8)) okc++;
      }
      check("60 sequential fork/wait all succeed (no leaks)", okc == 60); }


    /* ---- نداءات المكتبات الحديثة ---- */
    { /* TLS: set_thread_area ثم قراءة %gs:0 */
      static u32 tls_block[4] = { 0xC0FFEE42u, 7, 8, 9 };
      u32 ud[4] = { (u32)-1, (u32)tls_block, 0xFFFFF, 0x51 };
      int r0 = sys1(243, ud);
      u32 v = 0; if (r0 == 0) __asm__ volatile ("movl %%gs:0, %0" : "=r"(v));
      check("set_thread_area ok (entry=6)", r0 == 0 && ud[0] == 6);
      check("%gs:0 reads TLS block", v == 0xC0FFEE42u);
      int pid = sys0(SYS_fork);
      if (pid == 0) { u32 w = 0; __asm__ volatile ("movl %%gs:0, %0" : "=r"(w)); sys1(SYS_exit, w == 0xC0FFEE42u ? 5 : 6); }
      int st = 0; sys3(SYS_wait4, pid, &st, 0);
      check("TLS inherited by fork child (via gs)", st == (5 << 8)); }
    { u32 ts[2] = {0,0}; u32 ts2[2] = {0,0};
      check("clock_gettime(REALTIME) ok & >2020", sys2(265, 0, ts) == 0 && ts[0] > 1577836800u);
      sys2(265, 1, ts); sys0(158); sys2(265, 1, ts2);
      check("clock_gettime(MONOTONIC) non-decreasing", ts2[0] > ts[0] || (ts2[0] == ts[0] && ts2[1] >= ts[1])); }
    { u32 ts64[4]; check("clock_gettime64 ok", sys2(403, 0, ts64) == 0 && ts64[0] > 1577836800u && ts64[1] == 0); }
    { unsigned char rb[32]; memset(rb, 0, 32); int r0 = sys3(355, rb, 32, 0); int nz = 0; for (int i = 0; i < 32; i++) nz += rb[i] != 0;
      check("getrandom(32) returns 32 with entropy", r0 == 32 && nz > 8); }
    { u32 lim[4]; check("prlimit64(STACK) cur=8MB", sc(340, 0, 3, 0, (u32)lim, 0) == 0 && lim[0] == 8u * 1024 * 1024); }
    { u32 si[16]; memset(si, 0, 64); check("sysinfo totalram>0 freeram>0", sys1(SYS_sysinfo, si) == 0 && si[4] > 0 && si[5] > 0); }
    { int fd = sys3(295 /*openat*/, (u32)-100, "/motd.txt", 0); check("openat(AT_FDCWD) ok", fd >= 3); if (fd >= 3) sys1(SYS_close, fd);
      char st64[96]; check("fstatat64(AT_FDCWD,/motd.txt)", sc(300, (u32)-100, (u32)"/motd.txt", (u32)st64, 0, 0) == 0);
      { i32 rr = sys3(295, 77, "motd.txt", 0); check("openat(bad dirfd, relative) == -EBADF", rr == -9); }
      check("faccessat(/nope) == -ENOENT", sys3(307, (u32)-100, "/nope", 0) == -2); }
    { /* clone بنمط fork: flags = SIGCHLD|CLONE_CHILD_SETTID، مع كتابة tid في ذاكرة الطفل */
      static volatile int ctid_slot = 0;
      i32 c = sc(SYS_clone, 17 | 0x01000000, 0, 0, 0, (u32)&ctid_slot);
      if (c == 0) sys1(SYS_exit, ctid_slot == sys0(SYS_getpid) ? 11 : 12);
      int st = 0; sys3(SYS_wait4, c, &st, 0);
      check("clone(SIGCHLD|CHILD_SETTID) child sees own tid", c > 0 && st == (11 << 8));
      check("clone(CLONE_THREAD) == -ENOSYS", sc(SYS_clone, 0x10000 | 0x100 | 0x200 | 0x400 | 0x800, 0, 0, 0, 0) == -38); }

    /* ---- execve مع argv/envp/auxv ---- */
    { int pid = sys0(SYS_fork);
      if (pid == 0) {
          char* av[] = { "compattest", "execd", "arg2", 0 };
          char* ev[] = { "FOO=bar", 0 };
          i32 r = sys3(SYS_execve, "/init", av, ev);
          out("execve failed: "); num(r); out("\n"); sys1(SYS_exit, 99);
      }
      int st = 0; sys3(SYS_wait4, pid, &st, 0);
      check("execve'd child exited 42", st == (42 << 8)); }
    check("execve(nonexistent) == -ENOENT", sys3(SYS_execve, "/nope", 0, 0) == -2);

    /* ---- ذاكرة كبيرة (يُقيَّم يدويًا مع qemu -m 512؛ مع الذاكرة الافتراضية قد يفشل بـ ENOMEM وهذا طبيعي) ---- */
    { int pid = sys0(SYS_fork);
      if (pid == 0) { char* av[] = { "compattest", "bigmem", "300", 0 }; char* ev[] = { 0 }; sys3(SYS_execve, "/init", av, ev); sys1(SYS_exit, 99); }
      int st = 0; sys3(SYS_wait4, pid, &st, 0);
      out("[INFO] bigmem child status="); num(st >> 8); out(" (0=ok, 1=ENOMEM)\n"); }

    out("=== compattest done: passes="); num(passes); out(" fails="); num(fails); out(" ===\n");
    sys1(SYS_exit, fails ? 1 : 0);
}

__asm__ (
    ".global _start\n"
    "_start:\n"
    "  mov %esp, %eax\n"
    "  and $-16, %esp\n"
    "  sub $12, %esp\n"
    "  push %eax\n"
    "  call cmain\n"
    "  hlt\n"
);
