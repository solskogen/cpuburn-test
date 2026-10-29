# Workload catalogue

## Coverage boundaries

The source registries `tests_*.c` list the precise workloads and feature gates;
the following sections describe their checked behavior and limits.

| Area | Source modules | Evidence and limits |
|---|---|---|
| Integer execution and translation | `tests_int.c`, `tests_corpus.c`, `tests_sweeps.c` | Arithmetic/defined flags, decoding, branches, SMC and code footprints; exhaustive only where explicitly stated, otherwise deterministic samples. |
| x87 and MMX | `tests_fpu.c`, `tests_fp_edges.c`, `tests_mmx.c` | Results, formats, rounding, status/environment and scalar/exact references; not full x87 transcendental precision certification. |
| AMD/Cyrix extensions | `tests_vendor.c` | Feature-gated baseline/enhanced 3DNow! and all twelve Cyrix EMMI opcodes; integer-only references, finite dyadic floating-point cases and documented reciprocal precision bounds. Not exhaustive floating-point range/rounding certification. |
| Cyrix CPU controls | `tests_cyrix.c` | Model-qualified CCR4 CPUID/ID-flag controls and disabled-SMM opcode faults; saved configuration restored. GXm uses the `mediagxm` contract on ECS P5GX-M. Not full CCR/ARR or SMM entry/exit coverage. |
| Privilege, exceptions and restart | `tests_pmode.c`, `tests_heavy_pmode.c`, `tests_priority.c`, `tests_restart_complex.c` | Protected-mode architectural transitions and selected fault precedence/restart cases; not every combination of instructions, exceptions or pending interrupts. |
| Paging and translated memory | `tests_paging.c`, `tests_heavy_paging.c`, `tests_mem.c` | Permissions, aliases, TLB invalidation and checked accesses; selected memory sizes/alignments, not a proof over all physical mappings. |
| CPU/system state and sustained interactions | `tests_sys.c`, `tests_heavy_state.c`, `tests_burn.c` | Feature-gated control state, interrupts and mixed repetition; safe probes do not establish complete model-specific MSR behavior. |
| Supporting platform | `tests_chipset.c` | Legacy PIC/PIT/RTC, A20 and limited DMA/ROM/PCI checks; not comprehensive motherboard/chipset validation. Network, IDE and other peripheral throughput are outside the primary scope. |

`req` in each `TEST` record is the hexadecimal requirement bitmask defined
in `kernel.h`. It explains eligibility, not everything a test exercises.
`SKIP` retains the missing feature or runtime precondition; it is not coverage
or a pass. Inspect the selected machine, feature gates and actual result
inventory rather than assuming every CPU runs the same cases. 808x/286 are
not supported by this protected-mode image; 386 results are explicitly
warned as unreliable. Availability is not inferred from a CPU's marketing
name or its age: a K6 without 3DNow! does not run 3DNow! tests, and a
6x86 without MMX/EMMI does not run EMMI tests. A supported CPU model is
not a promise of exhaustive model-specific instruction/MSR coverage.

### Vendor-specific extensions: 3DNow! and Cyrix EMMI

This subsection describes additional instruction sets, not the CPU support
list. Intel and other supported vendors use the common integer, x87, MMX,
protected-mode, paging and system workloads above, according to their model
and available facilities. P6 and other optional common instructions are
feature/model gated too. See [model coverage and matrices](../README.md#model-coverage-and-matrices)
for the CPU catalogue and its remaining coverage gaps.

The same i486-built image contains these optional workloads. Standard MMX
and FPU gates still apply. `CPUID.80000001:EDX[31]` enables baseline
3DNow! tests; bit 30 additionally enables the enhanced instructions
implemented for K6-2+/K6-III+ in 86Box. This also permits other supported
86Box CPUs advertising baseline 3DNow!, without assuming AMD ownership.
Raw extended CPUID bits and decoded capabilities appear in `CPU` records.
Later AMD extended-MMX/SSE instructions are deliberately not included.

| Record | Instructions/behavior | Cases per invocation at scale 8 |
|---|---|---:|
| `3dnow_float` | PFADD/PFSUB/PFSUBR/PFMUL/PFACC, PFCMPEQ/PFCMPGE/PFCMPGT and PFMIN/PFMAX; signed finite integer inputs with exact binary32 references, including signed multiplication zero. | 30,720 |
| `3dnow_convert` | PI2FD and PF2ID; exact conversion and fractional truncation with positive/negative lane values. | 6,144 |
| `3dnow_integer` | PAVGUSB rounded unsigned-byte averages and AMD PMULHRW signed rounded products. | 6,144 |
| `3dnow_reciprocal` | PFRCP/PFRSQRT low-lane broadcast, plus documented PFRCPIT1/PFRSQIT1/PFRCPIT2 refinement sequences. Positive powers of four; initial relative-error bounds of 2^-14 (reciprocal) / 2^-15 (reciprocal square root), final bounds of 2^-23. | 2,048 sequences |
| `3dnow_state` | FEMMS empties MMX tags and permits exact x87 integer use; PREFETCH/PREFETCHW preserve flags and the referenced bytes. Cache-hit/timing effects are not asserted. | 1,024 transitions |
| `3dnow_enhanced_float` | PFNACC/PFPNACC horizontal subtraction/mixed accumulation. | 6,144 |
| `3dnow_word_convert` | PI2FW word selection and PF2IW truncation, signed saturation and sign extension into both dwords; poisoned unused input words and destination bits expose lane/preservation mistakes. | 6,144 |
| `3dnow_swap` | PSWAPD swaps both dwords, including arbitrary bit patterns. | 3,072 |
| `cyrix_emmi_arithmetic` | PAVEB, PADDSIW, PMAGW, PDISTIB, PSUBSIW, PMULHRWC, PMULHRIW and PMACHRIW; rounding, saturation, implicit destinations and unchanged source registers. | 28,672 |
| `cyrix_emmi_moves` | PMVZB/PMVNZB/PMVLZB/PMVGEZB byte masks, preserving unselected bytes, mask and source. | 8,192 |

`ops` counts packed instruction cases or the sequences/transitions shown
above, not scalar lanes or a universal instruction count. 3DNow! arithmetic
and conversion tests include register, memory and same-register source/destination
forms. EMMI checks both even/odd specified registers and their implicit partners;
memory-only instructions are never tested with invalid register operands.

The twelve additional `cyrix_emmi_<opcode>` records isolate each EMMI operation
using the same assertion forms, edge cases and vector counts as the two
aggregate records. Each single-opcode invocation resets the deterministic PRNG;
later opcodes in an aggregate continue its PRNG stream.
Both accuracy and benchmark builds include them; existing aggregate IDs remain
available for historical comparisons. Their work unit is
`emmi_instruction_case`, not scalar lanes or raw instruction throughput.

EMMI has no ordinary CPUID feature bit. The gate accepts `CyrixInstead`,
family 6, model 0 with MMX (6x86MX/MII), or GXm family 5, model 4 with
extended CPUID EDX bit 24. That extended bit means EMMI, **not FXSAVE**.
The workloads temporarily enable CCR7.EMMX, preserving CCR3 mapping and
restoring both configuration registers even after a guarded fault/watchdog.
Other CPUs skip without touching these ports. Tests do not enable CPUID
on CPUs whose firmware has disabled it; those cannot satisfy this EMMI gate.
Equal/opposite PMAGW magnitude ties are avoided because the reference used
here does not specify their tie-breaking rule.

The integer-only references follow AMD's
[3DNow! manual, 21928G](https://kib.kiev.ua/x86docs/AMD/AMD-3DNow/21928G.pdf)
and [extension manual, 22466D](https://kib.kiev.ua/x86docs/AMD/AMD-3DNow/22466D.pdf),
and the Cyrix instruction descriptions in the
[NASM 0.98.39 manual](https://github.com/netwide-assembler/nasm/blob/nasm-0.98.39/doc/nasmdoc.src).
They do not use 86Box results as expected values. NaNs, infinities, denormal
range/underflow cases, general reciprocal inputs and all rounding cases are
not exhaustively covered. A mismatch remains `FAIL`, not an emulator-specific
exception to the reference.

## Heavy interaction workloads

### CPU execution-boundary workloads

These five records target execution/state boundaries that matter to 86Box's
interpreter and recompiler. They assert architectural results independently;
agreement between two emulators or execution engines is not their oracle.
The same static instruction sites are reused, but the guest cannot prove
that a particular instruction was translated or linked by the host.

| Record | Checked behavior | Cases per invocation at scale 8 |
|---|---|---:|
| `int_flags_boundaries` | ADD/SUB result and all six defined arithmetic flags across taken JMP and CALL/RET, then SETcc and ADC/SBB consumers; edge values plus seeded inputs. | 4,096 producer/consumer cases |
| `int_partial_boundaries` | AL, AH and AX writes preserve the other EAX bits across branches/calls; full-width snapshots and an AH-dependent result. | 8,192 cases |
| `int_divide_state` | DIV by zero, unsigned quotient overflow and signed quotient overflow deliver #DE at the faulting EIP, preserving dividend and divisor registers both in the exception frame and after handler return. Undefined division flags are not asserted. | 3,072 faults |
| `mmx_x87_transitions` | FNSAVE/FRSTOR around MMX arithmetic and EMMS, nonempty MMX tags/TOP state, empty tags after EMMS, and exact integer x87 results plus final tags/TOP. Saved tags may classify register contents as valid, zero, or special; they need not all be zero. | 2,048 transitions |
| `heavy_pm_code_context` | Repeated far calls to identical code bytes at the same offset/base/selector while alternating the code descriptor's default operand size. Both 16- and 32-bit decoding paths must return the correct result. | 32,768 calls |

`ops` counts the cases/faults/transitions/calls in this table, **not guest
instructions**. The first three and code-context record require i486;
MMX/x87 transitions require both features and otherwise explicitly skip.
Context changes use guarded descriptor restoration; the divide handler is
restored after the workload. These additions do not claim new coverage of
all Jcc conditions, page-fault commit rules, real-mode transitions, SMM or
every x87 exception. Existing paging/restart and FPU tests cover separate cases.
Use `ONLY` with these exact names for an isolated diagnostic image.

`corpus_integer_sequences` generates deterministic 32-instruction programs
mixing 8/16/32-bit ADD/ADC/SUB/SBB/CMP, AND/OR/XOR, ROL/ROR, NOT and INC/DEC
across four registers. At default scale it regenerates 16,384 blocks at
64 instruction alignments and executes each eight times: **4,194,304 corpus
instructions**, with four register/flag checkpoints per block execution.
These counts describe one invocation; the sustained harness repeats it.
Every opcode-group/width combination must occur. Arithmetic results and
carry/borrow use an independent bit-serial reference; only architecturally
defined flags are compared. A failure logs the global seed, vector seed,
block/reuse/checkpoint, initial registers and the full instruction prefix
needed to reproduce it. This is a constrained, safe instruction corpus,
not unconstrained random privileged/invalid-opcode fuzzing.

The `fdiv_v*_rn/rd/ru/rz` records add the notorious 4,195,835 / 3,145,727 Pentium FDIV case,
nearby mantissas and scaled/signed ratios with exact raw x87 reference values
under all four rounding controls. `fprem_v*` and `fprem1_v*` exercise FPREM and FPREM1
with extreme extended-range exponents, signed zeros, nearest-even ties,
denormals and nonfinite status checks. Each of the 15 FDIV vectors and four
rounding controls has its own record (60 total); each of 11 remainder
vectors/instructions has its own record (22 total). Four classification records
independently exercise FPREM/FPREM1 denormals, finite/infinity and invalid
infinity/finite. A failure in one record cannot hide any other vector or
rounding mode. Default totals on a passing CPU are 122,880 divisions,
45,056 remainder cases and 32,768 classification cases. Individual records
still fail fast on their first bad iteration. Iterative C2 completion is bounded at 2,048
steps: noncompletion is a reported failure, not an infinite spin.
The embedded tables can be independently rechecked without floating-point
arithmetic using `python tools/check_fp_edges.py` (exact integer/rational
rounding and remainder identities).
Negative-quotient condition bits are diagnostic only because their signed
encoding is ambiguous in the reference wording; exact signed remainders and
nonnegative quotient bits are still asserted.

`heavy_decode_align` sweeps offsets 0–63 and 4093–4095 with prefixed,
variable-length i486 code and both branch paths (1,852,416 instructions at
default scale). `heavy_decode_worksets` covers executable footprints from
4 KiB to 4 MiB, contrasting code rewrite/taken-branch invalidation with
unchanged reuse (1,245,168 instructions with full-size owned RAM).
`heavy_mem_align` covers 13 footprints from 1 KiB to 4 MiB, the same 67
offsets, and 1/2/4/8-byte accesses with bytewise reference checks
(1,363,584 operations). Eight-byte accesses use integer halves, not optional
MMX/SSE. Available owned RAM caps the largest footprint and is logged.
Timing notes give per-size results and the worst alignment; memory timings
include the checked loop's initialization/reference overhead, not pure
load/store latency. The sweep is substantial on slow hardware (about
62 seconds for the memory record on the tested 33 MHz 86Box 486).

The paged-code/restart additions include six separate records: code modified
through physical aliases and remapped execution pages; linked direct/indirect
code blocks; cross-page instruction fetch and data access; partially completed
forward/backward REP MOVSD; and split-stack PUSH/POP fault restart. Assertions
check final results and fault-time architectural state, including preserved
registers. At the default scale these execute 53,477,376 SMC instructions,
1,048,576 linked blocks, 81,920 real page faults and 2,097,152 MOVSD elements.
These are workloads with fixed counts; they do not merely sleep for a duration.
They use owned memory and restore mappings/hooks after guarded failures.

Six exception-priority records add 98,304 default cases: segment-limit #GP
before operand paging; LGDT/LIDT privilege #GP; invalid LOCK MOV #UD;
nonpresent/protection page-fault error bits; instruction-fetch faults before
operand access; and mapped ring-3 alignment controls. They check defined
exception/state outcomes rather than unspecified overlaps; in particular,
same-data-access #AC versus #PF ordering is not asserted. Missing mandatory
traps remain failures. Setup and execution are guarded with mapping/hook cleanup.

Complex restart records exercise mixed 16/32-bit PUSHA/POPA, ENTER/LEAVE,
same-privilege far CALL/RETF and same-ring IRET across page boundaries.
The faulting stacks belong to ring 3 so faults can use a safe ring-0 TSS
stack. Checks cover restart EIP, CPL, CR2, defined page-fault bits, final
registers/frames and guard bytes. They deliberately do not require rollback
of intermediate memory stores or assert unspecified fault-time registers.
Default scale runs 32,768 cases per complex record (131,072 total), with
mode-specific instruction/fault counts logged separately.

Eight protected-mode stress records add 16-bit code/stack/address-size paths,
LDT/TI descriptor use, expand-down boundaries, ring-3 call gates with parameter
copying and RETF, privilege checks, VME/PVI virtual flags/VIP faults, and
interrupt shadows. Default coverage includes about 4.2 million 16-bit operations
and 16,384 call-gate transitions. STI/IRET use 64 actual pending IRQ samples;
MOV SS/POP SS use pending single-step debug traps (not a claim to test every
asynchronous IRQ ordering). Unsupported VME/PVI is skipped through feature gates.

Mixed-state stress alternates seeded integer/memory work, page remaps, x87 and
MMX with software interrupts, checking preserved state against scalar references.
At default scale it performs 131,072 iterations (about 1.84 million operations
on a 486 and 2.36 million with MMX). Separate state tests exercise
FNSAVE/FRSTOR and feature-gated FXSAVE/FXRSTOR. Active LAPIC tests exercise
periodic and one-shot interrupt delivery with PIT deadlines and explicit EOI;
they skip absent/disabled/relocated/x2APIC configurations and a preexisting
active timer rather than taking it over. They do not reroute the PIC, send
IPIs, or test other processors.
At default scale the interrupt-state test delivers 32,768 software interrupts;
each save/restore record runs 65,536 checked iterations. LAPIC periodic stress
requires at least 2,048 deliveries over at least two seconds; one-shot stress
rearms and checks 1,024 deliveries. Actual periodic count varies with timing.


## Inventory and reading the tables

This inventory contains **306 registered records** in the default source
registries, including `cpu_model_contract` and the core recovery test. It is an
inventory, not a promise that all 306 execute on a particular machine. `ONLY`
filters names (the contract is always selected), feature requirements and
runtime preconditions can skip records, and `burn_mixed` runs only in the final
round. The tables below name every record; group descriptions apply to every
name in that group. See [CPU contracts](cpu-contracts.md) for the distinction
between model expectations, claimed/probed features and exercised evidence.

Requirements are registry masks, not complete lists of exercised instructions.
`486` means the i486 instruction baseline, `P5`/`P6` family >=5/6;
`PAGING` means paging was enabled, `NOHV` excludes a detected hypervisor,
`MEM8M` requires owned RAM at 4–8 MiB, and `PCI` requires configuration
mechanism 1. Other capitalized names are the corresponding `REQ_` bits in
`kernel.h`. FXSR, PSE, PAE and PGE additionally need usable CR4; the runtime
also probes some advertised instructions. `AC` is gated by the i486 detection.
A dash means no registry bit, **not** absence of runtime checks or a guarantee
of execution. Arena size, model qualification, guard failures, APIC state and
other preconditions can cause additional skips.

Both `BENCHMARKS=0` and benchmark builds retain the same checked work and
assertions. Usually a checked warm-up is followed by one checked invocation in
accuracy mode or repeated invocations in benchmark mode; the contract always
uses accuracy mode. The reported `ops` is the sum of the workload's own
`t_ops` accounting for sampled invocations, excluding warm-up. It is not a
universal instruction count: older records may use approximate synthetic
counts, omit accounting entirely (zero), or count several kinds of work.
`op_unit=operation` is generic unless a record sets a more specific label.
`timing_scope=checked_workload` includes setup, reference calculations,
assertions and diagnostics executed inside the invocation; it is **not** pure
instruction or memory latency. Counts below describe successful individual
invocations at default `HEAVY_SCALE=8`, unless stated otherwise; failures may
stop early, runtime-dependent work may vary, and sustained sampling repeats
them. Cases, transitions and instruction counts are identified separately from
`ops` where they differ. This is selected, bounded evidence, not exhaustive
coverage unless explicitly qualified.

### Core, integer and generated code

| Record | Requirements | Checked work / unit and limitations |
|---|---|---|
| `cpu_model_contract` | — | One model-contract check: identity, configured FPU and required advertised/probed features; skips if no model expectation is available. Model selection and partial evidence are explained in the linked contracts document. |
| `core_fault_watchdog_recovery` | — | Three guard checks for fault, watchdog and subsequent #UD recovery; requires the PIT/watchdog. |
| `int_flags_boundaries` | 486 | 4,096 ADD/SUB producer/consumer cases across branches/calls and SETcc/ADC/SBB. |
| `int_partial_boundaries` | 486 | 8,192 partial AL/AH/AX writes and preserved-register checks across control flow. |
| `int_divide_state` | 486 | 3,072 #DE cases for zero divisor and quotient overflow, checking fault EIP and registers, not undefined flags. |
| `int_alu` | 486 | Deterministic 8/16/32-bit ALU results, conversions and carry chains against references; `ops` is an aggregate count. Separate records check flags. |
| `int_flags_exhaustive` | 486 | Exhausts all 256×256 byte operand pairs for ADD/ADC/SUB/SBB/CMP flag forms; other widths/operations are samples. Reports a synthetic 500,000 operations, not the exact number of cases. |
| `int_muldiv` | 486 | 8,192 seeded multiply/divide vectors across byte/word/dword forms and three division faults; reports 188,416 operations. |
| `int_shift_rot` | 486 | Shift/rotate results and defined flags over widths, counts and sampled values; counted instruction-form work, not exhaustive operands. |
| `int_bits_misc` | 486 | Bit scan/test/set/reset, byte swap, atomics, SETcc, conversions and selected traps; aggregate operation count. |
| `int_cx8` | CX8 | Two CMPXCHG8B success/failure cases. |
| `int_cmov` | CMOV | Conditional moves against flag combinations; 512 counted operations. |
| `int_bcd` | 486 | BCD/ASCII-adjust operations and flag/result references over byte inputs; aggregate count. |
| `int_string` | 486 | REP string moves, stores, compares and scans including direction and counts; aggregate element work. |
| `int_prefix_addr` | 486 | Prefix decoding, address/operand sizes and addressing forms with checked results; aggregate operations. |
| `int_smc` | 486 | Self-modified immediate/opcode and executing-loop cases at six offsets including page crossings; `ops` is a synthetic mixed instruction/case count. |
| `int_smc_ram_top` | 486 | Generated-code rewrites near the RAM top; counts 32,768 operations. Probes by writing just above firmware-reported RAM when below the 15 MiB hole, and may execute there if readback succeeds; this is not limited to firmware-reported owned RAM. |
| `int_cpuid_cr` | 486 | CPUID where present, EFLAGS and control-register state checks; 100 generic operations, not exhaustive register semantics. |
| `int_alu_burn` | 486 | Repeated integer arithmetic, flags and branch-intensive checks; aggregate counted work, not duration-based. |
| `corpus_integer_sequences` | 486 | Deterministic generated ALU instruction blocks with independent bit-serial references; 4,194,304 generated instructions at default scale, with checkpoint checks. |

The following four records share a 4 KiB page between a patched snippet and a
hot loop. `k` is snippet calls per patch, `h` hot-loop calls per patch; each
invocation makes 2,048 patches. Reported operations are
`2048 * (k * 16 + h * 64 * 5)`; verification and code generation are timed.
They require 486 and available owned arena RAM, and cannot prove a particular
host translation or linking decision.

- `int_smc_mixed_k2_h1` — 2 snippet calls and 1 hot-loop calls per patch.
- `int_smc_mixed_k2_h16` — 2 snippet calls and 16 hot-loop calls per patch.
- `int_smc_mixed_k8_h1` — 8 snippet calls and 1 hot-loop calls per patch.
- `int_smc_mixed_k8_h16` — 8 snippet calls and 16 hot-loop calls per patch.

The rate records below require 486 and owned arena RAM. `k` is calls per
patch and `n` is the number of arithmetic instructions in the snippet. Each
invocation targets 16,384 calls, rounding up to a multiple of `k`; reported
operations are `ceil(16384/k) * k * (n+2)`. The unqualified records change a
MOV immediate; `op` also changes one ADD/SUB/XOR opcode per patch and computes
a reference from the modified code. They check results; neither count nor
timing isolates cache invalidation or proves a dynarec strategy.

**Immediate, n=16:** `int_smc_rate_k1_n16`, `int_smc_rate_k2_n16`, `int_smc_rate_k4_n16`, `int_smc_rate_k8_n16`, `int_smc_rate_k12_n16`, `int_smc_rate_k15_n16`, `int_smc_rate_k16_n16`, `int_smc_rate_k17_n16`, `int_smc_rate_k20_n16`, `int_smc_rate_k24_n16`, `int_smc_rate_k32_n16`, `int_smc_rate_k64_n16`.

**Immediate, n=256:** `int_smc_rate_k1_n256`, `int_smc_rate_k2_n256`, `int_smc_rate_k4_n256`, `int_smc_rate_k8_n256`, `int_smc_rate_k12_n256`, `int_smc_rate_k15_n256`, `int_smc_rate_k16_n256`, `int_smc_rate_k17_n256`, `int_smc_rate_k20_n256`, `int_smc_rate_k24_n256`, `int_smc_rate_k32_n256`, `int_smc_rate_k64_n256`.

**Opcode and immediate, n=16:** `int_smc_rate_op_k1_n16`, `int_smc_rate_op_k2_n16`, `int_smc_rate_op_k4_n16`, `int_smc_rate_op_k8_n16`, `int_smc_rate_op_k12_n16`, `int_smc_rate_op_k15_n16`, `int_smc_rate_op_k16_n16`, `int_smc_rate_op_k17_n16`, `int_smc_rate_op_k20_n16`, `int_smc_rate_op_k24_n16`, `int_smc_rate_op_k32_n16`, `int_smc_rate_op_k64_n16`.

**Opcode and immediate, n=256:** `int_smc_rate_op_k1_n256`, `int_smc_rate_op_k2_n256`, `int_smc_rate_op_k4_n256`, `int_smc_rate_op_k8_n256`, `int_smc_rate_op_k12_n256`, `int_smc_rate_op_k15_n256`, `int_smc_rate_op_k16_n256`, `int_smc_rate_op_k17_n256`, `int_smc_rate_op_k20_n256`, `int_smc_rate_op_k24_n256`, `int_smc_rate_op_k32_n256`, `int_smc_rate_op_k64_n256`.

**Opcode and immediate, n=8:** `int_smc_rate_op_k2_n8`, `int_smc_rate_op_k4_n8`, `int_smc_rate_op_k6_n8`, `int_smc_rate_op_k8_n8`, `int_smc_rate_op_k10_n8`, `int_smc_rate_op_k12_n8`, `int_smc_rate_op_k14_n8`, `int_smc_rate_op_k16_n8`.

**Opcode and immediate, n=32:** `int_smc_rate_op_k2_n32`, `int_smc_rate_op_k4_n32`, `int_smc_rate_op_k6_n32`, `int_smc_rate_op_k8_n32`, `int_smc_rate_op_k10_n32`, `int_smc_rate_op_k12_n32`, `int_smc_rate_op_k14_n32`, `int_smc_rate_op_k16_n32`.

**Opcode and immediate, n=48:** `int_smc_rate_op_k2_n48`, `int_smc_rate_op_k4_n48`, `int_smc_rate_op_k6_n48`, `int_smc_rate_op_k8_n48`, `int_smc_rate_op_k10_n48`, `int_smc_rate_op_k12_n48`, `int_smc_rate_op_k14_n48`, `int_smc_rate_op_k16_n48`.

### x87, MMX and vendor instruction records

All rows in this section use checked inputs, not exhaustive operand ranges.
Legacy `fpu_*` and `mmx_*` records report generic aggregate operation counts;
the count is not necessarily the number of assertions or lanes.

| Record | Requirements | Checked work / limitations |
|---|---|---|
| `fpu_arith` | FPU | x87 arithmetic forms, square root and precision-control results with selected exact values. |
| `fpu_formats` | FPU | Floating/integer and packed-BCD load/store formats and conversions. |
| `fpu_compare` | FPU | Comparisons and status flags for selected ordered/unordered operands. |
| `fpu_round` | FPU | Rounding-control modes and integer conversion results. |
| `fpu_trans` | FPU | Selected x87 transcendental identities using bounded tolerances, not full precision certification. |
| `fpu_exceptions` | FPU | Selected masked/unmasked exception and status behavior. |
| `fpu_stack` | FPU | Stack overflow/underflow, tags and status cases. |
| `fpu_p6` | FPU + CMOV | P6 x87 conditional moves and comparisons/flags. |
| `fpu_fxsr` | FPU + FXSR | FXSAVE/FXRSTOR x87 state round-trip; not SSE arithmetic. |
| `fpu_burn` | FPU | Fixed-count x87 arithmetic, series and transcendental identities with checked references/tolerances; not a wall-clock burn. |
| `mmx_alu` | MMX | Seeded packed arithmetic, saturation, packing, compares and logical instructions against scalar lane references. |
| `mmx_shifts` | MMX | Packed shifts, including counts beyond lane widths, checked against scalar references. |
| `mmx_state` | MMX | MMX register/state and EMMS behavior. |
| `mmx_alias` | MMX | MMX/x87 register aliasing and state effects. |
| `mmx_exc` | MMX | Selected MMX exception conditions. |
| `mmx_burn` | MMX | Fixed repeated packed instruction chains and reference checks. |
| `mmx_extended` | MMX + MMXEXT | Selected extended MMX operations, forms and edge/seeded inputs against scalar references. |
| `mmx_x87_transitions` | MMX + FPU | 2,048 checked MMX/x87 save/restore and EMMS transitions; tags can classify nonempty contents differently. |

The FDIV rows each require FPU. The four names on each row independently
repeat that vector 2,048 times at default scale; `rn`, `rd`, `ru`, `rz` mean
round-to-nearest, down, up and toward zero. `ops` counts divisions. The exact
raw 80-bit operands and independent rounded results are in `tests_fp_edges.c`;
these are selected vectors, not all x87 division inputs.

| Vector | Record names | Input distinction |
|---:|---|---|
| 0 | `fdiv_v0_rn`, `fdiv_v0_rd`, `fdiv_v0_ru`, `fdiv_v0_rz` | 4,195,835 / 3,145,727 FDIV erratum |
| 1 | `fdiv_v1_rn`, `fdiv_v1_rd`, `fdiv_v1_ru`, `fdiv_v1_rz` | numerator one less |
| 2 | `fdiv_v2_rn`, `fdiv_v2_rd`, `fdiv_v2_ru`, `fdiv_v2_rz` | numerator one more |
| 3 | `fdiv_v3_rn`, `fdiv_v3_rd`, `fdiv_v3_ru`, `fdiv_v3_rz` | denominator one less |
| 4 | `fdiv_v4_rn`, `fdiv_v4_rd`, `fdiv_v4_ru`, `fdiv_v4_rz` | denominator one more |
| 5 | `fdiv_v5_rn`, `fdiv_v5_rd`, `fdiv_v5_ru`, `fdiv_v5_rz` | reversed erratum operands |
| 6 | `fdiv_v6_rn`, `fdiv_v6_rd`, `fdiv_v6_ru`, `fdiv_v6_rz` | 123,456,789 / 98,765,431 |
| 7 | `fdiv_v7_rn`, `fdiv_v7_rd`, `fdiv_v7_ru`, `fdiv_v7_rz` | 2,147,483,647 / 2,147,483,646 |
| 8 | `fdiv_v8_rn`, `fdiv_v8_rd`, `fdiv_v8_ru`, `fdiv_v8_rz` | 19,088,743 / 124,076,833 |
| 9 | `fdiv_v9_rn`, `fdiv_v9_rd`, `fdiv_v9_ru`, `fdiv_v9_rz` | 35 / 8 |
| 10 | `fdiv_v10_rn`, `fdiv_v10_rd`, `fdiv_v10_ru`, `fdiv_v10_rz` | negative numerator |
| 11 | `fdiv_v11_rn`, `fdiv_v11_rd`, `fdiv_v11_ru`, `fdiv_v11_rz` | negative denominator |
| 12 | `fdiv_v12_rn`, `fdiv_v12_rd`, `fdiv_v12_ru`, `fdiv_v12_rz` | both operands negative |
| 13 | `fdiv_v13_rn`, `fdiv_v13_rd`, `fdiv_v13_ru`, `fdiv_v13_rz` | erratum ratio at high normal exponents |
| 14 | `fdiv_v14_rn`, `fdiv_v14_rd`, `fdiv_v14_ru`, `fdiv_v14_rz` | erratum ratio at low normal exponents |

The next rows require FPU. Each record performs 2,048 cases at default scale;
FPREM truncates the quotient and FPREM1 uses nearest-even. `ops` counts cases
**plus actual FPREM instruction steps**, so it varies by vector/implementation
and is not the case count. Each case is bounded to 2,048 steps. Negative
quotient bits are diagnostic only. The classification records each execute
8,192 checked single-instruction cases and count one operation per case.

| Record names | Operand / classification |
|---|---|
| `fprem_v0`, `fprem1_v0` | 11/2 |
| `fprem_v1`, `fprem1_v1` | 7/2 tie |
| `fprem_v2`, `fprem1_v2` | 5/2 tie |
| `fprem_v3`, `fprem1_v3` | -11/2 |
| `fprem_v4`, `fprem1_v4` | 11/-2 |
| `fprem_v5`, `fprem1_v5` | -14/7 signed zero |
| `fprem_v6`, `fprem1_v6` | negative zero / 3 |
| `fprem_v7`, `fprem1_v7` | maximum extended / 7 |
| `fprem_v8`, `fprem1_v8` | maximum extended / 11 |
| `fprem_v9`, `fprem1_v9` | 2^1000 / 3 |
| `fprem_v10`, `fprem1_v10` | maximum extended / minimum subnormal |
| `fprem_denormal` | FPREM denormal / one |
| `fprem1_denormal` | FPREM1 denormal / one |
| `fprem_finite_inf` | FPREM finite / infinity |
| `fprem1_inf_invalid` | FPREM1 infinity / finite invalid result/status |

The 3DNow! and Cyrix EMMI aggregate descriptions, default case counts, gate
and reference limitations are in the vendor section above. The eight 3DNow!
records require FPU + MMX + 3DNOW; the enhanced three additionally require
3DNOWEXT. `ops` counts the checked packed cases/sequences, not lanes. The two
aggregate EMMI records and all twelve isolated records require MMX + EMMI;
all set `op_unit=emmi_instruction_case`. Each isolated record
checks its named opcode using the aggregate's assertion forms; the total is
one packed instruction case per tested vector/form, not one scalar lane.
At default scale, PDISTIB, PMACHRIW and the four moves each have 2,048
cases (memory-only forms); the other six each have 4,096 cases (register and
memory forms). Both even and odd specified registers are exercised.

| Isolated EMMI record | Operation checked |
|---|---|
| `cyrix_emmi_paveb` | PAVEB byte average. |
| `cyrix_emmi_paddsiw` | PADDSIW saturated signed word addition. |
| `cyrix_emmi_pmagw` | PMAGW word magnitudes; equal/opposite ties excluded. |
| `cyrix_emmi_pdistib` | PDISTIB byte-distance accumulation. |
| `cyrix_emmi_psubsiw` | PSUBSIW saturated signed word subtraction. |
| `cyrix_emmi_pmulhrwc` | PMULHRWC rounded word products. |
| `cyrix_emmi_pmulhriw` | PMULHRIW rounded word products. |
| `cyrix_emmi_pmachriw` | PMACHRIW multiply-accumulate rounded word products. |
| `cyrix_emmi_pmvzb` | PMVZB zero-byte conditional move. |
| `cyrix_emmi_pmvnzb` | PMVNZB nonzero-byte conditional move. |
| `cyrix_emmi_pmvlzb` | PMVLZB negative-byte conditional move. |
| `cyrix_emmi_pmvgezb` | PMVGEZB nonnegative-byte conditional move. |

| Record | Requirements | Checked work / limitations |
|---|---|---|
| `cyrix_cpuid_control` | — | Two CCR4 CPUID disabled/enabled states, ID flag and #UD/vendor checks; documented Cyrix models only, with CCR3/4 restored. Unit `cpuid_control_state`. |
| `cyrix_smm_disabled` | — | Model-qualified disabled-SMAC opcodes must #UD without changing operand bytes; unit `smm_disabled_opcode_case`, count varies by model. No SMM entry; locked state can skip. |
| `mediagx_cpuid` | CPUID | Six GXm CPUID leaf cases checking documented fields; model-gated, reserved fields unasserted. Unit `cpuid_leaf_case`. |

### Protected mode, privilege and exceptions

These records check selected architectural paths, not all descriptors,
privilege combinations or interrupt orderings. `ops` is generic counted work;
some legacy setup/probe records do not increment it. Heavy tests use owned
arena memory and restore temporary descriptor/interrupt state.

| Record | Requirements | Checked work / limitations |
|---|---|---|
| `pm_exceptions` | 486 | Selected processor exceptions, vectors and saved state. |
| `pm_alignment_check` | 486 + AC | Ring-3 unaligned access must produce #AC with defined frame state. |
| `pm_segments` | 486 | Segment descriptors, limits, access rights and interrupt transitions. |
| `pm_gates_rings` | 486 | Ring transitions and interrupt/trap gate privilege and presence checks. |
| `pm_task_switch` | 486 | Hardware task-switch and return state; three counted transitions. |
| `pm_v86` | 486 | Selected virtual-8086 transitions and instruction traps. |
| `pm_sysenter` | SEP + MSR | SYSENTER/SYSEXIT state and privilege transitions using restored MSRs; additional runtime checks apply. |
| `pm_syscall` | SYSCALL + MSR | AMD-style SYSCALL/SYSRET transitions and state; two counted operations. |
| `pm_irq_nesting` | 486 | Nested interrupt/handler state and repeated software interrupt paths. |
| `pm_debug_regs` | 486 | Debug-register, single-step and breakpoint traps; counted delivered traps. |
| `heavy_pm_code_context` | 486 | 32,768 far calls alternating descriptor default operand size at identical code location. |
| `heavy_pm_16bit` | 486 | 16-bit code/stack/address-size operations and near/far calls; aggregate instruction/transition count. |
| `heavy_pm_ldt` | 486 | LDT/TI descriptors, segment use, limits and selected faults; aggregate operations. |
| `heavy_pm_expand_down` | 486 | Expand-down segment and stack boundaries, legal accesses and faults. |
| `heavy_pm_call_gates` | 486 | Ring-3 call gates, parameter copying and RETF; 16,384 transitions at default scale, `ops` also counts parameter/return work. |
| `heavy_pm_privilege` | 486 | Selected privileged instruction, gate and descriptor faults; aggregate operations. |
| `heavy_pm_pvi` | VME | Protected-mode virtual interrupt flags and VIP faults; runtime CR4 support needed. |
| `heavy_pm_vme` | VME | Virtual-8086 virtual flags and interrupt redirection/faults; runtime CR4 support needed. |
| `heavy_pm_irq_shadows` | 486 | STI/IRET pending IRQ samples and MOV SS/POP SS debug-trap shadows, not all asynchronous IRQ ordering. |

Each exception-priority record requires PAGING + 486 and executes 16,384
cases at default scale, counted one per case. The tests compare defined fault
vectors/state and restore mappings and hooks; they do **not** assert #AC versus
#PF ordering for the same data access.

| Record | Checked case |
|---|---|
| `priority_segment_page` | Segment-limit #GP precedes operand page access. |
| `priority_alignment_page` | Mapped ring-3 misaligned #AC and aligned page-fault controls. |
| `priority_privileged_page` | LGDT/LIDT privilege #GP precedes operand access. |
| `priority_lock_decode_page` | Invalid LOCK MOV #UD precedes data access. |
| `priority_page_permissions` | Nonpresent and protection page-fault error bits. |
| `priority_fetch_operand` | Instruction-fetch #PF before execution's operand access. |

Each complex restart record requires PAGING + 486 and performs 32,768 cases
at default scale. Reported `ops` counts the relevant instructions per case,
not faults: two for PUSHA/POPA and ENTER/LEAVE, three for far CALL/RETF,
one for IRET. Ring-3 stacks and a safe ring-0 TSS stack permit fault recovery;
checks do not require rollback of intermediate stores or unspecified
fault-time registers.

| Record | Checked restart path |
|---|---|
| `restart_pusha_popa` | Mixed 16/32-bit PUSHA/POPA across page boundaries. |
| `restart_enter_leave` | Mixed 16/32-bit ENTER/LEAVE across page boundaries. |
| `restart_far_call_retf` | Same-privilege far CALL/RETF across stack pages. |
| `restart_iret` | Same-ring IRET across stack pages. |

### Paging, memory and sweeps

| Record | Requirements | Checked work / limitations |
|---|---|---|
| `pg_map_alias` | PAGING + 486 | Page mappings, remapping and alias coherence; 2,048 counted operations. |
| `pg_faults` | PAGING + 486 | Access permissions, demand faults and REP restart; counted demand faults, not all memory accesses. |
| `pg_accessed_dirty` | PAGING + 486 | Page accessed/dirty behavior; accessed bit on a fault is not required. |
| `pg_tlb_stress` | PAGING + 486 | 65,536 checked accesses across virtual/physical page mappings; includes TLB stress. |
| `pg_global` | PAGING + PGE + P5 | Global-page behavior through CR3 reload; actual retention is optional. |
| `pg_pse4m` | PAGING + 486 + PSE + MEM8M | 4 MiB mappings/permissions; high physical addresses above 4 GiB are not read. |
| `pg_pae` | PAGING + 486 + PAE + MEM8M | PAE mappings, 2 MiB pages, aliases, A/D and faults; reserved PDPTE behavior is partly observational. |
| `pg_pse36_permissions` | PAGING + PSE + PSE36 | Two high-address-bit/permission precedence cases without reading unowned physical memory. |
| `pg_apic_mmio` | PAGING + APIC + MSR | Read-only LAPIC MMIO through paging; disabled, relocated, x2APIC or unreadable configurations skip. |
| `heavy_pg_smc_alias` | PAGING + 486 | Generated code modified through physical aliases and remapped execution pages; counted guest instructions. |
| `heavy_pg_block_links` | PAGING + 486 | Direct/indirect linked generated blocks and expected results; counts executed blocks. |
| `heavy_pg_split_fetch` | PAGING + 486 | Cross-page instruction fetch faults and restart state; counts fault cases. |
| `heavy_pg_split_data` | PAGING + 486 | Cross-page data faults and restart state; counts fault cases. |
| `heavy_pg_rep_restart` | PAGING + 486 | Partially completed forward/backward REP MOVSD and preserved state; counts MOVSD elements. |
| `heavy_pg_stack_restart` | PAGING + 486 | Split-stack PUSH/POP fault and restart; counts fault cases. |
| `mem_walk` | PAGING | Walking bits, address alias and five patterns over up to 2 MiB owned RAM; `ops` counts pattern reads/writes. |
| `mem_bandwidth` | PAGING | Read/write, REP copy/fill, pointer chase and strided work at multiple owned-RAM sizes; generic count combines accesses; individual notes include loop overhead. |
| `mem_unaligned` | PAGING | Checked 1/2/4-byte accesses near ordinary and page boundaries; 2,304 aggregate operations. |
| `mem_lock` | PAGING + 486 | Locked arithmetic, XCHG/CMPXCHG, optional CX8 and INVLPG; 5,120 aggregate operations, not a multiprocessor test. |
| `mem_cache_ctl` | PAGING + 486 + NOHV | Guarded WBINVD and bounded CR0.CD/NW windows with data and restoration checks; no explicit `t_ops` count. |
| `heavy_decode_align` | PAGING + 486 | Prefixed variable-length code at 67 offsets and both branch paths; 1,852,416 counted instructions. |
| `heavy_decode_worksets` | PAGING + 486 | Rewrite versus reuse of generated code over 4 KiB–4 MiB footprints; up to 1,245,168 counted instructions with sufficient owned RAM. |
| `heavy_mem_align` | PAGING + 486 | 1/2/4/8-byte access checks across 67 offsets and up to 13 footprints; up to 1,363,584 counted operations; arena limits sizes. |

### System, platform and sustained interactions

Platform records are bounded probes or observations, not comprehensive
chipset tests. Several `cs_*` and `sys_*` records have no explicit `t_ops`
count; zero `ops` must not be interpreted as no work. Timed waits, polling,
setup, result checks and any diagnostic output belong to the checked
invocation. Device/firmware and available memory may also cause skips.

| Record | Requirements | Checked work / limitations |
|---|---|---|
| `cs_pic` | — | PIC mask readback and RTC cascade delivery observation; RTC/platform state can skip. |
| `cs_pit` | — | PIT channel-0 latch and channel-2 mode/edge observations; many timing details are diagnostic rather than assertions. |
| `cs_cmos_rtc` | — | CMOS snapshot and RTC second progression with a bounded 2.5 s wait; vendor-dependent checksum is diagnostic. |
| `cs_kbc_a20` | — | KBC/port observations and A20-on alias check using owned RAM; does not disable A20. |
| `cs_dma` | — | Read-only DMA register observations; no transfer, status reads clear terminal-count flags. |
| `cs_shadow_rom` | — | Read/hash ROM/shadow ranges and report firmware data; no writes to unknown mappings. |
| `cs_pci` | PCI | Enumerates selected buses and checks config read consistency/widths; BARs are not sized. |
| `cs_apic` | APIC + MSR | Guarded APIC base and read-only LAPIC registers; skips nonstandard/disabled/x2APIC. |
| `cs_misc_ports` | — | Port observations and conditional COM2 scratch round-trip; no LPT output writes. |
| `sys_tsc` | TSC | 4,096 monotonic RDTSC reads plus PIT calibration and optional ring-3 CR4.TSD check. |
| `sys_msr_probe` | MSR | Selected model/feature-qualified MSR reads; one generic operation, many reads diagnostic or allowed to fault. |
| `sys_msr_rw` | MSR + P6 + NOHV | Guarded selected MSR read/write/restore, including SYSENTER and unchanged PAT where applicable; not all MSRs. |
| `sys_cr_regs` | 486 | CR0/2/3 and supported CR4 bits, selected debug-register checks and restoration; no explicit operation count. |
| `sys_rdpmc` | P5 | Guarded performance counter reads and ring-3 restriction; skips unavailable/trapped counters. |
| `sys_msr_tsc` | MSR + TSC | One TSC MSR read bracketed by RDTSC. |
| `sys_mtrr_state` | MTRR + MSR | Read and validate MTRR register inventory; `ops` is variable MTRR count + 1, not cache effects. |
| `sys_pat_state` | PAT + MSR | Eight PAT types and unchanged write/readback; no cache-type effects. |
| `sys_mce_control` | MCE | One CR4.MCE toggle/restore check; no injected machine check. |
| `sys_mca_state` | MSR | MCA capability and bank inventory; additionally requires MCA at runtime, `ops` is bank count + 1, no error injection. |
| `sys_debug_extensions` | DE | Two DR4/DR5 #UD checks with CR4.DE enabled/restored. |
| `sys_cpuid_leaves` | CPUID | CPUID leaf inventory and repeated consistency checks; 32 generic operations, not exhaustive leaf semantics. |
| `sys_irq_timer` | — | Approximately 500 ms IRQ0 delivery against PIT/TSC; `ops` counts observed ticks. |
| `heavy_mix_transitions` | PAGING + 486 | Seeded integer/memory, page remaps, optional x87/MMX and interrupts; aggregate operation count depends on available features. |
| `heavy_state_interrupt` | 486 | 32,768 software interrupt deliveries and checked register/optional FPU/MMX state; `ops` counts mixed state work. |
| `heavy_x87_save_restore` | 486 + FPU | 65,536 FNSAVE/FRSTOR iterations at default scale; `ops` counts 18 operations per iteration. |
| `heavy_fx_save_restore` | 486 + FXSR + FPU | 65,536 FXSAVE/FXRSTOR iterations at default scale; `ops` counts 40 operations per iteration. |
| `heavy_apic_periodic` | PAGING + 486 + APIC | Periodic LAPIC timer with PIT deadline and EOI; at least 2,048 deliveries over at least two seconds; count varies, `ops` is deliveries + 5. |
| `heavy_apic_oneshot` | PAGING + 486 + APIC | Rearm and check 1,024 one-shot LAPIC deliveries; `ops` is deliveries + 5. |
| `burn_mixed` | PAGING + 486 | Duration-bounded checked integer/CRC, optional x87/MMX, copy, optional page remaps, software interrupts and call/return; runs final round only. `ops` is a synthetic mixed estimate per iteration, not bytes or instructions; arena and watchdog affect work/duration. |
