#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "multiboot.h"

#define PMM_FRAME_SIZE 4096

void pmm_init(struct multiboot_info* mbi, uint32_t kernel_end);

/* تحجز [phys_start, phys_end) بالقوة (مثل frames الخاصة بالنواة نفسها) --
   ضرورية لأي منطقة ذاكرة مملوءة من bootloader (مثل multiboot modules)
   غير معروفة لـ pmm_init وقت بناء الخريطة الأولية، وإلا يمكن أن
   يعيد pmm_alloc_frame توزيعها على process أخرى فيكتب فوق محتواها. */
void pmm_reserve_region(uint32_t phys_start, uint32_t phys_end);

uint32_t pmm_alloc_frame(void);
void pmm_free_frame(uint32_t frame_addr);
uint32_t pmm_free_frame_count(void);
uint32_t pmm_total_frame_count(void);

#endif
