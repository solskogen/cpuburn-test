# CPU expectations, discovery and coverage

The configured CPU is the expectation, not the oracle. `cpu_models.h` contains
architectural minimum requirements; `tools/cpu_matrix.py` reads 86Box's source
tables only to discover selectable CPU identities and compatible machines.
Emulator feature implementations and observed test results never supply
expected instruction results.

The catalogue includes 486-class Cyrix/IBM 386-bus upgrades, Intel/AMD/Cyrix/ST
486s, Pentium/OverDrive/MMX, WinChip, AMD K5/K6, Cyrix 6x86/MII, Intel
Pentium Pro/II/Celeron/Xeon and VIA C3 Samuel. It excludes 386s and RapidCAD.
Entries are minimum contracts, **not exhaustive specifications** of every
stepping, erratum, configurable feature or model-specific register.

## Independent references

These are references for facts and behavioural expectations, not redistributed
manuals or golden results obtained from an emulator:

| Reference | Use |
|---|---|
| [Intel AP-485, 241618](https://datasheets.chipdb.org/Intel/x86/CPUID/24161818.pdf) | CPUID identity, feature definitions and historical exceptions |
| [Intel Pentium 60/66 datasheet, 241595-002](https://datasheets.chipdb.org/Intel/x86/Pentium/24159502.pdf) | Early Pentium virtual-mode, 4MiB-page and machine-check support |
| [Intel software developer manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) | Instruction results, flags, exceptions, paging and system registers |
| [AMD 20734T](https://datasheets.chipdb.org/AMD/All%20CPU/20734t.pdf), especially tables 29/30 | Am486/Am5x86/K5/K6 identity and feature differences |
| [AMD 22466D](https://datasheets.chipdb.org/AMD/All%20CPU/22466D.pdf) | Baseline/enhanced 3DNow! semantics |
| [AMD 21086C](https://www.ardent-tool.com/CPU/docs/AMD/21086.pdf) | Legacy 32-bit SYSCALL/SYSRET, selectors, ESP, IF and SCE-disabled exceptions |
| [Cyrix application note 112](https://datasheets.chipdb.org/Cyrix/112ap.pdf), table 4 | 6x86, 6x86L, 6x86MX and MII feature differences and CPUID configuration |
| [Cyrix SMM programmer's guide](https://datasheets.chipdb.org/Cyrix/SMM/Smm6x86.pdf), sections 2.5 and appendix C | Older Cyrix SMM opcodes, SMAC gate and model differences |
| [Cyrix MII data book](https://datasheets.chipdb.org/Cyrix/M2/mi_all.pdf), sections 2.9 and 2.15.3 | CCR4 CPUID/ID control and newer SMM instruction gates |
| [MediaGX MMX GXm data book rev. 2.0](https://datasheets.chipdb.org/Cyrix/MediaGX/gxmdb_v20.pdf), tables 3-11, 3-36 and 9-17..25 | CPU configuration, SMM opcodes and standard/extended CPUID; no integrated-device conformance |
| [IDT WinChip 2 datasheet](https://datasheets.chipdb.org/IDT/x86/WinChip2/wc2ds.pdf), chapter 3 | Programmable feature reporting and the default-hidden CMPXCHG8B flag |

Important distinctions encoded in the tool:

- Original 6x86 does not acquire TSC/MSR support merely because it is a
  fifth-generation processor. 6x86L adds DE/CX8; 6x86MX/MII add
  TSC/MSR/PGE/CMOV/MMX, not VME/PSE.
- AMD K5 model 0 uses standard CPUID EDX bit 9 for PGE, not APIC.
- K6 models 6/7 lack PGE; K6-2 requires it starting at stepping 8.
  The matrix supplies the expected signature so this decision need not
  trust an incorrectly emulated stepping.
- WinChip's CMPXCHG8B instruction is required even when its CPUID flag is
  masked for Windows NT compatibility.
- Disabling Intel's local APIC through IA32_APIC_BASE also clears its CPUID
  bit. A guarded register read distinguishes that state; required APIC
  workloads remain required.
- 386-bus 486-class upgrades do not acquire an alignment-check requirement
  from their marketing name. Instruction-set and AC-flag requirements are
  separate.

Other firmware-programmed feature masks, programmable vendor strings, unusual
early Pentium CPUID behaviour, and additional vendor-specific registers are
not comprehensively normalized. Such configurations need further contracts;
do not reinterpret a discrepancy as an accepted emulator baseline.

## Identity and optional discovery

After `CONTROL protocol=cpu1 ready=1`, a controller may send:

```text
@cpu1 CPU_FAMILY FPU_TYPE accuracy [EIGHT_HEX_DIGIT_CPUID_SIGNATURE]
```

`benchmark` is also accepted; `BENCHMARKS=0` always forces accuracy mode.
The guest acknowledges the effective mode and signature. Malformed or
incomplete frames are fatal, not an excuse to fall back to autodetection.
Without input it proceeds after two guest seconds.

The host checks automation's actual machine, memory, clock and, when supplied,
CPU family display name and speed/model label. The guest separately checks its
identity, signature, FPU and required capabilities. CPUID-less CPUs cannot
prove their exact model from CPUID; host configuration and actual automation
identity are particularly important for them.

Standalone detection intersects the minimum contracts of ambiguous identities
and labels the model `ambiguous`. It does not arbitrarily choose a more
feature-rich variant. Unknown models retain an unverified/SKIP model contract.
Guarded CPUID/FPU discovery and, for unknown models, TSC, CX8, CMOV, MMX,
3DNow!/enhanced 3DNow! and FXSAVE/FXRSTOR probes discover executable capabilities.
No arbitrary vendor MSRs or device configuration ports are written during
unknown-model discovery. Probes are not correctness proofs.

Required or claimed features still run their guarded tests when discovery
fails. A broken TSC advertisement does not disappear merely because it cannot
be used as the timing source. No required feature is silently downgraded to
SKIP because the emulator omits its CPUID bit.
The same capability policy applies to optional paths inside mixed workloads,
not only to their top-level registry gates.

## Feature verdicts and selection

`cpu_features.h` explicitly associates features with behavioural witnesses.
CPUID dumps alone do not count as verification. Additive serial records survive
both raw-log parsing and JSON capture:

| Record | Meaning |
|---|---|
| `CONFORMANCE` | Expected/detected model, contract availability, minimum requirements and mode |
| `PROBE` | Guarded instruction availability, not correctness |
| `FILTER` | A registered test omitted by `ONLY`, distinct from an executed SKIP |
| `FEATURE` | Required/detected/probed capability, witness, outcomes and filtered count |

Feature `FAIL` means its witness or another test requiring that feature failed
or faulted. This associates failures with relevant tests; it does not identify
the exact defective opcode automatically. `UNTESTED` means its witness is
missing, filtered or skipped, or some related coverage was omitted.
Tests requiring an unrelated unsupported extension do not count as missing
coverage for their prerequisite features (for example, skipping 3DNow! on an
Intel CPU does not make its baseline MMX coverage untested).
`NA` is not a pass. Feature `PASS` is explicitly **sampled** behavioural
coverage, not a proof of every input, interaction or architectural detail.
Raw standard/extended CPUID claims without a mapping remain named UNTESTED
records, rather than silently disappearing.

The model contract separately tests required advertisements. Consequently,
an instruction can execute correctly while the identity contract fails for
its missing required feature bit.

System-state witnesses include strict TSC-MSR reads, MTRR/MCA register inventory,
unchanged PAT writes, CR4.MCE control and DE's DR4/DR5 exception behaviour.
These do not claim cache-coherency measurements or external machine-check
injection. PSE36 checks high-address-bit acceptance and reserved-bit/protection
fault precedence without reading unowned physical memory above 4GiB.

Legacy AMD SYSCALL/SYSRET checks both SCE-disabled #UD and enabled transitions,
entry/exit selectors, unchanged ESP, return EIP and interrupt flags. MMX
extensions have separate integer-reference arithmetic, shuffle/extract/insert,
masked-store and non-temporal-store checks; they never run merely because
baseline MMX is supported. K6 models 6/7's older SYSCALL feature bit is decoded.

Other claims without behavioural witnesses remain visible UNTESTED.
This is a regression tool with an explicit
coverage ledger, not a certificate that all supported CPUs are fully accurate.

## Cyrix CPU-system cases and GXm integration

`cyrix_cpuid_control` checks CCR4.CPUID off/on, EFLAGS.ID writability,
disabled CPUID #UD, enabled vendor identification, and CCR3/CCR4 restoration
on 6x86MX/MII and GXm.
`cyrix_smm_disabled` clears SMAC without changing the SMM address range and
checks precise #UD EIP and unchanged owned operand bytes for the six descriptor
save/restore opcodes, RSM and the model's SMINT encoding. MII/6x86MX additionally
exercise RDSHR/WRSHR. SMI_LOCK with SMAC already set causes an explicit SKIP:
the test must not try to unlock it or enter an unowned SMM handler.
The selected older CPUs are Cx486DX2/DX4, 5x86 and 6x86/6x86L; CPUID-less
models require the host-supplied identity. Other unknown models never receive
configuration-port accesses from these tests.

These are disabled-path cases, **not** positive descriptor restore, header-pointer
roundtrip, SMM entry/exit, all register/addressing forms or complete CCR/ARR
coverage. Those remain gaps. SMINT is 0F 7E in the older guide, versus 0F 38
in the MII and GXm books; the tests must not assume one encoding for all Cyrix.

`mediagxm` matches the selectable MediaGX MMX family in 86Box.
`mediagx_cpuid` checks documented CPUID identity/features, TLB/cache descriptors
and leaf limits, leaving stepping and reserved fields unconstrained.
Family 5/model 4 is also used by 6x86L; standalone detection cannot uniquely
identify GXm from that signature. Use an explicit contract, supplied by the
runner's VM configuration or source-bound matrix. GXm extended EDX bit 24
is EMMI, not FXSR, and bit 16 denotes FPU
conditional moves, not PAT. The book's zero extended FCMOV flag conflicts with
its x87 instruction listing; do not infer x87 instruction absence solely from
that flag. The compatible machine is ECS P5GX-M (`p5gxm`); integrated
graphics/audio/device conformance remains outside this CPU contract.

The CPU-system records participate in ordinary checked benchmarking too.
Counted units distinguish CPUID control states, disabled-SMM opcode cases
and CPUID leaf cases. Their measured time includes configuration, validation
and in-function restoration; it is not raw instruction throughput. Separate
EMMI opcode records retain full checked vectors in both execution modes.

## Reproducible source-bound matrix

Generate compatible configurations without starting any emulator:

```sh
python3 tools/cpu_matrix.py --source ../86Box --output build/cpu-matrix
python3 tools/cpu_matrix.py --source ../86Box --all-speeds --output build/cpu-matrix-all
```

By default, select one eligible speed per named family, all supported execution
engines, and no-FPU/external-FPU choices where applicable. `--all-speeds` keeps
every CPU-table entry, including explicitly blocked entries. Package,
voltage, bus, multiplier and minimum RAM constraints are checked without CPU
override. Boards with unparsed CPU blocklists are conservatively excluded.
Failure to find a suitable board is recorded as blocked, never passed.

The manifest records the source revision, exact table hashes, whether those
tables are modified, and enabled preprocessor definitions. `#if 0` entries
are excluded. Pass `--define USE_AMD_K5` only when that option matches the
emulator build; the source revision alone does not establish build options.
The inventory with the MediaGX integration contains 67 families
and 146 configurations; these counts are not hard-coded expectations.
Both runtime engines are covered where dynarec exists, including CPUs for
which the GUI normally forces its dynarec checkbox on.
Generation requires Python, Git and the GNU C preprocessor (`cpp`).

Run a selection sequentially, with isolated mutable inputs:

```sh
python3 tools/cpu_matrix.py --source ../86Box --output build/matrix-run \
  --run --family pentium_p55c --family k6_2p --engine on \
  --emulator ../86Box/build/src/86Box --image floppy.img
```

Use `--fpu-backend both` to expand FPU-equipped rows across host and softfloat
implementations; `auto` preserves the usual host choice except on
software-only boards. No-FPU rows are not duplicated by this choice.
Single runs also accept `tools/run_86box.py --fpu-backend softfloat`.

The matrix defaults to accuracy mode. For sustained checked CPU benchmarks,
build with `BENCHMARKS=1` and select `--mode benchmark`:

```sh
make -B BENCHMARKS=1
python3 tools/cpu_matrix.py --source ../86Box --output build/cpu-benchmarks \
  --run --mode benchmark --family cx6x86mx
```

Omitting `--engine` runs each available engine separately, with the same image
and checked workload inventory. Results retain the requested engine/FPU backend
and point to each runner's complete timing/host-CPU evidence; requested engine
selection alone is not proof every instruction used generated code.
An accuracy-only image cannot silently satisfy a benchmark matrix request:
it is recorded as `benchmark_unavailable`. Keep engine reports separate;
guest counters alone do not measure emulator host throughput.

Omit family/engine selection to execute the generated matrix. `--rompath`
selects an existing local ROM tree; firmware is never downloaded.
`--wall-seconds` limits each configuration. Collection continues after guest
failures or infrastructure failures, retaining per-row details in `results.json`.
Output containing execution results is not overwritten.

Missing ROMs, failed boots, unsupported machine prerequisites and host deadlines
are infrastructure/incomplete outcomes, not architectural successes or new
CPU failures. Matrix generation does not establish that every selected board
has installed firmware or boots the image. Some boards need additional BIOS
setup; the runner's automated F1 handling cannot configure arbitrary firmware.
Engine/backend mode is recorded as requested configuration: current automation
does not independently prove translated-code execution or the actual FPU backend.

Use the per-configuration result files for regression comparisons. Keep CPU,
FPU, engine, image, selected tests and build settings matched; retain every
FAIL/FAULT and unexpected SKIP or missing/filtered test. Performance reporting
remains separate from architectural verdicts. There is no accepted-failure
whitelist and no host-speed threshold determining CPU correctness.
Reports retain per-round correctness observations. When `perf_report.py` uses
`--baseline`, `correctness_comparison` describes changed statuses, skip reasons
and first-assertion values independently of throughput comparisons. It does
not classify existing failures as acceptable or impose a new regression policy.
Older reports without assertion details explicitly lack this comparison.
