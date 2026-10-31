#include "vga.h"
#include "io.h"

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | (uint16_t) color << 8;
}

static void terminal_scroll(void) {
    if (terminal_buffer != (uint16_t*) 0xB8000 || terminal_row > VGA_HEIGHT ||
        terminal_column >= VGA_WIDTH) {
        extern void serial_write(const char* s);
        extern void serial_writehex(unsigned int val);
        serial_write("\r\n[CORRUPT-DETECT] terminal globals bad! buffer=");
        serial_writehex((unsigned int) terminal_buffer);
        serial_write(" row="); serial_writehex((unsigned int) terminal_row);
        serial_write(" col="); serial_writehex((unsigned int) terminal_column);
        serial_write("\r\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            uint16_t* src = terminal_buffer + (y + 1) * VGA_WIDTH + x;
            if ((uint32_t) src < 0xB8000 || (uint32_t) src > 0xB8FA0) {
                extern void kprintf(const char* fmt, ...);
                kprintf("\n[SCROLL-CORRUPT] src=%x y=%d x=%d terminal_buffer=%x row=%d col=%d\n",
                        (uint32_t) src, (int) y, (int) x, (uint32_t) terminal_buffer,
                        (int) terminal_row, (int) terminal_column);
                for (;;) __asm__ volatile ("cli; hlt");
            }
            terminal_buffer[y * VGA_WIDTH + x] = *src;
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
            vga_entry(' ', terminal_color);
    }
    terminal_row = VGA_HEIGHT - 1;
}

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    terminal_buffer = (uint16_t*) 0xB8000;
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
        }
    }
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

static void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    terminal_buffer[y * VGA_WIDTH + x] = vga_entry(c, color);
}

/* مؤشر العتاد (الشرطة الوامضة) -- يجب تحديثه يدويًا عبر منافذ CRTC */
static void cursor_update(void) {
    uint16_t pos = (uint16_t)(terminal_row * VGA_WIDTH + terminal_column);
    if (terminal_row >= VGA_HEIGHT) pos = (uint16_t)((VGA_HEIGHT - 1) * VGA_WIDTH);
    outb(0x3D4, 14); outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15); outb(0x3D5, (uint8_t)(pos & 0xFF));
}

/* كتابة حرف مع معالجة المحارف التحكمية. terminal_column < VGA_WIDTH دائمًا (يعتمدها terminal_scroll) */
static void put_raw(char c) {
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) terminal_scroll();
        return;
    }
    if (c == '\r') { terminal_column = 0; return; }
    if (c == '\b') { if (terminal_column > 0) terminal_column--; return; }   /* يتراجع فقط؛ المحو يتم بكتابة مسافة */
    if (c == '\t') {
        size_t next = (terminal_column + 8) & ~(size_t) 7;
        while (terminal_column < next && terminal_column < VGA_WIDTH - 1) terminal_putentryat(' ', terminal_color, terminal_column++, terminal_row);
        return;
    }
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) terminal_scroll();
    }
}

/* سجلات النواة (kprintf) تذهب إلى الشاشة فقط ما دام هذا مفعّلًا. في وضع الـ shell نطفئه: الشاشة ملك الـ tty،
   وتبقى السجلات على serial للتشخيص. */
static int kernel_log_enabled = 1;
void terminal_set_log_enabled(int on) { kernel_log_enabled = on; }

void terminal_putchar(char c) {
    if (!kernel_log_enabled) return;
    put_raw(c);
    cursor_update();
}

/* كتابة الـ tty/الـ console الخاصة بالعمليات: دائمًا تظهر على الشاشة */
void terminal_console_putchar(char c) {
    put_raw(c);
    cursor_update();
}

void terminal_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    terminal_row = 0; terminal_column = 0;
    cursor_update();
}

void terminal_clear_eol(void) {
    for (size_t x = terminal_column; x < VGA_WIDTH; x++)
        terminal_buffer[terminal_row * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    size_t i = 0;
    while (data[i])
        i++;
    terminal_write(data, i);
}
