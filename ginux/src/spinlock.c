#include "spinlock.h"
#include "serial.h"

/* --- منع الاحتكار (كتاب الفلسفة: "الاستخلاف"، الفصل الثالث عشر) ---
 *
 * الفكرة: الفقه الإسلامي يحرّم الاحتكار -- حبس مورد عن الانتفاع بدون
 * مبرر حقيقي، وليس مجرد "الاحتفاظ به". الترجمة التقنية هنا: أي قفل
 * محجوز لمدة تتجاوز threshold معقولًا يُعتبر مؤشر احتكار محتمل، ونسجّل
 * تحذيرًا -- دون أن "نكسر" القفل قسرًا (هذا خطير، قد يفسد بيانات
 * محمية بالقفل نفسه -- انظر كتاب الفلسفة، الفصل الثامن عشر: "تحدٍّ
 * صادق" -- التدخل القسري مشكلة لم تُحل بعد حتى بالكتاب نفسه).
 *
 * حدود صادقة (مثل باقي هذا المشروع): على معالج واحد (uniprocessor)،
 * `spin_lock_irqsave` توقف المقاطعات طوال مدة الحجز -- أي أن "تزاحمًا
 * حقيقيًا" بين processين على نفس القفل لا يمكن أن يقع فعليًا (process أخرى
 * لا تستطيع أن تُجدوَل أصلًا بينما IF=0). الذي نقيسه هنا ليس "احتكارًا ضد
 * process معينة"، بل "احتكارًا ضد النظام كاملًا" -- أي critical section
 * تأخذ وقتًا طويلًا بدون مبرر توقف كل شيء (حتى الـ timer IRQ) طوال مدتها،
 * وهذا فعليًا أخطر من تزاحم عادي. القياس صادق ومفيد حتى بهذا الشكل.
 *
 * (انظر spinlock.h لشرح لماذا RDTSC وليس PIT ticks -- bug حقيقي
 * أُصلح وقت الكتابة). */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static void check_monopoly(spinlock_t* lock, const char* desc) {
    uint64_t held = rdtsc() - lock->acquired_tsc;
    if (held > SPINLOCK_MONOPOLY_THRESHOLD_CYCLES) {
        lock->monopoly_count++;
        kprintf("[spinlock] MONOPOLY SUSPECTED: '%s' held for ~%d cycles "
                "(threshold=%d) -- count=%d. هذا يوقف النظام كله طوال "
                "هذه المدة (uniprocessor + IF=0). راجع الكود.\n",
                desc, (uint32_t)(held > 0xFFFFFFFFULL ? 0xFFFFFFFF : held),
                (uint32_t) SPINLOCK_MONOPOLY_THRESHOLD_CYCLES, lock->monopoly_count);
    }
}

void spinlock_init(spinlock_t* lock) {
    lock->locked = 0;
    lock->owner_desc = "unlocked";
    lock->acquired_tsc = 0;
    lock->monopoly_count = 0;
}

static inline uint32_t atomic_xchg(volatile uint32_t* addr, uint32_t val) {
    uint32_t old;
    __asm__ volatile ("xchg %0, %1" : "=r"(old), "+m"(*addr) : "0"(val) : "memory");
    return old;
}

void spin_lock(spinlock_t* lock, const char* desc) {
    uint32_t spins = 0;
    while (atomic_xchg(&lock->locked, 1) != 0) {
        __asm__ volatile ("pause" ::: "memory");
        spins++;
        if (spins == 100000000) {
            /* هذا مؤشر قوي على deadlock أو bug منطقي -- نسجّل تحذيرًا
               بدل أن نبقى معلقين بصمت للأبد بدون أي أثر للتشخيص */
            kprintf("[spinlock] WARNING: possible deadlock/starvation waiting on lock held by: %s\n",
                    lock->owner_desc);
            spins = 0;
        }
    }
    lock->owner_desc = desc;
    lock->acquired_tsc = rdtsc();
}

int spin_trylock(spinlock_t* lock, const char* desc) {
    if (atomic_xchg(&lock->locked, 1) == 0) {
        lock->owner_desc = desc;
        lock->acquired_tsc = rdtsc();
        return 1;
    }
    return 0;
}

void spin_unlock(spinlock_t* lock) {
    check_monopoly(lock, lock->owner_desc);
    lock->owner_desc = "unlocked";
    __asm__ volatile ("" ::: "memory"); /* compiler barrier */
    lock->locked = 0;
}

static inline uint32_t save_and_disable_irq(void) {
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void restore_irq(uint32_t flags) {
    __asm__ volatile ("push %0; popf" :: "r"(flags) : "memory", "cc");
}

uint32_t spin_lock_irqsave(spinlock_t* lock, const char* desc) {
    uint32_t flags = save_and_disable_irq();
    spin_lock(lock, desc);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* lock, uint32_t flags) {
    spin_unlock(lock);
    restore_irq(flags);
}

uint32_t spinlock_monopoly_count(const spinlock_t* lock) {
    return lock->monopoly_count;
}
