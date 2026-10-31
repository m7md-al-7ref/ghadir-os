/* drv_all.c -- يشغَّل كـ "init" داخل ginux، ويختبر كل أدوات gng بالترتيب ثم يطبع ملخصًا.
   كل خطوة: fork + execv + waitpid لأداة حقيقية (as, ld, objdump, tcc, gcc) ثم تشغيل البرنامج الناتج. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int run(const char* prog, char* const argv[]) {
    pid_t p = fork();
    if (p == 0) { execv(prog, argv); printf("execv(%s) failed\n", prog); fflush(stdout); _exit(127); }
    int st = 0; waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static int exists(const char* p) { struct stat st; return stat(p, &st) == 0; }
static int results[8]; static const char* names[8]; static int nres = 0;
static void record(const char* name, int ok) { names[nres] = name; results[nres++] = ok; printf(">>> %s: %s\n", name, ok ? "PASS" : "FAIL"); }

int main(void) {
    printf("=============== Ghadir OS demo: gng toolchain inside ginux ===============\n");
    mkdir("/tmp", 0777); setenv("TMPDIR", "/tmp", 1);

    /* 1) glibc: هذا المشغّل نفسه مبني بـ glibc ساكنة، وقد وصل إلى هنا (printf + fork + exec) */
    record("glibc (static) : printf/fork/exec/waitpid", 1);

    /* 2) GNU binutils: as ثم ld ثم تشغيل الناتج ثم objdump */
    { char* as_a[] = { "as", "--32", "-o", "/out/bu.o", "/src/hello.s", 0 };
      char* ld_a[] = { "ld", "-m", "elf_i386", "-Ttext-segment=0x40000000", "-o", "/out/bu_hello", "/out/bu.o", 0 };
      int a = run("/bin/as", as_a), l = run("/bin/ld", ld_a);
      int r = exists("/out/bu_hello") ? run("/out/bu_hello", (char*[]){ "bu_hello", 0 }) : -1;
      char* od[] = { "objdump", "-d", "/out/bu_hello", 0 };
      int o = run("/bin/objdump", od);
      record("binutils       : as + ld + objdump", a == 0 && l == 0 && r == 9 && o == 0); }

    /* 3) TinyCC */
    { char* t[] = { "tcc", "-nostdinc", "-I/dietinc", "-I/tccinc", "-static", "-nostdlib", "-Wl,-Ttext=0x40000000",
                    "-o", "/out/tcc_hello", "/src/hello_tcc.c", "/lib/start.o", "/lib/dietlibc.a", "/lib/libgcc.a", "/lib/libtcc1.a", 0 };
      int c = run("/bin/tcc", t);
      int r = exists("/out/tcc_hello") ? run("/out/tcc_hello", (char*[]){ "tcc_hello", 0 }) : -1;
      record("tcc            : compile + link + run", c == 0 && r == 42); }

    /* 4) GCC 4.7.4: gcc -> cc1 -> as -> collect2 -> ld */
    { char* g[] = { "gcc", "-B/gcclib/", "-B/bin/", "-nostdinc", "-I/gccinc", "-I/dietinc", "-O1", "-static", "-nostdlib",
                    "-Wl,-Ttext-segment=0x40000000", "-Wl,-z,noexecstack", "-o", "/out/gcc_hello", "/src/hello_gcc.c",
                    "/lib/start.o", "/lib/dietlibc.a", "/lib/libgcc.a", 0 };
      int c = run("/bin/gcc", g);
      int r = exists("/out/gcc_hello") ? run("/out/gcc_hello", (char*[]){ "gcc_hello", 0 }) : -1;
      record("GCC 4.7.4      : cc1 + as + collect2 + ld + run", c == 0 && r == 42); }

    int pass = 0; for (int i = 0; i < nres; i++) pass += results[i];
    printf("=============== SUMMARY: %d/%d passed ===============\n", pass, nres);
    for (int i = 0; i < nres; i++) printf("  [%s] %s\n", results[i] ? "PASS" : "FAIL", names[i]);
    printf("=============== done ===============\n");
    return pass == nres ? 0 : 1;
}
