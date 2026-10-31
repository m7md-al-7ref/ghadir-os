#include "elf.h"
#include "pmm.h"
#include "kstring.h"
#include "serial.h"

/* أي ELF قابل للتحميل يجب أن يكون بكامله داخل هذا المدى -- منطقة معزولة
   بالكامل عن النواة (PDPT entry خاص، وليس مشتركًا). هذا خط دفاع أساسي:
   بدون هذا التحقق، يستطيع ELF خبيث تحديد p_vaddr=0x00100000 (عنوان النواة)
   فيكتب فوق/يخرّب الذاكرة المشتركة بين كل العمليات. */
#define USER_SPACE_MIN 0x40000000
#define USER_SPACE_MAX 0x80000000

int elf_validate(const void* data, uint32_t size) {
    if (size < sizeof(elf32_ehdr_t)) return 0;
    const elf32_ehdr_t* eh = (const elf32_ehdr_t*) data;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        kprintf("[elf] invalid magic\n");
        return 0;
    }
    if (eh->e_ident[4] != 1) { kprintf("[elf] not 32-bit\n"); return 0; }
    if (eh->e_ident[5] != 1) { kprintf("[elf] not little-endian\n"); return 0; }
    if (eh->e_type != ET_EXEC) { kprintf("[elf] not an executable (ET_EXEC)\n"); return 0; }
    if (eh->e_machine != 3) { kprintf("[elf] not i386\n"); return 0; }
    if (eh->e_phoff == 0 || eh->e_phnum == 0) { kprintf("[elf] no program headers\n"); return 0; }
    /* bug حقيقي أُصلح (2026-08-17، مراجعة خارجية ثانية): كنا نفترض
       e_phentsize == sizeof(elf32_phdr_t) بلا أي تحقق -- ELF مشوَّه
       (malformed) بقيمة e_phentsize مختلفة كان يجعل القراءة اللاحقة
       (base + e_phoff + i*e_phentsize) تقرأ offsets خاطئة تمامًا. */
    if (eh->e_phentsize != sizeof(elf32_phdr_t)) {
        kprintf("[elf] SECURITY: e_phentsize=%d != expected %d -- rejecting\n",
                eh->e_phentsize, (int) sizeof(elf32_phdr_t));
        return 0;
    }
    if ((uint64_t) eh->e_phoff + (uint64_t) eh->e_phnum * sizeof(elf32_phdr_t) > size) {
        kprintf("[elf] program headers extend past file size\n");
        return 0;
    }
    return 1;
}

uint32_t elf_get_entry(const void* data) {
    return ((const elf32_ehdr_t*) data)->e_entry;
}

int elf_load(const void* data, uint32_t size, address_space_t* as, uint32_t* out_entry,
             uint32_t* out_min_vaddr, uint32_t* out_max_vaddr) {
    if (!elf_validate(data, size)) return 0;

    const elf32_ehdr_t* eh = (const elf32_ehdr_t*) data;
    const uint8_t* base = (const uint8_t*) data;
    uint32_t min_vaddr = 0xFFFFFFFF, max_vaddr = 0;
    int loadable_segment_count = 0;
    int entry_is_valid = 0; /* هل e_entry يقع داخل segment قابل للتنفيذ فعليًا محمَّل؟ */

    for (int i = 0; i < eh->e_phnum; i++) {
        const elf32_phdr_t* ph = (const elf32_phdr_t*)(base + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        loadable_segment_count++;

        /* --- تحقق أمني: حدود العنوان --- */
        uint64_t seg_end = (uint64_t) ph->p_vaddr + ph->p_memsz;
        if (ph->p_vaddr < USER_SPACE_MIN || seg_end > USER_SPACE_MAX || seg_end < ph->p_vaddr) {
            kprintf("[elf] SECURITY: PT_LOAD segment [%x-%x] outside allowed user range -- rejecting whole ELF\n",
                    ph->p_vaddr, (uint32_t) seg_end);
            return 0;
        }
        if ((uint64_t) ph->p_offset + ph->p_filesz > size) {
            kprintf("[elf] SECURITY: segment file offset/size exceeds ELF file size -- rejecting\n");
            return 0;
        }
        if (ph->p_filesz > ph->p_memsz) {
            kprintf("[elf] SECURITY: p_filesz > p_memsz (malformed) -- rejecting\n");
            return 0;
        }

        /* --- صلاحيات الصفحة حسب flags الـ segment (W^X) --- */
        uint64_t flags = PAGE_PRESENT | PAGE_USER;
        if (ph->p_flags & PF_W) flags |= PAGE_WRITE;
        if (!(ph->p_flags & PF_X)) flags |= PAGE_NX;
        /* ملاحظة: إذا كان segment فيه PF_W و PF_X معًا (writable+executable)،
           فهذا يكسر W^X -- نرفض الملف كاملًا، فهذا مؤشر خطر حقيقي */
        if ((ph->p_flags & PF_W) && (ph->p_flags & PF_X)) {
            kprintf("[elf] SECURITY: segment is both writable AND executable (violates W^X) -- rejecting\n");
            return 0;
        }

        /* bug حقيقي أُصلح (2026-08-17، مراجعة خارجية ثانية): e_entry
           كان يُقبل بلا أي تحقق -- ELF صالح بنيويًا لكن بـ
           entrypoint خارج أي segment محمَّل (أو داخل segment ليس
           قابلًا للتنفيذ) كان يجعل process_create_elf تبني process
           بحالة عبثية (تقفز إلى عنوان لا يوجد فيه كود فعلي). */
        if ((ph->p_flags & PF_X) &&
            eh->e_entry >= ph->p_vaddr && eh->e_entry < ph->p_vaddr + ph->p_memsz) {
            entry_is_valid = 1;
        }

        uint32_t page_start = ph->p_vaddr & ~0xFFF;
        uint32_t page_end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFF;

        if (page_start < min_vaddr) min_vaddr = page_start;
        if (page_end > max_vaddr) max_vaddr = page_end;

        for (uint32_t va = page_start; va < page_end; va += PMM_FRAME_SIZE) {
            /* segments تتشارك صفحة واحدة (شائع في binaries حقيقية مثل glibc:
               آخر .rodata وبداية .data في نفس الصفحة بدون -z max-page-size).
               لا نخصص frame ثانية (كانت تكتب فوق محتوى segment السابق)؛
               ندمج الصلاحيات، لكن نرفض إذا أعطى الدمج W+X (يكسر W^X). */
            uint32_t existing = paging_get_phys(as, va);
            if (existing) {
                uint64_t old = paging_get_flags(as, va);
                uint64_t merged = old;
                if (ph->p_flags & PF_W) merged |= PAGE_WRITE;
                if (ph->p_flags & PF_X) merged &= ~PAGE_NX;
                if ((merged & PAGE_WRITE) && !(merged & PAGE_NX)) {
                    kprintf("[elf] SECURITY: shared page %x would become W+X -- rejecting\n", va);
                    return 0;
                }
                if (merged != old) {
                    paging_unmap(as, va);
                    if (!paging_map(as, va, existing, merged)) return 0;
                }
                continue;
            }
            uint32_t phys = pmm_alloc_frame();
            if (!phys) { kprintf("[elf] out of memory while loading\n"); return 0; }
            memset((void*) phys, 0, PMM_FRAME_SIZE);
            if (!paging_map(as, va, phys, flags)) {
                /* bug حقيقي أُصلح (2026-08-17، مراجعة خارجية ثانية):
                   كان فشل paging_map لا يُفحص هنا أصلًا -- إذا نفدت
                   الذاكرة الفيزيائية لبنية PD/PT نفسها (نادر لكن
                   ممكن)، تبقى الصفحة بلا mapping فعلي بصمت. الآن
                   نحرر phys التي خصصناها للتو (لتجنب leak فوري)
                   ونرجع فشلًا صريحًا. ملاحظة صادقة: frames الخاصة بـ segments
                   *سابقة* في نفس هذا التحميل تبقى leak (limitation
                   معروفة، من نفس فئة §5 -- rollback كامل متعدد segments
                   خارج نطاق هذا الإصلاح). */
                kprintf("[elf] paging_map failed while loading (out of page-table memory)\n");
                pmm_free_frame(phys);
                return 0;
            }
        }

        /* ننسخ محتوى الملف: نحتاج الكتابة عبر عنوان قابل للوصول من النواة.
           مع paging_map الخاصة بنا، الصفحة الجديدة معلَّمة PAGE_USER لكن
           ليست PAGE_WRITE إلا إذا كان PF_W محددًا -- وإذا كان الكود read-only (R-X)
           فلا نستطيع الكتابة فيه عبر مساحة عناوين المستخدم، لكن الذاكرة
           الفيزيائية نفسها identity-mapped وقابلة للكتابة من جهة النواة
           (kernel identity map ليس عليها قيود W^X على هذه الصفحات المؤقتة). */
        for (uint32_t off = 0; off < ph->p_filesz; ) {
            uint32_t va = ph->p_vaddr + off;
            uint32_t page_va = va & ~0xFFF;
            uint32_t phys_page = paging_get_phys(as, page_va);
            uint32_t chunk = PMM_FRAME_SIZE - (va & 0xFFF);
            if (chunk > ph->p_filesz - off) chunk = ph->p_filesz - off;

            memcpy((void*)(phys_page + (va & 0xFFF)), base + ph->p_offset + off, chunk);
            off += chunk;
        }
    }

    /* bug حقيقي أُصلح (2026-08-17، مراجعة خارجية ثانية): يستطيع ELF أن يمر
       elf_validate() بلا أي PT_LOAD segment واحد (كل الـ headers من
       نوع آخر) -- كان min_vaddr يبقى 0xFFFFFFFF وmax_vaddr=0،
       والدالة ترجع "نجاح" بحالة عبثية تمامًا. */
    if (loadable_segment_count == 0) {
        kprintf("[elf] SECURITY: no PT_LOAD segments found -- rejecting\n");
        return 0;
    }
    if (!entry_is_valid) {
        kprintf("[elf] SECURITY: e_entry=%x not inside any executable loaded segment -- rejecting\n",
                eh->e_entry);
        return 0;
    }

    *out_entry = eh->e_entry;
    if (out_min_vaddr) *out_min_vaddr = min_vaddr;
    if (out_max_vaddr) *out_max_vaddr = max_vaddr;
    kprintf("[elf] loaded OK, entry=%x range=[%x-%x]\n", eh->e_entry, min_vaddr, max_vaddr);
    return 1;
}

/* عنوان program headers داخل الذاكرة المحمَّلة (لـ AT_PHDR في auxv) --
   ld يضع ELF header + phdrs داخل أول PT_LOAD (offset 0)، فالعنوان =
   p_vaddr + e_phoff. نرجع 0 إذا لم نجده (البرنامج الساكن لا يحتاجه إلا لـ TLS لاحقًا). */
int elf_phdr_info(const void* data, uint32_t* out_phdr_vaddr, uint32_t* out_phnum) {
    const elf32_ehdr_t* eh = (const elf32_ehdr_t*) data;
    const uint8_t* base = (const uint8_t*) data;
    *out_phdr_vaddr = 0;
    *out_phnum = eh->e_phnum;
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf32_phdr_t* ph = (const elf32_phdr_t*)(base + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_LOAD && ph->p_offset == 0) {
            *out_phdr_vaddr = ph->p_vaddr + eh->e_phoff;
            return 1;
        }
    }
    return 0;
}
