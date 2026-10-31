#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "pic.h"

#define KBD_BUF_SIZE 256

static char kbd_buffer[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;
static int shift_held = 0;

/* خريطة scancode set 1 -> ASCII (US layout, بدون shift) */
static const char scancode_ascii[] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};

static const char scancode_ascii_shift[] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

#define LSHIFT_PRESS   0x2A
#define RSHIFT_PRESS   0x36
#define LSHIFT_RELEASE 0xAA
#define RSHIFT_RELEASE 0xB6

static void kbd_buf_push(char c) {
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
    }
}

static int ctrl_held = 0, caps_lock = 0, ext_prefix = 0;

static void keyboard_callback(struct registers* r) {
    (void) r;
    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) { ext_prefix = 1; return; }   /* بادئة المفاتيح الممتدة (أسهم، Ctrl الأيمن...) */
    int ext = ext_prefix; ext_prefix = 0;

    if (scancode == 0x1D) { ctrl_held = 1; return; }    /* Ctrl (الأيسر أو الأيمن) */
    if (scancode == 0x9D) { ctrl_held = 0; return; }
    if (scancode == LSHIFT_PRESS || scancode == RSHIFT_PRESS) { shift_held = 1; return; }
    if (scancode == LSHIFT_RELEASE || scancode == RSHIFT_RELEASE) { shift_held = 0; return; }

    if (scancode & 0x80) return; /* key release، غير مهتمين به الآن */
    if (ext && scancode != 0x1C && scancode != 0x35) return;   /* نتجاهل الأسهم وغيرها؛ نقبل Enter و/ من لوحة الأرقام */
    if (scancode == 0x3A) { caps_lock ^= 1; return; }            /* Caps Lock */

    if (scancode < sizeof(scancode_ascii)) {
        char base = scancode_ascii[scancode];
        char c;
        if (base >= 'a' && base <= 'z') {
            int upper = shift_held ^ caps_lock;                  /* Shift مع Caps يعطي حروفًا صغيرة */
            c = upper ? (char)(base - 32) : base;
        } else {
            c = shift_held ? scancode_ascii_shift[scancode] : base;
        }
        if (ctrl_held && base >= 'a' && base <= 'z') c = (char)(base & 0x1F);   /* Ctrl-A..Ctrl-Z = 1..26 */
        if (c) kbd_buf_push(c);
    }
}

void keyboard_init(void) {
    register_interrupt_handler(33, keyboard_callback); /* IRQ1 = int 33 */
    irq_clear_mask(1);
}

int keyboard_haskey(void) {
    return kbd_head != kbd_tail;
}

char keyboard_getchar(void) {
    while (kbd_head == kbd_tail)
        __asm__ volatile ("hlt");
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return c;
}
