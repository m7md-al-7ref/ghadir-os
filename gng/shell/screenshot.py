#!/usr/bin/env python3
"""يلتقط لقطة شاشة حقيقية للنظام (ذاكرة VGA عبر QEMU monitor) بعد كتابة أوامر عبر مفاتيح PS/2.
الاستعمال:  screenshot.py ghadir-os-0.01.iso docs/screenshot.png"""
import subprocess, sys, time, os, select, re, zlib, struct
iso, outpng = sys.argv[1], sys.argv[2]
ppm = "/tmp/ghadir-shot.ppm"
q = subprocess.Popen(["qemu-system-i386", "-cpu", "max", "-m", "512", "-cdrom", iso, "-display", "none",
                      "-serial", "file:/tmp/ghadir-shot-serial.log", "-monitor", "stdio", "-no-reboot"],
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

def screen_text():
    t = mon("xp /2000hx 0xb8000", 1.5); cells = []
    for line in t.replace("\r", "").split("\n"):
        m = re.match(r".*?b8[0-9a-f]{3}:\s+(.*)", line)
        if m: cells += [int(x, 16) for x in re.findall(r"0x([0-9a-f]{4})", m.group(1))]
    return ["".join(chr(c & 0xFF) if 32 <= (c & 0xFF) < 127 else " " for c in cells[r*80:(r+1)*80]).rstrip() for r in range(25)]

KEYS = {" ": "spc", "\n": "ret", "/": "slash", ".": "dot", "-": "minus", "&": "shift-7", "|": "shift-backslash", ">": "shift-dot", "$": "shift-4", ";": "semicolon", "?": "shift-slash", "=": "equal", "_": "shift-minus"}
def typ(s, delay=0.25):
    for ch in s:
        k = KEYS.get(ch, ("shift-" + ch.lower()) if ch.isupper() else ch)
        mon("sendkey " + k, 0.0); time.sleep(delay)

def wait_for(text, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if any(text in r for r in screen_text()): return True
        time.sleep(1.5)
    return False

time.sleep(30); wait_for("root@ghadir:", 90)
typ("uname -a\n"); time.sleep(2)
typ("ls\n"); time.sleep(2)
typ("cat hello.c | head -n 6\n"); time.sleep(2)
typ("cc hello.c -o hello && ./hello\n")
ok = wait_for("square(12)", 150); time.sleep(2)
typ("ls -l hello\n"); time.sleep(2)
mon("screendump " + ppm, 1.0)
q.stdin.write(b"quit\n"); q.stdin.flush(); time.sleep(0.5); q.kill()
d = open(ppm, "rb").read(); parts = d.split(b"\n", 3); w, h = map(int, parts[1].split()); pix = parts[3]
raw = b"".join(b"\x00" + pix[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
open(outpng, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
print("compile+run visible on screen:", ok, "->", outpng)
sys.exit(0 if ok else 1)
