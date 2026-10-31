#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>
#include <stdint.h>

void  kheap_init(uint32_t start_vaddr, uint32_t initial_size);
void* kmalloc(size_t size);
void  kfree(void* ptr);
void  kheap_dump_stats(void);

#endif
