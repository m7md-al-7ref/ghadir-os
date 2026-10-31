#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
    /* debug/audit: نسجّل من كان آخر من أخذ هذا الـ lock، مفيد لتشخيص
       deadlocks -- خط دفاع إضافي وقت التطوير */
    const char* owner_desc;

    /* --- منع الاحتكار (كتاب الفلسفة، الفصل الثالث عشر: "تحريم
       الاحتكار") -- انظر الشرح الكامل في spinlock.c، دالة
       check_monopoly(). هذا الأمر يقيس "مدة الاحتفاظ الفعلية" لكل قفل،
       ليس فقط "هل القفل محجوز" -- الفرق بين استخدام مشروع (ولو طويل)
       واحتكار (احتفاظ بدون مبرر) هو المدة نسبةً إلى طبيعة العملية.

       ملاحظة تقنية مهمة (bug حقيقي اكتُشف وأُصلح وقت الكتابة، 2026-08):
       لم نستخدم pit_get_ticks() هنا لأنها لا تزداد إلا من داخل معالج IRQ0 --
       و`spin_lock_irqsave` توقف المقاطعات (cli) طوال مدة الحجز، أي أن
       العداد الخاص بـ PIT يتجمد بالضبط حين نقيس به! استعملنا RDTSC
       (عداد دورات المعالج) بدلها -- مستقل تمامًا عن حالة المقاطعات،
       ويستمر في الزيادة حتى عندما IF=0. */
    volatile uint64_t acquired_tsc;    /* rdtsc() وقت آخر spin_lock ناجحة */
    volatile uint32_t monopoly_count;  /* عدد المرات التي تجاوزنا فيها threshold الاحتكار */
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0, .owner_desc = "unlocked", .acquired_tsc = 0, .monopoly_count = 0 }

/* عتبة "الاحتكار المشتبه فيه" بوحدة دورات معالج (TSC cycles). القيمة
   هذه معايرة تجريبيًا في QEMU/TCG (2026-08، انظر HANDOFF.md للتفاصيل):
   وجدنا أن عمليات شرعية بحتة (process_create_elf، تحميل ELF كامل مع
   paging) تأخذ ~14 مليون دورة بشكل طبيعي تمامًا -- محاولة أولى بـ
   10 مليون كانت حساسة أكثر من اللازم، وتُصنِّف عملًا شرعيًا كـ"احتكار" خطأً (false
   positive). العتبة الحالية (200 مليون) معايرة حتى تلتقط فقط الحالات
   الشاذة بشكل واضح (مثل حلقة لانهائية أو bug حقيقي)، وليس عمليات
   بطيئة لكن شرعية. هذه المعايرة تقريبية، مرتبطة بسرعة TCG (وليس عتادًا
   حقيقيًا)، وقد تحتاج تعديلًا إضافيًا مع تجارب أكثر. */
#define SPINLOCK_MONOPOLY_THRESHOLD_CYCLES 200000000ULL

void spinlock_init(spinlock_t* lock);
void spin_lock(spinlock_t* lock, const char* desc);
void spin_unlock(spinlock_t* lock);
int  spin_trylock(spinlock_t* lock, const char* desc);

/* نسخة توقف المقاطعات أيضًا -- ضرورية إذا كان resource مشتركًا بين kernel
   code وinterrupt handler (مثل scheduler)، وإلا لو لم نوقف المقاطعات
   فقد يقع deadlock: يقع interrupt في نفس المعالج بينما lock محجوز، ويحاول
   أخذ نفس الـ lock -> فيبقى معلقًا للأبد (المعالج واحد، ولا يوجد معالج آخر
   يحرر الـ lock). */
uint32_t spin_lock_irqsave(spinlock_t* lock, const char* desc);
void spin_unlock_irqrestore(spinlock_t* lock, uint32_t flags);

/* عدد المرات التي سُجّل فيها احتكار مشتبه فيه لهذا القفل -- مفيدة
   للاختبار والمراقبة. القراءة بدون قفل (race نظري بسيط، مقبول لعداد
   تشخيصي غير حرج). */
uint32_t spinlock_monopoly_count(const spinlock_t* lock);

#endif
