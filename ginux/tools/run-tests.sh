#!/bin/bash
# run-tests.sh -- اختبار النواة فعليًا على QEMU. يرجع 0 عند النجاح وغير 0 عند أي فشل (للـ CI).
#  1) طبقة التوافق: compattest (عشرات الاختبارات: syscalls، fork/exec، pipes، TLS، ذاكرة، ...) يجب fails=0
#  2) الـ ABI الأصلي: لا panic ولا فشل مستخدم (الـ FAIL الواحد في writable-test مقصود: نافذة الأمانة تؤجل الحذف)
set -u
cd "$(dirname "$0")/.."
LOGDIR="${TMPDIR:-/tmp}"
make iso iso-compat >/dev/null 2>&1 || { echo "FAIL: build"; exit 1; }
rc=0

echo "== compat test suite =="
qemu-system-i386 -cpu max -m 256 -cdrom ginux-compat.iso -display none -serial stdio -no-reboot > "$LOGDIR/ginux-compat.log" 2>&1 &
qpid=$!
for _ in $(seq 1 300); do   # ننتظر سطر النهاية (بحد أقصى 300 ثانية) ثم نوقف QEMU: النواة لا تخرج وحدها
  sleep 1
  grep -aq 'compattest done' "$LOGDIR/ginux-compat.log" && break
  kill -0 $qpid 2>/dev/null || break
done
kill $qpid 2>/dev/null; wait $qpid 2>/dev/null
line=$(tr -d '\r' < "$LOGDIR/ginux-compat.log" | grep -a 'compattest done' | tail -1)
echo "$line"
if ! echo "$line" | grep -q 'fails=0'; then echo "FAIL: compat tests"; rc=1; fi
if ! echo "$line" | grep -Eq 'passes=[1-9][0-9]*'; then echo "FAIL: no passing tests reported"; rc=1; fi
if grep -aq 'PANIC' "$LOGDIR/ginux-compat.log"; then echo "FAIL: kernel panic"; rc=1; fi

echo "== original ABI (userland demos) =="
timeout 40 qemu-system-i386 -cpu max -cdrom ginux.iso -display none -serial stdio -no-reboot > "$LOGDIR/ginux-legacy.log" 2>&1
npass=$(grep -ac '\bPASS\b' "$LOGDIR/ginux-legacy.log")
echo "PASS lines: $npass"
if grep -aq 'PANIC' "$LOGDIR/ginux-legacy.log"; then echo "FAIL: kernel panic"; rc=1; fi
if grep -aq 'USER FAULT' "$LOGDIR/ginux-legacy.log"; then echo "FAIL: unexpected user fault"; rc=1; fi
if [ "$npass" -lt 5 ]; then echo "FAIL: too few PASS lines"; rc=1; fi

[ $rc = 0 ] && echo "ALL KERNEL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $rc
