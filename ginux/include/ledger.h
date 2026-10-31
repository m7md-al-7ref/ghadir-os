#ifndef LEDGER_H
#define LEDGER_H

#include <stdint.h>

/* ledger.h -- السجل اللارجعي (Append-Only Ledger)
 *
 * أول تطبيق فعلي في الكود لمبدأ "عدم توثيق الذات" (الفصل السابع من كتاب
 * الفلسفة: "لا سلطة مطلقة"). القاعدة: أي حدث جسيم في حياة process
 * (إنشاء، fork، خروج) يُسجَّل هنا، ولا توجد دالة واحدة في هذا الملف
 * تستطيع تعديل entry أو مسحها بعد كتابتها -- بما فيها الكود الذي كتب
 * الـ entry نفسه. القراءة مفتوحة لأي process عبر SYS_LEDGER_READ (انظر
 * syscall.c) -- الشفافية جزء من المبدأ، ليس استثناء.
 *
 * هذا أول حجر في خارطة الطريق (كتاب الفلسفة، الفصل العشرون) لأنه:
 *   1. غير مرتبط بالـ scheduler إطلاقًا (بدون مخاطرة بالبق الرابع المعروف)
 *   2. مستقل تمامًا، قابل للاختبار بمفرده
 *   3. يبني الأساس الذي ستعتمد عليه "نافذة الأمانة" لاحقًا
 *
 * حدود معروفة (موثقة بصدق، ليس مخفية):
 *   - السعة محدودة (LEDGER_CAPACITY entries) في هذه النسخة الأولى --
 *     التوسع الديناميكي (كتلة جديدة من kheap كلما امتلأت كتلة) خارج
 *     نطاق هذه الخطوة الأولى، مذكور في TODO أدناه.
 *   - لا يوجد تشفير ولا توقيع cryptographic بعد للـ entries -- الضمان
 *     الحالي بنيوي بحت (لا توجد دالة كتابة إلا append، ولا توجد دالة
 *     تعديل أو حذف أصلًا في هذا الملف) وليس cryptographic.
 */

#define LEDGER_CAPACITY 4096

typedef enum {
    LEDGER_EVT_PROC_CREATE_KERNEL = 1,
    LEDGER_EVT_PROC_CREATE_USER   = 2,
    LEDGER_EVT_PROC_CREATE_ELF    = 3,
    LEDGER_EVT_PROC_FORK          = 4,
    LEDGER_EVT_PROC_EXIT          = 5,
    LEDGER_EVT_PROC_EXECVE        = 7,
    LEDGER_EVT_LEDGER_FULL        = 6, /* تسجيل ذاتي: السجل امتلأ (انظر ledger.c) */
    LEDGER_EVT_TRUST_WINDOW_REQUESTED = 8,  /* طلب حذف دخل نافذة الأمانة */
    LEDGER_EVT_TRUST_WINDOW_CANCELLED = 9,  /* طلب حذف أُلغي قبل التنفيذ */
    LEDGER_EVT_TRUST_WINDOW_EXECUTED  = 10, /* نُفِّذ الحذف فعليًا بعد انتهاء النافذة */
} ledger_event_type_t;

typedef struct {
    uint32_t seq;          /* رقم تسلسلي، يزداد دائمًا، ولا يرجع للوراء */
    uint32_t tick;         /* pit_get_ticks() وقت التسجيل */
    uint32_t event_type;   /* ledger_event_type_t */
    uint32_t pid;          /* الـ process المعنية بالحدث */
    uint32_t related_pid;  /* مستعملة لـ fork (parent) و waitpid وما شابه، 0 إن لم تكن مستعملة */
    char name[16];         /* اسم الـ process، نسخة وقت التسجيل (snapshot) */
} ledger_entry_t;

/* تهيئة السجل -- تُستدعى مرة واحدة من kernel_main، قبل process_init */
void ledger_init(void);

/* الكتابة الوحيدة الممكنة: append. لا يوجد ledger_edit() ولا ledger_delete()
   في هذا الملف كامل -- هذا الأمر مقصود، ليس نسيانًا. */
void ledger_record(ledger_event_type_t type, uint32_t pid, uint32_t related_pid, const char* name);

/* القراءة: مفتوحة، بدون شرط صلاحية -- أي process تستطيع قراءة أي entry.
   ترجع 0 عند نجاح النسخ إلى *out، أو -1 إذا كان index خارج الحدود
   (>= ledger_count()). */
int ledger_read(uint32_t index, ledger_entry_t* out);

/* عدد الـ entries المكتوبة فعليًا حتى الآن (يزداد دائمًا، أو يتجمد عند
   LEDGER_CAPACITY إذا امتلأ السجل -- انظر الحدود المعروفة أعلاه) */
uint32_t ledger_count(void);

/* طباعة ملخص كامل للسجل عبر kprintf -- ليست دالة "إدارية" خاصة، أي
   جهة تستطيع استعمالها (تطبيق مباشر لـ "الشفافية جزء من المبدأ"). مفيدة
   وقت debugging وكإثبات حي أن السجل يسجّل فعلًا. */
void ledger_dump_summary(void);

#endif
