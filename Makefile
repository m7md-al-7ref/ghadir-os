# Ghadir OS -- Makefile الجذر.  ginux (النواة) + gng (الأدوات).   make help
VERSION := $(shell cat VERSION)

.PHONY: help all kernel test toolchain-lite toolchain shell run-shell demo release clean

help:
	@echo "Ghadir OS $(VERSION)"
	@echo "  make kernel          build the ginux kernel"
	@echo "  make test            build + run the kernel test suites on QEMU (fails on any error)"
	@echo "  make toolchain-lite  build dietlibc + tinycc + shell   (~10 min)  -> a light image (tcc only)"
	@echo "  make toolchain       build everything incl. binutils + GCC 4.7.4 (hours on one core)"
	@echo "  make shell           build dist/ghadir-os-$(VERSION).iso (interactive shell + the tools built so far)"
	@echo "  make run-shell       build it and start it in QEMU"
	@echo "  make demo            build the automated tool-chain demo image (needs the full toolchain)"
	@echo "  make release         shell image + SHA256SUMS in dist/"

all: kernel

kernel:
	$(MAKE) -C ginux

test: kernel
	ginux/tools/run-tests.sh

toolchain-lite:
	gng/dietlibc/build.sh
	gng/tcc/build.sh
	gng/shell/build.sh

toolchain: toolchain-lite
	gng/binutils/build.sh
	gng/gcc/build.sh

dist:
	mkdir -p dist

shell: dist
	gng/shell/mkiso.sh $(CURDIR)/dist/ghadir-os-$(VERSION).iso

run-shell: shell
	qemu-system-i386 -cpu max -m 512 -cdrom dist/ghadir-os-$(VERSION).iso

demo: dist
	gng/demo/build-demo.sh $(CURDIR)/dist/ghadir-os-demo-$(VERSION).iso

release: shell
	cd dist && sha256sum ghadir-os-$(VERSION).iso > SHA256SUMS && cat SHA256SUMS

clean:
	$(MAKE) -C ginux clean
	rm -rf dist
