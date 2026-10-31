#include "ledger.h"
#include "spinlock.h"
#include "pit.h"
#include "kstring.h"
#include "serial.h"

/* ledger.c -- انظر ledger.h للشرح الكامل.
 *
 * السجل نفسه: array ثابت في .bss (وليس kheap) -- مقصود، حتى لا يوجد
 * أي اعتماد على heap قد يفسده أو يتلاعب به بطريقة غير مباشرة.
 * write_index هو العداد الوحيد الذي يتحرك، دائمًا إلى الأمام. */

static ledger_entry_t ledger[LEDGER_CAPACITY];
static volatile uint32_t write_index = 0;   /* عدد الـ entries المكتوبة */
static spinlock_t ledger_lock = SPINLOCK_INIT;
static int full_notice_written = 0;

void ledger_init(void) {
    memset(ledger, 0, sizeof(ledger));
    write_index = 0;
    full_notice_written = 0;
    kprintf("[ledger] initialized: append-only, capacity=%d entries\n", LEDGER_CAPACITY);
}

static void copy_name(char* dst, const char* src) {
    size_t i = 0;
    for (; i < 15 && src && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void ledger_record(ledger_event_type_t type, uint32_t pid, uint32_t related_pid, const char* name) {
    uint32_t f = spin_lock_irqsave(&ledger_lock, "ledger_record");

    if (write_index >= LEDGER_CAPACITY) {
        /* السجل امتلأ. لا يوجد "دوران" (wrap-around) هنا عن قصد -- الدوران
           يعني الكتابة فوق entry قديمة، أي فعليًا حذف/تعديل، وهذا
           بالضبط ما لا يفعله هذا الملف. نفضّل إيقاف التسجيل والإعلام
           مرة واحدة، بدل أن نخرق الضمان الأساسي (لا تعديل ولا حذف).
           التوسع الديناميكي الحقيقي (الفصل 20 من خارطة الطريق) سيحل هذا
           القيد لاحقًا. */
        if (!full_notice_written) {
            full_notice_written = 1;
            kprintf("[ledger] FULL at %d entries -- further events NOT recorded "
                    "(no overwrite, by design; see ledger.h)\n", LEDGER_CAPACITY);
        }
        spin_unlock_irqrestore(&ledger_lock, f);
        return;
    }

    ledger_entry_t* e = &ledger[write_index];
    e->seq          = write_index;
    e->tick         = pit_get_ticks();
    e->event_type   = (uint32_t) type;
    e->pid          = pid;
    e->related_pid  = related_pid;
    copy_name(e->name, name);

    write_index++;

    spin_unlock_irqrestore(&ledger_lock, f);
}

int ledger_read(uint32_t index, ledger_entry_t* out) {
    if (index >= write_index) return -1;
    /* نسخة بسيطة -- القراءة لا تحتاج قفلًا صارمًا (write_index يزداد فقط،
       ولا يرجع، فقراءة entry تحت write_index الحالي آمنة ومستقرة دائمًا) */
    *out = ledger[index];
    return 0;
}

uint32_t ledger_count(void) {
    return write_index;
}

static const char* event_name(uint32_t t) {
    switch (t) {
        case LEDGER_EVT_PROC_CREATE_KERNEL: return "CREATE_KERNEL";
        case LEDGER_EVT_PROC_CREATE_USER:   return "CREATE_USER";
        case LEDGER_EVT_PROC_CREATE_ELF:    return "CREATE_ELF";
        case LEDGER_EVT_PROC_FORK:          return "FORK";
        case LEDGER_EVT_PROC_EXIT:          return "EXIT";
        case LEDGER_EVT_PROC_EXECVE:        return "EXECVE";
        case LEDGER_EVT_TRUST_WINDOW_REQUESTED: return "TRUST_WINDOW_REQUESTED";
        case LEDGER_EVT_TRUST_WINDOW_CANCELLED: return "TRUST_WINDOW_CANCELLED";
        case LEDGER_EVT_TRUST_WINDOW_EXECUTED:  return "TRUST_WINDOW_EXECUTED";
        case LEDGER_EVT_LEDGER_FULL:        return "LEDGER_FULL";
        default:                            return "UNKNOWN";
    }
}

void ledger_dump_summary(void) {
    kprintf("[ledger] --- dump (%d entries, read-only, append-only) ---\n", write_index);
    for (uint32_t i = 0; i < write_index; i++) {
        ledger_entry_t* e = &ledger[i];
        kprintf("[ledger] #%d tick=%d %s pid=%d related=%d name=%s\n",
                e->seq, e->tick, event_name(e->event_type), e->pid, e->related_pid, e->name);
    }
    kprintf("[ledger] --- end dump ---\n");
}
