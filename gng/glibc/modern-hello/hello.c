#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc, char** argv) {
    printf("hello from MODERN glibc (static) on Ginux! argc=%d argv0=%s\n", argc, argv[0]);
    char* p = malloc(300000); strcpy(p, "malloc(300000) ok"); puts(p); free(p);
    pid_t c = fork();
    if (c == 0) { puts("child: hi"); _exit(7); }
    int st; waitpid(c, &st, 0);
    printf("child exit=%d\n", WEXITSTATUS(st));
    FILE* f = fopen("/tmp.txt", "w"); fprintf(f, "written by glibc %d\n", 42); fclose(f);
    f = fopen("/tmp.txt", "r"); char b[64] = {0}; fgets(b, 63, f); fclose(f); printf("read back: %s", b);
    printf("float: %.3f\n", 3.14159 * 2);
    return 0;
}
