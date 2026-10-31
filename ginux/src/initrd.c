#include "initrd.h"
#include "kstring.h"
#include "serial.h"
#include "kheap.h"

#define TAR_BLOCK_SIZE 512

/* بنية header الخاص بـ USTAR (POSIX tar) -- نقرأ منها مباشرة، دون أي
   نسخ، لأن initrd كامل identity-mapped (< 128MB). أغلب الحقول لا
   نستخدمها حاليًا (uid/gid/mtime/...) لكن يجب أن تبقى في البنية حتى
   الـ offsets الخاصة بالحقول التي نستخدمها (size, typeflag, magic)
   تبقى صحيحة -- ليس dead code، بل بنية format ثابتة. */
// cppcheck-suppress unusedStructMember
struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];     /* octal ASCII */
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];      /* "ustar\0" */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

/* === طبقة الحقائق (Content Layer) ===
   لا تعرف اسمًا ولا مالكًا -- بايتات فقط + إدارة ذاكرتها. */
typedef struct {
    int in_use;
    uint8_t* data;
    uint32_t size;
    uint32_t capacity; /* 0 = boot-backed (قراءة فقط حتى أول كتابة)، غير 0 = heap-backed */
    uint32_t refcount;   /* عدد أسماء طبقة الاعتباريات المشيرة لهذا المحتوى */
    uint32_t open_count; /* عدد fds المفتوحة حاليًا على هذا المحتوى (fix استقرار الهوية) */
} content_slot_t;

/* === طبقة الاعتباريات (Convention Layer) ===
   اسم + رابط لطبقة الحقائق فقط. */
typedef struct {
    char name[INITRD_MAX_NAME]; /* name[0]=='\0' يعني tombstoned */
    int content_id;              /* index في g_content، -1 = غير مرتبط (مجلد) */
    int is_dir;                  /* 1 = مجلد صريح (بلا محتوى) */
} convention_slot_t;

static content_slot_t g_content[INITRD_MAX_FILES];
static convention_slot_t g_conv[INITRD_MAX_FILES];
static int g_conv_count = 0;

/* بفر ساكن يُستخدم لبناء initrd_file_t المرجعة من initrd_get() --
   انظر initrd.h للتوثيق الصريح لهذا القيد التصميمي. */
static initrd_file_t g_scratch;

static uint32_t parse_octal(const char* s, int max_len) {
    uint32_t val = 0;
    for (int i = 0; i < max_len; i++) {
        char c = s[i];
        if (c < '0' || c > '7') break;
        val = (val << 3) + (uint32_t)(c - '0');
    }
    return val;
}

static const char* normalize_name(const char* name) {
    while (name[0] == '.' && name[1] == '/') name += 2;
    while (name[0] == '/') name += 1;
    return name;
}

static int content_alloc_slot(void) {
    for (int i = 0; i < INITRD_MAX_FILES; i++) {
        if (!g_content[i].in_use) return i;
    }
    return -1;
}

static int conv_alloc_slot(void) {
    /* أول شيء: نبحث عن slot tombstoned لنعيد استعماله */
    for (int i = 0; i < g_conv_count; i++) {
        if (g_conv[i].name[0] == '\0') { memset(&g_conv[i], 0, sizeof(g_conv[i])); g_conv[i].content_id = -1; return i; }
    }
    if (g_conv_count >= INITRD_MAX_FILES) return -1;
    int n = g_conv_count++;
    memset(&g_conv[n], 0, sizeof(g_conv[n]));
    g_conv[n].content_id = -1;
    return n;
}

void initrd_init(uint32_t phys_start, uint32_t phys_end) {
    g_conv_count = 0;
    memset(g_content, 0, sizeof(g_content));
    memset(g_conv, 0, sizeof(g_conv));

    if (phys_start == 0 || phys_end <= phys_start) {
        kprintf("[initrd] no module provided -- filesystem stays empty\n");
        return;
    }

    const uint8_t* cursor = (const uint8_t*) phys_start;
    const uint8_t* limit = (const uint8_t*) phys_end;

    while (cursor + TAR_BLOCK_SIZE <= limit && g_conv_count < INITRD_MAX_FILES) {
        const struct ustar_header* hdr = (const struct ustar_header*) cursor;

        if (hdr->name[0] == '\0') break;

        if (hdr->magic[0] != 'u' || hdr->magic[1] != 's' || hdr->magic[2] != 't' ||
            hdr->magic[3] != 'a' || hdr->magic[4] != 'r') {
            kprintf("[initrd] bad ustar magic at %x -- stopping parse\n",
                    (uint32_t)(uintptr_t) cursor);
            break;
        }

        uint32_t size = parse_octal(hdr->size, sizeof(hdr->size));
        const uint8_t* data = cursor + TAR_BLOCK_SIZE;

        if (data + size > limit) {
            kprintf("[initrd] file '%s' extends past module end -- truncating parse\n", hdr->name);
            break;
        }

        /* الاسم الكامل = prefix + "/" + name (ustar يقسم المسارات الطويلة) */
        char full[INITRD_MAX_NAME];
        int fl = 0;
        if (hdr->prefix[0]) {
            for (int k = 0; k < 155 && hdr->prefix[k] && fl < INITRD_MAX_NAME - 2; k++) full[fl++] = hdr->prefix[k];
            full[fl++] = '/';
        }
        for (int k = 0; k < 100 && hdr->name[k] && fl < INITRD_MAX_NAME - 1; k++) full[fl++] = hdr->name[k];
        full[fl] = '\0';
        const char* norm = normalize_name(full);
        char clean[INITRD_MAX_NAME];
        int cl = 0;
        for (; norm[cl] && cl < INITRD_MAX_NAME - 1; cl++) clean[cl] = norm[cl];
        clean[cl] = '\0';
        while (cl > 0 && clean[cl - 1] == '/') clean[--cl] = '\0'; /* "dir/" -> "dir" */

        if (hdr->typeflag == '5') {
            /* مجلد صريح (الجذر "./" يُهمل: هو موجود دائمًا) */
            if (clean[0] != '\0') {
                int idx = conv_alloc_slot();
                if (idx >= 0) {
                    convention_slot_t* v = &g_conv[idx];
                    memcpy(v->name, clean, (size_t) cl + 1);
                    v->content_id = -1;
                    v->is_dir = 1;
                }
            }
        } else if ((hdr->typeflag == '0' || hdr->typeflag == '\0') && clean[0] != '\0') {
            int cid = content_alloc_slot();
            int idx = conv_alloc_slot();
            if (cid >= 0 && idx >= 0) {
                content_slot_t* c = &g_content[cid];
                c->in_use = 1;
                c->data = (uint8_t*) data; /* boot-backed */
                c->size = size;
                c->capacity = 0;
                c->refcount = 1;

                convention_slot_t* v = &g_conv[idx];
                memcpy(v->name, clean, (size_t) cl + 1);
                v->content_id = cid;
            } else {
                kprintf("[initrd] table full -- '%s' skipped\n", clean);
            }
        }

        uint32_t data_blocks = (size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        cursor = data + (uint64_t) data_blocks * TAR_BLOCK_SIZE;
    }

    kprintf("[initrd] parsed %d file(s) from module [%x-%x) -- content/convention layers separated\n",
            g_conv_count, phys_start, phys_end);
    for (int i = 0; i < g_conv_count && i < 12; i++) {
        if (g_conv[i].is_dir) kprintf("[initrd]   %s/ (dir)\n", g_conv[i].name);
        else kprintf("[initrd]   %s (%d bytes, content_id=%d)\n",
                     g_conv[i].name, g_content[g_conv[i].content_id].size, g_conv[i].content_id);
    }
    if (g_conv_count > 12) kprintf("[initrd]   ... (%d more)\n", g_conv_count - 12);
}

int initrd_find(const char* path) {
    const char* norm = normalize_name(path);
    if (norm[0] == '\0') return -1;
    for (int i = 0; i < g_conv_count; i++) {
        if (g_conv[i].name[0] == '\0') continue; /* tombstoned */
        if (kstrcmp(g_conv[i].name, norm) == 0) return i;
    }
    return -1;
}

const initrd_file_t* initrd_get(int index) {
    if (index < 0 || index >= g_conv_count) return NULL;
    convention_slot_t* v = &g_conv[index];
    if (v->content_id < 0 || !g_content[v->content_id].in_use) return NULL;
    content_slot_t* c = &g_content[v->content_id];

    /* بناء العرض المدمج (view) من الطبقتين -- الدمج يقع هنا فقط،
       لحظة الاستخدام، لا في التخزين الفعلي. */
    int i = 0;
    for (; i < INITRD_MAX_NAME - 1 && v->name[i]; i++) g_scratch.name[i] = v->name[i];
    g_scratch.name[i] = '\0';
    g_scratch.data = c->data;
    g_scratch.size = c->size;
    g_scratch.capacity = c->capacity;
    return &g_scratch;
}

int initrd_count(void) { return g_conv_count; }

int initrd_content_id(int index) {
    if (index < 0 || index >= g_conv_count) return -1;
    return g_conv[index].content_id;
}

int initrd_create(const char* path) {
    const char* norm = normalize_name(path);
    if (norm[0] == '\0') return -1;
    if (initrd_find(norm) >= 0) return -1;

    int cid = content_alloc_slot();
    if (cid < 0) return -1;
    int idx = conv_alloc_slot();
    if (idx < 0) return -1;

    content_slot_t* c = &g_content[cid];
    c->in_use = 1;
    c->data = NULL;
    c->size = 0;
    c->capacity = 0;
    c->refcount = 1;

    convention_slot_t* v = &g_conv[idx];
    int i = 0;
    for (; i < INITRD_MAX_NAME - 1 && norm[i]; i++) v->name[i] = norm[i];
    v->name[i] = '\0';
    v->content_id = cid;

    return idx;
}

int initrd_link(const char* new_path, int existing_index) {
    const char* norm = normalize_name(new_path);
    if (norm[0] == '\0') return -1;
    if (initrd_find(norm) >= 0) return -1; /* الاسم الجديد موجود مسبقًا */
    if (existing_index < 0 || existing_index >= g_conv_count) return -1;

    int cid = g_conv[existing_index].content_id;
    if (cid < 0 || !g_content[cid].in_use) return -1;

    int idx = conv_alloc_slot();
    if (idx < 0) return -1;

    convention_slot_t* v = &g_conv[idx];
    int i = 0;
    for (; i < INITRD_MAX_NAME - 1 && norm[i]; i++) v->name[i] = norm[i];
    v->name[i] = '\0';
    v->content_id = cid;

    g_content[cid].refcount++; /* إثبات الفصل: نفس المحتوى، اسمان الآن */

    kprintf("[initrd] link: '%s' -> content_id=%d (refcount now %d, shared with existing name)\n",
            norm, cid, g_content[cid].refcount);
    return idx;
}

int32_t initrd_write(int idx, const void* buf, uint32_t offset, uint32_t len) {
    if (idx < 0 || idx >= g_conv_count) return -1;
    convention_slot_t* v = &g_conv[idx];
    if (v->name[0] == '\0') return -1; /* tombstoned */
    if (v->content_id < 0 || !g_content[v->content_id].in_use) return -1;
    content_slot_t* c = &g_content[v->content_id];

    uint32_t needed = offset + len;
    if (needed < offset) return -1; /* overflow */

    if (c->capacity == 0 || needed > c->capacity) {
        uint32_t new_cap = needed;
        if (c->capacity > 0 && new_cap < c->capacity * 2) new_cap = c->capacity * 2;

        uint8_t* new_buf = (uint8_t*) kmalloc(new_cap);
        if (!new_buf) return -1;

        if (c->size > 0 && c->data) memcpy(new_buf, c->data, c->size);
        if (new_cap > c->size) memset(new_buf + c->size, 0, new_cap - c->size);

        if (c->capacity > 0 && c->data) kfree(c->data);

        c->data = new_buf;
        c->capacity = new_cap;
    }

    memcpy(c->data + offset, buf, len);
    if (needed > c->size) c->size = needed;
    return (int32_t) len;
}

/* قصّ/توسيع محتوى حسب content_id (لا يحتاج اسمًا: يعمل أيضًا على fd مفتوحة اسمها حُذف) */
int initrd_truncate_content(int content_id, uint32_t new_size) {
    if (content_id < 0 || content_id >= INITRD_MAX_FILES || !g_content[content_id].in_use) return -1;
    content_slot_t* c = &g_content[content_id];

    /* bug حقيقي أُصلح (2026-08-17): كانت الدالة تزيد c->size دون التأكد من capacity.
       وأُصلح هنا خطأ ثانٍ (2026-10): عند قصّ محتوى boot-backed (capacity=0) إلى حجم
       أصغر من حجمه الأصلي كان memcpy ينسخ c->size كاملًا فوق buffer أصغر (overflow). */
    if (new_size > c->capacity) {
        uint32_t new_cap = new_size;
        uint8_t* new_buf = (uint8_t*) kmalloc(new_cap);
        if (!new_buf) return -1; /* لا توجد ذاكرة كافية -- يبقى الحجم القديم كما هو */
        uint32_t keep = c->size < new_size ? c->size : new_size;
        if (keep > 0 && c->data) memcpy(new_buf, c->data, keep);
        if (new_cap > keep) memset(new_buf + keep, 0, new_cap - keep); /* بايتات جديدة = صفر */
        if (c->capacity > 0 && c->data) kfree(c->data);
        c->data = new_buf;
        c->capacity = new_cap;
    } else if (new_size > c->size) {
        memset(c->data + c->size, 0, new_size - c->size);
    }
    c->size = new_size;
    return 0;
}

int initrd_truncate(int idx, uint32_t new_size) {
    if (idx < 0 || idx >= g_conv_count) return -1;
    convention_slot_t* v = &g_conv[idx];
    if (v->name[0] == '\0') return -1;
    return initrd_truncate_content(v->content_id, new_size);
}

int initrd_unlink(const char* path) {
    int idx = initrd_find(path);
    if (idx < 0) return -1;
    convention_slot_t* v = &g_conv[idx];
    if (v->content_id >= 0 && g_content[v->content_id].in_use) {
        content_slot_t* c = &g_content[v->content_id];
        if (c->refcount > 0) c->refcount--;
        /* إصلاح حقيقي (2026، مراجعة خارجية): قبل هذا لم نكن نحرر
           المحتوى أبدًا حتى لو refcount==0 -- قرار محافظ لتفادي
           use-after-free عبر fds مفتوحة مسبقًا. الآن، بما أن fds
           تُتتبَّع بدقة عبر open_count (مستقل عن refcount
           الأسماء)، فالتحرير آمن ومضبوط: فقط عندما *يصل الاثنان* إلى صفر
           معًا (لا أسماء تشير إليه، ولا fd مفتوحة عليه). */
        if (c->refcount == 0 && c->open_count == 0) {
            if (c->capacity > 0 && c->data) kfree(c->data);
            c->in_use = 0;
            c->data = NULL;
            c->size = 0;
            c->capacity = 0;
        }
    }
    v->name[0] = '\0'; /* tombstone -- طبقة الاعتباريات فقط تتأثر */
    return 0;
}

int initrd_open_by_index(int conv_idx) {
    if (conv_idx < 0 || conv_idx >= g_conv_count) return -1;
    int cid = g_conv[conv_idx].content_id;
    if (cid < 0 || !g_content[cid].in_use) return -1;
    g_content[cid].open_count++;
    return cid;
}

void initrd_close_content(int content_id) {
    if (content_id < 0 || content_id >= INITRD_MAX_FILES) return;
    content_slot_t* c = &g_content[content_id];
    if (!c->in_use) return;
    if (c->open_count > 0) c->open_count--;
    if (c->open_count == 0 && c->refcount == 0) {
        /* آخر fd أُغلقت *و* لا يوجد أي اسم يشير إليه -- نحرر فعليًا الآن
           (مثل unlink(2) الحقيقية: "الملف يُحذف فعليًا عند إغلاق آخر fd") */
        if (c->capacity > 0 && c->data) kfree(c->data);
        c->in_use = 0;
        c->data = NULL;
        c->size = 0;
        c->capacity = 0;
    }
}

int32_t initrd_write_content(int content_id, const void* buf, uint32_t offset, uint32_t len) {
    if (content_id < 0 || content_id >= INITRD_MAX_FILES) return -1;
    content_slot_t* c = &g_content[content_id];
    if (!c->in_use) return -1; /* المحتوى حُرِّر (كل الـ fds أُغلقت والاسم حُذف) -- أصبحت fd stale */

    uint32_t needed = offset + len;
    if (needed < offset) return -1; /* overflow */

    if (c->capacity == 0 || needed > c->capacity) {
        uint32_t new_cap = needed;
        if (c->capacity > 0 && new_cap < c->capacity * 2) new_cap = c->capacity * 2;
        uint8_t* new_buf = (uint8_t*) kmalloc(new_cap);
        if (!new_buf) return -1;
        if (c->size > 0 && c->data) memcpy(new_buf, c->data, c->size);
        if (new_cap > c->size) memset(new_buf + c->size, 0, new_cap - c->size);
        if (c->capacity > 0 && c->data) kfree(c->data);
        c->data = new_buf;
        c->capacity = new_cap;
    }

    memcpy(c->data + offset, buf, len);
    if (needed > c->size) c->size = needed;
    return (int32_t) len;
}

int32_t initrd_read_content(int content_id, void* buf, uint32_t offset, uint32_t len) {
    if (content_id < 0 || content_id >= INITRD_MAX_FILES) return -1;
    content_slot_t* c = &g_content[content_id];
    if (!c->in_use) return -1;
    if (offset >= c->size) return 0; /* EOF */
    uint32_t avail = c->size - offset;
    uint32_t n = (len < avail) ? len : avail;
    memcpy(buf, c->data + offset, n);
    return (int32_t) n;
}

int initrd_content_size_of(int content_id) {
    if (content_id < 0 || content_id >= INITRD_MAX_FILES) return -1;
    if (!g_content[content_id].in_use) return -1;
    return (int) g_content[content_id].size;
}

/* زيادة عداد الفتح لمحتوى مفتوح أصلا (fork/dup: نفس الـ content_id يُشارَك بين
   fds متعددة، فلازم كل fd إضافي يُحسب حتى لا يُحرَّر المحتوى عند أول close). */
void initrd_ref_content(int content_id) {
    if (content_id < 0 || content_id >= INITRD_MAX_FILES) return;
    if (!g_content[content_id].in_use) return;
    g_content[content_id].open_count++;
}


/* ===================== المجلدات ===================== */

static int name_under(const char* name, const char* dir, size_t dn) {
    for (size_t i = 0; i < dn; i++) if (name[i] != dir[i]) return 0;
    return name[dn] == '/';
}

int initrd_is_dir(const char* path) {
    const char* norm = normalize_name(path);
    if (norm[0] == '\0') return 1; /* الجذر */
    size_t n = kstrlen(norm);
    for (int i = 0; i < g_conv_count; i++) {
        const char* nm = g_conv[i].name;
        if (nm[0] == '\0') continue;
        if (g_conv[i].is_dir && kstrcmp(nm, norm) == 0) return 1;
        if (name_under(nm, norm, n)) return 1; /* مجلد ضمني */
    }
    return 0;
}

/* يتحقق أن أب المسار مجلد. 0 = نعم، -3 غير موجود، -4 موجود لكنه ملف */
static int parent_check(const char* norm) {
    int k = -1;
    for (int i = 0; norm[i]; i++) if (norm[i] == '/') k = i;
    if (k < 0) return 0; /* الأب هو الجذر */
    char parent[INITRD_MAX_NAME];
    memcpy(parent, norm, (size_t) k);
    parent[k] = '\0';
    if (initrd_is_dir(parent)) return 0;
    return initrd_find(parent) >= 0 ? -4 : -3;
}

int initrd_mkdir(const char* path) {
    const char* norm = normalize_name(path);
    if (norm[0] == '\0') return -2;
    if (initrd_find(norm) >= 0 || initrd_is_dir(norm)) return -2;
    int pc = parent_check(norm);
    if (pc < 0) return pc;
    size_t n = kstrlen(norm);
    if (n >= INITRD_MAX_NAME) return -6;
    int idx = conv_alloc_slot();
    if (idx < 0) return -1;
    memcpy(g_conv[idx].name, norm, n + 1);
    g_conv[idx].content_id = -1;
    g_conv[idx].is_dir = 1;
    return idx;
}

int initrd_rmdir(const char* path) {
    const char* norm = normalize_name(path);
    if (norm[0] == '\0') return -6;
    int idx = initrd_find(norm);
    if (idx >= 0 && !g_conv[idx].is_dir) return -4;
    if (!initrd_is_dir(norm)) return -3;
    size_t n = kstrlen(norm);
    for (int i = 0; i < g_conv_count; i++)
        if (g_conv[i].name[0] && name_under(g_conv[i].name, norm, n)) return -5;
    if (idx >= 0) g_conv[idx].name[0] = '\0'; /* tombstone */
    return 0;
}

int initrd_list(const char* dirpath, int cookie, char* out, int out_max, int* is_dir) {
    const char* d = normalize_name(dirpath);
    size_t dn = kstrlen(d);
    for (int i = cookie < 0 ? 0 : cookie; i < g_conv_count; i++) {
        const char* nm = g_conv[i].name;
        if (nm[0] == '\0') continue;
        const char* rest;
        if (dn == 0) rest = nm;
        else if (name_under(nm, d, dn)) rest = nm + dn + 1;
        else continue;
        if (rest[0] == '\0') continue;
        int len = 0;
        while (rest[len] && rest[len] != '/') len++;
        if (len >= out_max) continue;
        int deeper = rest[len] == '/';
        if (deeper) {
            /* مجلد ضمني: نصدره مرة واحدة فقط، وإذا لم يكن له entry صريحة (تصدره بنفسها) */
            char child[INITRD_MAX_NAME];
            size_t cl = 0;
            if (dn) { memcpy(child, d, dn); child[dn] = '/'; cl = dn + 1; }
            if (cl + (size_t) len >= INITRD_MAX_NAME) continue;
            memcpy(child + cl, rest, (size_t) len);
            child[cl + (size_t) len] = '\0';
            int ex = initrd_find(child);
            if (ex >= 0 && g_conv[ex].is_dir) continue;
            int dup = 0;
            for (int j = 0; j < i && !dup; j++) {
                const char* nj = g_conv[j].name;
                if (nj[0] == '\0') continue;
                const char* rj;
                if (dn == 0) rj = nj; else if (name_under(nj, d, dn)) rj = nj + dn + 1; else continue;
                int k = 0;
                while (k < len && rj[k] == rest[k]) k++;
                if (k == len && (rj[k] == '/' || rj[k] == '\0')) dup = 1;
            }
            if (dup) continue;
        }
        memcpy(out, rest, (size_t) len);
        out[len] = '\0';
        *is_dir = deeper ? 1 : g_conv[i].is_dir;
        return i + 1;
    }
    return 0;
}

int initrd_rename(const char* old_path, const char* new_path) {
    char o[INITRD_MAX_NAME], n[INITRD_MAX_NAME];
    const char* po = normalize_name(old_path);
    const char* pn = normalize_name(new_path);
    if (po[0] == '\0' || pn[0] == '\0') return -6;
    size_t ol = kstrlen(po), nl = kstrlen(pn);
    if (ol >= INITRD_MAX_NAME || nl >= INITRD_MAX_NAME) return -6;
    memcpy(o, po, ol + 1);
    memcpy(n, pn, nl + 1);
    if (kstrcmp(o, n) == 0) return 0;

    int oi = initrd_find(o);
    int odir = initrd_is_dir(o);
    if (oi < 0 && !odir) return -3;
    int pc = parent_check(n);
    if (pc < 0) return pc;

    int ni = initrd_find(n);
    if (odir) {
        if (name_under(n, o, ol)) return -6; /* نقل مجلد إلى داخل نفسه */
        if (ni >= 0 && !g_conv[ni].is_dir) return -4;
        if (initrd_is_dir(n)) {
            if (initrd_rmdir(n) < 0) return -5; /* مجلد الهدف غير فارغ */
        }
        for (int i = 0; i < g_conv_count; i++) {
            char* nm = g_conv[i].name;
            if (nm[0] == '\0') continue;
            int match = (kstrcmp(nm, o) == 0);
            if (!match && !name_under(nm, o, ol)) continue;
            const char* rest = match ? "" : nm + ol; /* يبدأ بـ "/" */
            size_t rl = kstrlen(rest);
            if (nl + rl >= INITRD_MAX_NAME) return -6;
            char tmp[INITRD_MAX_NAME];
            memcpy(tmp, n, nl);
            memcpy(tmp + nl, rest, rl + 1);
            memcpy(nm, tmp, nl + rl + 1);
        }
        return 0;
    }
    if (initrd_is_dir(n)) return -7;
    if (ni >= 0) initrd_unlink(n); /* الاستبدال: الاسم القديم للهدف يسقط (المحتوى يتحرر عند آخر fd) */
    memcpy(g_conv[oi].name, n, nl + 1);
    return 0;
}
