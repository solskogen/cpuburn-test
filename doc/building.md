# Building and booting the image

## Build and boot

Requirements: GNU make, GNU coreutils, NASM and a C compiler/linker/objcopy
targeting i386 ELF. Local release packaging also uses GNU tar and gzip. Python is **not**
used to build or package the image; Python 3.10+ is only needed for the
optional host tests, runner and reporting tools. Native
x86 Linux can use GCC and GNU binutils; Clang uses LLVM tools. The supplied
Makefile targets a Unix-like build environment and supports both x86-64 and
aarch64 Linux build hosts.
No multilib C library or cross-compiled OS is needed. Compiler support
routines (64-bit division, `memset`/`memcpy`/`memmove`/`memcmp`) are built from
source with the same i486 instruction baseline.

```sh
make
make CC=clang
make run-qemu CPU=pentium2
```

On **aarch64 Linux**, `make` defaults to Clang's built-in cross-target support:

```sh
# Install host-native clang, lld, llvm (including llvm-objcopy), NASM,
# GNU make and GNU coreutils (plus tar/gzip for packaging), then:
make
make release
```

The Makefile explicitly selects `--target=i386-unknown-linux-gnu`,
`ld.lld -m elf_i386` and `llvm-objcopy`; it never asks an aarch64 system
linker to link i386 objects. NASM runs on the build host and emits x86 code.
No x86 runtime, multilib libc, target sysroot or emulator is required to build.

Alternatively, install an i386/i686-targeting GNU cross-toolchain:

```sh
# Debian/Ubuntu tool names (also available as aarch64-host packages):
make CROSS_COMPILE=i686-linux-gnu-
make release CROSS_COMPILE=i686-linux-gnu-
# A bare-metal toolchain can use CROSS_COMPILE=i686-elf- instead.
```

`CROSS_COMPILE` supplies the default GCC, linker, objcopy and nm prefixes.
Explicit `CC`, `LD`, `OBJCOPY`, `NM` and `NASM` settings take precedence,
including tool installations with versioned executable names. Do not select
an aarch64-targeting GCC: the **host** may be aarch64, but the **output**
must remain 32-bit i486-compatible x86. Building and running the image are
separate: the image still boots inside an x86 PC emulated by 86Box on either host.

Changing compilers, flags or test settings rebuilds automatically through
`build/config.stamp`; `make clean` also removes `build/` results and `dist/`.
Compilation, linking,
image creation and packaging are direct Makefile recipes, without helper
scripts or generated provenance headers. `VERSION` supplies the guest version;
the compiler's built-in version string supplies its `BUILD` identification.
Serial parsers/reports retain this record, including with plain COM1 logs.

The default `-O2` (the `OPT` make variable) preserves the established guest
instruction mix the records were measured with; image size no longer decides
it, and `make OPT=-O3` links and boots too (GCC: 461,020-byte runtime against
334,044 at `-O2`). The runtime executes at **1 MiB** in protected mode;
its zero-initialized state no longer competes with low-memory V86 fixtures.
Diagnostic notes use the upper 32 KiB of
the arena's already-reserved header, leaving workload allocations unchanged.
The ELF has separate code/read-only and writable load segments; these describe
the image layout, not guest page protections for self-modifying-code tests.

### Floppy boot and memory layout

The 512-byte BIOS boot sector loads only the 4 KiB bootstrap (`rm.asm`, 8
sectors). The bootstrap gathers the BIOS memory map, enables A20, and in a
brief protected-mode call verifies that the entire runtime range, including
BSS, is usable extended RAM **before reading any of the runtime**.
Enabled E820 reservations override overlapping usable entries; adjacent usable
entries may cover the range together. Without E820, the contiguous RAM size
reported by E801 or INT 15h/AH=88 is checked. Missing RAM produces a visible
halt (`runtime extended RAM unavailable`, also on port `0xE9`), not an
unchecked copy. The bootstrap then reads the runtime with INT 13h one track per
call (never a multi-track read; CHS from the boot sector's BPB, 18 sectors × 2
heads × 80 cylinders on the 1.44 MB floppy, or the geometry the BIOS reports
when it presents the image as a hard disk) into a bounce buffer at `0x11000`,
and copies each track to its final address at `0x100000` with 32-bit
`rep movsd` from a 16-bit protected-mode window, returning to real mode for the
next read. Failed reads are retried five times with a drive reset, then halt
with `disk read error`. Startup clears BSS, and the allocator places its
reserved header and workload buffers after the linked runtime's end.
The `MEM` record includes `kernel_start` and `kernel_end`.

The main stack remains below `0x90000`, and V86 code/stack fixtures retain
their low-memory addresses; the runtime is linked at 1 MiB and cannot overlap
them, the loader or the bounce buffer. The runtime and its exception
tables/stacks stay inside the first 16 MiB identity-mapped window. This is
still a normal 1,474,560-byte raw floppy image, not an ISO.

Size limits, enforced by the linker script and the image recipe:

- **Runtime file:** at most 2,871 sectors (1,469,952 bytes): the 1.44 MB floppy
  minus the boot sector and the bootstrap. BSS does not occupy the floppy.
- **Runtime end including BSS:** at most `0x3F0000`, so the arena's 64 KiB
  header still fits below `0x400000` and `REQ_MEM8M` workloads keep their
  `0x400000`–`0x7FFFFF` window. The bootstrap rejects machines whose
  contiguous RAM from 1 MiB does not reach the runtime end.

A boot with the 334,044-byte `-O2` runtime makes 39 INT 13h calls (one reset,
one read for the bootstrap, one for the rest of track 0, then one per track),
against 43 with the former loader, which staged the whole file below
`0x70000` and also split reads at 64 KiB DMA boundaries.

To exercise the actual bootstrap RAM validator and linker budgets, run
`PYTHONPATH=tools python3 -m unittest discover -s tools/tests -p 'test_boot_memory.py' -v`.
With NASM and QEMU installed, it also boots the real boot sector and bootstrap
with a synthetic runtime that fills the floppy and checks every dword of it,
and checks rejection with too little or no extended RAM.
With QEMU installed and a full `BENCHMARKS=0` image, setting
`CPUBURN_BOOT_IMAGE="$PWD/floppy.img"` for that command also checks relocated
486 execution and safe rejection of a machine with no extended RAM.

Boot the image as a **legacy BIOS floppy**, preferably write-protected, with
at least 576 KiB conventional RAM and 8 MiB total RAM (16–32 MiB recommended).
Smaller extended-memory configurations skip workloads requiring larger buffers.
This is not a FAT filesystem image; do not mount it or copy files onto it.

The same legacy-PC BIOS boot path is intended for real PCs and VMs. Use a
BIOS/CSM-capable machine, a Hyper-V Generation 1 VM, or the BIOS firmware option
in VMware/VirtualBox. UEFI-only systems and Hyper-V Generation 2 cannot boot this
image directly. A floppy controller, legacy PIC/PIT, and PC-compatible port I/O
are assumed; this is not a UEFI, ARM, or SMP test.

In the default **benchmark-enabled** run, each supported workload gets a
checked warm-up invocation, followed by repeated
checked invocations in a window lasting **at least `TEST_SECONDS` (default 1)
guest second**. The guest timer ticks every 10 ms, so a 1-second window has
about 1% timing resolution on its own; raise `TEST_SECONDS` for more precise
standalone guest timing. Under the host runner the window is held open until
the host-measured speed settles (see
[sustained host profiling](running.md#sustained-host-profiling)).
Assertions remain active throughout. Warm-up failures remain FAIL even when
later invocations pass, preserving cold dynarec/cache bugs. Assertion failures
do not terminate the sustained window; unexpected faults or unavailable safe
preconditions do. Feature-gated skips do not wait for a sample window.
For execution without timed harness repetition, use
[`BENCHMARKS=0`](coverage.md#accuracy-only-builds) or the runner's `--accuracy` option.

The default is one suite round and the final mixed burn, whose individual
invocations last 10 seconds. Heavy workloads use `HEAVY_SCALE=8`; each invocation
does real fixed-count work, rather than sleeping to fill its window.
The default build registers every record, including unavailable-feature
skips; the `SUMMARY` line reports how many ran in each state.
Suite duration varies with CPU, applicable features and invocation lengths;
it is not a fixed wall-clock deadline. In benchmark mode, the
[host profiling runner](running.md#sustained-host-profiling) can extend sampling to
obtain enough actual emulator CPU time. Standalone runs need no controller
and end automatically.
Examples:

```sh
make ROUNDS=1 BURN_SECONDS=5 WATCHDOG_SECONDS=90
make ROUNDS=4 HEAVY_SCALE=8 BURN_SECONDS=20
# Shorter validation build; identical algorithm and checks, shorter burn:
make ROUNDS=1 BURN_SECONDS=1
# Only the named tests (comma-separated, exact names); ids match a full run:
make ONLY=corpus_integer_sequences,int_smc
```

`HEAVY_SCALE` accepts 1–32 and scales the new stress workloads, not every legacy
test. The deterministic seed is defined in `src/kernel.h`, not exposed as a
normal Makefile setting; it is logged with the scale in
`CFG` so a failing vector can be reproduced. `ROUNDS` accepts 1–32. Larger
scales on a slow 486 may need a larger `WATCHDOG_SECONDS`; a watchdog abort is
a `FAULT`, not a successful shortened benchmark. The watchdog applies to each
invocation, not the whole sustained window: `WATCHDOG_SECONDS` for the first
(warm-up) invocation, then the larger of 5 seconds and 50 times the warm-up
time, capped at `WATCHDOG_SECONDS`. A timeout inside a nested probe
first returns to that probe's caller so the test can clean up; the test is
then given two more seconds before it is abandoned, and either way it is
reported as `FAULT`. `TEST_SECONDS` accepts 1–3600. The host runner does
not need a longer window, since it extends sampling itself; raise it only for
more precise standalone guest timing.
`ONLY` runs just the listed tests and logs them as `CFG only=`; `SUMMARY`
counts only those. A name that is no test fails the build. A clean default
rebuild selects the full suite.

`make QUICK=1` checks the records listed in `QUICK_RECORDS` once, as an
accuracy-only build would, instead of sampling them; it logs them as
`CFG quick=`. They still run, so every feature stays tested and FAIL/FAULT
still shows; only their capacity numbers are missing. The default list holds
the sampled records that cost the most host time while their capacity tracked
another record's in recorded full runs (SMC rate/mixed variants, restart and
paging-restart records, `int_bn_muladd`). Override the list with
`QUICK_RECORDS=a,b`; a name that is no test fails the build.

## Local release preparation

```sh
make test
make release CC=gcc
cd dist/cpuburn-test-0.1.0
sha256sum -c SHA256SUMS
```

`make release` **only writes local artifacts**; it never creates a GitHub
release, pushes Git commits, downloads ROMs or uploads results.
The directory contains `floppy.img`, `BUILD.txt`, a source tarball,
README, release notes, MIT license and checksums for every artifact.
`BUILD.txt` records the release settings and compiler/tool versions.
Use `SHA256SUMS` for the actual image identity rather
than copying a hash from an older README.

The source tarball contains build sources, host tools, documentation and CI;
it excludes Git metadata, build products, VM state, captured logs and ROMs.
Rebuild it with the same tool versions/settings as the original release. With matching
inputs/toolchain, artifacts contain no build timestamp or absolute build
directory. Builds do not inspect Git or generate source/build digest headers.
This is not a claim that different compiler versions produce identical images.

The CI workflow builds with x86-64 GCC/Clang and aarch64 Clang/GNU-cross,
runs the Python tool tests and checks packaged hashes. A separate job runs
`make guest-test`, which boots three focused images under QEMU.
It does not run 86Box regression comparisons or
decide which emulator failures are acceptable. Guest execution still requires
the user's selected emulator, machine and locally supplied ROMs.
See [RELEASE_NOTES.md](../RELEASE_NOTES.md) for preview limitations.
