#ifndef GDT_H
#define GDT_H

#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;      /* stack مستعمل عند القفز من ring3 لـ ring0 */
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* Segment selectors */
#define SEG_KERNEL_CODE 0x08
#define SEG_KERNEL_DATA 0x10
#define SEG_USER_CODE   0x1B  /* RPL=3 */
#define SEG_USER_DATA   0x23  /* RPL=3 */
#define SEG_TSS         0x28
#define SEG_USER_TLS    0x33  /* RPL=3: مدخل GDT رقم 6، قاعدته تتبدل لكل عملية (set_thread_area) */

void gdt_init(void);
/* يضبط قاعدة مدخل TLS (segment بيانات ring3 بحد 4GB). يُستدعى عند set_thread_area وعند كل تبديل سياق */
void gdt_set_user_tls(uint32_t base);
void tss_set_kernel_stack(uint32_t stack);

#endif
