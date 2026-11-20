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
TEST_SECONDS ?= 1
# SUITE selects what the image runs: full (default), benchmark (the sampled
# workloads only, skipping check-only records) or accuracy (every record,
# checked once; the same as BENCHMARKS=0).
SUITE ?= full
ifeq ($(filter full benchmark accuracy,$(SUITE)),)
$(error SUITE must be full, benchmark or accuracy)
endif
SUITE_BENCHMARKS_full =
SUITE_BENCHMARKS_benchmark = 1
SUITE_BENCHMARKS_accuracy = 0
SUITE_BENCHMARKS := $(SUITE_BENCHMARKS_$(SUITE))
ifneq ($(SUITE_BENCHMARKS),)
ifneq ($(origin BENCHMARKS),undefined)
ifneq ($(BENCHMARKS),$(SUITE_BENCHMARKS))
$(error SUITE=$(SUITE) implies BENCHMARKS=$(SUITE_BENCHMARKS); drop BENCHMARKS=$(BENCHMARKS))
endif
endif
BENCHMARKS ?= $(SUITE_BENCHMARKS)
endif
BENCHMARKS ?= 1
ifneq ($(BENCHMARKS),0)
ifneq ($(BENCHMARKS),1)
$(error BENCHMARKS must be 0 or 1)
endif
endif
ONLY ?=
# Output locations; guest-test uses separate ones so it leaves these alone.
BUILD ?= build
IMAGE ?= floppy.img
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

# OPT defaults to -O2 for the established guest instruction mix the records and
# their documented 86Box paths were measured with. Size no longer decides it:
# the loader reads the runtime track by track, so OPT=-O3 also links and boots.
OPT ?= -O2
CFLAGS  = $(TARGET_FLAGS) -march=i486 -mtune=i486 $(STACK_FLAGS) \
          -mno-sse -mno-mmx -mno-80387 -msoft-float $(OPT) -ffreestanding -fno-pic -fno-pie \
          -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -fcf-protection=none -nostdlib -Wall -Wno-unused-function \
          -fno-strict-aliasing \
          -DROUNDS=$(ROUNDS) -DBURN_SECONDS=$(BURN_SECONDS) -DWATCHDOG_SECONDS=$(WATCHDOG_SECONDS) \
          -DHEAVY_SCALE=$(HEAVY_SCALE) -DTEST_SECONDS=$(TEST_SECONDS) -DONLY='"$(ONLY)"' \
          -DBENCHMARKS=$(BENCHMARKS) -DSUITE_BENCHMARK=$(if $(filter benchmark,$(SUITE)),1,0) \
          -DCPUBURN_VERSION='"$(VERSION)"'
# Per-file overrides: FPU/MMX test files need the FPU/MMX instruction sets enabled (inline asm only, no compiler FP code on 486 path).
SRC     = src
SRCS    = core.c arith.c cpu_contract.c $(sort $(notdir $(wildcard $(SRC)/tests_*.c)))
OBJS    = $(SRCS:%.c=$(BUILD)/%.o) $(BUILD)/start.o
DIST    = dist/cpuburn-test-$(VERSION)
SOURCES = $(sort .gitignore LICENSE VERSION README.md RELEASE_NOTES.md Makefile \
          $(wildcard $(SRC)/*.c $(SRC)/*.h $(SRC)/*.asm $(SRC)/*.ld tools/*.py tools/tests/*.py \
                     doc/*.md .github/workflows/*))

all: $(IMAGE)

$(BUILD):
	mkdir -p $(BUILD)

# Records the tools and settings the image is built with. It is rewritten
# only when they differ from the last build, so changing BENCHMARKS, ONLY,
# ROUNDS, CC and so on rebuilds everything without needing `make -B`.
CONFIG_TEXT = $(CC) $(CFLAGS) | $(NASM) | $(LD) | $(OBJCOPY) | $(NM)
$(BUILD)/config.stamp: FORCE | $(BUILD)
	@printf '%s\n' '$(subst ','\'',$(CONFIG_TEXT))' > $@.new; \
	if cmp -s $@.new $@; then rm -f $@.new; else mv -f $@.new $@; fi

$(BUILD)/core.o: VERSION
$(BUILD)/cpu_contract.o: $(SRC)/cpu_models.h $(SRC)/cpu_features.h

$(BUILD)/%.o: $(SRC)/%.c $(SRC)/kernel.h Makefile $(BUILD)/config.stamp | $(BUILD)
	$(CC) $(CFLAGS) $(CFLAGS_$*) -c $< -o $@

$(BUILD)/start.o: $(SRC)/start.asm Makefile $(BUILD)/config.stamp | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/kernel.elf: $(SRC)/link.ld $(OBJS)
	$(LD) -m elf_i386 -T $(SRC)/link.ld -Map $(BUILD)/kernel.map -o $@ $(OBJS)

comma := ,
# Every name in ONLY must be a test: each test's name is a whole string in the
# kernel, so a misspelt one is an error rather than a test silently left out
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	$(if $(ONLY),@bad=; for t in $(subst $(comma), ,$(ONLY)); do \
	    tr '\0' '\n' < $@ | grep -qxF -- "$$t" || { echo "ONLY: no test named '$$t'" >&2; bad=1; }; \
	done; test -z "$$bad")

$(BUILD)/rm.bin: $(SRC)/rm.asm $(SRC)/boot_memory.asm $(BUILD)/kernel.elf $(BUILD)/kernel.bin Makefile | $(BUILD)
	$(NASM) -f bin -I$(SRC)/ -DKERNEL_BYTES=$$(stat -c %s $(BUILD)/kernel.bin) \
	    -DKERNEL_END=0x$$($(NM) -n $(BUILD)/kernel.elf | awk '$$3 == "_end" { print $$1 }') $< -o $@

# boot.asm loads only rm.bin; rm.asm reads the runtime track by track, so the
# limit is the floppy: boot sector + loader + runtime within 2880 sectors.
$(IMAGE): $(SRC)/boot.asm $(BUILD)/rm.bin $(BUILD)/kernel.bin
	@set -eu; rm_secs=$$(( ($$(stat -c %s $(BUILD)/rm.bin) + 511) / 512 )); \
	kernel_bytes=$$(stat -c %s $(BUILD)/kernel.bin); \
	secs=$$(( 1 + rm_secs + (kernel_bytes + 511) / 512 )); \
	echo "floppy: runtime $$kernel_bytes bytes, $$secs of 2880 sectors"; \
	test $$secs -le 2880 || { echo "runtime too large for the 1.44 MB floppy" >&2; exit 1; }; \
	$(NASM) -f bin -I$(SRC)/ -DSTAGE2_SECTORS=$$rm_secs $(SRC)/boot.asm -o $(BUILD)/boot.bin; \
	cat $(BUILD)/boot.bin $(BUILD)/rm.bin $(BUILD)/kernel.bin > $(BUILD)/floppy.tmp; \
	truncate -s 1474560 $(BUILD)/floppy.tmp; \
	mv $(BUILD)/floppy.tmp $@
	@ls -l $@

run-qemu: $(IMAGE)
	qemu-system-i386 -cpu $(or $(CPU),pentium2) -m 32 -drive file=$(IMAGE),format=raw,if=floppy,readonly=on -boot a -display none -serial stdio -debugcon file:$(BUILD)/e9.log -no-reboot

# The host tests import the tools as top-level modules.
UNITTEST = PYTHONPATH=tools $(PYTHON) -m unittest discover -s tools/tests -p 'test_*.py' -v

test:
	$(UNITTEST)

# Builds the three focused images the guest tests in tools/tests/ document, in their
# own build directories, and runs the whole suite under QEMU with them set.
GUEST = $(BUILD)/guest
guest-test:
	$(MAKE) SUITE=full BUILD=$(GUEST)/contract IMAGE=$(GUEST)/contract.img \
	    ONLY=int_partial_boundaries,mmx_alu,3dnow_float
	$(MAKE) SUITE=full BUILD=$(GUEST)/extension IMAGE=$(GUEST)/extension.img \
	    BENCHMARKS=0 ONLY=pm_syscall,pm_rt_syscall,mmx_extended,mmx_ext_kernel
	$(MAKE) SUITE=full BUILD=$(GUEST)/boot IMAGE=$(GUEST)/boot.img \
	    BENCHMARKS=0 BURN_SECONDS=1 TEST_SECONDS=1 ONLY=
	CPUBURN_CONTRACT_IMAGE=$(abspath $(GUEST)/contract.img) \
	CPUBURN_EXTENSION_IMAGE=$(abspath $(GUEST)/extension.img) \
	CPUBURN_BOOT_IMAGE=$(abspath $(GUEST)/boot.img) \
	    $(UNITTEST)

release: $(IMAGE)
	rm -rf $(DIST)
	mkdir -p $(DIST)
	cp $(IMAGE) $(DIST)/floppy.img
	cp LICENSE README.md RELEASE_NOTES.md $(DIST)/
	cp -r doc $(DIST)/
	{ printf 'version=%s\nsuite=%s\nrounds=%s\nbenchmarks=%s\nburn_seconds=%s\ntest_seconds=%s\nheavy_scale=%s\nwatchdog_seconds=%s\nonly=%s\n' \
	    '$(VERSION)' '$(SUITE)' '$(ROUNDS)' '$(BENCHMARKS)' '$(BURN_SECONDS)' '$(TEST_SECONDS)' '$(HEAVY_SCALE)' '$(WATCHDOG_SECONDS)' '$(ONLY)'; \
	  $(CC) --version && $(NASM) --version && $(LD) --version && $(OBJCOPY) --version && $(NM) --version; \
	} > $(DIST)/BUILD.txt
	tar --format=gnu --owner=0 --group=0 --numeric-owner --mtime=@0 --mode=0644 \
	    --transform='s,^,cpuburn-test-$(VERSION)/,' -cf $(DIST)/cpuburn-test-$(VERSION)-source.tar $(SOURCES)
	gzip -n -f $(DIST)/cpuburn-test-$(VERSION)-source.tar
	cd $(DIST) && sha256sum BUILD.txt LICENSE README.md RELEASE_NOTES.md doc/*.md cpuburn-test-$(VERSION)-source.tar.gz floppy.img > SHA256SUMS

clean:
	rm -rf build floppy.img dist

.DELETE_ON_ERROR:
.PHONY: all clean run-qemu test guest-test release FORCE
