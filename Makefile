CROSS_COMPILE ?=
HOST_ARCH := $(shell uname -m)
DEFAULT_CC = $(CROSS_COMPILE)gcc
ifeq ($(CROSS_COMPILE),)
ifneq ($(filter aarch64 arm64,$(HOST_ARCH)),)
DEFAULT_CC = clang
endif
endif
ifneq ($(filter default undefined,$(origin CC)),)
CC = $(DEFAULT_CC)
endif
NASM    ?= nasm
PYTHON  ?= python3
ROUNDS  ?= 1
BURN_SECONDS ?= 10
WATCHDOG_SECONDS ?= 90
HEAVY_SCALE ?= 8
TEST_SECONDS ?= 5
BENCHMARKS ?= 1
ifneq ($(BENCHMARKS),0)
ifneq ($(BENCHMARKS),1)
$(error BENCHMARKS must be 0 or 1)
endif
endif
ONLY ?=
VERSION := $(strip $(shell cat VERSION))

IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo 1)
ifeq ($(IS_CLANG),1)
STACK_FLAGS = -mstack-alignment=4
TARGET_FLAGS = --target=i386-unknown-linux-gnu -m32
ifeq ($(origin LD),default)
LD = ld.lld
endif
LD ?= ld.lld
OBJCOPY ?= llvm-objcopy
NM ?= llvm-nm
else
STACK_FLAGS = -mpreferred-stack-boundary=2 -mincoming-stack-boundary=2
TARGET_FLAGS = -m32
ifeq ($(origin LD),default)
LD = $(CROSS_COMPILE)ld
endif
LD ?= $(CROSS_COMPILE)ld
OBJCOPY ?= $(CROSS_COMPILE)objcopy
NM ?= $(CROSS_COMPILE)nm
endif

# Keep -O2 for the established guest instruction mix and compact floppy payload.
# The runtime executes in extended RAM; the BIOS loader still stages the file
# below 0x70000. Keep that size check rather than dropping coverage to fit.
CFLAGS  = $(TARGET_FLAGS) -march=i486 -mtune=i486 $(STACK_FLAGS) \
          -mno-sse -mno-mmx -mno-80387 -msoft-float -O2 -ffreestanding -fno-pic -fno-pie \
          -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -fcf-protection=none -nostdlib -Wall -Wno-unused-function \
          -fno-strict-aliasing \
          -DROUNDS=$(ROUNDS) -DBURN_SECONDS=$(BURN_SECONDS) -DWATCHDOG_SECONDS=$(WATCHDOG_SECONDS) \
          -DHEAVY_SCALE=$(HEAVY_SCALE) -DTEST_SECONDS=$(TEST_SECONDS) -DONLY='"$(ONLY)"' \
          -DBENCHMARKS=$(BENCHMARKS) \
          -DCPUBURN_VERSION='"$(VERSION)"'
# Per-file overrides: FPU/MMX test files need the FPU/MMX instruction sets enabled (inline asm only, no compiler FP code on 486 path).
SRCS    = core.c arith.c cpu_contract.c $(sort $(wildcard tests_*.c))
OBJS    = $(SRCS:%.c=build/%.o) build/start.o
DIST    = dist/cpuburn-test-$(VERSION)
SOURCES = $(sort .gitignore LICENSE VERSION README.md RELEASE_NOTES.md Makefile \
          $(wildcard *.c *.h *.asm *.ld tools/*.py doc/*.md .github/workflows/*))

all: floppy.img

build:
	mkdir -p build

build/core.o: VERSION
build/cpu_contract.o: cpu_models.h cpu_features.h

build/%.o: %.c kernel.h Makefile | build
	$(CC) $(CFLAGS) $(CFLAGS_$*) -c $< -o $@

build/start.o: start.asm Makefile | build
	$(NASM) -f elf32 $< -o $@

build/kernel.elf: link.ld $(OBJS)
	$(LD) -m elf_i386 -T link.ld -Map build/kernel.map -o $@ $(OBJS)

build/kernel.bin: build/kernel.elf
	$(OBJCOPY) -O binary $< $@

build/rm.bin: rm.asm boot_memory.asm build/kernel.elf build/kernel.bin Makefile | build
	$(NASM) -f bin -DKERNEL_BYTES=$$(stat -c %s build/kernel.bin) \
	    -DKERNEL_END=0x$$($(NM) -n build/kernel.elf | awk '$$3 == "_end" { print $$1 }') $< -o $@

floppy.img: boot.asm build/rm.bin build/kernel.bin
	@set -eu; bytes=$$(( $$(stat -c %s build/rm.bin) + $$(stat -c %s build/kernel.bin) )); \
	secs=$$(( (bytes + 511) / 512 )); \
	echo "stage2: $$bytes bytes, $$secs sectors"; \
	test $$secs -le 768 || { echo "kernel too large for low-memory loader staging"; exit 1; }; \
	$(NASM) -f bin -DSTAGE2_SECTORS=$$secs boot.asm -o build/boot.bin; \
	cat build/boot.bin build/rm.bin build/kernel.bin > build/floppy.tmp; \
	truncate -s 1474560 build/floppy.tmp; \
	mv build/floppy.tmp $@
	@ls -l $@

run-qemu: floppy.img
	qemu-system-i386 -cpu $(or $(CPU),pentium2) -m 32 -drive file=floppy.img,format=raw,if=floppy,readonly=on -boot a -display none -serial stdio -debugcon file:build/e9.log -no-reboot

test:
	$(PYTHON) -m unittest discover -s tools -p 'test_*.py' -v

release: floppy.img
	mkdir -p $(DIST)
	cp floppy.img LICENSE README.md RELEASE_NOTES.md $(DIST)/
	cp -r doc $(DIST)/
	{ printf 'version=%s\nrounds=%s\nbenchmarks=%s\nburn_seconds=%s\ntest_seconds=%s\nheavy_scale=%s\nwatchdog_seconds=%s\nonly=%s\n' \
	    '$(VERSION)' '$(ROUNDS)' '$(BENCHMARKS)' '$(BURN_SECONDS)' '$(TEST_SECONDS)' '$(HEAVY_SCALE)' '$(WATCHDOG_SECONDS)' '$(ONLY)'; \
	  $(CC) --version && $(NASM) --version && $(LD) --version && $(OBJCOPY) --version && $(NM) --version; \
	} > $(DIST)/BUILD.txt
	tar --format=gnu --owner=0 --group=0 --numeric-owner --mtime=@0 --mode=0644 \
	    --transform='s,^,cpuburn-test-$(VERSION)/,' -cf $(DIST)/cpuburn-test-$(VERSION)-source.tar $(SOURCES)
	gzip -n -f $(DIST)/cpuburn-test-$(VERSION)-source.tar
	cd $(DIST) && sha256sum BUILD.txt LICENSE README.md RELEASE_NOTES.md doc/*.md cpuburn-test-$(VERSION)-source.tar.gz floppy.img > SHA256SUMS

clean:
	rm -rf build floppy.img dist

.DELETE_ON_ERROR:
.PHONY: all clean run-qemu test release
