#include "gdt.h"

#define GDT_ENTRIES 7

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gp;
static struct tss_entry tss;

extern void gdt_flush(uint32_t);
extern void tss_flush(void);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle  = (base >> 16) & 0xFF;
    gdt[num].base_high    = (base >> 24) & 0xFF;
    gdt[num].limit_low    = (limit & 0xFFFF);
    gdt[num].granularity  = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access       = access;
}

static void write_tss(int num, uint16_t ss0, uint32_t esp0) {
    uint32_t base  = (uint32_t) &tss;
    uint32_t limit = base + sizeof(tss);

    gdt_set_gate(num, base, limit, 0xE9, 0x00);

    for (unsigned i = 0; i < sizeof(tss); i++)
        ((uint8_t*)&tss)[i] = 0;

    tss.ss0  = ss0;
    tss.esp0 = esp0;
    /* CS/SS/DS/ES/FS/GS الافتراضية عند عودتنا من ring3 عبر iret لسنا بحاجة إليها هنا،
       فقط esp0/ss0 اللذان سيستعملهما المعالج ليبدّل الـ stack عند interrupt من ring3 */
    tss.cs = SEG_KERNEL_CODE;
    tss.ss = tss.ds = tss.es = tss.fs = tss.gs = SEG_KERNEL_DATA;
    tss.iomap_base = sizeof(tss);
}

void tss_set_kernel_stack(uint32_t stack) {
    tss.esp0 = stack;
}

void gdt_set_user_tls(uint32_t base) {
    gdt_set_gate(6, base, 0xFFFFFFFF, 0xF2, 0xCF);
}

void gdt_init(void) {
    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base  = (uint32_t) &gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                        /* null */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);          /* kernel code (ring0) */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);          /* kernel data (ring0) */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);          /* user code   (ring3) */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);          /* user data   (ring3) */
    write_tss(5, SEG_KERNEL_DATA, 0);                    /* TSS */
    gdt_set_gate(6, 0, 0xFFFFFFFF, 0xF2, 0xCF);          /* TLS (ring3 data، القاعدة تُضبط لكل عملية) */

    gdt_flush((uint32_t) &gp);
    tss_flush();
}
