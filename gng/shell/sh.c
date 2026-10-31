/* sh.c -- shell تفاعلي لـ Ghadir OS (يعمل فوق النواة ginux، مبني بـ glibc ساكنة 32-bit).
 *
 * المدعوم: أنابيب (|)، تحويل (< > >>)، تسلسل (; && ||)، اقتباس ('...' "...")، متغيرات ($VAR ${VAR} $? $$)،
 *          ~ و * ? في آخر مكوّن من المسار، أوامر داخلية، وتشغيل برامج خارجية (PATH)، وسكربتات (sh file / source file).
 * غير مدعوم: jobs بالخلفية (&)، تاريخ الأوامر وأسهم التحرير (الـ tty في النواة canonical فقط)، if/for/while، aliases.
 * كل المخرجات بالإنجليزية لأن شاشة VGA النصية لا تعرض العربية. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>

extern char** environ;

#ifndef GHADIR_VERSION
#define GHADIR_VERSION "dev"
#endif

#define MAXTOK   256
#define MAXARG   64
#define MAXPIPE  8
#define WORDMAX  1024
#define SYS_GINUX_UNLINK_CANCEL 400   /* نداء خاص بالنواة: يلغي آخر unlink (نافذة الأمانة) */
#define TIOCSPGRP_ 0x5410             /* يحدد للـ tty من يُقتل بـ Ctrl-C (سالب = أبناء هذه العملية فقط) */

static int last_status = 0;
static int interactive = 0;

static void eprint(const char* s) { size_t n = strlen(s); if (write(2, s, n) < 0) {} }
static void eprintf_(const char* a, const char* b) { eprint("sh: "); eprint(a); if (b) { eprint(": "); eprint(b); } eprint("\n"); }
static char* xstrdup(const char* s) { char* d = strdup(s); if (!d) { eprint("sh: out of memory\n"); _exit(1); } return d; }

/* ======================= التحليل ======================= */
typedef enum { T_WORD, T_PIPE, T_SEMI, T_AND, T_OR, T_IN, T_OUT, T_APP } ttype;
typedef struct { ttype t; char* s; int glob; } tok_t;
static tok_t toks[MAXTOK];
static int ntok;

static void add_tok(ttype t, const char* s, int glob) {
    toks[ntok].t = t; toks[ntok].s = s ? xstrdup(s) : NULL; toks[ntok].glob = glob; ntok++;
}
static void free_toks(void) { for (int i = 0; i < ntok; i++) free(toks[i].s); ntok = 0; }

/* يوسّع $... عند p (يشير إلى '$') ويضيف القيمة إلى w. يرجع الطول الجديد */
static int expand_var(const char** pp, char* w, int n) {
    const char* p = *pp + 1;
    char name[64]; int k = 0; const char* val = NULL; char num[16];
    if (*p == '?') { snprintf(num, sizeof num, "%d", last_status); val = num; p++; }
    else if (*p == '$') { snprintf(num, sizeof num, "%d", (int) getpid()); val = num; p++; }
    else if (*p == '{') { p++; while (*p && *p != '}' && k < 63) name[k++] = *p++; if (*p == '}') p++; name[k] = 0; val = getenv(name); }
    else if (isalpha((unsigned char) *p) || *p == '_') { while ((isalnum((unsigned char) *p) || *p == '_') && k < 63) name[k++] = *p++; name[k] = 0; val = getenv(name); }
    else { w[n++] = '$'; *pp = p; return n; }       /* '$' وحدها */
    if (val) { size_t l = strlen(val); if (n + (int) l < WORDMAX - 2) { memcpy(w + n, val, l); n += (int) l; } }
    *pp = p;
    return n;
}

#define DELIMS " \t\n|&;<>"

/* يحلّل مقطعًا واحدًا (pipeline) حتى أول ; أو && أو ||، ويضع المعامل في *op ويقدّم *pp.
   التوسيع ($VAR و$?) يتم هنا، أي بعد تنفيذ المقاطع السابقة بنفس السطر. */
static int tokenize(const char** pp, ttype* op) {
    const char* p = *pp; ntok = 0; *op = T_SEMI;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == '#') { while (*p) p++; }               /* تعليق حتى نهاية السطر */
        if (!*p) break;
        if (ntok >= MAXTOK - 2) { eprintf_("too many tokens", NULL); return -1; }
        if (*p == '|') { if (p[1] == '|') { *op = T_OR; p += 2; break; } add_tok(T_PIPE, NULL, 0); p++; continue; }
        if (*p == '&') {
            if (p[1] == '&') { *op = T_AND; p += 2; break; }
            eprintf_("background jobs (&) are not supported", NULL); return -1;
        }
        if (*p == ';') { *op = T_SEMI; p++; break; }
        if (*p == '<') { add_tok(T_IN, NULL, 0); p++; continue; }
        if (*p == '>') { if (p[1] == '>') { add_tok(T_APP, NULL, 0); p += 2; } else { add_tok(T_OUT, NULL, 0); p++; } continue; }
        char w[WORDMAX]; int n = 0, glob = 0, quoted = 0;
        while (*p && !strchr(DELIMS, *p)) {
            if (n >= WORDMAX - 64) { eprintf_("word too long", NULL); return -1; }
            if (*p == '\'') {
                quoted = 1; p++;
                while (*p && *p != '\'' && n < WORDMAX - 64) w[n++] = *p++;
                if (!*p) { eprintf_("unterminated quote", NULL); return -1; }
                p++;
            } else if (*p == '"') {
                quoted = 1; p++;
                while (*p && *p != '"' && n < WORDMAX - 64) {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) { p++; w[n++] = *p++; }
                    else if (*p == '$') n = expand_var(&p, w, n);
                    else w[n++] = *p++;
                }
                if (!*p) { eprintf_("unterminated quote", NULL); return -1; }
                p++;
            } else if (*p == '\\' && p[1]) { w[n++] = p[1]; p += 2; }
            else if (*p == '$') n = expand_var(&p, w, n);
            else if (*p == '~' && n == 0 && (p[1] == 0 || p[1] == '/' || strchr(DELIMS, p[1]))) {
                const char* h = getenv("HOME"); if (!h) h = "/";
                size_t l = strlen(h); memcpy(w + n, h, l); n += (int) l; p++;
            } else { if (*p == '*' || *p == '?') glob = 1; w[n++] = *p++; }
        }
        if (n == 0 && !quoted) continue;
        w[n] = 0;
        add_tok(T_WORD, w, glob);
    }
    *pp = p;
    return 0;
}

typedef struct { char* argv[MAXARG + 1]; int argc; char* in; char* out; int app; } cmd_t;
typedef struct { cmd_t c[MAXPIPE]; int n; } pipeline_t;

static int cmp_str(const void* a, const void* b) { return strcmp(*(char* const*) a, *(char* const*) b); }

/* glob لآخر مكوّن فقط. يضيف النتائج إلى cmd، وإن لم يوجد تطابق يضيف النص كما هو */
static void add_arg(cmd_t* c, const char* s) { if (c->argc < MAXARG) c->argv[c->argc++] = xstrdup(s); }

static void add_glob(cmd_t* c, const char* pat) {
    char dir[512]; const char* base = strrchr(pat, '/');
    if (base) { size_t l = (size_t)(base - pat); if (l == 0) l = 1; memcpy(dir, pat, l); dir[l] = 0; base++; }
    else { strcpy(dir, "."); base = pat; }
    DIR* d = opendir(dir); char* found[128]; int nf = 0;
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) && nf < 128) {
            if (fnmatch(base, e->d_name, FNM_PERIOD) == 0) {
                char full[1100];
                if (base == pat) snprintf(full, sizeof full, "%s", e->d_name);
                else snprintf(full, sizeof full, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", e->d_name);
                found[nf++] = xstrdup(full);
            }
        }
        closedir(d);
    }
    if (nf == 0) { add_arg(c, pat); return; }
    qsort(found, (size_t) nf, sizeof(char*), cmp_str);
    for (int i = 0; i < nf; i++) { add_arg(c, found[i]); free(found[i]); }
}

static void free_pipeline(pipeline_t* pl) {
    for (int i = 0; i < pl->n; i++) {
        for (int j = 0; j < pl->c[i].argc; j++) free(pl->c[i].argv[j]);
        free(pl->c[i].in); free(pl->c[i].out);
    }
    pl->n = 0;
}

/* يحلّل pipeline يبدأ من toks[*pi]؛ يرجع 0 نجاح، -1 خطأ صياغة */
static int parse_pipeline(int* pi, pipeline_t* pl) {
    memset(pl, 0, sizeof *pl);
    for (;;) {
        if (pl->n >= MAXPIPE) { eprintf_("pipeline too long", NULL); return -1; }
        cmd_t* c = &pl->c[pl->n];
        memset(c, 0, sizeof *c);
        while (*pi < ntok && toks[*pi].t != T_PIPE && toks[*pi].t != T_SEMI && toks[*pi].t != T_AND && toks[*pi].t != T_OR) {
            tok_t* t = &toks[*pi];
            if (t->t == T_WORD) { if (t->glob) add_glob(c, t->s); else add_arg(c, t->s); (*pi)++; }
            else {   /* < > >> */
                ttype rt = t->t; (*pi)++;
                if (*pi >= ntok || toks[*pi].t != T_WORD) { eprintf_("syntax error: missing file after redirection", NULL); pl->n++; return -1; }
                if (rt == T_IN) { free(c->in); c->in = xstrdup(toks[*pi].s); }
                else { free(c->out); c->out = xstrdup(toks[*pi].s); c->app = (rt == T_APP); }
                (*pi)++;
            }
        }
        if (c->argc == 0) { eprintf_("syntax error: empty command", NULL); pl->n++; return -1; }
        pl->n++;
        if (*pi < ntok && toks[*pi].t == T_PIPE) { (*pi)++; continue; }
        return 0;
    }
}

/* ======================= الأوامر الداخلية ======================= */
static int run_line(const char* line);

static const char* base_of(const char* p) { const char* s = strrchr(p, '/'); return s ? s + 1 : p; }

static int copy_fd(int in, int out) {
    char buf[4096]; ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) { ssize_t w = write(out, buf + off, (size_t)(n - off)); if (w < 0) return -1; off += w; }
    }
    return n < 0 ? -1 : 0;
}

static int is_dir(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static int b_echo(int argc, char** argv) {
    int i = 1, nl = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { nl = 0; i = 2; }
    for (; i < argc; i++) { fputs(argv[i], stdout); if (i + 1 < argc) fputc(' ', stdout); }
    if (nl) fputc('\n', stdout);
    return 0;
}
static int b_pwd(int argc, char** argv) { (void) argc; (void) argv; char b[512]; if (!getcwd(b, sizeof b)) { eprintf_("pwd", strerror(errno)); return 1; } puts(b); return 0; }
static int b_cd(int argc, char** argv) {
    const char* d = argc > 1 ? argv[1] : (getenv("HOME") ? getenv("HOME") : "/");
    if (chdir(d) != 0) { eprintf_(d, strerror(errno)); return 1; }
    return 0;
}
static int b_true(int a, char** v) { (void) a; (void) v; return 0; }
static int b_false(int a, char** v) { (void) a; (void) v; return 1; }
static int b_clear(int a, char** v) { (void) a; (void) v; fputs("\033[2J\033[H", stdout); return 0; }

static int cat_one(const char* path) {
    int fd = strcmp(path, "-") == 0 ? 0 : open(path, O_RDONLY);
    if (fd < 0) { eprintf_(path, strerror(errno)); return 1; }
    fflush(stdout);
    int r = copy_fd(fd, 1);
    if (fd != 0) close(fd);
    return r < 0 ? 1 : 0;
}
static int b_cat(int argc, char** argv) {
    if (argc == 1) return cat_one("-");
    int st = 0; for (int i = 1; i < argc; i++) st |= cat_one(argv[i]);
    return st;
}

static int b_ls(int argc, char** argv) {
    int lflag = 0, aflag = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char* f = argv[i] + 1; *f; f++) { if (*f == 'l') lflag = 1; else if (*f == 'a') aflag = 1; else { eprintf_("ls: unknown option", argv[i]); return 2; } }
    char* paths[MAXARG]; int np = 0;
    for (; i < argc; i++) paths[np++] = argv[i];
    if (np == 0) paths[np++] = ".";
    int status = 0;
    for (int pi = 0; pi < np; pi++) {
        const char* path = paths[pi];
        struct stat st;
        if (stat(path, &st) != 0) { eprintf_(path, strerror(errno)); status = 1; continue; }
        char* names[512]; int nn = 0;
        if (S_ISDIR(st.st_mode)) {
            DIR* d = opendir(path);
            if (!d) { eprintf_(path, strerror(errno)); status = 1; continue; }
            struct dirent* e;
            while ((e = readdir(d)) && nn < 512) if (aflag || e->d_name[0] != '.') names[nn++] = xstrdup(e->d_name);
            closedir(d);
            qsort(names, (size_t) nn, sizeof(char*), cmp_str);
            if (np > 1) printf("%s:\n", path);
        } else names[nn++] = xstrdup(path);
        int col = 0;
        for (int k = 0; k < nn; k++) {
            char full[1100]; struct stat s2;
            if (S_ISDIR(st.st_mode)) snprintf(full, sizeof full, "%s/%s", path, names[k]); else snprintf(full, sizeof full, "%s", names[k]);
            int ok = stat(full, &s2) == 0;
            if (lflag) {
                char m[11]; mode_t md = ok ? s2.st_mode : 0;
                m[0] = S_ISDIR(md) ? 'd' : S_ISCHR(md) ? 'c' : S_ISFIFO(md) ? 'p' : '-';
                const char* rwx = "rwxrwxrwx";
                for (int b = 0; b < 9; b++) m[1 + b] = (md & (0400 >> b)) ? rwx[b] : '-';
                m[10] = 0;
                printf("%s %8ld %s%s\n", m, ok ? (long) s2.st_size : 0L, names[k], ok && S_ISDIR(md) ? "/" : "");
            } else {
                int len = (int) strlen(names[k]) + (ok && S_ISDIR(s2.st_mode) ? 1 : 0) + 2;
                if (col + len > 78 && col > 0) { putchar('\n'); col = 0; }
                printf("%s%s  ", names[k], ok && S_ISDIR(s2.st_mode) ? "/" : "");
                col += len;
            }
            free(names[k]);
        }
        if (!lflag && col) putchar('\n');
        if (np > 1 && pi + 1 < np) putchar('\n');
    }
    return status;
}

static int b_mkdir(int argc, char** argv) {
    int pflag = 0, st = 0, i = 1;
    if (argc > 1 && strcmp(argv[1], "-p") == 0) { pflag = 1; i = 2; }
    if (i >= argc) { eprintf_("mkdir: missing operand", NULL); return 1; }
    for (; i < argc; i++) {
        if (pflag) {
            char tmp[512]; snprintf(tmp, sizeof tmp, "%s", argv[i]);
            for (char* c = tmp + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(tmp, 0755); *c = '/'; }
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { eprintf_(argv[i], strerror(errno)); st = 1; }
        } else if (mkdir(argv[i], 0755) != 0) { eprintf_(argv[i], strerror(errno)); st = 1; }
    }
    return st;
}
static int b_rmdir(int argc, char** argv) {
    int st = 0; if (argc < 2) { eprintf_("rmdir: missing operand", NULL); return 1; }
    for (int i = 1; i < argc; i++) if (rmdir(argv[i]) != 0) { eprintf_(argv[i], strerror(errno)); st = 1; }
    return st;
}
static int b_touch(int argc, char** argv) {
    int st = 0; if (argc < 2) { eprintf_("touch: missing operand", NULL); return 1; }
    for (int i = 1; i < argc; i++) { int fd = open(argv[i], O_WRONLY | O_CREAT, 0644); if (fd < 0) { eprintf_(argv[i], strerror(errno)); st = 1; } else close(fd); }
    return st;
}

static int removed_any = 0;
static int rm_path(const char* path, int rflag, int fflag) {
    struct stat st;
    if (stat(path, &st) != 0) { if (!fflag) { eprintf_(path, strerror(errno)); return 1; } return 0; }
    if (S_ISDIR(st.st_mode)) {
        if (!rflag) { eprintf_(path, "is a directory (use rm -r)"); return 1; }
        DIR* d = opendir(path); int r = 0;
        if (d) {
            struct dirent* e; char* kids[512]; int nk = 0;
            while ((e = readdir(d)) && nk < 512) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) kids[nk++] = xstrdup(e->d_name);
            closedir(d);
            for (int i = 0; i < nk; i++) { char full[1100]; snprintf(full, sizeof full, "%s/%s", path, kids[i]); r |= rm_path(full, rflag, fflag); free(kids[i]); }
        }
        if (rmdir(path) != 0) { eprintf_(path, strerror(errno)); r = 1; }
        return r;
    }
    if (unlink(path) != 0) { eprintf_(path, strerror(errno)); return 1; }
    removed_any = 1;
    return 0;
}
static int b_rm(int argc, char** argv) {
    int rflag = 0, fflag = 0, i = 1, st = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char* f = argv[i] + 1; *f; f++) { if (*f == 'r' || *f == 'R') rflag = 1; else if (*f == 'f') fflag = 1; }
    if (i >= argc) { eprintf_("rm: missing operand", NULL); return 1; }
    removed_any = 0;
    for (; i < argc; i++) st |= rm_path(argv[i], rflag, fflag);
    /* نافذة الأمانة في النواة: الحذف الفعلي يتأجل نحو 3 ثوانٍ، ويمكن التراجع بـ undo */
    if (removed_any) puts("(deletion is delayed ~3s by the trust window; type 'undo' to cancel)");
    return st;
}
static int b_undo(int argc, char** argv) {
    (void) argc; (void) argv;
    long r = syscall(SYS_GINUX_UNLINK_CANCEL, -1);
    if (r == 0) { puts("last removal cancelled"); return 0; }
    puts("nothing to undo (window expired or no pending removal)"); return 1;
}
static int b_cp(int argc, char** argv) {
    if (argc != 3) { eprintf_("usage: cp SRC DST", NULL); return 1; }
    char dst[1100]; snprintf(dst, sizeof dst, "%s", argv[2]);
    if (is_dir(dst)) snprintf(dst, sizeof dst, "%s/%s", argv[2], base_of(argv[1]));
    int in = open(argv[1], O_RDONLY); if (in < 0) { eprintf_(argv[1], strerror(errno)); return 1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { eprintf_(dst, strerror(errno)); close(in); return 1; }
    int r = copy_fd(in, out); close(in); close(out);
    if (r < 0) { eprintf_("cp: write error", NULL); return 1; }
    return 0;
}
static int b_mv(int argc, char** argv) {
    if (argc != 3) { eprintf_("usage: mv SRC DST", NULL); return 1; }
    char dst[1100]; snprintf(dst, sizeof dst, "%s", argv[2]);
    if (is_dir(dst)) snprintf(dst, sizeof dst, "%s/%s", argv[2], base_of(argv[1]));
    if (rename(argv[1], dst) != 0) { eprintf_(argv[1], strerror(errno)); return 1; }
    return 0;
}

static int count_fd(int fd, long* l, long* w, long* c) {
    char buf[4096]; ssize_t n; int inword = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        *c += n;
        for (ssize_t i = 0; i < n; i++) { if (buf[i] == '\n') (*l)++; if (isspace((unsigned char) buf[i])) inword = 0; else if (!inword) { inword = 1; (*w)++; } }
    }
    return n < 0 ? -1 : 0;
}
static int b_wc(int argc, char** argv) {
    int lonly = 0, i = 1;
    if (argc > 1 && strcmp(argv[1], "-l") == 0) { lonly = 1; i = 2; }
    long L = 0, W = 0, C = 0;
    if (i >= argc) { count_fd(0, &L, &W, &C); if (lonly) printf("%ld\n", L); else printf("%ld %ld %ld\n", L, W, C); return 0; }
    for (; i < argc; i++) {
        long l = 0, w = 0, c = 0; int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { eprintf_(argv[i], strerror(errno)); continue; }
        count_fd(fd, &l, &w, &c); close(fd);
        if (lonly) printf("%ld %s\n", l, argv[i]); else printf("%ld %ld %ld %s\n", l, w, c, argv[i]);
    }
    return 0;
}

/* grep بسيط: نص جزئي (ليس regex). -v عكس، -n أرقام الأسطر */
static int grep_stream(int fd, const char* pat, int vflag, int nflag, const char* label) {
    static char buf[1 << 16]; size_t have = 0; ssize_t n; long ln = 0; int found = 0;
    for (;;) {
        n = read(fd, buf + have, sizeof buf - have - 1);
        if (n <= 0 && have == 0) break;
        if (n > 0) have += (size_t) n;
        size_t start = 0;
        for (size_t i = 0; i < have; i++) {
            if (buf[i] == '\n' || (n <= 0 && i + 1 == have)) {
                size_t end = (buf[i] == '\n') ? i : i + 1;
                char save = buf[end]; buf[end] = 0; ln++;
                int match = strstr(buf + start, pat) != NULL;
                if (match != vflag) {
                    found = 1;
                    if (label) printf("%s:", label);
                    if (nflag) printf("%ld:", ln);
                    printf("%s\n", buf + start);
                }
                buf[end] = save; start = i + 1;
            }
        }
        if (start < have) memmove(buf, buf + start, have - start);
        have -= start;
        if (n <= 0) break;
        if (have >= sizeof buf - 1) have = 0;     /* سطر أطول من المخزن: نتجاوزه */
    }
    return found;
}
static int b_grep(int argc, char** argv) {
    int vflag = 0, nflag = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char* f = argv[i] + 1; *f; f++) { if (*f == 'v') vflag = 1; else if (*f == 'n') nflag = 1; }
    if (i >= argc) { eprintf_("usage: grep [-vn] PATTERN [FILE...]", NULL); return 2; }
    const char* pat = argv[i++]; int found = 0;
    fflush(stdout);
    if (i >= argc) return grep_stream(0, pat, vflag, nflag, NULL) ? 0 : 1;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { eprintf_(argv[i], strerror(errno)); continue; }
        found |= grep_stream(fd, pat, vflag, nflag, argc - i > 1 || i > 2 ? argv[i] : NULL); close(fd);
    }
    return found ? 0 : 1;
}
static int b_head(int argc, char** argv) {
    int nlines = 10, i = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) { nlines = atoi(argv[2]); i = 3; }
    int fd = i < argc ? open(argv[i], O_RDONLY) : 0;
    if (fd < 0) { eprintf_(argv[i], strerror(errno)); return 1; }
    char c; int seen = 0; fflush(stdout);
    while (seen < nlines && read(fd, &c, 1) == 1) { if (write(1, &c, 1) < 0) break; if (c == '\n') seen++; }
    if (fd != 0) close(fd);
    return 0;
}

static int b_date(int a, char** v) { (void) a; (void) v; time_t t = time(NULL); struct tm* tm = gmtime(&t); char b[64]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S UTC", tm); puts(b); return 0; }
static int b_uptime(int a, char** v) {
    (void) a; (void) v; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("up %ld min %ld s\n", (long) ts.tv_sec / 60, (long) ts.tv_sec % 60); return 0;
}
static int b_free(int a, char** v) {
    (void) a; (void) v; struct sysinfo si;
    if (sysinfo(&si) != 0) { eprintf_("free", strerror(errno)); return 1; }
    unsigned long tot = si.totalram / 1024, fr = si.freeram / 1024;
    printf("total %lu KB   used %lu KB   free %lu KB\n", tot, tot - fr, fr); return 0;
}
static int b_uname(int argc, char** argv) {
    struct utsname u; uname(&u);
    if (argc > 1 && strcmp(argv[1], "-a") == 0) printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
    else puts(u.sysname);
    return 0;
}
static int b_sleep(int argc, char** argv) { if (argc < 2) return 1; struct timespec ts = { atoi(argv[1]), 0 }; nanosleep(&ts, NULL); return 0; }
static int b_whoami(int a, char** v) { (void) a; (void) v; puts("root"); return 0; }
static int b_env(int a, char** v) { (void) a; (void) v; for (char** e = environ; e && *e; e++) puts(*e); return 0; }
static int b_export(int argc, char** argv) {
    if (argc == 1) return b_env(0, NULL);
    for (int i = 1; i < argc; i++) {
        char* eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; setenv(argv[i], eq + 1, 1); *eq = '='; }
    }
    return 0;
}
static int b_unset(int argc, char** argv) { for (int i = 1; i < argc; i++) unsetenv(argv[i]); return 0; }

static int source_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { eprintf_(path, strerror(errno)); return 127; }
    char ln[1024]; int st = 0;
    while (fgets(ln, sizeof ln, f)) st = run_line(ln);
    fclose(f);
    return st;
}
static int b_source(int argc, char** argv) { if (argc < 2) { eprintf_("source: missing file", NULL); return 1; } return source_file(argv[1]); }

static int b_exit(int argc, char** argv) {
    int code = argc > 1 ? atoi(argv[1]) : last_status;
    if (getpid() == 1) { puts("this is the init shell; close the QEMU window to power off (or just keep working)"); return code; }
    fflush(NULL); _exit(code);
}
static int b_help(int a, char** v);

typedef struct { const char* name; int (*fn)(int, char**); const char* help; } builtin_t;
static const builtin_t builtins[] = {
    { "cd", b_cd, "cd [DIR]            change directory" },
    { "pwd", b_pwd, "pwd                 print working directory" },
    { "ls", b_ls, "ls [-la] [PATH...]   list files" },
    { "cat", b_cat, "cat [FILE...]       print files (no args: read stdin, Ctrl-D ends)" },
    { "echo", b_echo, "echo [-n] ARGS      print arguments" },
    { "mkdir", b_mkdir, "mkdir [-p] DIR...   create directories" },
    { "rmdir", b_rmdir, "rmdir DIR...       remove empty directories" },
    { "rm", b_rm, "rm [-rf] PATH...    remove (delayed by the trust window)" },
    { "undo", b_undo, "undo                cancel the last rm (within ~3s)" },
    { "cp", b_cp, "cp SRC DST          copy a file" },
    { "mv", b_mv, "mv SRC DST          move/rename" },
    { "touch", b_touch, "touch FILE...       create empty files" },
    { "wc", b_wc, "wc [-l] [FILE...]   count lines/words/bytes" },
    { "grep", b_grep, "grep [-vn] TEXT [FILE...]  find lines containing TEXT" },
    { "head", b_head, "head [-n N] [FILE]  first N lines" },
    { "date", b_date, "date                 current date (UTC)" },
    { "uptime", b_uptime, "uptime               time since boot" },
    { "free", b_free, "free                 memory usage" },
    { "uname", b_uname, "uname [-a]          system information" },
    { "sleep", b_sleep, "sleep N             wait N seconds" },
    { "whoami", b_whoami, "whoami" },
    { "env", b_env, "env                  list environment" },
    { "export", b_export, "export VAR=VALUE    set an environment variable" },
    { "unset", b_unset, "unset VAR" },
    { "source", b_source, "source FILE         run commands from a file (also: . FILE)" },
    { ".", b_source, NULL },
    { "clear", b_clear, "clear                clear the screen" },
    { "true", b_true, NULL }, { "false", b_false, NULL },
    { "exit", b_exit, "exit [N]             leave the shell" },
    { "help", b_help, "help                 this list" },
};
#define NBUILTIN (sizeof builtins / sizeof builtins[0])

static int b_help(int a, char** v) {
    (void) a; (void) v;
    puts("Ghadir OS shell. Builtins:");
    for (size_t i = 0; i < NBUILTIN; i++) if (builtins[i].help) printf("  %s\n", builtins[i].help);
    puts("Operators: | > >> < ; && ||   quotes: '..' \"..\"   vars: $VAR $?   globs: * ?");
    puts("Programs:  gcc, cc, tcc, as, ld, objdump, ar    Ctrl-C stops a running program, Ctrl-D ends input");
    puts("Compile:   cat > hello.c   (type code, Ctrl-D)  then:  cc hello.c -o hello && ./hello");
    return 0;
}

/* أوامر تغيّر حالة الـ shell نفسه (أو تعتمد على pid الـ shell كـ undo) تُنفَّذ داخل عمليته؛ الباقي بعملية ابن
   حتى يستطيع Ctrl-C إيقافها (sleep، cat بدون ملف، grep على stdin...). */
static int is_special(const char* n) {
    static const char* s[] = { "cd", "exit", "export", "unset", "source", ".", "rm", "undo", NULL };
    for (int i = 0; s[i]; i++) if (strcmp(s[i], n) == 0) return 1;
    return 0;
}

static const builtin_t* find_builtin(const char* n) { for (size_t i = 0; i < NBUILTIN; i++) if (strcmp(builtins[i].name, n) == 0) return &builtins[i]; return NULL; }

/* ======================= التنفيذ ======================= */
static int apply_redirs(const cmd_t* c) {
    if (c->in) { int fd = open(c->in, O_RDONLY); if (fd < 0) { eprintf_(c->in, strerror(errno)); return -1; } dup2(fd, 0); close(fd); }
    if (c->out) {
        int fd = open(c->out, O_WRONLY | O_CREAT | (c->app ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { eprintf_(c->out, strerror(errno)); return -1; }
        dup2(fd, 1); close(fd);
    }
    return 0;
}

static int status_of(int st) { return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); }

/* ينفّذ أمرًا واحدًا في عملية الابن الحالية (لا يرجع) */
static void exec_child(const cmd_t* c) {
    if (apply_redirs(c) < 0) _exit(1);
    const builtin_t* b = find_builtin(c->argv[0]);
    if (b) { int r = b->fn(c->argc, (char**) c->argv); fflush(NULL); _exit(r); }
    execvp(c->argv[0], (char* const*) c->argv);
    if (errno == ENOENT) eprintf_(c->argv[0], "command not found");
    else if (errno == ENOEXEC) eprintf_(c->argv[0], "cannot execute (not a valid static 32-bit ELF at 0x40000000)");
    else eprintf_(c->argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static int run_pipeline(pipeline_t* pl) {
    fflush(NULL);
    if (pl->n == 1) {
        cmd_t* c = &pl->c[0];
        const builtin_t* b = find_builtin(c->argv[0]);
        if (b && is_special(c->argv[0])) {   /* داخل عملية الـ shell نفسها (حتى يؤثر cd/export)، مع تحويل مؤقت للمخارج */
            int s0 = dup(0), s1 = dup(1), r;
            if (apply_redirs(c) < 0) r = 1; else { r = b->fn(c->argc, c->argv); fflush(stdout); }
            dup2(s0, 0); dup2(s1, 1); close(s0); close(s1);
            return r;
        }
        pid_t p = fork();
        if (p < 0) { eprintf_("fork", strerror(errno)); return 1; }
        if (p == 0) exec_child(c);
        int st = 0; waitpid(p, &st, 0);
        return status_of(st);
    }
    int prev = -1; pid_t pids[MAXPIPE];
    for (int i = 0; i < pl->n; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < pl->n && pipe(fds) < 0) { eprintf_("pipe", strerror(errno)); return 1; }
        pid_t p = fork();
        if (p < 0) { eprintf_("fork", strerror(errno)); return 1; }
        if (p == 0) {
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[1]); close(fds[0]); }
            exec_child(&pl->c[i]);
        }
        pids[i] = p;
        if (prev >= 0) close(prev);
        if (fds[1] >= 0) close(fds[1]);
        prev = fds[0];
    }
    int last = 0;
    for (int i = 0; i < pl->n; i++) { int st = 0; waitpid(pids[i], &st, 0); if (i == pl->n - 1) last = status_of(st); }
    return last;
}

static int run_line(const char* line) {
    const char* p = line; int skip = 0, status = last_status;
    for (;;) {
        ttype op;
        if (tokenize(&p, &op) < 0) { free_toks(); status = 2; break; }
        if (ntok > 0) {
            int pi = 0; pipeline_t pl;
            if (parse_pipeline(&pi, &pl) < 0) { free_pipeline(&pl); free_toks(); status = 2; break; }
            if (!skip) { status = run_pipeline(&pl); last_status = status; }
            free_pipeline(&pl);
        } else if (op == T_AND || op == T_OR) { eprintf_("syntax error near operator", NULL); free_toks(); status = 2; break; }
        free_toks();
        if (op == T_AND) skip = (status != 0);
        else if (op == T_OR) skip = (status == 0);
        else skip = 0;
        if (!*p) break;
    }
    last_status = status;
    return status;
}

/* ======================= main ======================= */
static void prompt(void) {
    char cwd[256]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
    printf("root@ghadir:%s# ", cwd); fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) { if (!getenv("PATH")) setenv("PATH", "/bin", 1); return run_line(argv[2]); }
    if (argc >= 2) { if (!getenv("PATH")) setenv("PATH", "/bin", 1); return source_file(argv[1]); }

    interactive = 1;
    if (!getenv("PATH")) setenv("PATH", "/bin", 1);
    if (!getenv("HOME")) setenv("HOME", "/", 1);
    setenv("TMPDIR", "/tmp", 1);
    mkdir("/tmp", 0777);
    /* Ctrl-C يقتل أبناء هذا الـ shell فقط (القيمة السالبة = لا تقتل صاحب الـ pid نفسه) */
    int neg = -(int) getpid(); ioctl(0, TIOCSPGRP_, &neg);
    setvbuf(stdout, NULL, _IOLBF, 0);

    puts("");
    puts("  Ghadir OS " GHADIR_VERSION "  -  ginux kernel + gng tools");
    puts("  type 'help' for commands;  Ctrl-C stops a program,  Ctrl-D ends input");
    puts("");
    if (access("/etc/profile", R_OK) == 0) source_file("/etc/profile");

    char buf[512];
    for (;;) {
        prompt();
        ssize_t n = read(0, buf, sizeof buf - 1);
        if (n < 0) continue;
        if (n == 0) {                       /* Ctrl-D على سطر فارغ */
            if (getpid() == 1) { puts("(use 'exit' only in a child shell; this is init)"); continue; }
            break;
        }
        buf[n] = 0;
        run_line(buf);
    }
    return last_status;
}
