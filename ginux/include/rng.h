#ifndef RNG_H
#define RNG_H

#include <stdint.h>

void rng_init(void);
uint32_t rng_next_u32(void);

#endif
