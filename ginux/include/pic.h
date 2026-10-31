#ifndef PIC_H
#define PIC_H

#include <stdint.h>

void pic_remap(int offset1, int offset2);
void pic_send_eoi(uint8_t irq);
void irq_set_mask(uint8_t irq);
void irq_clear_mask(uint8_t irq);

#endif
