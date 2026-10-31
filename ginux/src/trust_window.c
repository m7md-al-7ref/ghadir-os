#include "trust_window.h"
#include "initrd.h"
#include "ledger.h"
#include "kstring.h"
#include "serial.h"
#include "pit.h"

/* trust_window.c -- انظر trust_window.h للشرح الكامل.
 *
 * جدول ثابت في .bss (مثل ledger.c، مقصود -- لا يوجد اعتماد على kheap
 * لبنية النواة الأساسية هذه). لا يوجد قفل (spinlock) هنا عمدًا: كل
 * التعاملات (request/cancel/tick) تحدث من syscall handlers أو من
 * pit_callback -- كلاهما يعمل بـ IF=0 (ring0، عبر cli الخاص بـ
 * ISR/IRQ stub)، لذلك لا يوجد تزاحم حقيقي ممكن الحدوث (uniprocessor، نفس
 * المنطق المشروح في spinlock.c بخصوص منع الاحتكار). */

typedef struct {
    int active;
    pending_op_type_t type;
    char path[INITRD_MAX_NAME];
    uint32_t requested_tick;
    uint32_t execute_at_tick;
    uint32_t owner_pid;
} pending_deletion_t;

static pending_deletion_t g_pending[TRUST_WINDOW_MAX_PENDING];

void trust_window_init(void) {
    memset(g_pending, 0, sizeof(g_pending));
    kprintf("[trust_window] initialized: %d slots, window=%d ticks\n",
            TRUST_WINDOW_MAX_PENDING, TRUST_WINDOW_TICKS);
}

int trust_window_request_delete(const char* path, uint32_t requesting_pid) {
    if (initrd_find(path) < 0) return -1; /* الملف غير موجود أصلًا، لا شيء نحذفه */

    int slot = -1;
    for (int i = 0; i < TRUST_WINDOW_MAX_PENDING; i++) {
        if (!g_pending[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        kprintf("[trust_window] table full (%d pending), rejecting request for '%s'\n",
                TRUST_WINDOW_MAX_PENDING, path);
        return -1;
    }

    pending_deletion_t* p = &g_pending[slot];
    p->active = 1;
    p->type = PENDING_DELETE_INITRD_UNLINK;
    int i = 0;
    for (; i < INITRD_MAX_NAME - 1 && path[i]; i++) p->path[i] = path[i];
    p->path[i] = '\0';
    p->owner_pid = requesting_pid;
    uint32_t now = pit_get_ticks();
    p->requested_tick = now;
    p->execute_at_tick = now + TRUST_WINDOW_TICKS;

    kprintf("[trust_window] pid=%d requested delete of '%s' -- handle=%d, "
            "executes at tick=%d (now=%d, +%d ticks)\n",
            requesting_pid, p->path, slot, p->execute_at_tick, now, TRUST_WINDOW_TICKS);
    ledger_record(LEDGER_EVT_TRUST_WINDOW_REQUESTED, requesting_pid, (uint32_t) slot, p->path);

    return slot;
}

int trust_window_cancel(int handle, uint32_t requesting_pid) {
    if (handle < 0 || handle >= TRUST_WINDOW_MAX_PENDING) return -1;
    pending_deletion_t* p = &g_pending[handle];
    if (!p->active) return -1;
    if (p->owner_pid != requesting_pid) {
        /* أمانة صريحة: لا تستطيع process أخرى إلغاء طلب حذف ليس لها،
           حتى لو عرفت الـ handle -- كل عملية مسؤولة عن قراراتها هي فقط. */
        kprintf("[trust_window] SECURITY: pid=%d tried to cancel handle=%d owned by pid=%d -- rejected\n",
                requesting_pid, handle, p->owner_pid);
        return -1;
    }

    kprintf("[trust_window] pid=%d cancelled pending delete of '%s' (handle=%d)\n",
            requesting_pid, p->path, handle);
    ledger_record(LEDGER_EVT_TRUST_WINDOW_CANCELLED, requesting_pid, (uint32_t) handle, p->path);

    p->active = 0;
    return 0;
}

void trust_window_tick(uint32_t current_tick) {
    for (int i = 0; i < TRUST_WINDOW_MAX_PENDING; i++) {
        pending_deletion_t* p = &g_pending[i];
        if (!p->active) continue;
        if (current_tick < p->execute_at_tick) continue;

        /* النافذة انتهت -- ننفّذ الحذف الفعلي الآن */
        kprintf("[trust_window] window expired for '%s' (handle=%d, pid=%d) -- executing delete now\n",
                p->path, i, p->owner_pid);
        int rc = initrd_unlink(p->path);
        ledger_record(LEDGER_EVT_TRUST_WINDOW_EXECUTED, p->owner_pid, (uint32_t) i, p->path);
        if (rc != 0) {
            kprintf("[trust_window] WARNING: delayed unlink('%s') failed at execution time "
                    "(file already gone?)\n", p->path);
        }
        p->active = 0;
    }
}

int trust_window_pending_count(void) {
    int c = 0;
    for (int i = 0; i < TRUST_WINDOW_MAX_PENDING; i++) if (g_pending[i].active) c++;
    return c;
}

static const char* tw_norm(const char* s) {
    for (;;) { if (s[0] == '/') s++; else if (s[0] == '.' && s[1] == '/') s += 2; else break; }
    return s;
}

int trust_window_cancel_path(const char* path) {
    const char* want = tw_norm(path);
    int n = 0;
    for (int i = 0; i < TRUST_WINDOW_MAX_PENDING; i++) {
        if (!g_pending[i].active) continue;
        if (kstrcmp(tw_norm(g_pending[i].path), want) != 0) continue;
        g_pending[i].active = 0;
        n++;
        kprintf("[trust_window] pending delete of '%s' (handle=%d) cancelled: name was re-created\n", want, i);
        ledger_record(LEDGER_EVT_TRUST_WINDOW_CANCELLED, g_pending[i].owner_pid, (uint32_t) i, g_pending[i].path);
    }
    return n;
}
