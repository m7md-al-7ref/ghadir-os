#include "pit.h"
#include "compat_abi.h"
#include "process.h"
#include "idt.h"
#include "io.h"
#include "pic.h"
#include "trust_window.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQ 1193182

static volatile uint32_t ticks = 0;

extern void schedule(struct registers* r); /* من process.c */

static void pit_callback(struct registers* r) {
    ticks++;
    /* نافذة الأمانة (كتاب الفلسفة، الفصل السادس) -- فحص خفيف لجدول
       صغير ثابت (16 slots)، بدون أي علاقة بـ schedule()/process.c،
       عمدًا، حتى لا تزيد مخاطرة البق الرابع المعروف. */
    trust_window_tick(ticks);
    process_clear_yielding();                 /* المنتظرون يعيدون المحاولة مرة واحدة في كل tick */
    tty_irq_poll();                           /* استقبال الكيبورد/serial (وضع الـ shell فقط) */
    if (tty_take_interrupt()) process_interrupt_foreground(r);   /* Ctrl-C */
    schedule(r);
}

void pit_init(uint32_t frequency_hz) {
    register_interrupt_handler(32, pit_callback); /* IRQ0 = int 32 */

    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_clear_mask(0);
}

uint32_t pit_get_ticks(void) { return ticks; }
