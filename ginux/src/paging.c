#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include <stddef.h>

extern uint32_t text_start, text_end;
extern uint32_t rodata_start, rodata_end;
extern uint32_t data_start, data_end;
extern uint32_t bss_start, bss_end;

/* جداول ثابتة لخريطة identity الأساسية الخاصة بالنواة:
   PDPT (4 entries) -> PD (512 entries, غطاء 1GB بصفحات 2MB) -> لأول 4MB
   نستعمل PT بدقة 4KB حتى نستطيع إعطاء صلاحيات مختلفة لكل section (W^X الحقيقي) */
static uint64_t pdpt[4]              __attribute__((aligned(32)));
static uint64_t kernel_pd[512]       __attribute__((aligned(4096)));
static uint64_t kernel_pt_low[2][512] __attribute__((aligned(4096)));

static address_space_t kernel_as;
static int nx_available = 0;

static int cpuid_check_pae_nx(int* pae_out, int* nx_out) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(1));
    *pae_out = (edx >> 6) & 1;

    __asm__ volatile ("cpuid" : "=a"(eax) : "a"(0x80000000));
    if (eax < 0x80000001) { *nx_out = 0; return *pae_out; }

    __asm__ volatile ("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0x80000001));
    *nx_out = (edx >> 20) & 1;
    return *pae_out;
}

static void set_pt_entry(uint64_t* pt, int index, uint32_t phys, uint64_t flags) {
    pt[index] = ((uint64_t) phys & 0xFFFFFFFFFF000ULL) | flags;
}

int paging_supports_nx(void) { return nx_available; }

void paging_init(uint32_t kernel_phys_end) {
    int pae = 0, nx = 0;
    cpuid_check_pae_nx(&pae, &nx);
    nx_available = nx;

    kprintf("[paging] PAE=%d  NX=%d  kernel_end=%x\n", pae, nx, kernel_phys_end);
    if (!pae) {
        kprintf("[paging] FATAL: this CPU does not support PAE, cannot continue safely\n");
        for (;;) __asm__ volatile ("hlt");
    }
    if (!nx) {
        kprintf("[paging] WARNING: CPU has no NX bit -- W^X will NOT be hardware-enforced!\n");
    }

    for (int i = 0; i < 4; i++) pdpt[i] = 0;
    for (int i = 0; i < 512; i++) kernel_pd[i] = 0;

    /* أول 4MB: 4KB granularity، صلاحيات مختلفة حسب كل section */
    for (int pt_idx = 0; pt_idx < 2; pt_idx++) {
        for (int i = 0; i < 512; i++) {
            uint32_t addr = (uint32_t)(pt_idx * 0x200000 + i * 0x1000);
            uint64_t flags = PAGE_PRESENT;

            uint32_t ts = (uint32_t)&text_start, te = (uint32_t)&text_end;
            uint32_t rs = (uint32_t)&rodata_start, re = (uint32_t)&rodata_end;
            uint32_t ds = (uint32_t)&data_start, de = (uint32_t)&data_end;
            uint32_t bs = (uint32_t)&bss_start, be = (uint32_t)&bss_end;

            if (addr >= ts && addr < te) {
                flags |= 0; /* R-X: ليست writable، وليست NX */
            } else if (addr >= rs && addr < re) {
                flags |= (nx ? PAGE_NX : 0); /* R--: read only */
            } else if ((addr >= ds && addr < de) || (addr >= bs && addr < be)) {
                flags |= PAGE_WRITE | (nx ? PAGE_NX : 0); /* RW-: data */
            } else {
                /* منطقة عامة (0-1MB BIOS/VGA، أو مسافة فارغة) -> RW غير قابلة للتنفيذ */
                flags |= PAGE_WRITE | (nx ? PAGE_NX : 0);
            }

            set_pt_entry(kernel_pt_low[pt_idx], i, addr, flags);
        }
        kernel_pd[pt_idx] = ((uint64_t)(uint32_t) kernel_pt_low[pt_idx] & 0xFFFFFFFFFF000ULL)
                            | PAGE_PRESENT | PAGE_WRITE;
    }

    /* الباقي من الذاكرة المتوفرة: صفحات 2MB، RW + NX (بيانات فقط، غير قابلة للتنفيذ).
       نحدد سقفًا ثابتًا (512MB، مطابقًا لـ MAX_FRAMES في pmm) لخريطة identity، حتى لو كانت الذاكرة المتوفرة
       أكبر -- الباقي يبقى مُدارًا عبر PMM وتُملأ صفحاته عند الحاجة (kheap،
       user processes، إلخ) دون أن يتصادم مع identity map الثابتة. */
    uint32_t total_ram_bytes = pmm_total_frame_count() * PMM_FRAME_SIZE;
    /* تقريب للأعلى: PMM قد يوزّع إطارات في آخر منطقة 2MB الناقصة من الذاكرة؛ التقريب للأسفل
       كان يتركها خارج الـ identity map فتنهار النواة (page fault) عند أول memset فيها. */
    uint32_t pdes_needed = (total_ram_bytes + 0x1FFFFF) / 0x200000;
    #define IDENTITY_MAP_CAP_PDES 256 /* 256 * 2MB = 512MB = أقصى ما يغطيه pmm (MAX_FRAMES) */
    if (pdes_needed > IDENTITY_MAP_CAP_PDES) pdes_needed = IDENTITY_MAP_CAP_PDES;

    for (uint32_t i = 2; i < pdes_needed; i++) {
        uint32_t phys = i * 0x200000;
        uint64_t flags = PAGE_PRESENT | PAGE_WRITE | (1ULL << 7); /* PS=1: صفحة 2MB */
        if (nx) flags |= PAGE_NX;
        /* قناع صفحات 2MB في PAE: البتات 51..21. القناع القديم (0x...C00000) كان لصفحات 4MB
           فيمسح البت 21، فكل منطقة 2MB فردية (6MB، 10MB، ...) كانت تُطابَق على التي قبلها
           (6MB -> 4MB) وتكتب النواة فوق ذاكرة فيزيائية خاطئة. */
        kernel_pd[i] = ((uint64_t) phys & 0xFFFFFFFE00000ULL) | flags;
    }

    pdpt[0] = ((uint64_t)(uint32_t) kernel_pd & 0xFFFFFFFFFF000ULL) | PAGE_PRESENT;

    kernel_as.pdpt = pdpt;
    kernel_as.pdpt_phys = (uint32_t) pdpt;

    uint32_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 5); /* PAE */
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));

    if (nx) {
        uint32_t efer_lo, efer_hi;
        __asm__ volatile ("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080));
        efer_lo |= (1 << 11); /* NXE */
        __asm__ volatile ("wrmsr" :: "c"(0xC0000080), "a"(efer_lo), "d"(efer_hi));
    }

    __asm__ volatile ("mov %0, %%cr3" :: "r"((uint32_t) pdpt));

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 31); /* PG */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    kprintf("[paging] enabled: PAE + Paging%s. Identity-mapped %d MB.\n",
            nx ? " + NX(W^X)" : "", (int)(pdes_needed * 2));
    kprintf("[paging] kernel .text=[%x-%x] R-X  .rodata=[%x-%x] R--  .data/.bss=[%x-%x] RW-NX\n",
            (uint32_t)&text_start, (uint32_t)&text_end,
            (uint32_t)&rodata_start, (uint32_t)&rodata_end,
            (uint32_t)&data_start, (uint32_t)&bss_end);
}

address_space_t* paging_kernel_space(void) { return &kernel_as; }

/* --- إدارة address spaces منفصلة لكل process (عزل العمليات) --- */

address_space_t* paging_new_address_space(void) {
    /* bug حقيقي أُصلح (2026-08-17، مراجعة خارجية): كانت هذه الدالة
       لا تفحص فشل pmm_alloc_frame() إطلاقًا -- إذا نفدت الذاكرة
       الفيزيائية، كان pdpt_phys/as يرجعان 0، وبعدها new_pdpt[i]=...
       أو as->pdpt=... كانا يكتبان عبر NULL pointer (عنوان 0) -- تلف
       ذاكرة حقيقي في منطقة حساسة (أسفل الذاكرة الفيزيائية)، وليس مجرد
       crash نظيف. الآن نرجع NULL بوضوح عند أي فشل تخصيص. */
    uint32_t pdpt_phys = pmm_alloc_frame();
    if (!pdpt_phys) {
        kprintf("[paging] new_address_space: pmm_alloc_frame failed (pdpt)\n");
        return NULL;
    }
    uint64_t* new_pdpt = (uint64_t*) pdpt_phys; /* identity-mapped، فيمكن استعماله كـ pointer مباشرة */
    for (int i = 0; i < 4; i++) new_pdpt[i] = 0;

    /* PDPT[0] يشارك نفس kernel_pd حتى تستطيع أي process الوصول إلى كود النواة (syscalls/IDT/إلخ)
       بصلاحيات supervisor-only -- هذا أمر عادي وقياسي في كل الأنظمة (kernel shared, user separate) */
    new_pdpt[0] = pdpt[0];

    uint32_t as_phys = pmm_alloc_frame();
    if (!as_phys) {
        kprintf("[paging] new_address_space: pmm_alloc_frame failed (as struct)\n");
        pmm_free_frame(pdpt_phys); /* دون تسريب الـ frame التي حجزناها قبل الفشل */
        return NULL;
    }
    address_space_t* as = (address_space_t*) as_phys;
    as->pdpt = new_pdpt;
    as->pdpt_phys = pdpt_phys;
    return as;
}

static uint64_t* get_or_create_pd(address_space_t* as, int pdpt_idx) {
    if (!(as->pdpt[pdpt_idx] & PAGE_PRESENT)) {
        uint32_t pd_phys = pmm_alloc_frame();
        if (!pd_phys) {
            /* bug حقيقي أُصلح (2026-08-17، مراجعة خارجية ثانية): كان
               لا يوجد فحص هنا -- pd_phys=0 كان يتحول إلى pd=(uint64_t*)0،
               وبعدها `for (...) pd[i]=0` تكتب عبر NULL pointer مباشرة.
               الآن نرجع NULL بوضوح، والمستدعي (paging_map) مسؤول
               عن التعامل مع الفشل. */
            kprintf("[paging] get_or_create_pd: pmm_alloc_frame failed\n");
            return NULL;
        }
        uint64_t* pd = (uint64_t*) pd_phys;
        for (int i = 0; i < 512; i++) pd[i] = 0;
        as->pdpt[pdpt_idx] = (pd_phys & 0xFFFFFFFFFF000ULL) | PAGE_PRESENT;
    }
    return (uint64_t*)(uint32_t)(as->pdpt[pdpt_idx] & 0xFFFFFFFFFF000ULL);
}

static uint64_t* get_or_create_pt(uint64_t* pd, int pd_idx) {
    if (!pd) return NULL; /* pd نفسها فشلت في get_or_create_pd */
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            kprintf("[paging] get_or_create_pt: pmm_alloc_frame failed\n");
            return NULL;
        }
        uint64_t* pt = (uint64_t*) pt_phys;
        for (int i = 0; i < 512; i++) pt[i] = 0;
        pd[pd_idx] = (pt_phys & 0xFFFFFFFFFF000ULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    return (uint64_t*)(uint32_t)(pd[pd_idx] & 0xFFFFFFFFFF000ULL);
}

int paging_map(address_space_t* as, uint32_t virt, uint32_t phys, uint64_t flags) {
    int pdpt_idx = (virt >> 30) & 0x3;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t* pd = get_or_create_pd(as, pdpt_idx);
    uint64_t* pt = get_or_create_pt(pd, pd_idx);
    if (!pt) return 0; /* نفدت الذاكرة الفيزيائية لبنية الـ page tables نفسها */

    if (!nx_available) flags &= ~PAGE_NX;
    pt[pt_idx] = ((uint64_t) phys & 0xFFFFFFFFFF000ULL) | flags | PAGE_PRESENT;

    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return 1;
}

void paging_unmap(address_space_t* as, uint32_t virt) {
    int pdpt_idx = (virt >> 30) & 0x3;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(as->pdpt[pdpt_idx] & PAGE_PRESENT)) return;
    uint64_t* pd = (uint64_t*)(uint32_t)(as->pdpt[pdpt_idx] & 0xFFFFFFFFFF000ULL);
    if (!(pd[pd_idx] & PAGE_PRESENT)) return;
    uint64_t* pt = (uint64_t*)(uint32_t)(pd[pd_idx] & 0xFFFFFFFFFF000ULL);
    pt[pt_idx] = 0;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

uint32_t paging_get_phys(address_space_t* as, uint32_t virt) {
    int pdpt_idx = (virt >> 30) & 0x3;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(as->pdpt[pdpt_idx] & PAGE_PRESENT)) return 0;
    const uint64_t* pd = (const uint64_t*)(uint32_t)(as->pdpt[pdpt_idx] & 0xFFFFFFFFFF000ULL);
    if (!(pd[pd_idx] & PAGE_PRESENT)) return 0;
    if (pd[pd_idx] & (1ULL << 7)) return (uint32_t)(pd[pd_idx] & 0xFFFFFFFE00000ULL) + (virt & 0x1FFFFF);
    const uint64_t* pt = (const uint64_t*)(uint32_t)(pd[pd_idx] & 0xFFFFFFFFFF000ULL);
    if (!(pt[pt_idx] & PAGE_PRESENT)) return 0;
    return (uint32_t)(pt[pt_idx] & 0xFFFFFFFFFF000ULL) + (virt & 0xFFF);
}

uint64_t paging_get_flags(address_space_t* as, uint32_t virt) {
    int pdpt_idx = (virt >> 30) & 0x3;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(as->pdpt[pdpt_idx] & PAGE_PRESENT)) return 0;
    const uint64_t* pd = (const uint64_t*)(uint32_t)(as->pdpt[pdpt_idx] & 0xFFFFFFFFFF000ULL);
    if (!(pd[pd_idx] & PAGE_PRESENT)) return 0;
    if (pd[pd_idx] & (1ULL << 7)) return pd[pd_idx] & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
    const uint64_t* pt = (const uint64_t*)(uint32_t)(pd[pd_idx] & 0xFFFFFFFFFF000ULL);
    if (!(pt[pt_idx] & PAGE_PRESENT)) return 0;
    return pt[pt_idx] & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
}

void paging_switch(address_space_t* as) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(as->pdpt_phys) : "memory");
}
