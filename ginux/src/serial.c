#include "serial.h"
#include "io.h"
#include "vga.h"
#include <stdarg.h>

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!serial_transmit_empty());
    outb(COM1, c);
}

void serial_write(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_writehex(unsigned int val) {
    const char* hex = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 28; i >= 0; i -= 4) {
        serial_putc(hex[(val >> i) & 0xF]);
    }
}

static void print_uint(unsigned int val, int base) {
    char buf[32];
    const char* digits = "0123456789ABCDEF";
    int i = 0;
    if (val == 0) buf[i++] = '0';
    while (val > 0) {
        buf[i++] = digits[val % base];
        val /= base;
    }
    while (i > 0) {
        i--;
        serial_putc(buf[i]);
        terminal_putchar(buf[i]);
    }
}

/* kprintf: نسخة صغيرة تدعم %s %d %x %c %% وتكتب على serial + شاشة VGA معًا */
void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            if (*p == '\n') serial_putc('\r');
            serial_putc(*p);
            terminal_putchar(*p);
            continue;
        }
        p++;
        switch (*p) {
            case 's': {
                const char* s = va_arg(args, const char*);
                while (*s) {
                    serial_putc(*s);
                    terminal_putchar(*s);
                    s++;
                }
                break;
            }
            case 'd': {
                int v = va_arg(args, int);
                if (v < 0) { serial_putc('-'); terminal_putchar('-'); v = -v; }
                print_uint((unsigned int)v, 10);
                break;
            }
            case 'x': {
                unsigned int v = va_arg(args, unsigned int);
                serial_write("0x");
                terminal_writestring("0x");
                print_uint(v, 16);
                break;
            }
            case 'c': {
                char c = (char) va_arg(args, int);
                serial_putc(c);
                terminal_putchar(c);
                break;
            }
            case '%':
                serial_putc('%');
                terminal_putchar('%');
                break;
            default:
                serial_putc('%');
                serial_putc(*p);
                terminal_putchar('%');
                terminal_putchar(*p);
        }
    }

    va_end(args);
}
