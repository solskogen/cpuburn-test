# Bare-metal CPU/chipset burn-in for 86Box

Version **0.1.0 preview**. Licensed under [MIT](LICENSE).

## TL;DR

- **Purpose:** check CPU/FPU correctness and collect benchmark data when
  changing 86Box's interpreter or dynamic recompiler. No single overall score.
- **Build:** run `make` (defaults to GCC on x86 Linux and Clang on aarch64 Linux).
  See [build requirements](#build-and-boot); Python is not needed to build
  or package the bootable `floppy.img`.
- **Run:** boot the floppy in your chosen 86Box machine, with an i486 or newer
  CPU. No guest OS or hard disk is needed; the same image works regardless
  of the host architecture. Optional Intel, AMD and Cyrix instructions are
  feature-gated; unsupported features produce `SKIP`.
- **Collect:** capture COM1 at 115200 baud, 8N1. Wait for `SUMMARY` and `DONE`;
  the guest then halts. VGA and port `0xE9` also show results.
- **Read:** `PASS` means the checked cases passed; `FAIL` is an assertion
  mismatch; `FAULT` is an unexpected exception/watchdog; `SKIP` is untested
  coverage. `DONE` alone does not mean success.
- **Timing:** guest cycles/time are not host throughput. Keep image, guest
  configuration and host conditions consistent when comparing builds.
  `python tools/perf_report.py serial.log --output report.json` summarizes
  a complete log; users decide how to assess regressions.
- **Shorter runs:** `make clean && make BURN_SECONDS=1 TEST_SECONDS=1` keeps the selected
  test cases but reduces repetition/stress and timing confidence. Defaults
  are 10 and 5 guest seconds; invocations are never cut short.
- **Correctness only:** `make -B BENCHMARKS=0` removes timed harness repetition,
  not test cases or assertions. See [accuracy-only builds](#accuracy-only-builds).

## Usage

### Choose what to build

```sh
make                         # initial build; benchmarks enabled by default
make -B BENCHMARKS=0          # force rebuild without benchmark sampling
make -B BENCHMARKS=1          # force rebuild with benchmark sampling
```

**Keep `-B` when switching settings.** Make tracks file timestamps, not
changes to command-line variables. Once an image exists, `make BENCHMARKS=0`
or `make BENCHMARKS=1` alone may say "Nothing to be done" and leave the
existing image unchanged. Plain `make` does not restore defaults in an
already-built image either. Use `make -B` to rebuild with defaults, or
`make -B BENCHMARKS=0` for accuracy only. Unlike `make clean`, `-B` preserves
saved run results under `build/`.

The accuracy-only build keeps the same selected workloads, vectors, scale and
assertions. Each eligible workload normally runs a checked warm-up and one
checked invocation per round, rather than repeating for `TEST_SECONDS`.
Intrinsic stress durations still apply, so this is not an instantaneous run.
See [accuracy-only builds](#accuracy-only-builds) for details.

### Choose how to collect results

| Goal | Tool |
|---|---|
| Display and save serial results from your existing VM | [`capture_serial.py`](#boot-and-capture-results) |
| Measure each test's ability to sustain the configured emulation speed | [`run_86box.py`](#measure-per-test-emulation-capacity) |

Serial capture and guest timing alone do **not** measure host emulation
capacity. Use the performance runner below to find workloads that cannot
keep up with the configured CPU speed.

### Measure per-test emulation capacity

On Linux, with Python 3.10+, Xvfb, installed ROMs and an
`AUTOMATION=ON` 86Box build:

```sh
make -B BENCHMARKS=1
python3 tools/run_86box.py --vm "/path/to/My VM"
```

The runner starts your VM **in place**; stop it first and do not start another
instance or serial controller during the run. `--vm` accepts any VM directory
containing `86box.cfg`, or a configuration file such as
`--vm "/another/location/custom.cfg"`. There is no required VM storage location.
CPU family, clock, multiplier, RAM, FPU and the primary video card come from
that configuration. Omit `--dynarec` to preserve its engine selection; use
`--dynarec on|off` to override it for this run only.
The runner uses `86Box` from `PATH` by default; `--emulator /path/to/86Box`
selects another binary.

To compare execution engines, run the same VM twice:

```sh
python3 tools/run_86box.py --vm "/path/to/My VM" --dynarec on
python3 tools/run_86box.py --vm "/path/to/My VM" --dynarec off \
  --output-dir "/path/to/interpreter-results"
```

Other optional switches:

| Option | When to use it |
|---|---|
| `--bios-f1` | Automatically press F1 when a recognized text-mode BIOS prompt asks to resume/continue, such as after a CMOS configuration change. It does not configure the BIOS and cannot detect graphical prompts. |
| `--label interpreter` | Add an arbitrary descriptive label to `result.json` for identifying comparisons. It does **not** select the interpreter; only `--dynarec off` does that. Omit it unless you want a label. |
| `--key 20000:0x3c` | Press F2 at 20,000 emulated milliseconds, only for firmware that explicitly offers F2 to load defaults and continue. General syntax is `EMULATED_MS:SET1_SCANCODE`; this is not automatic BIOS configuration. |

Nothing is copied: the VM uses its own NVRAM, disks, devices and relative paths.
The runner temporarily inserts the test image in drive A: (write-protected),
enables COM1 capture and applies any explicit CPU/RAM/FPU overrides. After
86Box exits, the original configuration is restored byte-for-byte, including
the previous floppy and COM1 settings, also on ordinary errors or Ctrl+C.
NVRAM and disks remain live and can be updated by firmware or the guest;
they are **not** rolled back. Abrupt termination such as SIGKILL or a host
power failure cannot guarantee configuration restoration.
Ensure the BIOS boots drive A:, not your installed OS. Inspect `screen.log`
or `screen.png` if boot stalls.
Use `--rompath /path/to/roms` for firmware outside standard locations.
Missing configuration or required CPU/RAM settings produce an error;
firmware/boot problems remain explicit errors or incomplete runs, not passes.
No firmware is downloaded.

By default, `serial.log`, `result.json` and the other capture diagnostics go
directly into the supplied VM directory (beside the configuration file).
Use `--output-dir /path/to/results` to save them elsewhere. Existing capture
files are not overwritten; choose another output directory for another run.
In `result.json`, **each sampled test** has an entry under
`workload_capture.workloads`, identified by `name` and `id`:

- `full_speed_capacity_percent`: guest time advanced divided by unthrottled
  host step time, multiplied by 100. Sustained values below approximately
  **100%** indicate that test could not maintain the configured guest speed
  on this host.
- `under_sampled`: true when the test did not reach the requested host
  CPU-time minimum; do not treat a short sample as a firm performance result.

This is measured capacity, not the GUI speed reading or an automatic
slowdown verdict. Correctness remains separate: check FAIL/FAULT records,
and compare repeated runs under matching host conditions. Accuracy-only
images do not provide sustained benchmark samples.
See [Sustained host profiling](#sustained-host-profiling) for sampling limits,
optional host profiling; `--perf` is not required.

#### Example: passing and keeping up versus passing but too slow

These are **illustrative excerpts**, not measured results or complete records.
For the same test, both runs might print this in `serial.log`:

```text
TEST r=1 name=int_alu status=PASS ...
```

The assertions passed in both runs. The speed difference appears in
`result.json`, under `workload_capture.workloads`, not in that serial line.
A run with enough capacity to maintain real time might contain:

```json
{
  "name": "int_alu",
  "emulated_ms": 40000,
  "step_wall_seconds": 30.0,
  "process_cpu_seconds": 30.0,
  "full_speed_capacity_percent": 133.33333333333334,
  "under_sampled": false
}
```

It advanced 40 guest seconds in 30 host seconds: **133% capacity**, with
headroom above the configured speed. Together with PASS, this is the desired
result for the checked workload, not proof of perfect emulation.

A slower run could instead contain:

```json
{
  "name": "int_alu",
  "emulated_ms": 22500,
  "step_wall_seconds": 30.0,
  "process_cpu_seconds": 30.0,
  "full_speed_capacity_percent": 75.0,
  "under_sampled": false
}
```

It advanced only 22.5 guest seconds in 30 host seconds: **75% capacity**.
The checked results are still correct, but this host cannot sustain the
configured CPU speed during that workload. A FAIL at 133% would mean the
opposite problem: sufficient speed, incorrect results. Capacity near 100%
needs repeated measurements before drawing conclusions.

### Boot and capture results

Insert the built `floppy.img` into drive A: of an i486-or-newer VM and boot
from the floppy. No guest OS or hard disk is required.

To display and save serial output with the supplied Python 3 script, use
an 86Box build with `AUTOMATION=ON`. With 86Box stopped, enable COM1 capture
in the VM's existing `86box.cfg` section:

```ini
[Ports (COM & LPT)]
serial1_enabled = 1
serial1_device = automation
```

On Linux/macOS, start the VM in one terminal (replace the binary and VM paths):

```sh
86Box --vmpath /path/to/disposable-vm \
  --automation "unix:/tmp/cpuburn-$UID.sock" --automation-hold
```

In another terminal, from this project's directory:

```sh
python3 tools/capture_serial.py "unix:/tmp/cpuburn-$UID.sock"
python3 tools/perf_report.py serial.log --output report.json
```

The script displays results and saves `serial.log` until `DONE`. Inspect
`SUMMARY`: successful capture does **not** mean all tests passed. The VM
stays open and held afterward, including if capture fails; partial logs are
retained. Use `--output another-run.log` for subsequent runs; existing files
are not overwritten. Do not run another controller against the same VM
while capturing. See `python3 tools/capture_serial.py --help` for options.

For Windows or local TCP capture, start 86Box with
`--automation tcp:7086 --automation-hold` instead, then use:

```sh
python3 tools/capture_serial.py tcp:127.0.0.1:7086 \
  --token-file /path/to/disposable-vm/automation.token --output serial-tcp.log
```

Keep the socket local; do not expose it to the internet. If no results
appear, check the VGA screen for a BIOS prompt and ensure firmware enables
COM1. For automatic handling of supported BIOS F1 prompts, use
[`tools/run_86box.py --bios-f1`](#sustained-host-profiling).

## Overview

The output is a raw 1.44 MiB bootable floppy image, without a filesystem.
Port `0x80` carries progress checkpoints in addition to the result outputs
listed above.

Pentium MMX 233 MHz on Raspberry Pi 5 is one useful performance target, not a
restriction or an achieved performance claim. Pentium II and real-hardware
testing remain supported. The instruction baseline is i486, including
486SX configurations without an FPU. Optional coverage follows CPUs and
instruction sets implemented by 86Box, not an Intel-only target: CPUID and
exception probes gate MMX, P6 instructions, FXSAVE and AMD 3DNow!, while
Cyrix EMMI uses a restricted vendor/model gate. No SSE/SSE2, later SIMD,
64-bit execution or CPUs outside 86Box's supported range are targeted.

## Coverage overview

The i486-baseline image checks integer execution, x87, MMX and optional vendor
instructions, protected-mode transitions, paging/restart, system state and
selected chipset behavior. Tests are deterministic or explicitly bounded, not
exhaustive ISA or motherboard certification. Eligibility depends on detected
features and runtime resources; a SKIP is not a pass. Both benchmark and
accuracy builds run checked workloads, not isolated instruction latency.

See the [workload catalogue](doc/workloads.md) for every registered record,
requirements, work units and limitations, and [CPU contracts](doc/cpu-contracts.md)
for model expectations and feature-evidence distinctions.

## CPU contracts and accuracy runs

The 86Box runner supplies its requested CPU family and FPU configuration to
the guest automatically. The guest compares this independent expectation
against its detected identity and features in `cpu_model_contract`, even when
`ONLY` selects other tests. A required feature hidden by CPUID does **not**
disable its registered tests: those tests execute under the existing exception
guard. Missing advertisements fail the contract check; unsupported execution
is recorded as FAULT rather than SKIP. Existing failures remain failures.
Non-CPU requirements such as sufficient RAM and PCI availability still gate
tests normally. A configured FPU cannot remove an integrated FPU requirement.

```sh
python3 tools/run_86box.py --vm "/path/to/My VM" --accuracy
```

`--accuracy` runs one warmup and one checked invocation per selected test,
without benchmark sampling extensions. Every invocation keeps the same cases,
seed and assertions as benchmark mode. Duration-based stress workloads retain
their own guest-time durations. Accuracy runs require the new guest's control
acknowledgement and DONE; early `--expect` markers cannot end them. Guest
FAIL/FAULT results give the runner a nonzero exit. Host timeouts remain
incomplete runs, not architectural failures. Fast-forward and host emulation
capacity do not determine architectural verdicts. The performance report
retains these correctness outcomes but does not treat them as benchmark samples.
With a benchmark-enabled image, sustained benchmarking remains the default
unless `--accuracy` is selected.

### Accuracy-only builds

`BENCHMARKS` defaults to `1` and accepts only `0` or `1`. With `0`, the timed
sampling/repetition loop is compiled out, the image always uses accuracy mode,
and `TEST_SECONDS` and host sampling leases cannot extend a test.
No `--accuracy` argument or host controller is needed. The 86Box runner
recognizes this build automatically.
Correctness cases, including assertions in burn-named workloads, remain intact;
their intrinsic stress durations are not removed. Warmup and one checked
invocation still run unless a fault or skip prevents completion, but their
times are diagnostic data, not benchmark samples. `ROUNDS`, `ONLY`,
`HEAVY_SCALE`, `BURN_SECONDS` and watchdog behavior still apply.
BUILD/CFG records and release BUILD.txt identify the setting. Use `-B` when
changing it, just as for the other Makefile build settings.

### Model coverage and matrices

`cpu_models.h` contains minimum contracts for all 67 currently active 486+
families, plus the two build-conditional K5 families. The catalogue includes
486-class 386-bus upgrades and later Intel, AMD, Cyrix, IDT and VIA models.
Requirements are independently sourced architectural minima, not generated
from emulator implementations or accepted because master passes.
[CPU contracts and coverage](doc/cpu-contracts.md) documents sources,
configuration exceptions, protocol details and remaining coverage gaps.

Standalone boot waits at most two guest seconds for optional host input, then
uses detection. Ambiguous identities use intersected minimum requirements;
unknown models use guarded feature discovery and retain unverified identity.
`CONFORMANCE` records
explicitly say `coverage=partial` and `catalogue=known/unknown`; unknown model
contracts are SKIP, never verified conformance. UART control records and
conformance metadata are preserved in JSON and plain serial logs. Runs with
different contracts cannot be pooled into one performance report.

`FEATURE` records distinguish sampled PASS, FAIL, UNTESTED and NA. Missing
witnesses, filtered tests, skipped coverage and unmapped CPUID claims are not
passes. `FILTER` records retain tests omitted by `ONLY`. Strict system-state
checks cover TSC MSRs, MTRR/PAT/MCA state, MCE control, DE exceptions and PSE36
address-bit/fault handling. AMD legacy SYSCALL/SYSRET and MMX integer
extensions have separate behavioural witnesses and applicability gates.
These are behavioural samples, not exhaustive
proof of every architectural feature or interaction.

Generate a source-bound compatible-machine/FPU/engine matrix, or execute a
selection with actual CPU identity checks:

```sh
python3 tools/cpu_matrix.py --source ../86Box --output build/cpu-matrix
python3 tools/cpu_matrix.py --source ../86Box --all-speeds --output build/cpu-matrix-all
python3 tools/cpu_matrix.py --source ../86Box --output build/cpu-matrix-run \
  --run --family pentium_p55c --family k6_2p --engine on
```

Generation does not require ROMs or start VMs. Execution uses local ROMs,
isolated inputs, accuracy mode and sequential runs; failures, missing firmware,
blocked entries and incomplete runs remain explicit. Some architectural claims
still need behavioural witnesses; see the coverage document rather than
treating a clean selected run as complete CPU certification.

The optional contract integration tests use QEMU TCG to inject a hidden MMX
feature bit and an incorrect expected model. These check the test harness,
not whether QEMU is an accuracy oracle for 86Box:

```sh
make -B ONLY=int_partial_boundaries,mmx_alu,3dnow_float
CPUBURN_CONTRACT_IMAGE="$PWD/floppy.img" python3 -m unittest discover -s tools -p test_cpu_contract.py
make -B BENCHMARKS=0 ONLY=pm_syscall,mmx_extended
CPUBURN_EXTENSION_IMAGE="$PWD/floppy.img" python3 -m unittest discover -s tools -p test_cpu_contract.py
make -B # restore the complete default image after focused validation
```

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
make clean && make CC=clang
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

Force a rebuild with `make -B` when changing compilers, flags or test settings
(or clean first, noting that `make clean` also removes `build/` results and
`dist/`). Make tracks file dependencies, not command-line changes.
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
`python3 -m unittest discover -s tools -p 'test_boot_memory.py' -v`.
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
checked invocations in a window lasting **at least `TEST_SECONDS` (default 5)
guest seconds**.
Assertions remain active throughout. Warm-up failures remain FAIL even when
later invocations pass, preserving cold dynarec/cache bugs. Assertion failures
do not terminate the sustained window; unexpected faults or unavailable safe
preconditions do. Feature-gated skips do not wait for a sample window.
For execution without timed harness repetition, use
[`BENCHMARKS=0`](#accuracy-only-builds) or the runner's `--accuracy` option.

The default is one suite round and the final mixed burn, whose individual
invocations last 10 seconds. Heavy workloads use `HEAVY_SCALE=8`; each invocation
does real fixed-count work, rather than sleeping to fill its window.
The default inventory is 306 records, including unavailable-feature skips.
Suite duration varies with CPU, applicable features and invocation lengths;
it is not a fixed wall-clock deadline. In benchmark mode, the
[host profiling runner](#sustained-host-profiling) can extend sampling to
obtain enough actual emulator CPU time. Standalone runs need no controller
and end automatically.
Clean-rebuild when changing settings, for example:

```sh
make clean && make ROUNDS=1 BURN_SECONDS=5 WATCHDOG_SECONDS=90
make clean && make ROUNDS=4 HEAVY_SCALE=8 BURN_SECONDS=20
# Shorter validation build; identical algorithm and checks, less sampling time:
make clean && make ROUNDS=1 BURN_SECONDS=1 TEST_SECONDS=1
# Only the named tests (comma-separated, exact names); ids match a full run:
make clean && make ONLY=corpus_integer_sequences,int_smc
```

`HEAVY_SCALE` accepts 1–32 and scales the new stress workloads, not every legacy
test. The deterministic seed is defined in `kernel.h`, not exposed as a
normal Makefile setting; it is logged with the scale in
`CFG` so a failing vector can be reproduced. `ROUNDS` accepts 1–32. Larger
scales on a slow 486 may need a larger `WATCHDOG_SECONDS`; a watchdog abort is
a `FAULT`, not a successful shortened benchmark. The watchdog applies to each
invocation, not the whole sustained window. `TEST_SECONDS` accepts 1–3600;
use the default 5 or more for profiling, not the shortened validation build.
`ONLY` runs just the listed tests and logs them as `CFG only=`; `SUMMARY`
counts only those. A clean default rebuild selects the full suite.

## Results for automation

Example record (numbers are illustrative):

```text
BEGIN r=1 name=workload phase=first
WORK phase=warmup r=1 id=1 name=workload
WORK phase=sample r=1 id=1 name=workload
WORK phase=end r=1 id=1 name=workload
NOTE test=workload detail=value
TEST r=1 name=workload status=PASS ms=30000 cyc=123456789 unit=tsc ops=100000000 sum=12345678 iterations=1000 warmup_ms=30 warmup_failed=0 failed_iterations=0 id=1 iteration_ops=100000
RUNTIME scope=test r=1 id=1 name=workload total_ms=30120 warmup_ms=30 sample_ms=30000 overhead_ms=90 resolution_ms=10
SUMMARY rounds=2 tests=100 pass=80 fail=0 skip=20 fault=0 ms=2500000
RUNTIME scope=suite total_ms=2500000 warmup_ms=3000 sample_ms=2400000 overhead_ms=96000 suite_overhead_ms=1000 resolution_ms=10
DONE
```

`RUNTIME` records show where **guest time** goes, measured with the guest's
100 Hz timer on any host. Per test, `total_ms` covers the whole test,
including skip handling, notes and result output. `warmup_ms` and `sample_ms`
time only the test invocations; `overhead_ms` is the remainder, including state
restoration, arena reset, dispatch and serial output. Individual invocation
times have 10 ms quantization error, which can accumulate across repetitions;
for tiny tests, the remainder is not a precise measurement of framework cost.
The suite record sums these, and `suite_overhead_ms` covers work between tests
and the runtime records themselves. Use total durations to find which tests
dominate a full run before trimming anything. Guest time is not host cost;
host CPU per workload comes from the 86Box runner. `perf_report.py` validates
and prints the breakdown, including skips and failed tests. These records
do not change correctness checks, warm-up, sampling duration or test scale.

The [Usage commands](#boot-and-capture-results) capture and summarize a complete
COM1 log without QEMU or a host profiler. The report lists the ten longest
test records and retains every runtime record in JSON. Older logs remain
supported, with the breakdown marked
unavailable. The guest totals exclude firmware boot and guest initialization.

`PASS` means the workload's assertions passed, **not** exhaustive ISA compliance.
`SKIP` gives the reason a feature or safe test precondition is unavailable.
`FAIL` is an assertion mismatch (`FAILDETAIL` provides context); `FAULT` is an
unexpected CPU exception or a watchdog recovery, with vector, error code, EIP,
and CR2. Missing `DONE`, `FATAL`, or a nonzero fault/fail count is not success.
On the supported i486+ baseline, missing required alignment-check and hardware
breakpoint traps are now **FAIL**, with `required_trap_missing=AC/DB`
diagnostics, rather than SKIP. Optional instruction/feature absence remains
SKIP through the existing requirement gates. More FAIL records after splitting
the FP vectors or tightening trap accounting do not by themselves imply a
new emulator regression: the coverage and record inventory changed.

`cyc` is a guest TSC delta where available, otherwise a PIT count delta
(1,193,182 Hz). `ms` comes from the 100 Hz guest timer and has 10 ms granularity.
TSC timestamps are bracketed by CPUID serialization; this adds fixed overhead
and may trap to the host hypervisor. A counter regression invalidates timing,
not the workload's correctness result.
TSC rate need not equal the configured/emulated clock; on VMs it may be a
host-derived invariant counter. A logged MHz value is a PIT calibration, not a
claim about emulation throughput. `ops` is a workload-specific operation count,
not a universally comparable instruction count. Checksums may intentionally
include CPU-specific results and must not replace the individual assertions.
`op_unit` names the counted work where specified; older workloads retain the
generic `operation` label. `timing_scope=checked_workload` explicitly includes
operand/reference preparation, assertions, and any in-function enable/restore
sequence. These are not isolated opcode latency measurements. Reports preserve
these fields and refuse to pool or compare incompatible work units/scopes.
`ms`, `cyc`, and 64-bit `ops` aggregate measured invocations, excluding warm-up.
`iterations` counts measured invocations; `iteration_ops` is their constant
operation count, or zero if it varied. `failed_iterations` includes warm-up.
On failure, the final `TEST` record also retains the first failing invocation:
`failure_iteration=0` is warm-up, `1` is the first measured invocation,
and `failure_phase` names the phase. `seed` repeats the configured seed.
For `CHECK_EQ`, `got` and `want` retain the first mismatching 32-bit values
even if the failure first occurs during quiet sampling. Plain `CHECK` has
no automatic operand values; its expression and line are retained.
These are not an automatic instruction/register trace: detailed case indices,
input operands and initial state are supplied by workload-specific diagnostics
where implemented. The seed does not control every legacy test's fixed vectors.
Full logs remain the authoritative evidence; the shared parser preserves
warm-up `FAILDETAIL` lines as `failure_details` in each parsed test record.
Repeated checks and failure counts are unchanged.

`ops_constant=1` means measured invocations had one identical nonzero operation
count. It is not a claim that different workloads count equivalent operations.
`ms_resolution=10`, `ms_unresolved=1`, and `counter_unresolved=1` explicitly
describe coarse or zero timing. Absence of a resolved duration is not zero
cost. Reports retain per-record `measurement_notes` for unavailable/invalid
timing, missing or inconsistent operation counts, and non-PASS results;
these notes describe data quality, not master-versus-branch policy.
Workload IDs are unique across rounds. `WORK` boundaries distinguish warm-up
from sampling and exclude buffered diagnostics from host attribution.

Compare runs with identical image, machine/chipset configuration, CPU selection,
RAM, emulator version, dynamic-recompiler mode, and host conditions. Keep both
**host elapsed time** and guest timing: 86Box may maintain a configured guest
clock while using different amounts of host CPU. Neither timing source alone
establishes a speedup. Benchmark intervals include checking/dispatch overhead;
`NOTE` output is buffered until the interval ends. Repeated diagnostics are
suppressed during sampling, not the checks themselves. State restoration,
arena reset and dispatch occur between invocations; the
configured minimum and `sample_window_ms` include them, whereas guest invocation
counters do not. This prevents tiny or fail-fast invocations from stretching a
window indefinitely just because the guest timer cannot resolve each call.

For a QEMU smoke matrix and JSON records:

```sh
python tools/qemu_smoke.py
python tools/qemu_smoke.py --cpus 486 pentium2 --memory 8 --output build/low-memory
python tools/qemu_smoke.py --cpus host --accel kvm --output build/kvm
```

The runner boots the actual image read-only, requires nonempty test results,
`SUMMARY`, and `DONE`, rejects FAIL/FAULT/FATAL, terminates the halted VM, and
retains serial logs, QEMU diagnostics, host duration, and parsed records.
`build/kernel.elf` and `build/kernel.map` retain symbols for fault investigation.

For distributions and automated regression comparisons, build with four or
more rounds, then discard the first round from timing:

```sh
python tools/qemu_smoke.py --cpus host --accel kvm --output build/perf-a
python tools/perf_report.py build/perf-a/results.json --discard-rounds 1 \
  --output build/baseline.json
python tools/qemu_smoke.py --cpus host --accel kvm --output build/perf-b
python tools/perf_report.py build/perf-b/results.json --discard-rounds 1 \
  --baseline build/baseline.json --output build/comparison.json
```

The report contains min/median/p95/max guest counter and millisecond costs per
operation, correctness failures, source/image hashes, configuration, and host
boot-to-DONE durations. `--cpu` selects one CPU from a matrix JSON. Complete
serial logs are also accepted, but do not establish an image hash. The first
round is labeled `phase=first` and later rounds `phase=repeat`: these are not
claims of cold caches or a clean dynarec cache. Compare the explicit
code-generation/invalidation workloads to distinguish those paths.

Baseline gating requires matching workload inventory, configuration (including
seed/scale/duration), timer units, and fixed operation counts per invocation.
The sustained protocol permits different numbers of identical invocations and
normalizes by their total operations; variable-count workloads cannot establish
this comparison. Legacy logs still require equal total operation counts. Image and environment
changes require explicit `--allow-image-change` / `--allow-environment-change`;
the latter is appropriate for an intentional interpreter-versus-dynarec
comparison. Different emulator builds/labels may be compared, but cannot be
pooled into a single distribution. Default regression threshold is 10% with
at least three timing samples on each side. Exit codes are 0 for success,
1 for correctness failures or measured regressions, and 2 for invalid input
or an inconclusive baseline gate. Discarding warm-up timing never hides a
correctness failure. SKIP/FAULT/FAIL, zero-operation, and invalid-counter
records cannot establish a passing performance comparison.
Host median comparisons are also reported; `--gate-host-time` additionally
enforces the threshold on host time and requires the minimum number of
independent boots on both sides (multiple rounds are not independent boots).
Host metadata, CPU-time targets and perf enablement must match when pooling
86Box captures. With host-controlled sampling, faster emulation can simply do
more invocations within the same CPU budget: prefer per-phase capacity and
counter/operation data, not boot-to-DONE time, as the optimization signal.

Host durations include boot and logging; 86Box durations also include
automation stepping/capture overhead. Keep the step size identical between
runs and collect several independent boots for host-time distributions.

See [per-test emulation capacity](#measure-per-test-emulation-capacity) for
VM-based runner commands and prerequisites. Captures also include screen
state, emulator information and host elapsed time. The runner requires this
86Box checkout's automation interface, not necessarily an unmodified upstream
binary. `--expect DONE` checks completion, **not zero assertion failures**:
inspect `serial.log` and `SUMMARY`.

### Sustained host profiling

The runner uses your VM's CPU settings, 250 guest-ms automation steps,
and **30 actual process CPU seconds per sampled workload**, with 24 guest-hour
and eight host-hour overall deadlines. `--sample-cpu-seconds` changes that
minimum; `--step-ms` controls boundary uncertainty. Linux `/proc/PID/stat`
accounts for **all emulator threads**, not just the CPU core thread. Runs
require the local automation-enabled 86Box build; standalone floppy boot does
not require this interface.

The runner renews a 30-guest-second serial lease while more host CPU time is
needed, then releases it. A release never shortens the guest minimum. If the
controller disappears, the last lease expires, so the guest does not wait
forever. The timer interrupt drains serial control frames to avoid overrunning
the UART during long invocations; releases are retransmitted until the phase
ends. Interrupt-disabled intervals can defer receipt, and complete invocations
can overshoot the requested minimum. Frames are `+IIIIIIII\n` / `-IIIIIIII\n`, using eight
hexadecimal digits for the current workload ID. Malformed and stale frames
cannot extend a different workload.

`result.json` includes `workload_capture`: separate warm-up/sample phases,
interior-step host wall time, process CPU time, guest milliseconds, aggregated
workloads, and explicit undersampling warnings. Steps containing a phase
boundary or partial serial line are excluded, not assigned speculatively.
`full_speed_capacity_percent` is guest time / unthrottled step wall time × 100:
below 100 identifies a path unable to keep up on that host under these
conditions. It is **capacity, not the GUI's actual-speed reading**, and is not
a Pi 5 measurement unless run there. The CPU-normalized ratio additionally
charges every emulator thread; it is not a single-thread cycle estimate.
Boot, serial capture and non-step control time are recorded separately.

`--perf` explicitly attaches Linux `perf record` at 99 Hz with callchains and
`CLOCK_MONOTONIC` timestamps. It saves `perf.data`, `perf.script`, diagnostics,
and per-ID leaf/inclusive hotpaths aligned only to interior sampling steps.
Unresolved symbols retain their instruction addresses instead of collapsing
every JIT location into one `[unknown]` bucket. Useful callchains still depend
on emulator symbols and host unwinding support; raw samples remain in
`perf.data`.
Missing tools, event permissions and recording failures are explicit errors;
there is no pretend-success fallback. Without `--perf`, the same guest run and
host CPU-time attribution still operate. Host architecture/kernel, CPU,
emulator hash and image hash accompany results.

Use passing, sufficiently sampled workloads to evaluate performance; inspect
FAIL records to fix correctness first. A fail-fast vector may repeatedly
exercise only its failing path, not the complete intended instruction mix.
Unexpected FAULT/early SKIP and missing COM1 output cannot establish a useful
profile. An AL440LX/Pentium II VM may need BIOS serial setup before
per-workload host attribution is available, although the floppy itself can
still complete with VGA output. No Pi 5 or physical Pentium II result is
claimed from local desktop-emulator validation.

## Safety and limits

Only the boot loader uses BIOS disk reads. The protected-mode kernel does not
access disk controllers or issue storage writes. Most memory patterns use
firmware-reported usable extended RAM owned by the test. The
`int_smc_ram_top` workload also writes a probe just above reported RAM and
may execute there if it reads back; see the [catalogue](doc/workloads.md).
PCI configuration and ROM inspection are read-only. The suite does not
reprogram MTRRs, initiate
DMA transfers, write-probe option ROMs, disable A20 during workloads, invoke SMM,
or run destructive board-specific chipset configuration tests. Device absence
is reported rather than treated as a benchmark failure.

Exception guards, a separate double-fault stack, and a timer watchdog contain
ordinary probing faults. **They cannot guarantee recovery from arbitrary
hardware/emulator failures**, a broken interrupt controller, a hard lockup with
interrupts disabled, or corruption of the kernel's own mappings. Double faults
produce a fatal diagnostic and halt rather than continuing in an unknown state.
Real-hardware compatibility cannot be established solely by VM results.

Legacy chipset tests necessarily exercise PIC/PIT/RTC state and owned CPU
control state. Run on a dedicated test machine or VM, not as a resident program
alongside an OS. Cache, TLB, instruction, and floating-point checks are not
electrical stress certification, exhaustive memory testing, or universal x86
conformance validation.

## Interpreting emulator differences

QEMU TCG and 86Box are not correctness oracles for each other. For example,
some QEMU TCG CPU models return zero from a successful TSC MSR read while
RDTSC advances; `sys_msr_probe` intentionally reports that mismatch. TCG's
missing x87 stack-fault/MMX-tag metadata checks have explicit, narrowly scoped
SKIP records that retain the raw state; transcendental checks allow a documented
TCG precision tolerance. Ordinary arithmetic and memory assertions are not
silently relaxed.

Local K6-2+ validation of the vendor workloads exposed PI2FW using the wrong
word for the upper lane and PF2IW failing to produce two sign-extended dword
results in both 86Box execution engines. AMD 22466D specifies the checked
lane layout. These remain `FAIL` in `3dnow_word_convert`; the tests are not
adjusted to agree with the emulator.

The heavy expand-down test uses byte-granularity descriptors: valid offsets
must be strictly greater than the limit and the entire operand must fit below
the B-bit-selected upper bound. It expects #GP(0) through FS and #SS(0)
through SS. TCG and the tested 86Box engines omit some of those faults;
the assertions remain strict (KVM passes). The paged SMC alias workload also
exposes a dynarec invalidation mismatch in 86Box, and split POP reports a
fault-time EAX preservation mismatch in both tested 86Box engines. These
are FAIL records rather than unexpected exceptions or reasons to mask checks.
Some workloads fail fast on their first bad vector and consequently report
zero completed operations; that is not a valid performance sample.
The added FDIV raw extended-precision checks can expose loss of mantissa bits
in a host-double FPU implementation, and extreme-range FPREM cases distinguish
80-bit exponent handling from host-double overflow. Nonfinite/denormal status
checks remain strict. Remainder correctness and quotient status are separate
checks; do not treat emulator agreement as an arithmetic oracle.

86Box configurations using a host-double x87 implementation can differ from
80-bit x87 in precision, exponent range, denormal classification and exception
delivery. Those are retained as assertion failures, not treated as a reason
to crash the suite or remove its checks. Compare with a hardware-backed KVM
run and the instruction's architectural semantics before changing either test
or emulator.

## Validation scope

Validation results apply to the particular image, CPU, execution engine and
FPU backend tested, not automatically to later builds or packaged artifacts.
The earlier 281-record vendor-integrated image completed a full default
86Box Pentium MMX/dynarec run: 187 PASS, 72 FAIL, 22 SKIP, no FAULT.
All earlier outcomes stayed unchanged by the vendor and boundary additions.
The strengthened boundary assertions passed focused interpreter and dynarec
runs; 486DX correctly skipped the MMX-specific record.
See [release notes](RELEASE_NOTES.md#known-results-and-limitations) for
additional image-specific results.

Focused vendor checks exercised both 86Box engines on K6-2, K6-2+ and
6x86MX, plus negative feature gates on Pentium MMX, 486DX and 486SX.
K6-2 baseline 3DNow! and all selected Cyrix EMMI cases passed. The K6-2+
exception is described under [emulator differences](#interpreting-emulator-differences).
These focused runs used default guest durations
and a short host sampling minimum for correctness, not benchmark precision.

Earlier images were also exercised on QEMU TCG/KVM and other 86Box CPU
profiles. Those older inventories and results are not validation of the
current release image. No fresh full current-image KVM result is claimed.
Physical PCs, Raspberry Pi hosts, VMware, VirtualBox and Hyper-V are
not certified by these local x86-64 Linux runs.

## Local release preparation

```sh
make test
make clean && make release CC=gcc
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

The CI workflow includes x86-64 GCC/Clang and aarch64 Clang/GNU-cross builds,
runs the Python tool tests and checks packaged hashes. These aarch64 jobs
are configured for future runs, not evidence of already executed CI.
It does not run 86Box regression comparisons or
decide which emulator failures are acceptable. Guest execution still requires
the user's selected emulator, machine and locally supplied ROMs.
See [RELEASE_NOTES.md](RELEASE_NOTES.md) for preview limitations.
