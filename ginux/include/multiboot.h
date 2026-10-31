#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
} __attribute__((packed));

/* عنصر واحد في لائحة mods_addr -- واحد لكل `module /path/to/file` مكتوبة
   في grub.cfg. mod_start/mod_end عناوين فيزيائية (identity-mapped حاليًا
   لأنها دائمًا في أول جزء صغير من الذاكرة الذي يضع GRUB فيه الـ modules). */
struct multiboot_mod_entry {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;   /* مؤشر لنص cmdline الخاص بالـ module (لا نستخدمه حاليًا) */
    uint32_t reserved;
} __attribute__((packed));

#define MULTIBOOT_INFO_MODS (1 << 3)

struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type; /* 1 = available RAM */
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

#endif
