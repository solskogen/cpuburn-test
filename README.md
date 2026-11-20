# cpuburn-test: CPU conformance and benchmark suite for 86Box

Version **0.1.0 preview**. Licensed under [MIT](LICENSE).

cpuburn-test is a bootable floppy image for people changing 86Box's CPU
interpreter or dynamic recompiler. It checks that the emulated CPU and FPU
behave like the real ones (correctness), and measures how fast each
workload is emulated (benchmarks). It needs no guest OS or hard disk.

## TL;DR

- **Purpose:** check CPU/FPU correctness and collect benchmark data when
  changing 86Box's interpreter or dynamic recompiler. No single overall score.
- **Build:** run `make` (defaults to GCC on x86 Linux and Clang on aarch64 Linux).
  See [build requirements](doc/building.md#build-and-boot); Python is not needed to build
  or package the bootable `floppy.img`.
- **Run:** boot the floppy in your chosen 86Box machine, with an i486 or newer
  CPU. No guest OS or hard disk is needed; the same image works regardless
  of the host architecture. Optional Intel, AMD and Cyrix instructions are
  feature-gated; unsupported features produce `SKIP`.
- **Collect:** capture COM1 at 115200 baud, 8N1. Wait for `SUMMARY` and `DONE`;
  the guest then halts. VGA and port `0xE9` also show results.
- **Read:** `PASS` means the checked cases passed; `FAIL` is an assertion
  mismatch; `FAULT` is an unexpected exception/watchdog; `SKIP` is untested
  coverage. `DONE` alone does not mean success. Some `FAIL`s are expected
  on 86Box's default FPU backend; see [known results](#known-results).
- **Timing:** guest cycles/time are not host throughput. Keep image, guest
  configuration and host conditions consistent when comparing builds.
  `python tools/perf_report.py serial.log --output report.json` summarizes
  a complete log; users decide how to assess regressions.
- **Shorter runs:** `make BURN_SECONDS=1` keeps the selected test cases but
  shortens the mixed burn. Defaults are `BURN_SECONDS=10` and
  `TEST_SECONDS=1` guest seconds; invocations are never cut short.
- **Benchmarks only:** `make SUITE=benchmark` builds just the sampled
  workloads, leaving out check-only correctness records.
- **Correctness only:** `make BENCHMARKS=0` removes timed harness repetition,
  not test cases or assertions; each record still runs twice (a warm-up and
  one checked invocation). A host can also pick records at boot without
  rebuilding ([run-time selection](doc/running.md#select-records-at-run-time)).
  See [accuracy-only builds](doc/coverage.md#accuracy-only-builds).

## Usage

### Choose what to build

```sh
make                         # default build: every record, benchmarks enabled
make SUITE=benchmark         # only the benchmark workloads, sampled
make SUITE=accuracy          # every record checked once (same as BENCHMARKS=0)
```

`SUITE=benchmark` leaves out the check-only records (the `cs_*` platform
probes and records that run at least twice real time on every emulated CPU
tested; see the
[workload catalogue](doc/workloads.md#inventory-and-reading-the-tables)) and
counts them in `FILTER suite_selection=N`. `CFG` records the suite.
`BENCHMARKS=0` still works; combining it with a conflicting `SUITE` is an error.

Settings are not sticky: each `make` builds the image for the variables on
that command line, and plain `make` goes back to the defaults. The Makefile
records the compiler, flags and settings in `build/config.stamp` and rebuilds
whenever they change, so `-B` and `make clean` are not needed when switching.

The accuracy-only build keeps the same selected workloads, vectors, scale and
assertions. Each eligible workload normally runs a checked warm-up and one
checked invocation per round, rather than repeating for `TEST_SECONDS`.
Intrinsic stress durations still apply, so this is not an instantaneous run.
See [accuracy-only builds](doc/coverage.md#accuracy-only-builds) for details.

### Choose how to collect results

| Goal | Tool |
|---|---|
| Display and save serial results from your existing VM | [`capture_serial.py`](doc/running.md#boot-and-capture-results) |
| Measure each test's ability to sustain the configured emulation speed | [`run_86box.py`](doc/running.md#measure-per-test-emulation-capacity) |
| Drive a VM through an OS install or boot, logging its real-time speed and watching its screen | [`session_86box.py`](doc/running.md#drive-a-vm-and-log-its-real-time-speed) |

Serial capture and guest timing alone do **not** measure host emulation
capacity. Use the performance runner in [running and measuring](doc/running.md)
to find workloads that cannot keep up with the configured CPU speed.

## Known results

Out of the box, a correct 86Box build does **not** give zero `FAIL`s.
Reference run: Pentium II Deschutes 450 on the ASUS P3V4X, dynarec,
`make SUITE=accuracy`, booted with `tools/run_86box.py`:

| FPU backend | PASS | FAIL | SKIP |
|---|---:|---:|---:|
| host (86Box default) | 238 | 75 | 31 |
| softfloat | 308 | 5 | 31 |

86Box's default x87 backend computes with host doubles, so it does not
round, raise flags or handle the 80-bit range like a real x87. About 70
x87 tests fail for that reason alone. To check x87 conformance, run with
`tools/run_86box.py --fpu-backend softfloat` (the "Softfloat FPU" machine
setting); some machines always use softfloat. (These counts predate the
split of `fpu_burn` into separate FPU workloads and the MMX/3DNow! kernel
records, so current totals are slightly higher.)

The guest identifies the backend itself: before the tests it prints
`FPUPROBE backend=host|softfloat|native|none`, from 1/3 at 64-bit precision
(a host double loses the low 11 significand bits) and FSIN of a large
argument (a real x87 and softfloat reduce with a 66-bit pi, host libm
reduces exactly). Bit-exact emulation cannot be told from silicon, so
`softfloat` also covers real hardware and QEMU. Checks that a 53-bit
significand cannot pass stay strict but start their failure text with
`backend=host` on that backend; see [workloads](doc/workloads.md) for the
probe and the labelled checks. `run_86box.py` also stores the configured
backend as `fpu_backend` in `result.json` and prints a note when x87 tests
failed on the host backend; `perf_report.py` repeats that note.

With softfloat, current 86Box releases leave five failures that are not
FPU-related:

- `cpu_model_contract` (CPUID DE bit) and `pm_debug_regs` fail because
  86Box turns off debug registers on 486 and later CPUs by default. Both
  pass with the "Debug registers on 486 and later processors" machine
  setting (`cpu_debug_regs_486 = 1` in `[Machine]`).
- `pg_pse36_permissions`: 86Box does not fault on the reserved bits
  21:17 of a 4 MiB page directory entry on PSE-36 CPUs.
- `pm_alignment_check` and `priority_alignment_page`: 86Box does not
  raise the alignment-check exception (#AC).

Builds that include the PSE-36 and #AC fixes pass the last three, which
leaves only the two debug-register failures.

The tests deliberately keep these as `FAIL`; see
[interpreting emulator differences](doc/results.md#interpreting-emulator-differences).

Counts depend on the CPU model, machine, 86Box version and image. Record
your own reference run with the same configuration before comparing builds.

## Documentation

- [Running and measuring](doc/running.md): per-test emulation capacity,
  serial capture, driving a VM, sustained host profiling.
- [Results and interpreting them](doc/results.md): serial record format,
  `result.json`, emulator differences, validation scope.
- [Building and booting the image](doc/building.md): toolchains, aarch64
  hosts, build settings, floppy boot and memory layout, local releases.
- [Coverage, CPU contracts and safety](doc/coverage.md): what is covered,
  CPU contracts, accuracy-only builds, CPU model matrices, safety limits.
- [Workload catalogue](doc/workloads.md): every registered record.
- [CPU contracts](doc/cpu-contracts.md): expected features per CPU model.
- [Release notes](RELEASE_NOTES.md).

Source layout: guest sources are in `src/`, host tools in `tools/`, and
their tests in `tools/tests/`.
