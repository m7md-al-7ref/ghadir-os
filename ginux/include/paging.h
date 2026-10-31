#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_PRESENT   (1ULL << 0)
#define PAGE_WRITE     (1ULL << 1)
#define PAGE_USER      (1ULL << 2)
#define PAGE_NX        (1ULL << 63)

/* هوية page directory كاملة لعملية (حتى يكون لدينا عزل: لكل عملية
   PDPT خاص بها، وبدلها يبدّل CR3 كامل خريطة الذاكرة التي تراها) */
typedef struct {
    uint64_t* pdpt;       /* virtual pointer للـ 4 entries */
    uint32_t  pdpt_phys;
} address_space_t;

void paging_init(uint32_t kernel_phys_end);
int  paging_supports_nx(void);

/* تعمل على العنوان الظاهري الحالي (kernel أو current process) */
/* ترجع 1 عند النجاح، 0 إذا فشل تخصيص بنية PD/PT الداخلية (نفدت
   الذاكرة الفيزيائية) -- انظر paging.c، bug حقيقي أُصلح 2026-08-17. */
int paging_map(address_space_t* as, uint32_t virt, uint32_t phys, uint64_t flags);
void paging_unmap(address_space_t* as, uint32_t virt);
uint32_t paging_get_phys(address_space_t* as, uint32_t virt);

/* ترجع flags الصفحة (PRESENT|WRITE|USER|NX) بدون الجزء الخاص بالعنوان
   الفيزيائي، أو 0 إذا كانت الصفحة غير موجودة. لازمة لـ fork() حتى يعيد
   بناء نفس الصلاحيات بالضبط (W^X) في address space الطفل. */
uint64_t paging_get_flags(address_space_t* as, uint32_t virt);

address_space_t* paging_new_address_space(void);
void paging_switch(address_space_t* as);
address_space_t* paging_kernel_space(void);

#endif
