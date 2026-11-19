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

The default `-O2` preserves the established guest instruction mix and keeps the
floppy payload compact. The runtime now executes at **1 MiB** in protected mode;
its zero-initialized state no longer competes with low-memory V86 fixtures.
Diagnostic notes use the upper 32 KiB of
the arena's already-reserved header, leaving workload allocations unchanged.
The ELF has separate code/read-only and writable load segments; these describe
the image layout, not guest page protections for self-modifying-code tests.

### Floppy boot and memory layout

The 512-byte BIOS boot sector loads a 4 KiB bootstrap and the runtime file into
low-memory staging. The bootstrap gathers the BIOS memory map, enables A20,
enters protected mode and verifies that the entire runtime range, including
BSS, is usable extended RAM before copying the file to `0x100000`.
Enabled E820 reservations override overlapping usable entries; adjacent usable
entries may cover the range together. Without E820, the contiguous RAM size
reported by E801 or INT 15h/AH=88 is checked. Missing RAM produces a visible
halt, not an unchecked copy. Startup clears BSS, and the allocator places its
reserved header and workload buffers after the linked runtime's end.
The `MEM` record includes `kernel_start` and `kernel_end`.

The main stack remains below `0x90000`, and V86 code/stack fixtures retain
their low-memory addresses. The runtime and its exception tables/stacks stay
inside the first 16 MiB identity-mapped window. This is still a normal
1,474,560-byte raw floppy image, not an ISO.

This first-stage relocation deliberately retains the existing bounded BIOS
loader: bootstrap plus **file payload** must fit in 384 KiB of staging below
`0x70000`. BSS does not occupy that file budget. Linker and image-size checks
enforce both limits; relocation removes the former combined code/BSS
low-memory limit, not every future size limit. Growing beyond the staging
budget would require chunked loading through a reusable low-memory buffer.
It does not require changing media format.

To exercise the actual bootstrap RAM validator and linker budgets, run
`PYTHONPATH=tools python3 -m unittest discover -s tools/tests -p 'test_boot_memory.py' -v`.
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
invocation, not the whole sustained window. A timeout inside a nested probe
first returns to that probe's caller so the test can clean up; the test is
then given two more seconds before it is abandoned, and either way it is
reported as `FAULT`. `TEST_SECONDS` accepts 1–3600. The host runner does
not need a longer window, since it extends sampling itself; raise it only for
more precise standalone guest timing.
`ONLY` runs just the listed tests and logs them as `CFG only=`; `SUMMARY`
counts only those. A name that is no test fails the build. A clean default
rebuild selects the full suite.

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
