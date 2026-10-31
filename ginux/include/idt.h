#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* الحالة المحفوظة عند دخول أي interrupt/exception.
   الترتيب هنا يجب أن يطابق بالضبط ترتيب push في isr_common_stub:
   push %ds  ثم  pusha (EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI بالترتيب، ويبقى EDI
   في أسفل الـ stack). في الذاكرة الترتيب من الأسفل إلى الأعلى: EDI..EAX ثم DS ثم
   int_no/err_code (اللذان دفعناهما يدويًا) ثم ما دفعه المعالج تلقائيًا. */
struct registers {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t ds;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed));

typedef void (*isr_t)(struct registers*);

void idt_init(void);
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
void register_interrupt_handler(uint8_t n, isr_t handler);

#endif
