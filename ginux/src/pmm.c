#include "pmm.h"
#include "serial.h"
#include "spinlock.h"

static spinlock_t pmm_lock = SPINLOCK_INIT;

/* bitmap: كل bit يمثل frame بحجم 4KB. 1 = مستعمل، 0 = خالٍ.
   نستعمل bitmap ثابتًا يغطي حتى 4GB (128K frames -> 16KB bitmap) حتى نبقى
   بعيدين عن الحاجة لـ dynamic allocation قبل أن يكون لدينا heap. */
#define MAX_FRAMES (128 * 1024)
static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t total_frames = 0;
static uint32_t used_frames_count = 0;
static uint32_t highest_frame = 0;

static inline void bitmap_set(uint32_t bit) {
    bitmap[bit / 32] |= (1u << (bit % 32));
}
static inline void bitmap_clear(uint32_t bit) {
    bitmap[bit / 32] &= ~(1u << (bit % 32));
}
static inline int bitmap_test(uint32_t bit) {
    return bitmap[bit / 32] & (1u << (bit % 32));
}

static uint32_t find_first_free(void) {
    for (uint32_t i = 0; i < total_frames / 32 + 1; i++) {
        if (bitmap[i] != 0xFFFFFFFF) {
            for (uint32_t j = 0; j < 32; j++) {
                if (!(bitmap[i] & (1u << j))) {
                    uint32_t frame = i * 32 + j;
                    if (frame < total_frames) return frame;
                }
            }
        }
    }
    return 0xFFFFFFFF; /* لا يوجد فيه */
}

void pmm_init(struct multiboot_info* mbi, uint32_t kernel_end) {
    /* في البداية، نعلّم الكل "مستعملًا" حتى تبقى أي منطقة غير مذكورة في mmap محجوزة للأمان */
    for (uint32_t i = 0; i < MAX_FRAMES / 32; i++) bitmap[i] = 0xFFFFFFFF;

    if (!(mbi->flags & (1 << 6))) {
        kprintf("[pmm] WARNING: no memory map from bootloader, using mem_upper only\n");
        total_frames = ((mbi->mem_upper * 1024) + 0x100000) / PMM_FRAME_SIZE;
        if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;
        for (uint32_t f = (0x100000 / PMM_FRAME_SIZE); f < total_frames; f++)
            bitmap_clear(f);
    } else {
        struct multiboot_mmap_entry* entry = (struct multiboot_mmap_entry*)(uintptr_t) mbi->mmap_addr;
        uint32_t end = mbi->mmap_addr + mbi->mmap_length;

        while ((uint32_t)(uintptr_t) entry < end) {
            if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                uint64_t start_addr = entry->addr;
                uint64_t region_len = entry->len;
                uint64_t region_end = start_addr + region_len;

                uint32_t first_frame = (uint32_t)(start_addr / PMM_FRAME_SIZE);
                uint32_t last_frame  = (uint32_t)(region_end / PMM_FRAME_SIZE);
                if (last_frame > MAX_FRAMES) last_frame = MAX_FRAMES;

                for (uint32_t f = first_frame; f < last_frame; f++) {
                    bitmap_clear(f);
                    if (f > highest_frame) highest_frame = f;
                }
            }
            entry = (struct multiboot_mmap_entry*)((uint32_t)(uintptr_t)entry + entry->size + 4);
        }
        total_frames = highest_frame + 1;
    }

    /* حجز أول 1MB (BIOS/VGA/إلخ) + مساحة النواة نفسها من frame 0 لـ kernel_end */
    uint32_t reserved_end_frame = kernel_end / PMM_FRAME_SIZE + 1;
    for (uint32_t f = 0; f < reserved_end_frame && f < MAX_FRAMES; f++)
        bitmap_set(f);

    used_frames_count = 0;
    for (uint32_t f = 0; f < total_frames; f++)
        if (bitmap_test(f)) used_frames_count++;

    kprintf("[pmm] total_frames=%d  used=%d  free=%d  (%d MB free)\n",
            (int) total_frames, (int) used_frames_count,
            (int)(total_frames - used_frames_count),
            (int)((total_frames - used_frames_count) * 4 / 1024));
}

void pmm_reserve_region(uint32_t phys_start, uint32_t phys_end) {
    uint32_t f = spin_lock_irqsave(&pmm_lock, "pmm_reserve_region");
    uint32_t first_frame = phys_start / PMM_FRAME_SIZE;
    uint32_t last_frame  = (phys_end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE; /* exclusive */
    if (last_frame > MAX_FRAMES) last_frame = MAX_FRAMES;

    for (uint32_t frame = first_frame; frame < last_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames_count++;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, f);
}

uint32_t pmm_alloc_frame(void) {
    uint32_t f = spin_lock_irqsave(&pmm_lock, "pmm_alloc_frame");
    uint32_t frame = find_first_free();
    if (frame == 0xFFFFFFFF) {
        kprintf("[pmm] OUT OF MEMORY\n");
        spin_unlock_irqrestore(&pmm_lock, f);
        return 0;
    }
    bitmap_set(frame);
    used_frames_count++;
    spin_unlock_irqrestore(&pmm_lock, f);
    return frame * PMM_FRAME_SIZE;
}

void pmm_free_frame(uint32_t frame_addr) {
    uint32_t f = spin_lock_irqsave(&pmm_lock, "pmm_free_frame");
    uint32_t frame = frame_addr / PMM_FRAME_SIZE;
    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        used_frames_count--;
    }
    spin_unlock_irqrestore(&pmm_lock, f);
}

uint32_t pmm_free_frame_count(void) { return total_frames - used_frames_count; }
uint32_t pmm_total_frame_count(void) { return total_frames; }
