#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include "serial.h"
#include "spinlock.h"

static spinlock_t kheap_lock = SPINLOCK_INIT;

/* Allocator بسيط مثل K&R: لكل بلوكة هيدر (size + free flag + next)،
   مرتبطة في linked list. نكبّر الـ heap (باستعمال pmm_alloc_frame + paging_map)
   عندما لا يكفي المكان، وليس مثل bump allocator بدون free. */

typedef struct block_header {
    size_t size;               /* حجم البلوكة (بدون الهيدر) */
    int free;
    struct block_header* next;
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)
#define HEAP_ALIGN 16

static uint32_t heap_start;
static uint32_t heap_end;      /* الحد الأقصى المخصص في الذاكرة الظاهرية الآن */
static uint32_t heap_max;      /* الحد الأقصى المسموح (حتى لا نتجاوزه إلى مناطق أخرى) */
static block_header_t* free_list_head = NULL;

static size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static int heap_expand(size_t min_increase) {
    size_t increase = align_up(min_increase, PMM_FRAME_SIZE);
    if (heap_end + increase > heap_max) {
        kprintf("[kheap] cannot expand: would exceed heap_max\n");
        return 0;
    }

    address_space_t* kas = paging_kernel_space();
    for (uint32_t addr = heap_end; addr < heap_end + increase; addr += PMM_FRAME_SIZE) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) {
            kprintf("[kheap] OUT OF PHYSICAL MEMORY during heap expand\n");
            return 0;
        }
        /* RW + NX: الكومة يجب ألا تكون قابلة للتنفيذ أبدًا (W^X) */
        if (!paging_map(kas, addr, frame, PAGE_PRESENT | PAGE_WRITE | PAGE_NX)) {
            kprintf("[kheap] paging_map failed during heap expand\n");
            pmm_free_frame(frame);
            return 0;
        }
    }

    heap_end += increase;
    return 1;
}

void kheap_init(uint32_t start_vaddr, uint32_t initial_size) {
    heap_start = start_vaddr;
    heap_end   = start_vaddr;
    heap_max   = start_vaddr + 0x0F000000; /* حد أقصى 240MB للـ kernel heap (تنتهي قبل 0x40000000 = بداية مساحة المستخدم) */

    if (!heap_expand(initial_size)) {
        kprintf("[kheap] FATAL: could not allocate initial heap\n");
        for (;;) __asm__ volatile ("hlt");
    }

    free_list_head = (block_header_t*) heap_start;
    free_list_head->size = heap_end - heap_start - HEADER_SIZE;
    free_list_head->free = 1;
    free_list_head->next = NULL;

    kprintf("[kheap] initialized at %x, initial size %d KB\n",
            heap_start, (int)((heap_end - heap_start) / 1024));
}

static void split_block(block_header_t* blk, size_t size) {
    if (blk->size >= size + HEADER_SIZE + HEAP_ALIGN) {
        block_header_t* new_blk = (block_header_t*)((uint8_t*)blk + HEADER_SIZE + size);
        new_blk->size = blk->size - size - HEADER_SIZE;
        new_blk->free = 1;
        new_blk->next = blk->next;

        blk->size = size;
        blk->next = new_blk;
    }
}

void* kmalloc(size_t size) {
    uint32_t f = spin_lock_irqsave(&kheap_lock, "kmalloc");
    size = align_up(size, HEAP_ALIGN);

    block_header_t* prev = NULL;
    block_header_t* cur = free_list_head;

    while (cur) {
        if (cur->free && cur->size >= size) {
            split_block(cur, size);
            cur->free = 0;
            spin_unlock_irqrestore(&kheap_lock, f);
            return (uint8_t*) cur + HEADER_SIZE;
        }
        prev = cur;
        cur = cur->next;
    }

    /* لم نجد بلوكة كافية -- نوسّع الـ heap */
    size_t needed = size + HEADER_SIZE;
    uint32_t old_end = heap_end;
    if (!heap_expand(needed)) { spin_unlock_irqrestore(&kheap_lock, f); return NULL; }

    block_header_t* new_blk = (block_header_t*) old_end;
    new_blk->size = (heap_end - old_end) - HEADER_SIZE;
    new_blk->free = 1;
    new_blk->next = NULL;

    if (prev) prev->next = new_blk; else free_list_head = new_blk;

    split_block(new_blk, size);
    new_blk->free = 0;
    spin_unlock_irqrestore(&kheap_lock, f);
    return (uint8_t*) new_blk + HEADER_SIZE;
}

static void coalesce(void) {
    block_header_t* cur = free_list_head;
    while (cur && cur->next) {
        const uint8_t* end_of_cur = (uint8_t*)cur + HEADER_SIZE + cur->size;
        if (cur->free && cur->next->free && end_of_cur == (uint8_t*) cur->next) {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint32_t f = spin_lock_irqsave(&kheap_lock, "kfree");
    block_header_t* blk = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);

    /* تحقق بسيط من سلامة المؤشر (heap bounds) قبل التحرير -- خط دفاع أول
       ضد استعمالات خاطئة (use-after-free العشوائي، double free بسيط) */
    if ((uint32_t) blk < heap_start || (uint32_t) blk >= heap_end) {
        kprintf("[kheap] SECURITY: kfree() called with out-of-heap pointer %x -- ignoring\n", (uint32_t) ptr);
        spin_unlock_irqrestore(&kheap_lock, f);
        return;
    }
    if (blk->free) {
        kprintf("[kheap] SECURITY: double-free detected at %x -- ignoring\n", (uint32_t) ptr);
        spin_unlock_irqrestore(&kheap_lock, f);
        return;
    }

    blk->free = 1;
    coalesce();
    spin_unlock_irqrestore(&kheap_lock, f);
}

void kheap_dump_stats(void) {
    size_t total_free = 0, total_used = 0, blocks = 0;
    block_header_t* cur = free_list_head;
    while (cur) {
        if (cur->free) total_free += cur->size; else total_used += cur->size;
        blocks++;
        cur = cur->next;
    }
    kprintf("[kheap] blocks=%d used=%d free=%d heap_size=%d KB\n",
            (int) blocks, (int) total_used, (int) total_free,
            (int)((heap_end - heap_start) / 1024));
}
