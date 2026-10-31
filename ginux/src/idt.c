#include "idt.h"
#include "pic.h"
#include "serial.h"
#include "process.h"

static struct idt_entry idt[256];
static struct idt_ptr   ip;
static isr_t interrupt_handlers[256];

extern void idt_flush(uint32_t);

/* ISR stubs (من isr.s) */
extern void isr0(void); extern void isr1(void); extern void isr2(void); extern void isr3(void);
extern void isr4(void); extern void isr5(void); extern void isr6(void); extern void isr7(void);
extern void isr8(void); extern void isr9(void); extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr128(void);

extern void irq0(void); extern void irq1(void); extern void irq2(void); extern void irq3(void);
extern void irq4(void); extern void irq5(void); extern void irq6(void); extern void irq7(void);
extern void irq8(void); extern void irq9(void); extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel      = sel;
    idt[num].always0  = 0;
    idt[num].flags    = flags;
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

static const char* exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved"
};

void isr_handler(struct registers* r) {
    if (interrupt_handlers[r->int_no] != 0) {
        interrupt_handlers[r->int_no](r);
        return;
    }

    if (r->int_no < 32) {
        /* CS.RPL == 3 يعني أن الخطأ جاء من ring3 (process مستخدم)، وليس من
           النواة نفسها. في هذه الحالة: نقتل هذه الـ process فقط ونكمل الجدولة
           للباقي -- بدون هذا، أي bug في برنامج مستخدم (null pointer،
           stack overflow، إلخ) كان سيُسقط النواة كاملة. */
        int from_user = (r->cs & 0x3) == 0x3;

        if (from_user) {
            uint32_t faulting_address = 0;
            if (r->int_no == 14) {
                __asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_address));
            }
            kprintf("\n*** USER FAULT: %s (int=%d err=%x eip=%x", 
                    exception_messages[r->int_no], (int)r->int_no, r->err_code, r->eip);
            if (r->int_no == 14) kprintf(" addr=%x", faulting_address);
            kprintf(") -- killing process, kernel continues ***\n");

            {   /* عمليات طبقة التوافق: الموت بسبب fault يُبلَّغ للأب كـ signal (wait status) */
                process_t* cu = process_current();
                if (cu && cu->abi_compat) {
                    int sig = (r->int_no == 0 || r->int_no == 16) ? 8 :   /* SIGFPE */
                              (r->int_no == 6) ? 4 :                      /* SIGILL */
                              (r->int_no == 17) ? 7 : 11;                 /* SIGBUS / SIGSEGV */
                    cu->exit_status = 0x10000 | sig;
                }
            }
            process_kill_current_and_reschedule(r);
            return; /* r الآن فيه trap frame الخاص بـ process أخرى -- iret سيقفز إليها */
        }

        /* خطأ جاء من النواة نفسها (ring0) -- هذا bug حقيقي في النواة،
           لا يستحق أن نكمل -- panic صحيح */
        kprintf("\n*** KERNEL PANIC: %s (int=%d err=%x eip=%x) ***\n",
                exception_messages[r->int_no], (int)r->int_no, r->err_code, r->eip);
        if (r->int_no == 14) {
            uint32_t faulting_address;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_address));
            kprintf("Faulting address: %x  present=%d write=%d user=%d\n",
                    faulting_address,
                    r->err_code & 0x1, (r->err_code >> 1) & 0x1, (r->err_code >> 2) & 0x1);
        }
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
}

void irq_handler(struct registers* r) {
    if (interrupt_handlers[r->int_no] != 0) {
        interrupt_handlers[r->int_no](r);
    }
    pic_send_eoi((uint8_t)(r->int_no - 32));
}

void idt_init(void) {
    ip.limit = sizeof(struct idt_entry) * 256 - 1;
    ip.base  = (uint32_t) &idt;

    for (int i = 0; i < 256; i++)
        idt_set_gate(i, 0, 0, 0);

    pic_remap(32, 40);

    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
    idt_set_gate(1, (uint32_t)isr1, 0x08, 0x8E);
    idt_set_gate(2, (uint32_t)isr2, 0x08, 0x8E);
    idt_set_gate(3, (uint32_t)isr3, 0x08, 0x8E);
    idt_set_gate(4, (uint32_t)isr4, 0x08, 0x8E);
    idt_set_gate(5, (uint32_t)isr5, 0x08, 0x8E);
    idt_set_gate(6, (uint32_t)isr6, 0x08, 0x8E);
    idt_set_gate(7, (uint32_t)isr7, 0x08, 0x8E);
    idt_set_gate(8, (uint32_t)isr8, 0x08, 0x8E);
    idt_set_gate(9, (uint32_t)isr9, 0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);

    /* 0x80: syscall gate - flags 0xEE يسمح لـ ring3 يستدعي int 0x80 (DPL=3) */
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);

    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3, 0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5, 0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6, 0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8, 0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9, 0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    idt_flush((uint32_t) &ip);
    __asm__ volatile ("sti");
}
