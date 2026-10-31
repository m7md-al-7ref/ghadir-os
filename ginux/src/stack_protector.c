#include <stdint.h>
#include "serial.h"
#include "rng.h"

/* GCC/Clang يتوقعان هذا الاسم بالضبط لـ -fstack-protector.
   نبدؤه بقيمة ثابتة بسيطة عند الإقلاع (قبل أن يصبح RNG جاهزًا)،
   ونستبدله بقيمة عشوائية حقيقية عندما يبدأ RNG (stack_protector_init). */
uintptr_t __stack_chk_guard = 0x0BAD0BAD;

void stack_protector_init(void) {
    uint32_t r1 = rng_next_u32();
    uint32_t r2 = rng_next_u32();
    /* نتفادى أي بايت = 0 حتى نصعّب استغلال off-by-one/string overflow الذي
       يتوقف عند null byte -- تقنية مستعملة فعليًا في glibc/gcc */
    uintptr_t guard = ((uintptr_t) r1 << 0) ^ ((uintptr_t) r2 << 16);
    guard |= 0x00000100; /* نضمن بايتًا غير صفري في مكان معروف */
    __stack_chk_guard = guard;
    kprintf("[ssp] stack canary randomized: %x\n", (uint32_t) __stack_chk_guard);
}

/* تُستدعى من gcc تلقائيًا عندما يُكتشف corruption في canary */
__attribute__((noreturn))
void __stack_chk_fail(void) {
    kprintf("\n*** KERNEL PANIC: STACK SMASHING DETECTED ***\n");
    kprintf("*** a function's stack canary was overwritten -- halting for safety ***\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}
