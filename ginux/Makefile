SHELL := /bin/bash

CC = gcc
AS = as
LD = ld

# -MMD -MP: تتبع تبعيات الـ headers تلقائيا. بدونها، تعديل struct في process.h يترك
# ملفات .o قديمة بتخطيط مختلف (كان سبب bug حقيقي: exit_status يُكتب بإزاحة وتُقرأ بإزاحة ثانية).
# رقم إصدار Ghadir OS (من ../VERSION) يظهر في شعار الإقلاع وفي uname
GHADIR_VERSION := $(shell cat ../VERSION 2>/dev/null || echo dev)
CFLAGS = -DGHADIR_VERSION='"$(GHADIR_VERSION)"' -m32 -ffreestanding -fstack-protector-all -fno-pie -Wall -Wextra -Iinclude -g -MMD -MP
LDFLAGS = -m elf_i386 -T boot/linker.ld -nostdlib

# ملاحظة: boot/userland_evil.s موجود في المشروع كمثال هجوم تجريبي (out-of-bounds
# pointer, syscall غير مسموح، integer overflow) لاختبار التحقق الأمني، وليس
# مدمجًا في البناء الافتراضي. أضفه إلى AS_SRCS يدويًا إن أردت إعادة الاختبار.
AS_SRCS = boot/boot.s boot/gdt_flush.s boot/idt_flush.s boot/isr.s boot/context_restore.s boot/userland_demo.s boot/hello_elf_blob.s boot/pipe_writer.s boot/pipe_reader.s boot/gng_hello_blob.s boot/cat_motd_blob.s boot/fork_test_blob.s boot/writable_test_blob.s boot/trust_window_test_blob.s boot/separation_test_blob.s boot/fd_identity_test_blob.s
C_SRCS  = src/kernel.c src/vga.c src/serial.c src/gdt.c src/idt.c src/pic.c src/keyboard.c \
          src/pmm.c src/paging.c src/kheap.c src/rng.c src/stack_protector.c src/pit.c \
          src/process.c src/syscall.c src/kstring.c src/spinlock.c src/elf.c src/pipe.c \
          src/initrd.c src/ledger.c src/trust_window.c src/tty.c src/compat_syscall.c

AS_OBJS = $(AS_SRCS:.s=.o)
C_OBJS  = $(C_SRCS:.c=.o)
OBJS = $(AS_OBJS) $(C_OBJS)

KERNEL = ginux.bin
ISO = ginux.iso

all: $(KERNEL)

userland/hello.elf: userland/hello.c userland/user.ld userland/ginux_syscall.h
	$(CC) -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -c userland/hello.c -o userland/hello.o
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o userland/hello.elf userland/hello.o

boot/hello_elf_blob.o: boot/hello_elf_blob.s userland/hello.elf
	$(AS) --32 boot/hello_elf_blob.s -o boot/hello_elf_blob.o

userland/cat_motd.elf: userland/cat_motd.c userland/user.ld userland/ginux_syscall.h
	$(CC) -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -c userland/cat_motd.c -o userland/cat_motd.o
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o userland/cat_motd.elf userland/cat_motd.o

boot/cat_motd_blob.o: boot/cat_motd_blob.s userland/cat_motd.elf
	$(AS) --32 boot/cat_motd_blob.s -o boot/cat_motd_blob.o

userland/fork_test.elf: userland/fork_test.c userland/user.ld userland/ginux_syscall.h
	$(CC) -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -c userland/fork_test.c -o userland/fork_test.o
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o userland/fork_test.elf userland/fork_test.o

boot/fork_test_blob.o: boot/fork_test_blob.s userland/fork_test.elf
	$(AS) --32 boot/fork_test_blob.s -o boot/fork_test_blob.o

userland/writable_test.elf: userland/writable_test.c userland/user.ld userland/ginux_syscall.h
	$(CC) -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -c userland/writable_test.c -o userland/writable_test.o
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o userland/writable_test.elf userland/writable_test.o

boot/writable_test_blob.o: boot/writable_test_blob.s userland/writable_test.elf
	$(AS) --32 boot/writable_test_blob.s -o boot/writable_test_blob.o

userland/separation_test.elf: userland/separation_test.c userland/user.ld userland/ginux_syscall.h
	$(CC) -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -c userland/separation_test.c -o userland/separation_test.o
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o userland/separation_test.elf userland/separation_test.o

boot/separation_test_blob.o: boot/separation_test_blob.s userland/separation_test.elf
	$(AS) --32 boot/separation_test_blob.s -o boot/separation_test_blob.o

userland/fd_identity_test.elf: userland/fd_identity_test.c userland/user.ld userland/ginux_syscall.h
	$(CC) -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -c userland/fd_identity_test.c -o userland/fd_identity_test.o
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o userland/fd_identity_test.elf userland/fd_identity_test.o

boot/fd_identity_test_blob.o: boot/fd_identity_test_blob.s userland/fd_identity_test.elf
	$(AS) --32 boot/fd_identity_test_blob.s -o boot/fd_identity_test_blob.o

userland/trust_window_test.elf: userland/trust_window_test.c userland/user.ld userland/ginux_syscall.h
	$(CC) -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -c userland/trust_window_test.c -o userland/trust_window_test.o
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o userland/trust_window_test.elf userland/trust_window_test.o

boot/trust_window_test_blob.o: boot/trust_window_test_blob.s userland/trust_window_test.elf
	$(AS) --32 boot/trust_window_test_blob.s -o boot/trust_window_test_blob.o

boot/gng_hello_blob.o: boot/gng_hello_blob.s boot/prebuilt/gng-hello.elf
	$(AS) --32 boot/gng_hello_blob.s -o boot/gng_hello_blob.o

%.o: %.s
	$(AS) --32 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJS)

INITRD_DIR = rootfs
INITRD = iso/boot/initrd.tar

# execve_target.elf: تُحمَّل فقط وقت التشغيل عبر execve() (انظر
# userland/cat_motd.c)، وليست مضمَّنة وقت البناء مثل باقي البرامج --
# لذلك يجب أن تكون في rootfs/ نفسها، لا في boot/*_blob.s. كانت موجودة
# كـ binary جاهز بلا قاعدة بناء (bug تغليف حقيقي، اكتُشف بالفحص الشامل
# 2026-08-14) -- أُصلح هنا.
userland/execve_target.elf: userland/execve_target.c userland/user.ld userland/ginux_syscall.h
	$(CC) -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -c userland/execve_target.c -o userland/execve_target.o
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o userland/execve_target.elf userland/execve_target.o

$(INITRD_DIR)/execve_target.elf: userland/execve_target.elf
	cp userland/execve_target.elf $(INITRD_DIR)/execve_target.elf

$(INITRD): $(wildcard $(INITRD_DIR)/*) $(INITRD_DIR)/execve_target.elf
	mkdir -p iso/boot
	tar --format=ustar -cf $(INITRD) -C $(INITRD_DIR) .

iso: $(KERNEL) $(INITRD)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/ginux.bin
	cp grub/grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso

run: iso
	qemu-system-i386 -cdrom $(ISO)

run-serial: iso
	qemu-system-i386 -cdrom $(ISO) -display none -serial stdio

audit:
	@# bug حقيقي أُصلح (2026-08-17، مراجعة خارجية): كان exit status
	@# لهذا الـ target هو status الأمر tee، وليس cppcheck -- فإذا
	@# فشل cppcheck (صلاحيات، أو غير مثبّت)، كان make يرجع
	@# نجاحًا بالخطأ. set -o pipefail يعالج هذه المشكلة.
	set -o pipefail; cppcheck --enable=warning,style,performance,portability --inconclusive --std=c11 --inline-suppr \
		-Iinclude src/ 2>&1 | tee audit_report.txt

-include $(C_OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(KERNEL) $(ISO) src/*.d
	rm -f iso/boot/ginux.bin iso/boot/initrd.tar

.PHONY: all iso run run-serial audit clean

# --- وضع طبقة التوافق (compat): initrd منفصل (rootfs_compat/) فيه ملف "init" => kernel_main يشغّله وحده ---
ISO_COMPAT = ginux-compat.iso
rootfs_compat/init: userland_compat/compattest.c userland_compat/compat.ld userland_compat/build.sh
	userland_compat/build.sh

iso-compat: $(KERNEL) rootfs_compat/init
	rm -rf iso_compat && mkdir -p iso_compat/boot/grub
	tar --format=ustar -cf iso_compat/boot/initrd.tar -C rootfs_compat .
	cp $(KERNEL) iso_compat/boot/ginux.bin
	cp grub/grub.cfg iso_compat/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO_COMPAT) iso_compat 2>/dev/null

.PHONY: iso-compat
