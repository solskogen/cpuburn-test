# Results and interpreting them

## Results for automation

Example record (numbers are illustrative):

```text
CFG rounds=2 ... seed=86b04a21 ... suite=full timing_scope=checked_workload ms_resolution=10
WORK phase=warmup r=1 id=1 name=workload
WORK phase=sample r=1 id=1 name=workload
WORK phase=end r=1 id=1 name=workload
NOTE test=workload detail=value
TEST r=1 name=workload status=PASS ms=30000 us=29987411 cyc=123456789 unit=tsc ops=100000000 sum=12345678 iterations=1000 invocations=1001 warmup_ms=30 warmup_failed=0 failed_iterations=0 id=1 iteration_ops=100000 ... total_ms=30120 overhead_ms=90 harness_cyc=2100000 harness_full=0 exc=2000 excv=14:2000 irq0=3012 tags=paging,exc fn=0012f870
FILTER suite_selection=0 build_selection=0 runtime_selection=0
SUMMARY rounds=2 tests=100 pass=80 fail=0 skip=20 fault=0 ms=2500000 warmup_ms=3000 sample_ms=2400000 overhead_ms=96000 suite_overhead_ms=1000
DONE
```

The runtime fields show where **guest time** goes, measured with the guest's
100 Hz timer on any host (10 ms resolution). Per test, `total_ms` covers the whole test,
including skip handling, notes and result output. `warmup_ms` and `sample_ms`
time only the test invocations; `overhead_ms` is the remainder, including state
restoration, arena reset, dispatch and serial output. Individual invocation
times have 10 ms quantization error, which can accumulate across repetitions;
for tiny tests, the remainder is not a precise measurement of framework cost.
`SUMMARY` sums these, and `suite_overhead_ms` covers work between tests. Use total durations to find which tests
dominate a full run before trimming anything. Guest time is not host cost;
host CPU per workload comes from the 86Box runner. `perf_report.py` validates
and prints the breakdown, including skips and failed tests. These records
do not change correctness checks, warm-up, sampling duration or test scale.

The [Usage commands](running.md#boot-and-capture-results) capture and summarize a complete
COM1 log without QEMU or a host profiler. The report lists the ten longest
test records and retains every runtime record in JSON. Older logs remain
supported, with the breakdown marked
unavailable. The guest totals exclude firmware boot and guest initialization.

Log format changes: older logs had a `BEGIN` line per record (now only
`WORK phase=warmup`), separate `RUNTIME scope=test|suite` lines (now the
`total_ms`/`overhead_ms` fields of `TEST` and the totals on `SUMMARY`),
`timing_scope`, `ms_resolution` and `seed` on every `TEST` (now on `CFG`)
and a `FILTER name=... reason=...` line per filtered record (now one
`FILTER suite_selection=N build_selection=N runtime_selection=N` count line).
The shared parser (`tools/qemu_smoke.py`) reads both formats and gives the
same records: it copies the `CFG` constants back onto executed records, the
seed onto failed ones, and rebuilds the runtime records. Quoted values
(`first="..."`, `FAILDETAIL ... check="..."`) escape `"` and `\` with `\`.

Attribution fields on `TEST`, counted over all invocations of the record
(warm-up included): `exc` exceptions and `excv` per vector (`vector:count`),
`irq0` timer interrupts, `irq` other serviced IRQs, `spur` spurious IRQs,
`guard` guard recoveries (a fault or watchdog unwinding to its guard);
zero counts other than `irq0` are left out. `harness_cyc` is the time spent
restoring kernel state after each invocation, in `unit` counts (TSC cycles,
or 1,193,182 Hz PIT clocks), and `harness_full` how many of those restores
had to reload the descriptor tables, segments, TR/LDTR and paging (after a
fault, a timeout, or a test that left system state changed); the others only
verify that nothing changed. `fn` is the test function's address in the
kernel map (`build/kernel.map`). `tags` names the subsystems a record
exercises (or `-`):

| Tag | Meaning |
| --- | --- |
| `smc` | self-modifying or freshly written code |
| `paging` | paging enabled, page tables or TLB |
| `segload` | protected-mode segment/descriptor loads |
| `exc` | expected CPU exceptions |
| `irq` | hardware interrupts |
| `io` | port I/O to chipset or devices |
| `x87` | x87 FPU |
| `mmx` | MMX/3DNow!/vendor SIMD |
| `modesw` | mode switches (real/V86/protected) |
| `rep` | string instructions with REP |

Tags come from a default per test table (`kmain()` in `src/core.c`), the
record's own `T_TAG_*` flags (`src/kernel.h`) and its name (`smc`, `irq`,
`rep`); they are a filtering aid, not a complete description.

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
`sum` mixes the values returned by every measured invocation, so it is only
reproducible when the invocation count is fixed, as in the accuracy suite.
Timer, TSC, RTC and performance-counter readings are kept out of `sum` and
appear only in `NOTE` lines. Results can still differ legitimately between
FPU backends or CPU models (for example `fpu_trans` and `fpu_burn`), so
compare `sum` only between runs on the same configuration.
`op_unit` names the counted work where specified; older workloads retain the
generic `operation` label. `timing_scope=checked_workload` (on `CFG`) explicitly includes
operand/reference preparation, assertions, and any in-function enable/restore
sequence. These are not isolated opcode latency measurements. Reports preserve
these fields and refuse to pool or compare incompatible work units/scopes.
`ms`, `cyc`, and 64-bit `ops` aggregate measured invocations, excluding warm-up.
`iterations` counts measured invocations and `invocations` all of them,
including the warm-up; `iteration_ops` is their constant
operation count, or zero if it varied. `failed_iterations` includes warm-up.
On failure, the final `TEST` record also retains the first failing invocation:
`failure_iteration=0` is warm-up, `1` is the first measured invocation,
and `failure_phase` names the phase. The configured seed is on `CFG`.
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
`ms_resolution=10` (on `CFG`), `ms_unresolved=1`, and `counter_unresolved=1` explicitly
describe coarse or zero timing. `us` is the measured time from `cyc`
(TSC rate calibrated against the PIT, or PIT clocks), not summed 10 ms
ticks; it is `0` without a calibrated counter. `sample_window_ms`, which
decides when sampling stops, uses the same counter. Absence of a resolved duration is not zero
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
`results.json` is rewritten after each CPU, so an interrupted matrix keeps the
CPUs already run. Each CPU gets 30 minutes once the guest reports an accuracy
build, otherwise 8 hours; `--timeout` overrides both.
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
Host metadata, sampling settings and perf enablement must match when pooling
86Box captures. With host-controlled sampling, sample length depends on how
quickly the speed settles: prefer per-phase capacity and
counter/operation data, not boot-to-DONE time, as the optimization signal.

Host durations include boot and logging; 86Box durations also include
automation stepping/capture overhead. Keep the step size identical between
runs and collect several independent boots for host-time distributions.

See [per-test emulation capacity](running.md#measure-per-test-emulation-capacity) for
VM-based runner commands and prerequisites. Captures also include screen
state, emulator information and host elapsed time. The runner requires this
86Box checkout's automation interface, not necessarily an unmodified upstream
binary. `--expect DONE` checks completion, **not zero assertion failures**:
inspect `serial.log` and `SUMMARY`.

## Capacity comparisons and summary.json

Guest `cyc`/`ms` per operation follow **emulated** time: in 86Box they show
the emulator's timing model, not how much host work a test costs. For
emulator performance work use **capacity**: guest seconds advanced per host
CPU second of 86Box's emulation thread, × 100 (speed % per host CPU second),
from `workload_capture` in the 86Box runner's `result.json`.

`perf_report.py` prints and saves a `capacity` section for runner results:

- Per sample, the leading quarter of its interior steps (at least one, for
  four or more steps) is left out as warm-in; capacity is the remaining guest
  ms over the remaining emulation-thread CPU seconds.
- Per test, `median_percent` is the median over samples (rounds and
  `--repeat` runs), and `ci95_percent` a two-level bootstrap interval:
  samples are resampled, then each sample's steps in moving blocks of
  ⌈√n⌉ consecutive steps, so correlated neighbouring steps do not make the
  interval falsely narrow. `--bootstrap N` (default 1000) and `--seed`.
- `rank` orders the suite's tests from 1 (lowest capacity) up; `z` is the
  test's log capacity standardised against the suite's tests.
- Results from before step series fall back to
  `cpu_full_speed_capacity_percent` per sample, without an interval.

Compare two binaries (A/B), each with several runs, and establish the noise
floor first with the same binary twice (A/A):

```sh
python3 tools/run_86box.py --vm "/path/to/VM" --emulator ./86Box-old --aa --pin 13 \
  --output-dir build/aa
python3 tools/run_86box.py --vm "/path/to/VM" --emulator ./86Box-new --repeat 3 --pin 13 \
  --output-dir build/new
python3 tools/perf_report.py build/new/repeat-*/result.json \
  --compare-capacity build/aa/repeat-*/result.json \
  --noise-floor build/aa/summary.json --summary build/compare-summary.json
```

`--compare-capacity BASELINE...` bootstraps current/baseline − 1 per test:
**faster** when the 95% interval lies above `+floor` percent, **slower** when
below `−floor`, otherwise **noise** (`missing_in_current`/`missing_in_baseline`
when a test is absent). `--noise-floor` takes a percentage or a report or
summary JSON holding an A/A floor; `--gate-capacity` exits 1 on any slower
test. `--aa` (in `perf_report.py`, or `run_86box.py --aa`, which runs at least
twice) compares even- against odd-numbered runs of one binary;
`noise_floor_percent` is the 95th percentile of |change| over tests, and any
A/B change smaller than that is not evidence. Keep host conditions equal
(`--pin`, same governor; `host` and `host_state` are recorded).

The runner writes `summary.json` into each run directory (and, with
`--repeat`, one pooling all repeats in the parent); `perf_report.py --summary`
writes the same format. It is meant for scripts and AI agents; fields that a
guest image or option did not provide are `null`:

```json
{
  "format": "cpuburn-summary/1",
  "runs": 1, "directories": ["..."], "statuses": ["observed"],
  "image_sha256": "...", "emulator_sha256": "...", "machine": {}, "host": {},
  "cpu_scope": "emulation_thread_schedstat_ns",
  "fpu_backend": "softfloat",
  "capacity_definition": "...", "bootstrap_replicates": 1000,
  "tests": [
    {"name": "int_alu", "status": "PASS", "statuses": {"PASS": 2},
     "capacity": {"median_percent": 412.5, "ci95_percent": [405.1, 420.3],
                  "samples": 2, "steps": 37, "wall_median_percent": 398.0},
     "rank": 1, "z": -1.2, "ipc": 1.9,
     "tags": ["int", "alu"], "exc": 0, "irq": 312, "harness_cyc": 1840,
     "fpu_backend": "softfloat",
     "perf": {"samples": 4100, "jit_share": 0.71,
              "top_symbols": [["codegen_timing_486", 310], ["[unknown] 0x7f3a1c2040", 95]]}}
  ],
  "compare": {"noise_floor_percent": 1.5, "counts": {"noise": 40, "slower": 1},
              "tests": {"int_alu": {"current_percent": 412.5, "baseline_percent": 430.0,
                                    "delta_percent": -4.1, "ci95_percent": [-5.0, -3.2],
                                    "verdict": "slower"}}},
  "noise_floor": {"noise_floor_percent": 1.5, "tests": {}}
}
```

- `tests` is sorted by `rank` (slowest first); tests without capacity (SKIP,
  accuracy-only images, `HANG`) follow by name.
- `status` is the worst status seen over runs, in the order HANG, FAULT,
  FAIL, PASS, SKIP; `statuses` counts each. `HANG` comes from the runner's
  hang detector (`hangs` in `result.json`).
- `tags`, `exc`, `irq` and `harness_cyc` are the `TEST` record's `tags=`,
  `exc=`, `irq=` and `harness_cyc=` fields, when the image prints them.
- `fpu_backend` comes from `FPUPROBE backend=...` lines (per test when the
  line names one, otherwise for the whole run).
- `perf` is present only with `--perf`: samples, `jit_share` (leaf in
  generated code) and the ten top leaf symbols. `ipc` needs `--perf-stat`.
- `compare` is present only with `--compare-capacity`, `noise_floor` only
  with an A/A run.

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
See [known results](../README.md#known-results) for the current reference runs and
[release notes](../RELEASE_NOTES.md#known-results-and-limitations) for earlier
image-specific results.

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
