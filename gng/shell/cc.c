/* cc.c -- غلاف صغير حول gcc (أو tcc إذا لم يوجد gcc): يضيف كل ما يلزم لربط برامج بـ dietlibc داخل Ghadir OS، فتكتب:  cc hello.c -o hello
   مثل gcc تمامًا، لكن بدون كتابة المسارات والخيارات الطويلة. مع -c أو -S أو -E لا يضيف مكتبات الربط. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    char* out[256]; int n = 0, compile_only = 0, has_opt = 0;
    /* بدون gcc (نسخة "خفيفة" بُنيت بـ tcc فقط): نستعمل tcc بنفس الفكرة */
    if (access("/bin/gcc", X_OK) != 0 && access("/bin/tcc", X_OK) == 0) {
        for (int i = 1; i < argc; i++)
            if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-S") || !strcmp(argv[i], "-E")) compile_only = 1;
        out[n++] = "tcc"; out[n++] = "-nostdinc"; out[n++] = "-I/dietinc"; out[n++] = "-I/tccinc";
        if (!compile_only) { out[n++] = "-static"; out[n++] = "-nostdlib"; out[n++] = "-Wl,-Ttext=0x40000000"; }
        for (int i = 1; i < argc && n < 240; i++) out[n++] = argv[i];
        if (!compile_only) { out[n++] = "/lib/start.o"; out[n++] = "/lib/dietlibc.a"; out[n++] = "/lib/libgcc.a"; out[n++] = "/lib/libtcc1.a"; }
        out[n] = NULL;
        execv("/bin/tcc", out);
        perror("cc: cannot run /bin/tcc");
        return 127;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "-S") || !strcmp(argv[i], "-E")) compile_only = 1;
        if (argv[i][0] == '-' && argv[i][1] == 'O') has_opt = 1;
    }
    out[n++] = "gcc";
    out[n++] = "-B/gcclib/"; out[n++] = "-B/bin/";
    out[n++] = "-nostdinc"; out[n++] = "-I/gccinc"; out[n++] = "-I/dietinc";
    if (!has_opt) out[n++] = "-O1";
    if (!compile_only) {
        out[n++] = "-static"; out[n++] = "-nostdlib";
        out[n++] = "-Wl,-Ttext-segment=0x40000000"; out[n++] = "-Wl,-z,noexecstack";
        out[n++] = "-Wl,--no-warnings";             /* يخفي تحذير dietlibc الدعائي عن حجم printf (يظهر مع كل برنامج ويبدو كخطأ) */
        out[n++] = "/lib/start.o";                  /* مدخل البرنامج (_start) يجب أن يسبق باقي الملفات */
    }
    for (int i = 1; i < argc && n < 240; i++) out[n++] = argv[i];
    if (!compile_only) { out[n++] = "/lib/dietlibc.a"; out[n++] = "/lib/libgcc.a"; }
    out[n] = NULL;
    execv("/bin/gcc", out);
    perror("cc: cannot run /bin/gcc");
    return 127;
}
