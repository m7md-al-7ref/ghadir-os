#!/usr/bin/env python3
"""يختبر مسار نافذة QEMU الرسومية: ضغطات مفاتيح PS/2 (عبر monitor: sendkey) ثم قراءة ذاكرة شاشة VGA (0xB8000)
وإعادة بناء النص المعروض. الاستعمال:  test-gui.py ghadir-os-shell.iso"""
import subprocess, sys, time, os, select, re
iso = sys.argv[1]
q = subprocess.Popen(["qemu-system-i386", "-cpu", "max", "-m", "512", "-cdrom", iso, "-display", "none",
                      "-serial", "file:/tmp/gui-serial.log", "-monitor", "stdio", "-no-reboot"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fd = q.stdout.fileno()

def mon(cmd, wait=0.5):
    q.stdin.write((cmd + "\n").encode()); q.stdin.flush()
    out = b""; end = time.time() + wait
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            d = os.read(fd, 1 << 20)
            if not d: break
            out += d; end = time.time() + 0.3
    return out.decode("utf-8", "replace")

def screen():
    t = mon("xp /2000hx 0xb8000", 1.5)
    cells = []
    for line in t.replace("\r", "").split("\n"):
        m = re.match(r".*?b8[0-9a-f]{3}:\s+(.*)", line)
        if m: cells += [int(x, 16) for x in re.findall(r"0x([0-9a-f]{4})", m.group(1))]
    rows = []
    for r in range(25):
        row = "".join(chr(c & 0xFF) if 32 <= (c & 0xFF) < 127 else " " for c in cells[r*80:(r+1)*80])
        rows.append(row.rstrip())
    return rows

KEYS = {" ": "spc", "\n": "ret", "/": "slash", ".": "dot", "-": "minus", "=": "equal", ">": "shift-dot", "<": "shift-comma",
        "|": "shift-backslash", "$": "shift-4", "_": "shift-minus", "'": "apostrophe", ";": "semicolon", ":": "shift-semicolon", "*": "shift-8", "&": "shift-7", "?": "shift-slash"}
def typ(s, delay=0.25):
    for ch in s:
        k = KEYS.get(ch, ("shift-" + ch.lower()) if ch.isupper() else ch)
        mon("sendkey " + k, 0.0); time.sleep(delay)

def wait_for(text, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        rows = screen()
        if any(text in r for r in rows): return rows
        time.sleep(1.5)
    return screen()

res = []
def check(name, cond, rows=None):
    res.append(cond); print(("[PASS] " if cond else "[FAIL] ") + name)
    if not cond and rows: print("\n".join("   | " + r for r in rows if r))

time.sleep(30)
rows = wait_for("root@ghadir:", 90)
check("boot: prompt visible ON THE VGA SCREEN", any("root@ghadir:" in r for r in rows), rows)
check("boot: kernel logs do NOT pollute the screen", not any(("[process]" in r or "[compat]" in r or "still alive" in r) for r in rows), rows)
check("boot: banner visible", any("Ghadir OS" in r for r in rows), rows)

typ("echo hello gui\n"); rows = wait_for("hello gui", 15)
check("keyboard: 'echo hello gui' typed via PS/2 and output shown on VGA", sum("hello gui" in r for r in rows) >= 2, rows)

typ("echo AbC\n"); rows = wait_for("AbC", 15)
check("keyboard: Shift gives uppercase (AbC)", sum("AbC" in r for r in rows) >= 2, rows)

mon("sendkey caps_lock", 0.0); time.sleep(0.5); typ("echo abc\n"); mon("sendkey caps_lock", 0.0); time.sleep(0.5)
rows = wait_for("ECHO ABC", 15)
check("keyboard: Caps Lock gives uppercase (typed echo abc -> ECHO ABC)", any("ECHO ABC" in r for r in rows), rows)

typ("echox"); mon("sendkey backspace", 0.0); time.sleep(0.3); typ(" ok\n"); rows = wait_for("ok", 15)
check("keyboard: Backspace edits the line (echox<BS> ok -> 'ok')", any(r.strip() == "ok" for r in rows), rows)

typ("echo one\n"); wait_for("one", 10)
mon("sendkey ctrl-l", 0.0)
typ("clear\n"); time.sleep(3); rows = screen()
nonempty = sum(1 for r in rows if r.strip())
check("clear: screen is cleared (ESC[2J works on VGA)", nonempty <= 2, rows)

typ("ls\n"); rows = wait_for("hello.c", 15)
check("ls output on screen", any("hello.c" in r for r in rows), rows)

typ("sleep 100\n"); time.sleep(4); mon("sendkey ctrl-c", 0.0); rows = wait_for("^C", 15)
time.sleep(3); typ("echo st=$?\n"); rows = wait_for("st=130", 20)
check("Ctrl-C from the PS/2 keyboard stops 'sleep 100' (status 130)", any(r.strip() == "st=130" for r in rows), rows)

typ("cat > k.txt\n"); time.sleep(1); typ("typed by hand\n"); time.sleep(1); mon("sendkey ctrl-d", 0.0); time.sleep(2)
typ("cat k.txt\n"); rows = wait_for("typed by hand", 15)
check("Ctrl-D ends 'cat > k.txt'; file contains the typed line", sum("typed by hand" in r for r in rows) >= 1 and any(r.strip() == "typed by hand" for r in rows), rows)

print("\n--- final screen ---"); print("\n".join(screen()))
mon("screendump /tmp/gui-screen.ppm", 1.0)
q.stdin.write(b"quit\n"); q.stdin.flush(); time.sleep(0.5); q.kill()
print(f"\n=== {sum(res)}/{len(res)} GUI-path tests passed ===")
sys.exit(0 if all(res) else 1)
