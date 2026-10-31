#include "pipe.h"
#include "spinlock.h"
#include "kstring.h"
#include "serial.h"

typedef struct {
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t head, tail, count;
    spinlock_t lock;
    int in_use;
    int readers, writers; /* طبقة التوافق: عدد الـ fds المفتوحة لكل طرف */
} pipe_t;

static pipe_t pipes[PIPE_COUNT];

void pipe_init(void) {
    for (int i = 0; i < PIPE_COUNT; i++) {
        spinlock_init(&pipes[i].lock);
        pipes[i].head = pipes[i].tail = pipes[i].count = 0;
        pipes[i].in_use = 0;
    }
    kprintf("[pipe] %d pipe slots ready (%d bytes each)\n", PIPE_COUNT, PIPE_BUF_SIZE);
}

int pipe_create(void) {
    for (int i = 0; i < PIPE_COUNT; i++) {
        uint32_t f = spin_lock_irqsave(&pipes[i].lock, "pipe_create");
        if (!pipes[i].in_use) {
            pipes[i].in_use = 1;
            pipes[i].readers = pipes[i].writers = 1;
            pipes[i].head = pipes[i].tail = pipes[i].count = 0;
            spin_unlock_irqrestore(&pipes[i].lock, f);
            return i;
        }
        spin_unlock_irqrestore(&pipes[i].lock, f);
    }
    return -1;
}

/* ملاحظة تصميم: هذا الـ pipe غير blocking -- pipe_read ترجع فورًا (يمكن
   0 بايت إذا كانت فارغة) بدلًا من أن تعلق process. هذا تبسيط مقصود: pipe
   حقيقي (POSIX) يحجب القارئ حتى تصل بيانات، لكن هذا يتطلب
   context-switch مباشر من داخل syscall handler (معقد جدًا لهذه المرحلة).
   المستهلك (consumer) يجب أن يعيد المحاولة (polling) -- نمط بسيط وآمن. */

int pipe_write(int id, const void* data, uint32_t len) {
    if (id < 0 || id >= PIPE_COUNT) return -1;
    pipe_t* p = &pipes[id];
    uint32_t f = spin_lock_irqsave(&p->lock, "pipe_write");
    if (!p->in_use) { spin_unlock_irqrestore(&p->lock, f); return -1; }

    const uint8_t* src = (const uint8_t*) data;
    uint32_t written = 0;
    while (written < len && p->count < PIPE_BUF_SIZE) {
        p->buf[p->head] = src[written];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
        written++;
    }
    spin_unlock_irqrestore(&p->lock, f);
    return (int) written;
}

int pipe_read(int id, void* data, uint32_t len) {
    if (id < 0 || id >= PIPE_COUNT) return -1;
    pipe_t* p = &pipes[id];
    uint32_t f = spin_lock_irqsave(&p->lock, "pipe_read");
    if (!p->in_use) { spin_unlock_irqrestore(&p->lock, f); return -1; }

    uint8_t* dst = (uint8_t*) data;
    uint32_t r = 0;
    while (r < len && p->count > 0) {
        dst[r] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
        r++;
    }
    spin_unlock_irqrestore(&p->lock, f);
    return (int) r;
}

void pipe_ref(int id, int is_writer) {
    if (id < 0 || id >= PIPE_COUNT) return;
    pipe_t* p = &pipes[id];
    uint32_t f = spin_lock_irqsave(&p->lock, "pipe_ref");
    if (p->in_use) { if (is_writer) p->writers++; else p->readers++; }
    spin_unlock_irqrestore(&p->lock, f);
}

void pipe_unref(int id, int is_writer) {
    if (id < 0 || id >= PIPE_COUNT) return;
    pipe_t* p = &pipes[id];
    uint32_t f = spin_lock_irqsave(&p->lock, "pipe_unref");
    if (p->in_use) {
        if (is_writer) { if (p->writers > 0) p->writers--; }
        else           { if (p->readers > 0) p->readers--; }
        if (p->readers == 0 && p->writers == 0) p->in_use = 0;
    }
    spin_unlock_irqrestore(&p->lock, f);
}

int pipe_read_ex(int id, void* data, uint32_t len) {
    if (id < 0 || id >= PIPE_COUNT) return 0;
    pipe_t* p = &pipes[id];
    uint32_t f = spin_lock_irqsave(&p->lock, "pipe_read_ex");
    if (!p->in_use) { spin_unlock_irqrestore(&p->lock, f); return 0; }
    uint8_t* dst = (uint8_t*) data;
    uint32_t n = 0;
    while (n < len && p->count > 0) {
        dst[n++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
    }
    int writers = p->writers;
    spin_unlock_irqrestore(&p->lock, f);
    if (n > 0) return (int) n;
    return writers > 0 ? -1 : 0;
}

int pipe_write_ex(int id, const void* data, uint32_t len) {
    if (id < 0 || id >= PIPE_COUNT) return -2;
    pipe_t* p = &pipes[id];
    uint32_t f = spin_lock_irqsave(&p->lock, "pipe_write_ex");
    if (!p->in_use || p->readers == 0) { spin_unlock_irqrestore(&p->lock, f); return -2; }
    const uint8_t* src = (const uint8_t*) data;
    uint32_t n = 0;
    while (n < len && p->count < PIPE_BUF_SIZE) {
        p->buf[p->head] = src[n++];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
    }
    spin_unlock_irqrestore(&p->lock, f);
    return n > 0 ? (int) n : (len == 0 ? 0 : -1);
}
