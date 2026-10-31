#include <stdio.h>
extern void* malloc(unsigned int);
extern void free(void*);
#include <string.h>
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int main(int argc, char** argv) {
    char* buf = malloc(1000);
    strcpy(buf, "compiled INSIDE Ginux by GCC 4.7.4");
    printf("%s | fib(20)=%d | argc=%d\n", buf, fib(20), argc);
    free(buf);
    return 42;
}
