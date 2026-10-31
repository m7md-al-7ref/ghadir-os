#include "process.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "kstring.h"
#include "rng.h"
#include "serial.h"
#include "gdt.h"
#include "spinlock.h"
#include "elf.h"
#include "ledger.h"
#include "compat_abi.h"
#include "initrd.h"
#include "pipe.h"

static spinlock_t task_table_lock = SPINLOCK_INIT;

static process_t tasks[MAX_TASKS];
static int current_task_idx = -1;
static int scheduler_active = 0;

/* عناوين مناطق مساحة المستخدم (لكل process هنا PDPT entry خاص بها،
   وليس مشتركًا -- عزل حقيقي على مستوى جدول الصفحات) */
#define USER_CODE_REGION_BASE  0x40000000
#define USER_CODE_REGION_RANGE 0x00100000  /* 1MB لإمكانيات ASLR */
#define USER_STACK_REGION_BASE 0x50000000
#define USER_STACK_REGION_RANGE 0x00100000

extern uint8_t userland_demo_start[];
extern uint8_t userland_demo_end[];

extern void context_restore(struct registers* r) __attribute__((noreturn));

/* حالة FPU/SSE الأولية (بعد fninit) تُنسخ لكل process جديدة */
static uint8_t fpu_template[512] __attribute__((aligned(16)));
static int fpu_fxsr = 0;   /* 1 = المعالج يدعم FXSR+SSE: نستعمل fxsave/fxrstor (512 بايت) */

static inline void fpu_save(uint8_t* area) {
    if (fpu_fxsr) __asm__ volatile ("fxsave %0" : "=m"(*(uint8_t(*)[512]) area));
    else          __asm__ volatile ("fnsave %0" : "=m"(*(uint8_t(*)[112]) area));
}
static inline void fpu_restore(const uint8_t* area) {
    if (fpu_fxsr) __asm__ volatile ("fxrstor %0" :: "m"(*(const uint8_t(*)[512]) area));
    else          __asm__ volatile ("frstor %0" :: "m"(*(const uint8_t(*)[112]) area));
}

static void fpu_init(void) {
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(uint32_t)((1u << 2) | (1u << 3)); /* EM=0, TS=0 */
    cr0 |= (1u << 1) | (1u << 5);              /* MP=1, NE=1 */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    /* SSE: لو أعلن CPUID دعم FXSR وSSE فلازم نفعّل CR4.OSFXSR وOSXMMEXCPT، وإلا فأي تعليمة SSE
       في كود المستخدم تعطي #UD (glibc تختار memcpy/strrchr المبنية على SSE2 تلقائيًا من CPUID). */
    uint32_t ea = 1, eb, ec, ed;
    __asm__ volatile ("cpuid" : "+a"(ea), "=b"(eb), "=c"(ec), "=d"(ed));
    (void) eb; (void) ec;
    if ((ed & (1u << 24)) && (ed & (1u << 25))) {
        uint32_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 9) | (1u << 10);         /* OSFXSR | OSXMMEXCPT */
        __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));
        uint32_t mxcsr = 0x1F80;               /* القيمة الافتراضية: كل الاستثناءات مقنَّعة */
        __asm__ volatile ("fninit; ldmxcsr %0" :: "m"(mxcsr));
        fpu_fxsr = 1;
        fpu_save(fpu_template);
    } else {
        __asm__ volatile ("fninit");
        fpu_save(fpu_template);
    }
    kprintf("[fpu] init: %s\n", fpu_fxsr ? "x87+SSE (fxsave, 512B)" : "x87 only (fnsave)");
}

extern uint32_t user_gs_sel; /* boot/isr.s: selector يُحمَّل في gs عند الرجوع إلى ring3 */

/* يفعّل TLS الخاصة بالعملية القادمة: قاعدة مدخل GDT رقم 6 + selector الخاص بـ gs */
static void tls_activate(const process_t* p) {
    if (p->tls_valid) { gdt_set_user_tls(p->tls_base); user_gs_sel = SEG_USER_TLS; }
    else              { user_gs_sel = SEG_USER_DATA; }
}

void process_set_tls(process_t* p, uint32_t base) {
    p->tls_base = base; p->tls_valid = 1;
    tls_activate(p);
}

void process_init(void) {
    fpu_init();
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
        tasks[i].pid = i;
        tasks[i].parent_pid = -1; /* ليس لها أب إلى أن تُنشأ عبر fork() */
    }
    kprintf("[process] task table initialized (%d slots)\n", MAX_TASKS);
}

process_t* process_current(void) {
    if (current_task_idx < 0) return NULL;
    return &tasks[current_task_idx];
}

static int alloc_task_slot(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            /* slots تُعاد استعمالها: يجب مسح حقول طبقة التوافق حتى لا تتسرب من process سابقة */
            tasks[i].abi_compat = 0;
            memset(tasks[i].lfds, 0, sizeof(tasks[i].lfds));
            tasks[i].mmap_base = tasks[i].mmap_cur = 0;
            tasks[i].kstack_base = NULL;
            tasks[i].wake_tick = 0;
            tasks[i].tls_base = 0;
            tasks[i].yielding = 0;
            tasks[i].tls_valid = 0;
            tasks[i].cwd[0] = '\0';
            memcpy(tasks[i].fpu, fpu_template, sizeof(tasks[i].fpu));
            return i;
        }
    }
    return -1;
}

int process_create_kernel(void (*entry)(void), const char* name) {
    uint32_t f = spin_lock_irqsave(&task_table_lock, "process_create_kernel");
    int idx = alloc_task_slot();
    if (idx < 0) { kprintf("[process] no free task slots\n"); spin_unlock_irqrestore(&task_table_lock, f); return -1; }

    process_t* p = &tasks[idx];
    memset(&p->regs, 0, sizeof(p->regs));
    memset(&p->fds, 0, sizeof(p->fds)); /* لا توجد fds متسرّبة من process قديمة في نفس الـ slot */
    p->parent_pid = -1; /* إنشاء مباشر (بدون fork)، ليس لها أب ينتظر waitpid() */

    void* stack = kmalloc(KERNEL_STACK_SIZE);
    if (!stack) {
        /* bug حقيقي أُصلح (2026-08-17، مراجعة خارجية ثانية): لم يكن
           هناك فحص هنا -- kmalloc فاشلة كانت تجعل stack_top
           يُحسب من العنوان 0 حرفيًا (NULL + KERNEL_STACK_SIZE)، ونواصل
           بحالة process فاسدة بالكامل بلا أي رسالة تشخيصية. */
        kprintf("[process] create_kernel: out of memory (kernel stack) for %s\n", name);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    uint32_t stack_top = (uint32_t) stack + KERNEL_STACK_SIZE;
    stack_top &= ~0xF; /* محاذاة 16 بايت */

    p->regs.eip = (uint32_t) entry;
    p->regs.cs  = SEG_KERNEL_CODE;
    p->regs.ds  = SEG_KERNEL_DATA;
    p->regs.eflags = 0x202; /* IF=1 */
    p->regs.useresp = stack_top; /* غير مستعملة فعليًا لـ ring0->ring0 لكننا نملؤها للاتساق المنطقي */
    p->regs.ss = SEG_KERNEL_DATA;

    p->as = paging_kernel_space();
    p->kernel_stack_top = stack_top;
    p->is_user = 0;
    p->state = TASK_READY;
    int name_i = 0;
    for (; name_i < 15 && name[name_i]; name_i++) p->name[name_i] = name[name_i];
    p->name[name_i] = '\0'; /* لازمة -- قد يكون في الـ slot اسم process قديمة أطول (بقايا) */

    kprintf("[process] created kernel task pid=%d name=%s entry=%x\n", idx, name, (uint32_t) entry);
    ledger_record(LEDGER_EVT_PROC_CREATE_KERNEL, (uint32_t) idx, 0, name);
    spin_unlock_irqrestore(&task_table_lock, f);
    return idx;
}

int process_create_user(const void* code, uint32_t code_len, const char* name) {
    uint32_t f = spin_lock_irqsave(&task_table_lock, "process_create_user");
    int idx = alloc_task_slot();
    if (idx < 0) { kprintf("[process] no free task slots\n"); spin_unlock_irqrestore(&task_table_lock, f); return -1; }
    if (code_len > PMM_FRAME_SIZE) {
        kprintf("[process] demo loader only supports <= 4KB code\n");
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }

    process_t* p = &tasks[idx];
    memset(&p->regs, 0, sizeof(p->regs));
    memset(&p->fds, 0, sizeof(p->fds)); /* لا توجد fds متسرّبة من process قديمة في نفس الـ slot */
    p->parent_pid = -1; /* إنشاء مباشر (بدون fork)، ليس لها أب ينتظر waitpid() */
    /* لكل process address space خاص بها -- عزل حقيقي (لكل process
       page tables منفصلة لمساحة عناوين ring3) */
    p->as = paging_new_address_space();
    if (!p->as) {
        kprintf("[process] create_user: out of memory (address space) for %s\n", name);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    /* ASLR: نختار offset عشوائيًا محاذيًا لصفحة للكود وللـ stack */
    uint32_t code_page_off  = (rng_next_u32() % (USER_CODE_REGION_RANGE / PMM_FRAME_SIZE)) * PMM_FRAME_SIZE;
    uint32_t stack_page_off = (rng_next_u32() % (USER_STACK_REGION_RANGE / PMM_FRAME_SIZE)) * PMM_FRAME_SIZE;
    uint32_t code_vaddr  = USER_CODE_REGION_BASE  + code_page_off;
    uint32_t stack_vaddr = USER_STACK_REGION_BASE + stack_page_off;

    /* --- تحميل الكود: R-X فقط (ليس writable) = W^X حقيقي لبرنامج المستخدم --- */
    uint32_t code_phys = pmm_alloc_frame();
    if (!code_phys) {
        kprintf("[process] create_user: out of memory (code frame) for %s\n", name);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1; /* p->as leak موثق (من نفس فئة limitations §5) -- لا يوجد paging_free_address_space بعد */
    }
    memset((void*) code_phys, 0, PMM_FRAME_SIZE);
    memcpy((void*) code_phys, code, code_len);
    if (!paging_map(p->as, code_vaddr, code_phys, PAGE_PRESENT | PAGE_USER)) { /* بدون WRITE، بدون NX -> R-X */
        kprintf("[process] create_user: paging_map failed (code) for %s\n", name);
        pmm_free_frame(code_phys);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    /* --- الـ stack: RW فقط، وغير قابلة للتنفيذ (NX) = W^X من الجهة الأخرى --- */
    uint32_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        kprintf("[process] create_user: out of memory (stack frame) for %s\n", name);
        pmm_free_frame(code_phys);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    memset((void*) stack_phys, 0, PMM_FRAME_SIZE);
    if (!paging_map(p->as, stack_vaddr, stack_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX)) {
        kprintf("[process] create_user: paging_map failed (stack) for %s\n", name);
        pmm_free_frame(code_phys);
        pmm_free_frame(stack_phys);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }

    p->user_code_base  = code_vaddr;
    p->user_code_size  = PMM_FRAME_SIZE;
    p->user_stack_base = stack_vaddr;
    p->user_stack_size = PMM_FRAME_SIZE;
    p->brk_base = p->brk_current = USER_HEAP_BASE;

    /* kernel stack خاص بهذه الـ process -- يُستعمل عندما يقع interrupt/syscall
       من ring3 (TSS.esp0). غير مربوط في مساحة عناوين المستخدم = معزول */
    void* kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) {
        kprintf("[process] create_user: out of memory (kernel stack) for %s\n", name);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    uint32_t kstack_top = ((uint32_t) kstack + KERNEL_STACK_SIZE) & ~0xF;
    p->kernel_stack_top = kstack_top;

    p->regs.eip = code_vaddr;
    p->regs.cs  = SEG_USER_CODE;   /* RPL=3 مضمّنة في الـ selector */
    p->regs.ds  = SEG_USER_DATA;
    p->regs.eflags = 0x202;
    p->regs.useresp = stack_vaddr + PMM_FRAME_SIZE - 16;
    p->regs.ss  = SEG_USER_DATA;

    p->is_user = 1;
    p->state = TASK_READY;
    int name_i = 0;
    for (; name_i < 15 && name[name_i]; name_i++) p->name[name_i] = name[name_i];
    p->name[name_i] = '\0'; /* لازمة -- قد يكون في الـ slot اسم process قديمة أطول (بقايا) */

    kprintf("[process] created USER task pid=%d name=%s  code=%x(ASLR)  stack=%x(ASLR)\n",
            idx, name, code_vaddr, p->regs.useresp);
    ledger_record(LEDGER_EVT_PROC_CREATE_USER, (uint32_t) idx, 0, name);
    spin_unlock_irqrestore(&task_table_lock, f);
    return idx;
}

/* --- إنشاء process من ملف ELF32 حقيقي (statically-linked, non-PIE) --- */
int process_create_elf(const void* elf_data, uint32_t elf_size, const char* name) {
    uint32_t f = spin_lock_irqsave(&task_table_lock, "process_create_elf");
    int idx = alloc_task_slot();
    if (idx < 0) { kprintf("[process] no free task slots\n"); spin_unlock_irqrestore(&task_table_lock, f); return -1; }

    process_t* p = &tasks[idx];
    memset(&p->regs, 0, sizeof(p->regs));
    memset(&p->fds, 0, sizeof(p->fds)); /* لا توجد fds متسرّبة من process قديمة في نفس الـ slot */
    p->parent_pid = -1; /* إنشاء مباشر (بدون fork)، ليس لها أب ينتظر waitpid() */

    p->as = paging_new_address_space();
    if (!p->as) {
        kprintf("[process] create_elf: out of memory (address space) for %s\n", name);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }

    uint32_t entry = 0, min_v = 0, max_v = 0;
    if (!elf_load(elf_data, elf_size, p->as, &entry, &min_v, &max_v)) {
        kprintf("[process] ELF load failed for %s\n", name);
        p->state = TASK_UNUSED;
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }

    /* الـ stack: نعطيه ASLR مثل العمليات الأخرى، حتى لو كان كود ELF نفسه
       بعنوان ثابت (non-PIE) -- W^X: RW فقط، بدون تنفيذ */
    uint32_t stack_page_off = (rng_next_u32() % (USER_STACK_REGION_RANGE / PMM_FRAME_SIZE)) * PMM_FRAME_SIZE;
    uint32_t stack_vaddr = USER_STACK_REGION_BASE + stack_page_off;
    uint32_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        kprintf("[process] create_elf: out of memory (stack frame) for %s\n", name);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    memset((void*) stack_phys, 0, PMM_FRAME_SIZE);
    if (!paging_map(p->as, stack_vaddr, stack_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX)) {
        kprintf("[process] create_elf: paging_map failed (stack) for %s\n", name);
        pmm_free_frame(stack_phys);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }

    p->user_code_base  = min_v;
    p->user_code_size  = max_v - min_v;
    p->user_stack_base = stack_vaddr;
    p->user_stack_size = PMM_FRAME_SIZE;
    p->brk_base = p->brk_current = USER_HEAP_BASE;

    void* kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) {
        kprintf("[process] create_elf: out of memory (kernel stack) for %s\n", name);
        pmm_free_frame(stack_phys);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    uint32_t kstack_top = ((uint32_t) kstack + KERNEL_STACK_SIZE) & ~0xF;
    p->kernel_stack_top = kstack_top;

    p->regs.eip = entry;
    p->regs.cs  = SEG_USER_CODE;
    p->regs.ds  = SEG_USER_DATA;
    p->regs.eflags = 0x202;
    p->regs.useresp = stack_vaddr + PMM_FRAME_SIZE - 16;
    p->regs.ss  = SEG_USER_DATA;

    p->is_user = 1;
    p->state = TASK_READY;
    int name_i = 0;
    for (; name_i < 15 && name[name_i]; name_i++) p->name[name_i] = name[name_i];
    p->name[name_i] = '\0'; /* لازمة -- قد يكون في الـ slot اسم process قديمة أطول (بقايا) */

    kprintf("[process] created ELF task pid=%d name=%s entry=%x stack=%x(ASLR)\n",
            idx, name, entry, p->regs.useresp);
    ledger_record(LEDGER_EVT_PROC_CREATE_ELF, (uint32_t) idx, 0, name);
    spin_unlock_irqrestore(&task_table_lock, f);
    return idx;
}

/* --- execve(): تستبدل صورة process الحالية ببرنامج ELF جديد كليًا ---
 *
 * أول لبنة "توافق GNU حقيقي": بدون execve، لا تستطيع صدفة (shell) "تشغيل"
 * برنامج آخر بالطريقة العادية (fork ثم execve، مثل يونكس فعليًا).
 * قبل هذه الدالة، كانت كل process يجب أن تُنشأ مسبقًا وقت الترجمة
 * (process_create_elf مباشرة من kernel_main) -- execve هي أول مرة
 * يستطيع فيها برنامج أن "يبدّل جلده" وقت التشغيل الفعلي.
 *
 * الفرق عن process_create_elf: execve لا تنشئ process جديدة (pid يبقى
 * نفسه)، بل تبدّل المحتوى الداخلي الخاص بـ process موجودة -- بالضبط مثل
 * execve(2) الحقيقية في يونكس. كل الذاكرة القديمة (كود، stack، heap)
 * تتحرر، ومساحة عناوين جديدة بالكامل تُبنى للبرنامج الجديد.
 *
 * لا تُحرَّر: الملفات المفتوحة (fds) -- مثل يونكس، execve يحافظ عليها
 * (POSIX: تبقى fds مفتوحة عبر execve إلا إذا كان FD_CLOEXEC مضبوطًا، وليس لدينا
 * هذا المفهوم حاليًا في هذه النواة، فتبقى مفتوحة دائمًا، وهو أبسط سلوك ومطابق
 * للافتراضي).
 *
 * r هو trap frame الحقيقي الحالي (الـ syscall جاءت من int 0x80 ring3->ring0،
 * مثل fork()) -- نعدّل r مباشرة (eip/useresp/cs/ss/eflags) حتى عندما
 * ترجع common_return إلى ring3، ترجع مباشرة إلى بداية البرنامج الجديد، وليس
 * إلى السطر الذي جاء بعد استدعاء execve() (بالضبط مثل execve الحقيقية:
 * "لا ترجع إلا إذا فشلت"). ترجع 0 عند النجاح (لن يحدث فعليًا رجوع عادي
 * للمستدعي، فـ r نفسها هي التي تتبدل)، -1 عند الفشل (لا تلمس state الخاص بـ
 * process، ويكمل البرنامج القديم عاديًا مثل execve الحقيقية عند الفشل). */

/* تحرر كل الذاكرة المستخدمة (كود+stack+heap) في منطقة معينة من address
   space، دون أن تحرر منطقة النواة المشتركة (PDPT[0]) -- تستعملها execve
   قبل أن تبني مساحة عناوين جديدة. */
static void free_user_region(address_space_t* as, uint32_t base, uint32_t size) {
    if (size == 0) return;
    uint32_t start = base & ~0xFFF;
    uint32_t end = (base + size + 0xFFF) & ~0xFFF;
    for (uint32_t va = start; va < end; va += PMM_FRAME_SIZE) {
        uint32_t phys = paging_get_phys(as, va);
        if (!phys) continue;
        paging_unmap(as, va);
        pmm_free_frame(phys);
    }
}

/* --- argv حقيقية لـ execve() (2026-08-17) ---
 *
 * قبل هذا التحديث، كانت execve تبدأ البرنامج الجديد دائمًا بـ argc=0،
 * argv=NULL -- عائق حقيقي أمام أي صدفة (shell) تحاول تمرير أوامر
 * ("ls -la /home") إلى البرنامج المُشغَّل. كان هذا القيد موثقًا صراحة في
 * HANDOFF.md كأولوية تقنية عالية.
 *
 * البنية المبنية هنا معيار مبسّط شبيه بـ ABI يونكس القياسي عند بداية
 * البرنامج: [esp]=argc, [esp+4]=argv (مصفوفة مؤشرات + NULL)،
 * [esp+8]=envp (فارغة حاليًا -- NULL واحد فقط، وenvp الحقيقية مؤجلة
 * إلى تحديث لاحق، خارج نطاق هذا التحديث). ثوابت EXECVE_MAX_ARGV/
 * EXECVE_MAX_ARG_LEN معرَّفة في process.h (مشتركة مع syscall.c). */

/* تبني مكدسًا أوليًا (argc/argv/envp) داخل صفحة stack جديدة، وترجع قيمة
   esp الابتدائية الصحيحة. الكتابة الفعلية عبر العنوان الفيزيائي
   (identity-mapped، مرئي مباشرة من النواة مثل memset في الكود
   الأصلي)، بينما المؤشرات المخزَّنة داخل البنية تستعمل العناوين
   الافتراضية (stack_vaddr) -- وسيراها البرنامج الجديد صحيحة في مساحة
   عناوينه الخاصة وقت التنفيذ. */
static uint32_t build_initial_stack(uint32_t stack_phys, uint32_t stack_vaddr,
                                     int argc, char argv_data[][EXECVE_MAX_ARG_LEN]) {
    uint32_t cursor_phys = stack_phys + PMM_FRAME_SIZE;
    uint32_t str_vaddr[EXECVE_MAX_ARGV];

    /* 1. السلاسل النصية الخاصة بـ argv، من الأخير إلى الأول (نازلين في العنوان) */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = kstrlen(argv_data[i]) + 1; /* شامل \0 */
        cursor_phys -= len;
        cursor_phys &= ~0x3u; /* محاذاة 4 بايت -- لتفادي مشاكل الوصول غير المحاذى البسيطة */
        memcpy((void*) cursor_phys, argv_data[i], len);
        str_vaddr[i] = stack_vaddr + (cursor_phys - stack_phys);
    }

    /* 2. مصفوفة argv[] + NULL terminator (مثل ABI يونكس القياسي) */
    cursor_phys -= sizeof(uint32_t);
    *(uint32_t*) cursor_phys = 0; /* argv[argc] = NULL */
    for (int i = argc - 1; i >= 0; i--) {
        cursor_phys -= sizeof(uint32_t);
        *(uint32_t*) cursor_phys = str_vaddr[i];
    }
    uint32_t argv_vaddr = stack_vaddr + (cursor_phys - stack_phys);

    /* 3. envp[] -- فارغة حاليًا، NULL واحد فقط (envp الحقيقية = تحديث لاحق) */
    cursor_phys -= sizeof(uint32_t);
    *(uint32_t*) cursor_phys = 0;
    uint32_t envp_vaddr = stack_vaddr + (cursor_phys - stack_phys);

    /* 4. argc/argv/envp فوق esp مباشرة، بالترتيب الذي تتوقعه _start */
    cursor_phys -= sizeof(uint32_t);
    *(uint32_t*) cursor_phys = envp_vaddr;
    cursor_phys -= sizeof(uint32_t);
    *(uint32_t*) cursor_phys = argv_vaddr;
    cursor_phys -= sizeof(uint32_t);
    *(uint32_t*) cursor_phys = (uint32_t) argc;

    return stack_vaddr + (cursor_phys - stack_phys);
}

int process_execve(process_t* p, const void* elf_data, uint32_t elf_size,
                    const char* name, struct registers* r,
                    int argc, char argv_data[][EXECVE_MAX_ARG_LEN]) {
    if (!p || !p->is_user) return -1; /* execve() لبرامج ring3 فقط، مثل يونكس */

    /* 1. نتأكد أن البرنامج الجديد صالح *قبل* أن نمسح أي شيء قديم --
       فشل بعد أن نبدأ التحرير سيترك process في حالة نصف محررة، وهي خطيرة.
       نبني address space جديدة كاملة أولًا، منفصلة عن القديمة، ونحمّل
       فيها. إذا فشل التحميل، تبقى القديمة كاملة بدون مساس. */
    address_space_t* new_as = paging_new_address_space();
    if (!new_as) return -1;

    uint32_t entry = 0, min_v = 0, max_v = 0;
    if (!elf_load(elf_data, elf_size, new_as, &entry, &min_v, &max_v)) {
        kprintf("[process] execve: ELF load failed for %s (pid=%d unaffected, old image kept)\n",
                name, p->pid);
        /* لا يوجد paging_free_address_space حاليًا (limitation معروفة، مثل
           باقي memory leaks الموثقة في HANDOFF.md §5) -- تبقى new_as
           مخصصة بدون استعمال. مقبول مؤقتًا: فشل execve نادر (برنامج فاسد)،
           وليس مسارًا عاديًا متكررًا. */
        return -1;
    }

    uint32_t stack_page_off = (rng_next_u32() % (USER_STACK_REGION_RANGE / PMM_FRAME_SIZE)) * PMM_FRAME_SIZE;
    uint32_t stack_vaddr = USER_STACK_REGION_BASE + stack_page_off;
    uint32_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        kprintf("[process] execve: out of memory allocating stack for %s\n", name);
        return -1;
    }
    memset((void*) stack_phys, 0, PMM_FRAME_SIZE);
    if (!paging_map(new_as, stack_vaddr, stack_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX)) {
        kprintf("[process] execve: paging_map failed (stack) for %s (pid=%d unaffected, old image kept)\n",
                name, p->pid);
        pmm_free_frame(stack_phys);
        return -1; /* new_as leak موثق (من نفس فئة limitations §5) -- تبقى process القديمة كاملة */
    }

    /* 2. من هنا، لا يوجد رجوع للخلف -- كل شيء نجح، نبدأ بتحرير القديم
       ونستبدله. */
    address_space_t* old_as = p->as;
    free_user_region(old_as, p->user_code_base, p->user_code_size);
    free_user_region(old_as, p->user_stack_base, p->user_stack_size);
    if (p->brk_current > p->brk_base)
        free_user_region(old_as, p->brk_base, p->brk_current - p->brk_base);
    /* ملاحظة: بنى PDPT/PD/PT الخاصة بـ old_as نفسها (وليس المحتوى)
       لا تُحرَّر هنا -- نفس limitation الخاص بـ fork/exit، موثقة في HANDOFF. */

    p->as = new_as;
    paging_switch(p->as);

    p->user_code_base  = min_v;
    p->user_code_size  = max_v - min_v;
    p->user_stack_base = stack_vaddr;
    p->user_stack_size = PMM_FRAME_SIZE;
    p->brk_base = p->brk_current = USER_HEAP_BASE;

    int ni = 0;
    for (; ni < 15 && name[ni]; ni++) p->name[ni] = name[ni];
    p->name[ni] = '\0';

    /* 3. نعدّل r مباشرة -- هذا هو "الاستبدال" الفعلي: common_return
       سترجع إلى ring3 بهذه القيم، وليس بالقيم القديمة. */
    r->eip     = entry;
    r->useresp = build_initial_stack(stack_phys, stack_vaddr, argc, argv_data);
    r->cs      = SEG_USER_CODE;
    r->ds      = SEG_USER_DATA;
    r->ss      = SEG_USER_DATA;
    r->eflags  = 0x202;
    r->eax = r->ebx = r->ecx = r->edx = r->esi = r->edi = r->ebp = 0;

    kprintf("[process] execve: pid=%d -> %s entry=%x stack=%x(ASLR) argc=%d\n",
            p->pid, p->name, entry, r->useresp, argc);
    ledger_record(LEDGER_EVT_PROC_EXECVE, (uint32_t) p->pid, 0, p->name);

    return 0;
}

/* --- fork(): نسخ address space كامل --- */

/* تنسخ منطقة [base, base+size) صفحة بصفحة من src_as إلى dst_as، بنفس
   الصلاحيات بالضبط (W^X محفوظة). الصفحات غير الموجودة (holes) تُتجاوز --
   أمر عادي، مثلًا brk_current ليس محاذيًا لـ 4KB. ترجع 0 عند out-of-memory. */
static int copy_user_region(address_space_t* src_as, address_space_t* dst_as, uint32_t base, uint32_t size) {
    if (size == 0) return 1;
    uint32_t start = base & ~0xFFF;
    uint32_t end = (base + size + 0xFFF) & ~0xFFF;

    for (uint32_t va = start; va < end; va += PMM_FRAME_SIZE) {
        uint32_t src_phys = paging_get_phys(src_as, va);
        if (!src_phys) continue; /* hole -- ليست مخصصة أصلًا */

        uint64_t flags = paging_get_flags(src_as, va);
        uint32_t dst_phys = pmm_alloc_frame();
        if (!dst_phys) { kprintf("[process] fork: out of memory copying %x\n", va); return 0; }

        memcpy((void*) dst_phys, (void*) src_phys, PMM_FRAME_SIZE);
        if (!paging_map(dst_as, va, dst_phys, flags)) {
            kprintf("[process] fork: paging_map failed copying %x\n", va);
            pmm_free_frame(dst_phys);
            return 0;
        }
    }
    return 1;
}

int process_fork(process_t* parent, const struct registers* r) {
    uint32_t f = spin_lock_irqsave(&task_table_lock, "process_fork");
    int idx = alloc_task_slot();
    if (idx < 0) {
        kprintf("[process] fork: no free task slots\n");
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }

    process_t* p = &tasks[idx];
    memset(&p->fds, 0, sizeof(p->fds));

    p->as = paging_new_address_space();
    if (!p->as) {
        kprintf("[process] fork: out of memory (address space), pid=%d not created\n", idx);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }

    /* --- نسخ الكود (قد يكون عدة صفحات بصلاحيات مختلفة إذا كان ELF
       بعدة segments -- copy_user_region تحترم هذا عبر paging_get_flags) --- */
    int ok = copy_user_region(parent->as, p->as, parent->user_code_base, parent->user_code_size);
    ok = ok && copy_user_region(parent->as, p->as, parent->user_stack_base, parent->user_stack_size);
    if (parent->brk_current > parent->brk_base) {
        uint32_t heap_size = parent->brk_current - parent->brk_base;
        ok = ok && copy_user_region(parent->as, p->as, parent->brk_base, heap_size);
    }
    if (parent->abi_compat && parent->mmap_cur > parent->mmap_base)
        ok = ok && copy_user_region(parent->as, p->as, parent->mmap_base, parent->mmap_cur - parent->mmap_base);
    if (!ok) {
        kprintf("[process] fork failed (out of memory), pid=%d not created\n", idx);
        p->state = TASK_UNUSED; /* address space/frames المخصصة الآن: leak موثق، مثل process_kill */
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }

    p->user_code_base  = parent->user_code_base;
    p->user_code_size  = parent->user_code_size;
    p->user_stack_base = parent->user_stack_base;
    p->user_stack_size = parent->user_stack_size;
    p->brk_base    = parent->brk_base;
    p->brk_current = parent->brk_current;

    /* --- نسخ جدول fds: تبسيط موثق -- كل fd يصبح لديه نسخة خاصة به من
       الموقع (pos)، وليس "shared file description" حقيقية مثل Unix
       الحقيقي (الذي يشارك فيه fork نفس الموقع بين parent/child عبر
       نفس struct file). كافٍ لبرامج بسيطة، وليس دقيقًا 100% لبرامج
       تعتمد على هذا التفصيل (نادر جدًا). */
    memcpy(&p->fds, &parent->fds, sizeof(p->fds));

    p->tls_base = parent->tls_base;
    p->tls_valid = parent->tls_valid;
    if (parent->abi_compat) {
        p->abi_compat = 1;
        memcpy(p->lfds, parent->lfds, sizeof(p->lfds));
        for (int i = 0; i < LFD_MAX; i++)
        {
            if (p->lfds[i].type == LFD_FILE) initrd_ref_content(p->lfds[i].content_id);
            else if (p->lfds[i].type == LFD_PIPE_R) pipe_ref(p->lfds[i].pipe_id, 0);
            else if (p->lfds[i].type == LFD_PIPE_W) pipe_ref(p->lfds[i].pipe_id, 1);
            else if (p->lfds[i].type == LFD_DIR) compat_dir_ref(p->lfds[i].content_id);
        }
        memcpy(p->cwd, parent->cwd, sizeof(p->cwd));
        p->mmap_base = parent->mmap_base;
        p->mmap_cur  = parent->mmap_cur;
    }

    void* kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) {
        kprintf("[process] fork: out of memory (kernel stack)\n");
        p->state = TASK_UNUSED;
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    p->kstack_base = kstack;
    uint32_t kstack_top = ((uint32_t) kstack + KERNEL_STACK_SIZE) & ~0xF;
    p->kernel_stack_top = kstack_top;

    p->regs = *r;          /* الطفل يبدأ بالضبط من نفس نقطة الـ syscall */
    p->regs.eax = 0;        /* رجوع fork() في الطفل = 0 (مثل Unix) -- الأب سيملأ eax الخاص به بنفسه في syscall.c */

    p->is_user = 1;
    p->state = TASK_READY;
    p->parent_pid = parent->pid;
    int ni = 0;
    for (; ni < 15 && parent->name[ni]; ni++) p->name[ni] = parent->name[ni];
    p->name[ni] = '\0'; /* لازمة -- قد يكون في الـ slot اسم process قديمة أطول (بقايا) */

    kprintf("[process] fork: pid=%d (%s) -> child pid=%d\n", parent->pid, parent->name, p->pid);
    ledger_record(LEDGER_EVT_PROC_FORK, (uint32_t) p->pid, (uint32_t) parent->pid, p->name);
    spin_unlock_irqrestore(&task_table_lock, f);
    return p->pid;
}

int process_waitpid(int parent_pid, int target_pid, int* out_status) {
    uint32_t f = spin_lock_irqsave(&task_table_lock, "process_waitpid");
    int has_matching_child = 0;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) continue;
        if (tasks[i].parent_pid != parent_pid) continue;
        if (target_pid != -1 && tasks[i].pid != target_pid) continue;
        has_matching_child = 1;

        if (tasks[i].state == TASK_ZOMBIE) {
            int pid = tasks[i].pid;
            if (out_status) *out_status = tasks[i].exit_status;
            tasks[i].state = TASK_UNUSED; /* تحرّر الـ slot الآن -- fds/frames الخاصة بها: leak موثق */
            spin_unlock_irqrestore(&task_table_lock, f);
            return pid;
        }
    }

    spin_unlock_irqrestore(&task_table_lock, f);
    return has_matching_child ? 0 : -1; /* 0 = حي ما زال (non-blocking)، -1 = لا يوجد أي طفل */
}

/* --- Scheduler: round-robin بسيط، مبني فوق نفس trap frame الخاص بـ IRQ0 --- */


/* ---- اختيار العملية التالية ----
   الوضع العادي: round-robin بسيط. وضع الـ shell (sched_idle_last): العمليات التي انتظرت للتو (yielding) وkidle
   لا تُجدوَل ما دام هناك عملية أخرى جاهزة تعمل فعلًا؛ وإذا لم يبقَ إلا هي نشغّل kidle (hlt حتى المقاطعة التالية).
   بهذا لا يضيع نصف وقت المعالج على kidle أثناء انتظار الـ shell لـ gcc. */
static int sched_idle_last = 0;
void process_set_idle_last(int on) { sched_idle_last = on; }

void process_clear_yielding(void) {
    for (int i = 0; i < MAX_TASKS; i++) tasks[i].yielding = 0;
}

static int pick_next(int from) {
    if (sched_idle_last) {
        for (int i = 1; i <= MAX_TASKS; i++) {
            int idx = (from + i) % MAX_TASKS;
            if (idx != 0 && tasks[idx].state == TASK_READY && !tasks[idx].yielding) return idx;
        }
        if (tasks[0].state == TASK_READY) return 0;
        for (int i = 1; i <= MAX_TASKS; i++) {          /* احتياط: لا شيء غير المنتظِرين */
            int idx = (from + i) % MAX_TASKS;
            if (tasks[idx].state == TASK_READY) return idx;
        }
        return -1;
    }
    for (int i = 1; i <= MAX_TASKS; i++) {
        int idx = (from + i) % MAX_TASKS;
        if (tasks[idx].state == TASK_READY) return idx;
    }
    return -1;
}

void schedule(struct registers* r) {
    if (!scheduler_active) return;
    uint32_t f = spin_lock_irqsave(&task_table_lock, "schedule");

    if (current_task_idx >= 0 && tasks[current_task_idx].state == TASK_RUNNING) {
        tasks[current_task_idx].regs = *r;      /* نحفظ الحالة الحالية */
        tasks[current_task_idx].state = TASK_READY;
        fpu_save(tasks[current_task_idx].fpu);

        /* ring0: useresp في الـ frame هو الآن esp الأصلي الحقيقي (isr.s
           BUILD_FRAME يعيد تموضع الـ frame) -- لا حاجة لأي حيلة عناوين. */
    }

    int next = pick_next(current_task_idx);
    if (next < 0) { spin_unlock_irqrestore(&task_table_lock, f); return; } /* لا توجد process جاهزة */

    current_task_idx = next;
    tasks[next].state = TASK_RUNNING;

    if (!tasks[next].is_user) {
        uint32_t target = tasks[next].regs.useresp;
        uint32_t top = tasks[next].kernel_stack_top;
        if (target > top || target < top - KERNEL_STACK_SIZE) {
            kprintf("[BUG-DETECT] pid=%d corrupt target esp=%x (valid range %x-%x) eip=%x\n",
                    tasks[next].pid, target, top - KERNEL_STACK_SIZE, top, tasks[next].regs.eip);
            for (;;) { __asm__ volatile ("cli; hlt"); }
        }
    }

    *r = tasks[next].regs;               /* نبدّل الـ trap frame الحالي في مكانه */
    fpu_restore(tasks[next].fpu);
    tls_activate(&tasks[next]);
    tss_set_kernel_stack(tasks[next].kernel_stack_top);
    paging_switch(tasks[next].as);       /* CR3 جديد = عزل كامل للذاكرة */

    spin_unlock_irqrestore(&task_table_lock, f);
}

/* تقتل process الحالية (fault fatal أو exit طوعي) وتنتقل مباشرة إلى
   process أخرى جاهزة، دون أن توقف الجدولة كاملة. هذا أساس العزل: bug
   في برنامج مستخدم (page fault، div by zero، exit عادي) يؤثر فقط على هذه
   الـ process، والباقي يكملون العمل عاديًا.

   ملاحظة (leak معروف): لا نحرر frames الفيزيائية ولا الـ address space
   الخاص بـ process المقتولة الآن (كود، stack، page tables، kernel stack) --
   هذا يحتاج reference counting / cleanup حقيقي، مؤجل لمرحلة قادمة. في هذه
   المرحلة، كانت الأولوية "ألا تسقط النواة"، وليس "استرجاع الذاكرة". */
void process_kill_current_and_reschedule(struct registers* r) {
    uint32_t f = spin_lock_irqsave(&task_table_lock, "process_kill");

    int dead = current_task_idx;
    if (dead >= 0 && tty_foreground_pid() == dead) tty_set_fg(0);
    if (dead >= 0) {
        kprintf("[process] pid=%d (%s) exit(%d)\n", tasks[dead].pid, tasks[dead].name, tasks[dead].exit_status);
        ledger_record(LEDGER_EVT_PROC_EXIT, (uint32_t) tasks[dead].pid,
                       (uint32_t) tasks[dead].exit_status, tasks[dead].name);

        /* ppid دائمًا == index الخاص به (process_init يملأ tasks[i].pid=i مرة
           واحدة ولا تتبدل)، فـ tasks[ppid].state != TASK_UNUSED كافية حتى
           نتأكد أن "هذا الأب بالذات ما زال حيًا" بدون خطر تطابق pid خاطئ */
        int ppid = tasks[dead].parent_pid;
        int parent_alive = (ppid >= 0 && ppid < MAX_TASKS && tasks[ppid].state != TASK_UNUSED);

        if (parent_alive) {
            tasks[dead].state = TASK_ZOMBIE; /* بانتظار waitpid() -- الأب هو المسؤول عن تحريرها */
        } else {
            tasks[dead].state = TASK_UNUSED; /* يتيمة (ليست من fork، أو مات الأب قبلها) -- تُحرَّر فورًا */
        }
    }

    int next = pick_next(dead);

    if (next < 0) {
        kprintf("[scheduler] no tasks left to run -- halting\n");
        spin_unlock_irqrestore(&task_table_lock, f);
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }

    current_task_idx = next;
    tasks[next].state = TASK_RUNNING;
    *r = tasks[next].regs;
    fpu_restore(tasks[next].fpu);
    tls_activate(&tasks[next]);
    tss_set_kernel_stack(tasks[next].kernel_stack_top);
    paging_switch(tasks[next].as);

    /* الآن لم يعد CR3 يشير إلى address space الميت: نستطيع تحرير frames الخاصة به.
       (عمليات طبقة التوافق فقط -- عمليات الـ ABI القديم تبقى بسلوكها القديم
       الموثق: leak، حتى لا نغيّر شيئًا في اختباراتها). */
    if (dead >= 0 && tasks[dead].abi_compat && dead != next) {
        process_free_compat_image(&tasks[dead]);
        /* kernel stack: لا نستطيع تحريره الآن لأننا ما زلنا نعمل عليه حتى iret.
           نؤجل: نحرر stack الميت *السابق* (لم يعد أحد يعمل عليه). */
        static void* pending_kstack = NULL;
        if (pending_kstack) kfree(pending_kstack);
        pending_kstack = tasks[dead].kstack_base;
        tasks[dead].kstack_base = NULL;
    }

    spin_unlock_irqrestore(&task_table_lock, f);
}

void scheduler_start(void) {
    /* BUG #5 (الجذر الحقيقي، اكتُشف عبر gdb conditional breakpoint): sti
       تُستدعى مبكرًا جدًا في idt_init() (بداية kernel_main تقريبًا)، وpit_init()
       يُفعّل IRQ0 قبل أن تصل scheduler_start(). هذا ينشئ نافذة
       خطيرة: بين `scheduler_active=1` (السطر التالي) و context_restore() الفعلية،
       نكون ما زلنا نعمل فوق stack الخاص بـ kernel_main نفسها (boot stack)،
       وليس kidle->kernel_stack_top. إذا وقع IRQ0 بالضبط في هذه النافذة،
       فإن schedule() سترى current_task_idx==0 (kidle) و scheduler_active==1
       وتحاول حفظ "حالة kidle" لكنها فعليًا تحفظ بيانات من stack خاطئ تمامًا
       (boot stack الخاص بـ kernel_main، جزء من .bss/.data) -- وهذا يفسد
       tasks[0].regs.useresp بعنوان يشير إلى ذاكرة ستُقرأ وتتبدل لاحقًا
       (stack قديمة "متقاعدة"، مذكور في رسالة الإقلاع نفسها!) -- وعندما تعود
       الجدولة إلى kidle لاحقًا (في أي وقت)، يُقرأ هذا العنوان الفاسد
       كـ "eip" ويقفز إلى عنوان عشوائي = بالضبط البق الذي كنا نراه (crashes
       متقطعة، eip يصل إلى قيم مثل useresp الخاص بـ processes أخرى).
       الحل: نمنع المقاطعات بالكامل خلال هذه الفترة الحساسة -- eflags الخاص بـ
       kidle (0x202، IF=1) مبني أصلًا في process_create_kernel، فـ iret
       الأخيرة (في common_return، atomic بفضل إصلاح BUG #3) ستفعّل IF
       تلقائيًا بالضبط عندما تصل kidle إلى كودها الحقيقي على stack الخاص بها هي. */
    __asm__ volatile ("cli");

    int first = -1;
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_READY) { first = i; break; }

    if (first < 0) {
        kprintf("[scheduler] no tasks to run!\n");
        for (;;) __asm__ volatile ("hlt");
    }

    current_task_idx = first;
    tasks[first].state = TASK_RUNNING;
    scheduler_active = 1;

    kprintf("[scheduler] starting with pid=%d (%s)\n", first, tasks[first].name);

    tss_set_kernel_stack(tasks[first].kernel_stack_top);
    paging_switch(tasks[first].as);
    context_restore(&tasks[first].regs);
}

void process_dump(void) {
    kprintf("PID  NAME             STATE    TYPE\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) continue;
        const char* st = tasks[i].state == TASK_READY ? "READY" :
                          tasks[i].state == TASK_RUNNING ? "RUNNING" :
                          tasks[i].state == TASK_ZOMBIE ? "ZOMBIE" : "BLOCKED";
        kprintf("%d    %s  %s  %s\n", tasks[i].pid, tasks[i].name, st,
                tasks[i].is_user ? "user" : "kernel");
    }
}

/* مستعملة من process_create_user لنسخ برنامج userland التجريبي.
   ملاحظة: cppcheck يعطي false-positive هنا (comparePointers) لأنه ليس لديه
   معرفة بأن هذين الرمزين (start/end) يمثلان حدود نفس بلوكة الذاكرة التي
   حددها linker script -- هذا نمط عادي وآمن في برمجة الأنظمة (تعريف رموز
   حدودية عبر linker). */
// cppcheck-suppress comparePointers
uint32_t process_userland_demo_size(void) {
    // cppcheck-suppress comparePointers
    return (uint32_t)(userland_demo_end - userland_demo_start);
}
const void* process_userland_demo_ptr(void) {
    return (const void*) userland_demo_start;
}


/* ============================================================
 * عمليات طبقة التوافق (compat_abi.h)
 * ============================================================ */

compat_args_t* compat_args_new(void) {
    compat_args_t* a = kmalloc(sizeof(compat_args_t));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    a->data = kmalloc(COMPAT_ARGS_BYTES);
    if (!a->data) { kfree(a); return NULL; }
    return a;
}

void compat_args_free(compat_args_t* a) {
    if (!a) return;
    kfree(a->data);
    kfree(a);
}

static char* args_store(compat_args_t* a, const char* s) {
    uint32_t n = (uint32_t) kstrlen(s) + 1;
    if (a->used + n > COMPAT_ARGS_BYTES) return NULL;
    char* d = a->data + a->used;
    memcpy(d, s, n);
    a->used += n;
    return d;
}

int compat_args_add_arg(compat_args_t* a, const char* s) {
    if (a->argc >= COMPAT_MAX_ARGS) return 0;
    char* d = args_store(a, s);
    if (!d) return 0;
    a->argv[a->argc++] = d;
    return 1;
}

int compat_args_add_env(compat_args_t* a, const char* s) {
    if (a->envc >= COMPAT_MAX_ENV) return 0;
    char* d = args_store(a, s);
    if (!d) return 0;
    a->envp[a->envc++] = d;
    return 1;
}

/* كتابة بايتات في ذاكرة مستخدم عبر العنوان الفيزيائي (identity-mapped للنواة)،
   لأن stack الهدف قد لا يكون هو المفعَّل في CR3 الآن (execve/create). */
static void user_poke(address_space_t* as, uint32_t vaddr, const void* src, uint32_t len) {
    const uint8_t* s = (const uint8_t*) src;
    while (len) {
        uint32_t phys = paging_get_phys(as, vaddr & ~0xFFFu);
        uint32_t off = vaddr & 0xFFF;
        uint32_t n = PMM_FRAME_SIZE - off;
        if (n > len) n = len;
        memcpy((void*)(phys + off), s, n);
        vaddr += n; s += n; len -= n;
    }
}

typedef struct {
    uint32_t entry, esp, min_v, max_v, stack_base, stack_size;
} compat_image_t;

/* يحمّل ELF + يبني stack (256KB) + argv/envp/auxv كما تتوقعها glibc:
     [esp]=argc, argv[]..NULL, envp[]..NULL, auxv[] (AT_PHDR,AT_PHENT,
     AT_PHNUM,AT_PAGESZ,AT_BASE,AT_FLAGS,AT_ENTRY,AT_UID..AT_EGID,AT_NULL) */
static int compat_setup_image(address_space_t* as, const void* elf, uint32_t size,
                              const compat_args_t* a, const char* name, compat_image_t* img) {
    uint32_t entry = 0, minv = 0, maxv = 0;
    if (!elf_load(elf, size, as, &entry, &minv, &maxv)) return 0;
    uint32_t phdr = 0, phnum = 0;
    elf_phdr_info(elf, &phdr, &phnum);

    uint32_t range_pages = USER_STACK_REGION_RANGE / PMM_FRAME_SIZE;
    uint32_t top_pages = COMPAT_STACK_PAGES + (rng_next_u32() % (range_pages - COMPAT_STACK_PAGES));
    uint32_t top  = USER_STACK_REGION_BASE + top_pages * PMM_FRAME_SIZE;
    uint32_t base = top - COMPAT_STACK_PAGES * PMM_FRAME_SIZE;

    for (uint32_t va = base; va < top; va += PMM_FRAME_SIZE) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) { kprintf("[process] compat: out of memory (stack)\n"); return 0; }
        memset((void*) phys, 0, PMM_FRAME_SIZE);
        if (!paging_map(as, va, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX)) {
            pmm_free_frame(phys);
            return 0;
        }
    }

    uint32_t sp = top - 16;
    uint32_t arg_ptr[COMPAT_MAX_ARGS], env_ptr[COMPAT_MAX_ENV];

    uint32_t l = (uint32_t) kstrlen(name) + 1;
    sp -= l; user_poke(as, sp, name, l);                    /* AT_EXECFN */
    uint32_t execfn_ptr = sp;
    { static const char plat[] = "i686"; sp -= sizeof(plat); user_poke(as, sp, plat, sizeof(plat)); }
    uint32_t plat_ptr = sp;
    { /* AT_RANDOM: 16 بايت عشوائية (glibc الحديثة تأخذ منها stack canary و pointer guard) */
      uint32_t rb[4]; for (int i = 0; i < 4; i++) rb[i] = rng_next_u32();
      sp -= 16; sp &= ~15u; user_poke(as, sp, rb, 16); }
    uint32_t rnd_ptr = sp;
    uint32_t hwcap = 0;
    { uint32_t ea = 1, eb, ec, ed; __asm__ volatile ("cpuid" : "+a"(ea), "=b"(eb), "=c"(ec), "=d"(ed)); (void) eb; (void) ec; hwcap = ed; }
    for (int i = a->envc - 1; i >= 0; i--) {
        l = (uint32_t) kstrlen(a->envp[i]) + 1;
        sp -= l; user_poke(as, sp, a->envp[i], l); env_ptr[i] = sp;
    }
    for (int i = a->argc - 1; i >= 0; i--) {
        l = (uint32_t) kstrlen(a->argv[i]) + 1;
        sp -= l; user_poke(as, sp, a->argv[i], l); arg_ptr[i] = sp;
    }
    sp &= ~15u;

    uint32_t aux[] = { 3, phdr, 4, 32, 5, phnum, 6, PMM_FRAME_SIZE, 7, 0, 8, 0,
                       9, entry, 11, 0, 12, 0, 13, 0, 14, 0,
                       15, plat_ptr, 16, hwcap, 17, 100, 23, 0, 25, rnd_ptr, 26, 0, 31, execfn_ptr,
                       0, 0 };
    uint32_t naux = sizeof(aux) / sizeof(aux[0]);
    uint32_t nwords = 1 + (uint32_t)(a->argc + 1) + (uint32_t)(a->envc + 1) + naux;
    sp -= nwords * 4;
    sp &= ~15u;

    uint32_t arr[1 + (COMPAT_MAX_ARGS + 1) + (COMPAT_MAX_ENV + 1) + 48];
    uint32_t k = 0;
    arr[k++] = (uint32_t) a->argc;
    for (int i = 0; i < a->argc; i++) arr[k++] = arg_ptr[i];
    arr[k++] = 0;
    for (int i = 0; i < a->envc; i++) arr[k++] = env_ptr[i];
    arr[k++] = 0;
    for (uint32_t i = 0; i < naux; i++) arr[k++] = aux[i];
    user_poke(as, sp, arr, k * 4);

    img->entry = entry; img->esp = sp; img->min_v = minv; img->max_v = maxv;
    img->stack_base = base; img->stack_size = COMPAT_STACK_PAGES * PMM_FRAME_SIZE;
    return 1;
}

static void compat_apply_image(process_t* p, const compat_image_t* img) {
    p->user_code_base  = img->min_v;
    p->user_code_size  = img->max_v - img->min_v;
    p->user_stack_base = img->stack_base;
    p->user_stack_size = img->stack_size;
    p->brk_base = p->brk_current = USER_HEAP_BASE;
    p->mmap_base = p->mmap_cur = USER_MMAP_BASE;
    p->abi_compat = 1;
}

int process_create_compat(const void* elf_data, uint32_t elf_size, const char* name, const void* args) {
    const compat_args_t* a = (const compat_args_t*) args;
    uint32_t f = spin_lock_irqsave(&task_table_lock, "process_create_compat");
    int idx = alloc_task_slot();
    if (idx < 0) { kprintf("[process] no free task slots\n"); spin_unlock_irqrestore(&task_table_lock, f); return -1; }

    process_t* p = &tasks[idx];
    memset(&p->regs, 0, sizeof(p->regs));
    memset(&p->fds, 0, sizeof(p->fds));
    p->parent_pid = -1;

    p->as = paging_new_address_space();
    if (!p->as) { spin_unlock_irqrestore(&task_table_lock, f); return -1; }

    compat_image_t img;
    if (!compat_setup_image(p->as, elf_data, elf_size, a, name, &img)) {
        kprintf("[process] compat ELF load failed for %s\n", name);
        spin_unlock_irqrestore(&task_table_lock, f);
        return -1;
    }
    compat_apply_image(p, &img);

    void* kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) { spin_unlock_irqrestore(&task_table_lock, f); return -1; }
    p->kstack_base = kstack;
    p->kernel_stack_top = ((uint32_t) kstack + KERNEL_STACK_SIZE) & ~0xFu;

    /* fd 0/1/2 = console (tty) */
    for (int i = 0; i < 3; i++) { p->lfds[i].type = LFD_CONSOLE; p->lfds[i].flags = (i == 0) ? 0 : 1; }

    p->regs.eip = img.entry;
    p->regs.cs  = SEG_USER_CODE;
    p->regs.ds  = SEG_USER_DATA;
    p->regs.eflags = 0x202;
    p->regs.useresp = img.esp;
    p->regs.ss  = SEG_USER_DATA;

    p->is_user = 1;
    p->state = TASK_READY;
    int ni = 0;
    for (; ni < 15 && name[ni]; ni++) p->name[ni] = name[ni];
    p->name[ni] = '\0';

    kprintf("[process] created COMPAT task pid=%d name=%s entry=%x esp=%x argc=%d\n",
            idx, name, img.entry, img.esp, a->argc);
    ledger_record(LEDGER_EVT_PROC_CREATE_ELF, (uint32_t) idx, 0, name);
    spin_unlock_irqrestore(&task_table_lock, f);
    return idx;
}

int process_execve_compat(process_t* p, const void* elf_data, uint32_t elf_size, const char* name,
                         struct registers* r, const void* args) {
    const compat_args_t* a = (const compat_args_t*) args;
    if (!p || !p->is_user || !p->abi_compat) return -1;

    address_space_t* new_as = paging_new_address_space();
    if (!new_as) return -1;
    compat_image_t img;
    if (!compat_setup_image(new_as, elf_data, elf_size, a, name, &img)) {
        kprintf("[process] execve: compat ELF load failed for %s (pid=%d unaffected)\n", name, p->pid);
        return -1;
    }

    address_space_t* old_as = p->as;
    free_user_region(old_as, p->user_code_base, p->user_code_size);
    free_user_region(old_as, p->user_stack_base, p->user_stack_size);
    if (p->brk_current > p->brk_base) free_user_region(old_as, p->brk_base, p->brk_current - p->brk_base);
    if (p->mmap_cur > p->mmap_base)   free_user_region(old_as, p->mmap_base, p->mmap_cur - p->mmap_base);

    p->as = new_as;
    paging_switch(p->as);
    compat_apply_image(p, &img);

    /* FD_CLOEXEC */
    for (int i = 0; i < LFD_MAX; i++) {
        if (p->lfds[i].type != LFD_NONE && p->lfds[i].cloexec) {
            if (p->lfds[i].type == LFD_FILE) initrd_close_content(p->lfds[i].content_id);
            else if (p->lfds[i].type == LFD_PIPE_R) pipe_unref(p->lfds[i].pipe_id, 0);
            else if (p->lfds[i].type == LFD_PIPE_W) pipe_unref(p->lfds[i].pipe_id, 1);
            else if (p->lfds[i].type == LFD_DIR) compat_dir_unref(p->lfds[i].content_id);
            p->lfds[i].type = LFD_NONE;
        }
    }

    int ni = 0;
    for (; ni < 15 && name[ni]; ni++) p->name[ni] = name[ni];
    p->name[ni] = '\0';

    r->eip = img.entry; r->useresp = img.esp;
    r->cs = SEG_USER_CODE; r->ds = SEG_USER_DATA; r->ss = SEG_USER_DATA;
    r->eflags = 0x202;
    r->eax = r->ebx = r->ecx = r->edx = r->esi = r->edi = r->ebp = 0;

    kprintf("[process] execve(compat): pid=%d -> %s entry=%x esp=%x argc=%d\n", p->pid, p->name, img.entry, img.esp, a->argc);
    ledger_record(LEDGER_EVT_PROC_EXECVE, (uint32_t) p->pid, 0, p->name);
    return 0;
}

/* تحرير frames (كود/stack/heap/mmap) وإغلاق fds لـ process ميتة من طبقة التوافق.
   يُستدعى بعد paging_switch لـ address space آخر. */
void process_free_compat_image(process_t* p) {
    if (!p->as) return;
    free_user_region(p->as, p->user_code_base, p->user_code_size);
    free_user_region(p->as, p->user_stack_base, p->user_stack_size);
    if (p->brk_current > p->brk_base) free_user_region(p->as, p->brk_base, p->brk_current - p->brk_base);
    if (p->mmap_cur > p->mmap_base)   free_user_region(p->as, p->mmap_base, p->mmap_cur - p->mmap_base);
    p->user_code_size = p->user_stack_size = 0;
    p->brk_current = p->brk_base;
    p->mmap_cur = p->mmap_base;
    for (int i = 0; i < LFD_MAX; i++) {
        if (p->lfds[i].type == LFD_FILE) initrd_close_content(p->lfds[i].content_id);
        else if (p->lfds[i].type == LFD_PIPE_R) pipe_unref(p->lfds[i].pipe_id, 0);
        else if (p->lfds[i].type == LFD_PIPE_W) pipe_unref(p->lfds[i].pipe_id, 1);
        else if (p->lfds[i].type == LFD_DIR) compat_dir_unref(p->lfds[i].content_id);
        p->lfds[i].type = LFD_NONE;
    }
}

process_t* process_by_pid(int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return NULL;
    if (tasks[pid].state == TASK_UNUSED) return NULL;
    return &tasks[pid];
}


/* ---------------- Ctrl-C ---------------- */

static int is_descendant(int i, int root) {
    int p = tasks[i].parent_pid;
    for (int depth = 0; p >= 0 && p < MAX_TASKS && depth < MAX_TASKS; depth++) {
        if (p == root) return 1;
        p = tasks[p].parent_pid;
    }
    return 0;
}

/* قتل عملية ليست الحالية (محفوظة في tasks[]، لا تعمل الآن): نحرر مواردها فورًا ونترك الـ scheduler كما هو.
   zombie=1 للجذر فقط (الـ shell ينتظره بـ waitpid)؛ الفروع تُحرَّر مباشرة لأن أباها مقتول ولن ينتظرها أحد. */
static void kill_other(int idx, int zombie) {
    process_t* d = &tasks[idx];
    d->exit_status = 0x10000 | 2;                    /* SIGINT: يظهر للأب كإشارة (status 130 في الـ shell) */
    kprintf("[process] pid=%d (%s) interrupted (Ctrl-C)\n", d->pid, d->name);
    ledger_record(LEDGER_EVT_PROC_EXIT, (uint32_t) d->pid, (uint32_t) d->exit_status, d->name);
    d->state = zombie ? TASK_ZOMBIE : TASK_UNUSED;
    if (d->abi_compat) {
        process_free_compat_image(d);
        if (d->kstack_base) { kfree(d->kstack_base); d->kstack_base = NULL; }
    }
}

/* tty_foreground_pid(): قيمة موجبة P = اقتل P وكل أبنائه. قيمة سالبة -P = اقتل أبناء P فقط (وضع الـ shell:
   الـ shell يبقى حيًا، وأبناؤه المباشرون يصيرون zombie حتى يجمع الـ shell حالتهم بـ waitpid). */
void process_interrupt_foreground(struct registers* r) {
    int fg = tty_foreground_pid();
    if (fg == 0) return;
    int self_too = fg > 0;
    int root = self_too ? fg : -fg;
    if (root <= 0 || root >= MAX_TASKS) return;
    uint32_t f = spin_lock_irqsave(&task_table_lock, "interrupt_fg");
    if (!tasks[root].is_user || tasks[root].state == TASK_UNUSED || tasks[root].state == TASK_ZOMBIE) {
        spin_unlock_irqrestore(&task_table_lock, f);
        return;
    }
    int cur = current_task_idx, cur_in_tree = 0, hit = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (i == root) continue;
        if ((tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING) && is_descendant(i, root)) {
            int direct = (tasks[i].parent_pid == root);
            hit = 1;
            if (i == cur) { cur_in_tree = 1; if (!direct) tasks[i].parent_pid = -1; }   /* غير المباشر يُحرَّر مباشرة (لا zombie) */
            else kill_other(i, direct);
        }
    }
    if (self_too) {
        if (root == cur) cur_in_tree = 1; else kill_other(root, 1);
        tty_set_fg(0);
    }
    (void) hit;
    spin_unlock_irqrestore(&task_table_lock, f);
    if (cur_in_tree) {
        tasks[cur].exit_status = 0x10000 | 2;
        process_kill_current_and_reschedule(r);      /* يبدّل الـ frame إلى عملية أخرى */
    }
}
