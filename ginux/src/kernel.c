#ifndef GHADIR_VERSION
#define GHADIR_VERSION "dev"
#endif

#include "vga.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "keyboard.h"
#include "multiboot.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "rng.h"
#include "pit.h"
#include "process.h"
#include "compat_abi.h"
#include "syscall.h"
#include "pipe.h"
#include "initrd.h"
#include "ledger.h"
#include "trust_window.h"
#include "spinlock.h"
#include <stdint.h>

#define MULTIBOOT_MAGIC 0x2BADB002

extern uint32_t kernel_end;
extern void stack_protector_init(void);

extern uint8_t hello_elf_start[];
extern uint8_t hello_elf_end[];
extern uint8_t pipe_writer_start[];
extern uint8_t pipe_writer_end[];
extern uint8_t pipe_reader_start[];
extern uint8_t pipe_reader_end[];
extern uint8_t gng_hello_start[];
extern uint8_t gng_hello_end[];
extern uint8_t cat_motd_elf_start[];
extern uint8_t cat_motd_elf_end[];
extern uint8_t fork_test_elf_start[];
extern uint8_t fork_test_elf_end[];
extern uint8_t writable_test_elf_start[];
extern uint8_t writable_test_elf_end[];
extern uint8_t separation_test_elf_start[];
extern uint8_t separation_test_elf_end[];
extern uint8_t fd_identity_test_elf_start[];
extern uint8_t fd_identity_test_elf_end[];
extern uint8_t trust_window_test_elf_start[];
extern uint8_t trust_window_test_elf_end[];

/* --- Task تجريبية ثانية بلغة C، تخدم في ring0 (kernel task) --- */
static volatile int g_idle_quiet = 0;   /* وضع الـ shell: kidle لا يطبع شيئًا، فقط ينام حتى المقاطعة التالية */

static void demo_kernel_task(void) {
    uint32_t counter = 0;
    if (g_idle_quiet) for (;;) __asm__ volatile ("hlt");
    for (;;) {
        counter++;
        if (counter % 50000 == 0) {
            kprintf("[kernel-task] still alive, tick group #%d\n", (int)(counter / 50000));
        }
    }
}

void kernel_main(uint32_t magic, uint32_t mbi_addr) {
    terminal_initialize();
    serial_init();

    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    terminal_writestring("=========================================\n");
    terminal_writestring("          GINUX KERNEL " GHADIR_VERSION "                \n");
    terminal_writestring("   +Spinlocks +ELF +Pipes +GNG +Fault  \n");
    terminal_writestring("            Isolation +Syscalls          \n");
    terminal_writestring("=========================================\n\n");
    terminal_setcolor(VGA_COLOR_LIGHT_GREY);

    kprintf("[boot] multiboot magic=%x  mbi=%x\n", magic, mbi_addr);
    if (magic != MULTIBOOT_MAGIC) {
        kprintf("[boot] WARNING: not loaded via a valid multiboot loader\n");
    }
    struct multiboot_info* mbi = (struct multiboot_info*)(uintptr_t) mbi_addr;

    kprintf("[gdt] initializing...\n");
    gdt_init();
    kprintf("[gdt] done (kernel + user segments + TSS loaded)\n");

    kprintf("[idt] initializing...\n");
    idt_init();
    kprintf("[idt] done (32 exceptions + 16 IRQs + syscall gate)\n");

    kprintf("[pmm] initializing...\n");
    pmm_init(mbi, (uint32_t)&kernel_end);

    /* --- initrd: يجب أن يُحجز في pmm فورًا بعد pmm_init، قبل أي تخصيص
       آخر (paging/kheap/process...) -- وإلا قد يعيد pmm_alloc_frame
       توزيع نفس الـ frames الخاصة به على process أخرى فتُكتب فوق المحتوى. GRUB
       يضع modules في ذاكرة منخفضة identity-mapped، فنستطيع قراءتها
       مباشرة بدون paging إضافي. */
    uint32_t initrd_start = 0, initrd_end = 0;
    if ((mbi->flags & MULTIBOOT_INFO_MODS) && mbi->mods_count > 0) {
        struct multiboot_mod_entry* mods = (struct multiboot_mod_entry*)(uintptr_t) mbi->mods_addr;
        initrd_start = mods[0].mod_start;
        initrd_end   = mods[0].mod_end;
        pmm_reserve_region(initrd_start, initrd_end);
        kprintf("[boot] initrd module found: [%x-%x) (%d bytes), reserved in pmm\n",
                initrd_start, initrd_end, initrd_end - initrd_start);
    } else {
        kprintf("[boot] no initrd module loaded by GRUB -- filesystem will be empty\n");
    }
    initrd_init(initrd_start, initrd_end);

    kprintf("[paging] initializing...\n");
    paging_init((uint32_t)&kernel_end);

    kprintf("[rng] initializing (for ASLR + stack canaries)...\n");
    rng_init();
    stack_protector_init();

    kprintf("[kheap] initializing...\n");
    #define KHEAP_VIRT_BASE 0x30000000 /* 768MB -- فوق سقف identity map (512MB) وتحت مساحة المستخدم (1GB) */
    kheap_init(KHEAP_VIRT_BASE, 1024 * 1024);

    kprintf("[keyboard] initializing...\n");
    keyboard_init();

    kprintf("[syscall] initializing...\n");
    syscall_init();

    kprintf("[ledger] initializing...\n");
    ledger_init();

    kprintf("[trust_window] initializing...\n");
    trust_window_init();

    kprintf("[process] initializing scheduler...\n");
    process_init();

    kprintf("[pipe] initializing...\n");
    pipe_init();
    int boot_pipe = pipe_create(); /* pipe id=0 -- نضمن أنها موجودة قبل العمليات */
    kprintf("[pipe] pre-created boot pipe id=%d for writer/reader demo\n", boot_pipe);

    process_create_kernel(demo_kernel_task, "kidle");
    int init_idx = initrd_find("init");
    if (init_idx >= 0) {
        /* وجود ملف "init" في initrd = وضع طبقة التوافق: نشغّله وحده (بدون عروض
           الـ ABI القديم) -- هذا هو المسار المستعمل لتجارب glibc. */
        const initrd_file_t* ini = initrd_get(init_idx);
        const void* ini_data = ini ? ini->data : NULL;
        uint32_t ini_size = ini ? ini->size : 0;
        compat_args_t* la = compat_args_new();
        if (la && ini_data) {
            compat_args_add_arg(la, "/init");
            compat_args_add_env(la, "PATH=/");
            compat_args_add_env(la, "HOME=/");
            compat_args_add_env(la, "TERM=ginux");
            process_create_compat(ini_data, ini_size, "init", la);
        }
        /* وضع الـ shell: الشاشة ملك الـ tty. الإدخال يُستقبل من مقاطعة المؤقت، وkidle يأتي أخيرًا في الجدولة،
           وسجلات النواة تبقى على serial فقط (تشخيص) ولا تلوّث الشاشة. */
        g_idle_quiet = 1;
        process_set_idle_last(1);
        tty_enable_irq(1);
        terminal_set_log_enabled(0);
        terminal_clear();
        compat_args_free(la);
    } else {
    process_create_user(process_userland_demo_ptr(), process_userland_demo_size(), "userdemo1");
    process_create_user(process_userland_demo_ptr(), process_userland_demo_size(), "userdemo2");
    // cppcheck-suppress comparePointers
    process_create_elf(hello_elf_start, (uint32_t)(hello_elf_end - hello_elf_start), "elf-hello");
    // cppcheck-suppress comparePointers
    process_create_user(pipe_writer_start, (uint32_t)(pipe_writer_end - pipe_writer_start), "pipe-writer");
    // cppcheck-suppress comparePointers
    process_create_user(pipe_reader_start, (uint32_t)(pipe_reader_end - pipe_reader_start), "pipe-reader");
    // cppcheck-suppress comparePointers
    process_create_elf(gng_hello_start, (uint32_t)(gng_hello_end - gng_hello_start), "gng-hello");
    // cppcheck-suppress comparePointers
    process_create_elf(cat_motd_elf_start, (uint32_t)(cat_motd_elf_end - cat_motd_elf_start), "cat-motd");
    // cppcheck-suppress comparePointers
    process_create_elf(fork_test_elf_start, (uint32_t)(fork_test_elf_end - fork_test_elf_start), "fork-test");
    // cppcheck-suppress comparePointers
    process_create_elf(writable_test_elf_start, (uint32_t)(writable_test_elf_end - writable_test_elf_start), "writable-test");
    process_create_elf(separation_test_elf_start, (uint32_t)(separation_test_elf_end - separation_test_elf_start), "separation-test");
    process_create_elf(fd_identity_test_elf_start, (uint32_t)(fd_identity_test_elf_end - fd_identity_test_elf_start), "fd-identity-test");
    // cppcheck-suppress comparePointers
    process_create_elf(trust_window_test_elf_start, (uint32_t)(trust_window_test_elf_end - trust_window_test_elf_start), "trust-window-test");
    }

    process_dump();

    kprintf("[pit] starting timer (100Hz preemptive scheduling)...\n");
    pit_init(100);

    /* --- اختبار حي لكشف الاحتكار (كتاب الفلسفة، الفصل الثالث عشر) ---
       نحجز قفلًا تجريبيًا عمدًا لمدة طويلة (busy-loop، وليس pit ticks --
       لا تتقدم أصلًا أثناء الحجز نفسه لأن IF=0، انظر spinlock.h)،
       ونتأكد أن الكشف اشتغل فعلًا -- إثبات حي، وليس مجرد كود ميت. */
    {
        static spinlock_t test_lock = SPINLOCK_INIT;
        uint32_t f = spin_lock_irqsave(&test_lock, "monopoly_self_test");
        uint32_t lo0, hi0;
        __asm__ volatile ("rdtsc" : "=a"(lo0), "=d"(hi0));
        uint64_t start = ((uint64_t) hi0 << 32) | lo0;
        uint64_t elapsed;
        do {
            uint32_t lo, hi;
            __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
            elapsed = (((uint64_t) hi << 32) | lo) - start;
        } while (elapsed < SPINLOCK_MONOPOLY_THRESHOLD_CYCLES + 20000000ULL);
        spin_unlock_irqrestore(&test_lock, f);
        uint32_t count = spinlock_monopoly_count(&test_lock);
        if (count > 0) {
            kprintf("[selftest] monopoly detection: PASS (detected %d time(s) as expected)\n", count);
        } else {
            kprintf("[selftest] monopoly detection: FAIL (expected detection, got none!)\n");
        }
    }

    terminal_setcolor(VGA_COLOR_WHITE);
    terminal_writestring("\nAll subsystems online. Starting scheduler...\n");
    kprintf("[boot] handing off to scheduler -- kernel_main's own stack is now retired\n");

    /* إثبات حي: كل عمليات إنشاء/fork الخاصة بالعمليات حتى هنا مسجّلة في
       السجل اللارجعي (ledger.c) -- انظر كتاب الفلسفة، الفصل السابع
       ("عدم توثيق الذات"). أي process تستطيع قراءة هذه البيانات عبر
       SYS_LEDGER_READ لاحقًا. */
    ledger_dump_summary();

    scheduler_start();

    /* لا نصل إلى هنا أبدًا */
    for (;;) __asm__ volatile ("hlt");
}
