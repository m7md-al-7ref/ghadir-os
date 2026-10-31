/* sample program: compile it inside Ghadir OS with:   cc hello.c -o hello   then run:   ./hello */
#include <stdio.h>

int square(int x) { return x * x; }

int main(int argc, char** argv) {
    printf("Hello from Ghadir OS! square(12) = %d, argc = %d\n", square(12), argc);
    return 0;
}
