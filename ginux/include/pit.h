#ifndef PIT_H
#define PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency_hz);
uint32_t pit_get_ticks(void);

#endif
