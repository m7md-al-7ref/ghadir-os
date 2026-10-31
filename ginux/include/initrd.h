#ifndef INITRD_H
#define INITRD_H

#include <stdint.h>

#define INITRD_MAX_FILES 512  /* كان 32: سلسلة أدوات (مترجم + مكتبة + headers) تحتاج مئات الملفات */
#define INITRD_MAX_NAME  192  /* مسار كامل (prefix + name في ustar) */

/* initrd.h -- فصل الحقائق عن الاعتباريات (2026-08-16)
 *
 * كتاب الفلسفة، الباب الرابع (الفصول 9-11): الطباطبائي يميز بين
 * الحقائق (أحكام تصف واقعًا موضوعيًا، مستقلة عن أي اتفاق) والاعتباريات
 * (أحكام وضعية وظيفية، قابلة للتغيير الكامل). محتوى الملف = حقيقة؛
 * اسمه وملكيته = اعتباري. هذا الملف يطبق هذا الفصل بنيويًا لأول مرة:
 *
 *   - **طبقة الحقائق** (content layer، داخلية في initrd.c): بايتات
 *     الملف الفعلية فقط. لا تعرف اسمًا ولا مالكًا ولا صلاحيات.
 *   - **طبقة الاعتباريات** (convention layer): اسم الملف + رابط
 *     (content_id) لطبقة الحقائق. هذه الطبقة وحدها القابلة للتغيير
 *     الكامل (إعادة تسمية مفهومية، حذف الاسم) دون أي أثر على المحتوى.
 *
 * الفرق عن التصميم القديم (قبل هذا التحديث): كانت initrd_file_t بنية
 * واحدة تجمع الاسم والمحتوى معًا -- بالضبط نفس مشكلة inode في يونكس
 * التي انتقدها كتاب الفلسفة (الفصل التاسع). الآن الفصل معماري حقيقي،
 * ليس تسمية فقط.
 *
 * إثبات ملموس للفصل: `initrd_link()` -- تستطيع إنشاء اسمين مختلفين
 * يشيران لنفس المحتوى (نفس content_id)، شيء مستحيل بنيويًا في التصميم
 * القديم المدمج. انظر HANDOFF.md للتفاصيل والاختبار الحي.
 *
 * توافق: كل الدوال العامة (initrd_find/get/create/write/truncate/
 * unlink) تحافظ على نفس التوقيع والسلوك الظاهري كما كان قبل هذا
 * التحديث -- الفصل داخلي بالكامل، لا حاجة لتعديل syscall.c. */

typedef struct {
    char name[INITRD_MAX_NAME]; /* اسم الملف -- جزء من طبقة الاعتباريات فقط */
    uint8_t* data;               /* مؤشر مباشر (boot-backed) أو بفر kmalloc'd (heap-backed) */
    uint32_t size;                /* الحجم الفعلي المستعمل */
    uint32_t capacity;            /* 0 = boot-backed (قراءة فقط حتى أول كتابة)، غير 0 = heap-backed */
} initrd_file_t;

void initrd_init(uint32_t phys_start, uint32_t phys_end);

/* ترجع index الملف إذا وجدته (وليس tombstoned)، وإلا -1. الاسم من
   دون "/" في البداية. الـ index هنا هو index طبقة الاعتباريات
   (convention)، وهو نفسه المستخدم كـ initrd_idx في fd_entry_t. */
int initrd_find(const char* path);

/* NULL إذا index خارج الحدود. ملاحظة تصميم: ترجع مؤشرًا لبفر ساكن
   داخلي (scratch buffer) يُعاد ملؤه في كل نداء -- آمن للاستخدام
   الفوري (كما هو الحال في كل نقاط الاستخدام الحالية في syscall.c)،
   لكن غير آمن لحفظ المؤشر عبر نداء initrd_get آخر. توثيق هذا القيد
   صراحة بدل إخفائه. */
const initrd_file_t* initrd_get(int index);

int initrd_count(void);

/* --- طبقة الاعتباريات: عمليات الأسماء --- */

int initrd_create(const char* path);
int32_t initrd_write(int idx, const void* buf, uint32_t offset, uint32_t len);
int initrd_truncate(int idx, uint32_t new_size);

/* حذف الاسم (tombstone) -- طبقة الاعتباريات فقط تتأثر. المحتوى نفسه
   (طبقة الحقائق) لا يُحرَّر فورًا حتى لو وصل refcount إلى صفر -- هذا
   قرار أمان متعمد (نفس فئة القيود الموثقة سابقًا في HANDOFF.md §5):
   fds مفتوحة مسبقًا على نفس الـ index تبقى تشير لنفس content_id عبر
   طبقة الاعتباريات القديمة (المؤرشفة داخليًا)، فالتحرير الفوري قد
   يسبب use-after-free حقيقيًا. التحرير الفعلي مؤجل لمرحلة تتبع fds
   المفتوحة بدقة (خارج نطاق هذا التحديث). */
int initrd_unlink(const char* path);

/* --- إثبات الفصل: ربط اسم جديد بمحتوى موجود --- */

/* ترجع content_id الذي يشير إليه الاسم عند index -- مفيدة لإثبات أن
   اسمين مختلفين يشيران لنفس المحتوى (انظر initrd_link). ترجع -1 إذا
   index غير صالح. */
int initrd_content_id(int index);

/* --- إصلاح: استقرار هوية الـ fd عبر unlink/إعادة استعمال الاسم
   (2026، مراجعة خارجية) ---
 *
 * bug حقيقي كان موجودًا: fd_entry_t كانت تخزّن index طبقة الاعتباريات
 * (نفس القيمة المرجعة من initrd_find/create) مباشرة. عندما تحذف initrd_unlink
 * اسمًا (tombstone)، تستطيع initrd_create اللاحقة إعادة استعمال
 * *نفس* الـ slot لملف جديد كليًا (conv_alloc_slot تبحث عن slot فارغ
 * أولًا). أي fd كانت مفتوحة *قبل* الحذف، لو بقيت تستعمل نفس الـ index،
 * كانت تتحول بصمت لتقرأ/تكتب في الملف الجديد -- انتهاك حقيقي لدلالات
 * unlink(2) القياسية (fd مفتوحة يجب أن تبقى مرتبطة بنفس المحتوى الذي
 * فُتحت عليه، بصرف النظر عمّا يحدث للاسم لاحقًا).
 *
 * الحل: fd_entry_t تخزّن الآن content_id (طبقة الحقائق مباشرة)، يُحسم
 * *مرة واحدة* وقت open()، ولا يُعاد حلّه من الاسم في كل عملية. طبقة
 * الحقائق (content slots) لا تُعاد استعمالها إلا بعد open_count==0
 * *و* refcount==0 معًا (انظر initrd_open_by_index/initrd_close_content) --
 * فمحتوى fd مفتوحة يبقى حيًا وسليمًا مهما حدث لاسمه. */

/* تُستدعى مرة واحدة وقت SYS_OPEN، بعد إيجاد الاسم (initrd_find/create).
   ترجع content_id (يُخزَّن في fd_entry_t)، وتزيد open_count. ترجع -1
   إذا conv_idx غير صالح. */
int initrd_open_by_index(int conv_idx);

/* تُستدعى وقت SYS_CLOSE. تُنقص open_count؛ إذا وصل لصفر *و* refcount
   (عدد الأسماء) وصل لصفر أيضًا، تُحرَّر ذاكرة المحتوى فعليًا (إصلاح حقيقي:
   قبل هذا التحديث، لم يكن المحتوى يُحرَّر أبدًا حتى بعد آخر إغلاق --
   limitation موثقة سابقًا، والآن محلولة بأمان). */
void initrd_close_content(int content_id);

/* عمليات القراءة/الكتابة/fstat على fd مفتوحة -- تعمل على content_id
   مباشرة، دون أي مرور عبر طبقة الأسماء إطلاقًا (استقرار كامل). */
int32_t initrd_write_content(int content_id, const void* buf, uint32_t offset, uint32_t len);
int32_t initrd_read_content(int content_id, void* buf, uint32_t offset, uint32_t len);
int initrd_content_size_of(int content_id); /* -1 إذا content_id غير صالح/محرر */

/* تنشئ اسمًا جديدًا (new_path) يشير إلى نفس محتوى الملف عند
   existing_index -- بلا أي نسخ للبايتات، فقط رابط جديد في طبقة
   الاعتباريات إلى نفس entry في طبقة الحقائق. هذا مستحيل بنيويًا في
   تصميم مدمج (اسم واحد = نسخة واحدة من المحتوى إجباريًا)؛ وهنا ممكن
   لأن الطبقتين منفصلتان فعليًا. ترجع index الاسم الجديد، أو -1 عند
   فشل (الاسم موجود مسبقًا، أو existing_index غير صالح، أو الجدول
   ممتلئ). */
int initrd_link(const char* new_path, int existing_index);

void initrd_ref_content(int content_id);


/* --- المجلدات ---
   الأسماء في طبقة الاعتباريات مسارات كاملة ("usr/include/stdio.h"). المجلد إما entry صريحة
   (is_dir، بلا محتوى) أو *ضمنية* (يوجد اسم تحتها). الجذر "" مجلد دائمًا.
   رموز الأخطاء: -2 موجود، -3 غير موجود، -4 ليس مجلدًا، -5 المجلد غير فارغ، -6 غير صالح، -7 هو مجلد. */
int initrd_is_dir(const char* path);
int initrd_mkdir(const char* path);
int initrd_rmdir(const char* path);
/* تعدّ أبناء المجلد المباشرين: cookie=0 للبداية. ترجع cookie التالي (>0) أو 0 عند النهاية. */
int initrd_list(const char* dirpath, int cookie, char* name_out, int name_max, int* is_dir);
int initrd_rename(const char* old_path, const char* new_path);
int initrd_truncate_content(int content_id, uint32_t new_size);

#endif
