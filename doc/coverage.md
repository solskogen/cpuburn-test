# Coverage, CPU contracts and safety

## Overview

The output is a raw 1.44 MiB bootable floppy image, without a filesystem.
Results go to COM1, VGA and port `0xE9`; port `0x80` carries progress
checkpoints.

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

See the [workload catalogue](workloads.md) for every registered record,
requirements, work units and limitations, and [CPU contracts](cpu-contracts.md)
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
unless `--accuracy` is selected. Benchmark runs that reach DONE keep status
`observed` and all their timing data, but also exit nonzero when the guest
reported FAIL/FAULT.

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
BUILD/CFG records and release BUILD.txt identify the setting.

### Model coverage and matrices

`src/cpu_models.h` contains minimum contracts for all 67 currently active 486+
families, plus the two build-conditional K5 families. The catalogue includes
486-class 386-bus upgrades and later Intel, AMD, Cyrix, IDT and VIA models.
Requirements are independently sourced architectural minima, not generated
from emulator implementations or accepted because master passes.
[CPU contracts and coverage](cpu-contracts.md) documents sources,
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
make guest-test
```

`make guest-test` builds the focused contract and extension images and a
shortened full accuracy image under `build/guest/`, leaving `floppy.img` and
`build/` untouched, then runs every host test with them, including the
otherwise skipped QEMU guest tests. It needs `qemu-system-i386`.

## Safety and limits

Only the boot loader uses BIOS disk reads. The protected-mode kernel does not
access disk controllers or issue storage writes. Most memory patterns use
firmware-reported usable extended RAM owned by the test. The
`int_smc_ram_top` workload also writes a probe just above reported RAM and
may execute there if it reads back; see the [catalogue](workloads.md).
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
