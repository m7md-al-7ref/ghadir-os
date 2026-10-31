#include "rng.h"
#include "serial.h"

static uint32_t state;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static int has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(1));
    return (ecx >> 30) & 1;
}

static uint32_t try_rdrand(void) {
    unsigned char ok = 0;
    uint32_t val = 0;
    /* rdrand تملأ val عبر inline asm -- cppcheck لا يتبع هذا ويظن أن
       val تبقى 0 في كلتا الحالتين؛ هذا false positive، والكود صحيح. */
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
    if (ok) {
        return val;
    }
    // cppcheck-suppress identicalConditionAfterEarlyExit
    return 0;
}

void rng_init(void) {
    uint64_t tsc = rdtsc();
    state = (uint32_t)(tsc ^ (tsc >> 32));

    if (has_rdrand()) {
        uint32_t hw = try_rdrand();
        if (hw) {
            state ^= hw;
            kprintf("[rng] seeded with RDTSC + RDRAND hardware entropy\n");
        } else {
            kprintf("[rng] RDRAND present but failed, using RDTSC only\n");
        }
    } else {
        kprintf("[rng] WARNING: no RDRAND, using RDTSC-only seed (weak entropy)\n");
    }

    if (state == 0) state = 0xA5A5A5A5;

    /* نخلط الحالة قليلًا حتى نبتعد عن seed بسيط */
    for (int i = 0; i < 16; i++) rng_next_u32();
}

/* xorshift32 -- PRNG سريع وغير آمن تشفيريًا، كافٍ لـ ASLR
   الخاص بـ toy kernel، وليس بديلًا لـ /dev/urandom في نظام حقيقي */
uint32_t rng_next_u32(void) {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}
