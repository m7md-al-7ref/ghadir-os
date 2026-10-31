#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr_t;

#define ET_EXEC 2
#define PT_LOAD 1

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#include "paging.h"

int elf_validate(const void* data, uint32_t size);
uint32_t elf_get_entry(const void* data);

/* تحمّل كل الـ PT_LOAD segments في مساحة العناوين الخاصة بـ process معينة،
   بصلاحيات صحيحة لكل segment (W^X: أي segment قابل للتنفيذ ليس قابلًا
   للكتابة، والعكس). ترجع 1 إذا نجحت، 0 إذا فشلت (صيغة غير صالحة،
   segment يتداخل مع منطقة النواة، إلخ). */
int elf_load(const void* data, uint32_t size, address_space_t* as, uint32_t* out_entry,
             uint32_t* out_min_vaddr, uint32_t* out_max_vaddr);

int elf_phdr_info(const void* data, uint32_t* out_phdr_vaddr, uint32_t* out_phnum);

#endif
