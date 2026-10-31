#include "compat_abi.h"
#include "serial.h"
#include "keyboard.h"
#include "io.h"
#include "vga.h"

/* ============================================================
 * tty: وضع canonical (line discipline) مثل Unix:
 *  - الإدخال يتجمع في سطر مع echo وتحرير بسيط، ويُسلَّم للقارئ عند '\n'
 *      Backspace = حذف حرف، Ctrl-U = حذف السطر، Ctrl-D = نهاية الملف (EOF)،
 *      Ctrl-C = إيقاف العملية الأمامية وكل فروعها
 *  - مصدران: الكيبورد (PS/2، لنافذة QEMU الرسومية) و COM1 (-serial stdio واختبار stdin آليًا)
 *  - الإخراج: الشاشة (VGA) و serial معًا. الشاشة تفهم: \n \r \b \t و ESC[2J (مسح) و ESC[K (مسح حتى نهاية السطر).
 *  - الاستقبال يتم من مقاطعة المؤقت (tty_irq_poll) حتى لا تضيع المفاتيح عندما لا أحد يقرأ (type-ahead)،
 *    ولأن Ctrl-C يجب أن يعمل حتى والعملية الأمامية لا تقرأ من الـ tty.
 * حدود معروفة: لا تاريخ أوامر ولا أسهم ولا raw mode ولا إشارات حقيقية.
 * ============================================================ */

#define LINE_MAX 256
static char line[LINE_MAX];
static int  line_len = 0;       /* ما كُتب حتى الآن */
static int  ready_len = 0;      /* > 0 = سطر كامل جاهز للتسليم، -1 = EOF جاهز */
static int  ready_pos = 0;

static int  irq_mode = 0;               /* 1 = المؤقت يستقبل الإدخال */
static volatile int intr_pending = 0;   /* Ctrl-C ضُغط */
static int  fg_pid = 0;                 /* العملية الأمامية (يضبطها الـ shell عبر ioctl TIOCSPGRP) */

#define COM1 0x3F8

void tty_enable_irq(int on) { irq_mode = on; }
void tty_set_fg(int pid)    { fg_pid = pid; }
int  tty_foreground_pid(void) { return fg_pid; }

/* ---------------- الإخراج ---------------- */
static int esc_state = 0, esc_param = 0;

static void vga_csi_final(char c) {
    if (c == 'J' && esc_param == 2) terminal_clear();
    else if (c == 'K') terminal_clear_eol();
    /* باقي التسلسلات (ألوان، تحريك المؤشر) تُتجاهل على الشاشة */
}

void console_write(const char* buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        char ch = buf[i];
        /* serial: نمرّر البايتات كما هي (الطرفية المضيفة تفهم ESC)، مع \n -> \r\n */
        if (ch == '\n') serial_putc('\r');
        serial_putc(ch);
        /* الشاشة: آلة حالات صغيرة لتسلسلات ESC [ ... */
        if (esc_state == 0) {
            if (ch == 27) { esc_state = 1; continue; }
            terminal_console_putchar(ch);
        } else if (esc_state == 1) {
            if (ch == '[') { esc_state = 2; esc_param = 0; } else esc_state = 0;
        } else {
            if (ch >= '0' && ch <= '9') esc_param = esc_param * 10 + (ch - '0');
            else if (ch == ';') esc_param = 0;
            else { vga_csi_final(ch); esc_state = 0; }
        }
    }
}

/* ---------------- الإدخال ---------------- */
static void tty_feed(char c) {
    if (c == '\r') c = '\n';
    if (c == 3) {                                   /* Ctrl-C */
        console_write("^C\n", 3);
        line_len = 0;
        intr_pending = 1;
        return;
    }
    if (c == 21) {                                  /* Ctrl-U: امسح السطر */
        while (line_len > 0) { line_len--; console_write("\b \b", 3); }
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (line_len > 0) { line_len--; console_write("\b \b", 3); }
        return;
    }
    if (c == 4) {                                   /* Ctrl-D */
        if (line_len == 0) { ready_len = -1; }
        else { ready_len = line_len; ready_pos = 0; }
        return;
    }
    if ((unsigned char) c < 32 && c != '\n' && c != '\t') return;   /* أي محرف تحكم آخر (Ctrl-L، ESC...) يُتجاهل */
    if (line_len < LINE_MAX - 1) {
        line[line_len++] = c;
        console_write(&c, 1);
        if (c == '\n') { ready_len = line_len; ready_pos = 0; }
    }
}

/* يسحب الإدخال المتاح فقط بينما لا يوجد سطر جاهز (وإلا يبقى في مخازن العتاد حتى يُستهلك السطر) */
static void tty_drain(void) {
    while (ready_len == 0) {
        if (keyboard_haskey()) tty_feed(keyboard_getchar());
        else if (inb(COM1 + 5) & 0x01) tty_feed((char) inb(COM1));
        else break;
    }
}

void tty_irq_poll(void) { if (irq_mode) tty_drain(); }

int tty_take_interrupt(void) { int v = intr_pending; intr_pending = 0; return v; }

/* ترجع: >0 عدد البايتات، 0 = لا يوجد سطر جاهز (would block)، -1 = EOF */
int tty_read(char* buf, uint32_t len) {
    tty_drain();
    if (ready_len == 0) return 0;
    if (ready_len < 0) { ready_len = 0; line_len = 0; return -1; }
    uint32_t n = 0;
    while (n < len && ready_pos < ready_len) buf[n++] = line[ready_pos++];
    if (ready_pos >= ready_len) { ready_len = 0; line_len = 0; ready_pos = 0; }
    return (int) n;
}
