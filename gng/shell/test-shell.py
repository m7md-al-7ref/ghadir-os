#!/usr/bin/env python3
"""يختبر الـ shell تفاعليًا عبر serial: يقلّد مستخدمًا يكتب أوامر وينتظر الـ prompt بعد كل أمر.
الاستعمال:  test-shell.py ghadir-os-shell.iso   (يحتاج qemu-system-i386)"""
import subprocess, sys, time, os, select, re

LITE = "--lite" in sys.argv          # نسخة خفيفة (tcc فقط): نتجاوز اختبارات gcc
iso = [a for a in sys.argv[1:] if not a.startswith("--")][0]
q = subprocess.Popen(["qemu-system-i386", "-cpu", "max", "-m", "512", "-cdrom", iso, "-display", "none",
                      "-serial", "stdio", "-no-reboot"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fd = q.stdout.fileno()
buf = b""
PROMPT = re.compile(rb"root@ghadir:[^\r\n#]*# $")
# سجلات النواة تصل على serial بشكل غير متزامن (قد تأتي بعد الـ prompt مباشرة): نزيلها قبل مطابقة الـ prompt
KLOG = re.compile(rb"\[(?:trust_window|process|compat|elf|kernel-task|pit|pmm|paging|kheap|selftest|syscall|ledger|spinlock|fpu|boot|scheduler|pipe|initrd|gdt|idt|tss|rng|keyboard|stack)[^\]]*\][^\n]*")
def strip_klog(b): return KLOG.sub(b"", b)

def read_until_prompt(timeout):
    """يقرأ حتى يظهر الـ prompt في آخر الإخراج؛ يرجع النص (بدون سجلات النواة [..])."""
    global buf
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            d = os.read(fd, 65536)
            if not d: break
            buf += d
        if PROMPT.search(strip_klog(buf.replace(b"\r", b"")).rstrip(b"\n ") + b" "):
            out = buf.replace(b"\r", b"").decode("utf-8", "replace"); buf = b""
            return out
    out = buf.replace(b"\r", b"").decode("utf-8", "replace"); buf = b""
    return None if not out else out + "\n<<TIMEOUT>>"

KLOG_S = re.compile(r"\[(?:trust_window|process|compat|elf|kernel-task|pit|pmm|paging|kheap|selftest|syscall|ledger|spinlock|fpu|boot|scheduler|pipe|initrd|gdt|idt|tss|rng|keyboard|stack)[^\]]*\][^\n]*")
def clean(out):
    """يزيل سجلات النواة (تصل على serial بشكل غير متزامن وقد تلتصق بنهاية سطر الـ prompt)"""
    return "\n".join(l for l in KLOG_S.sub("", out).split("\n"))

def send(text, raw=False):
    q.stdin.write(text.encode("latin1")); q.stdin.flush()

results = []
def run(cmd, expect=None, timeout=60, absent=None, note=None):
    send(cmd + "\n")
    out = read_until_prompt(timeout)
    c = clean(out) if out else "<<NO OUTPUT>>"
    ok = out is not None and "<<TIMEOUT>>" not in out
    if expect is not None: ok = ok and all(e in c for e in (expect if isinstance(expect, list) else [expect]))
    if absent is not None: ok = ok and all(a not in c for a in (absent if isinstance(absent, list) else [absent]))
    results.append((ok, note or cmd))
    print(("[PASS] " if ok else "[FAIL] ") + (note or cmd))
    if not ok: print("   --- output:\n" + "\n".join("   | " + l for l in c.split("\n")[-12:]))
    return c

first = read_until_prompt(120)
print("boot:", "prompt reached" if first else "NO PROMPT")
if not first: q.kill(); sys.exit(1)

run("echo hello world", "hello world")
run("pwd", "/home", note="profile did cd /home")
run("ls", "hello.c")
run("mkdir d1 d1/sub", note="mkdir")
run("cd d1/sub && pwd", "/home/d1/sub")
run("cd /; pwd", "\n/\n")
run("echo abc > /home/f.txt", note="redirect >")
run("echo def >> /home/f.txt; cat /home/f.txt", ["abc", "def"], note="redirect >> and cat")
run("cat /home/f.txt | grep def | wc -l", "1", note="pipeline cat|grep|wc")
run("cat < /home/f.txt", ["abc"], note="redirect <")
run("false || echo fallback; true && echo andok", ["fallback", "andok"], note="|| and &&")
run("nosuchcmd; echo status=$?", ["command not found", "status=127"], note="command not found = 127")
run("echo '$HOME' \"$HOME\"", ["$HOME", "/home"], note="quotes and variables")
run("export FOO=bar; echo $FOO", "bar", note="export")
run("cd /home; touch a.c b.c; ls *.c", ["a.c", "b.c"], note="glob *.c")
run("cp f.txt g.txt && mv g.txt h.txt", note="cp and mv")
run("ls", ["h.txt"], absent=["g.txt"], note="cp/mv result: h.txt exists, g.txt gone")
run("ls -l f.txt", ["rw", "f.txt"], note="ls -l")
run("free", "total", note="free")
run("uname -a", "ginux", note="uname")
run("date", "UTC", note="date")
# حذف مع نافذة الأمانة ثم undo
run("echo keep > u.txt; rm u.txt", "trust window", note="rm shows trust window message")
run("undo", "cancelled", note="undo cancels last rm")
time.sleep(5)
run("ls", "u.txt", note="file survived thanks to undo")
run("echo gone > v.txt; rm v.txt", "trust window")
time.sleep(5)
run("ls", absent="v.txt", note="removed file disappears after the window")
# ترجمة داخل النظام
c = run("cc hello.c -o hello && ./hello", "Hello from Ghadir OS! square(12) = 144", timeout=240, note="cc hello.c -o hello && ./hello  (GCC inside the shell)")
run("echo $?", "0", note="exit status 0")
# إنشاء ملف C بالكتابة المباشرة وإنهاء الإدخال بـ Ctrl-D
send("cat > t.c\n"); time.sleep(1)
send("int main(void) { return 7; }\n"); time.sleep(1)
send("\x04")
read_until_prompt(30)
run("cc t.c -o t; ./t; echo rc=$?", "rc=7", timeout=240, note="typed a C file with cat > t.c (Ctrl-D) then compiled/ran it")
# Ctrl-C
send("sleep 60\n"); time.sleep(3); send("\x03")
out = read_until_prompt(20); ok = out is not None and "<<TIMEOUT>>" not in out and "^C" in out
results.append((ok, "Ctrl-C stops 'sleep 60'")); print(("[PASS] " if ok else "[FAIL] ") + "Ctrl-C stops 'sleep 60'")
run("echo st=$?", "st=130", note="status after Ctrl-C is 130")
# Ctrl-C يقتل شجرة العمليات (gcc وأبناؤه) -- يحتاج gcc (يُتجاوز في النسخة الخفيفة)
if not LITE:
    send("cc big.c -o bigout\n"); time.sleep(4); send("\x03")
    out = read_until_prompt(30); ok = out is not None and "<<TIMEOUT>>" not in out
    results.append((ok, "Ctrl-C kills gcc and its children (prompt returns)")); print(("[PASS] " if ok else "[FAIL] ") + "Ctrl-C kills gcc and its children (prompt returns)")
    run("echo alive; ls", "alive", absent="bigout", note="shell healthy afterwards and the killed compile left NO output file")
run("cc hello.c -o hello3 && ./hello3", "square(12) = 144", timeout=240, note="compile again after a killed compile (no leaked state)")
run("help", "Builtins", note="help")

q.stdin.close(); time.sleep(0.5); q.kill()
p = sum(1 for ok, _ in results if ok); print(f"\n=== {p}/{len(results)} shell tests passed ===")
sys.exit(0 if p == len(results) else 1)
