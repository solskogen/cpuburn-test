# 0.1.0 preview

First public-release candidate of the bare-metal i486-and-newer CPU/FPU
correctness and benchmark-data suite for 86Box. This is a preview, not an
exhaustive x86 conformance certification.

The default image selects 423 workload records: one round, scale 8,
TEST_SECONDS=1 and BURN_SECONDS=10. Optional features produce explicit SKIP
records. The suite preserves failures during warm-up and measured execution,
reports coarse guest runtime and first-failure context, and leaves comparison
and regression policy to its users.

Coverage includes integer/defined flags, x87, MMX, protected-mode exceptions,
paging/restarts, translated-code boundaries and self-modification, AMD
baseline/enhanced 3DNow!, and Cyrix EMMI. See README.md for exact workloads,
feature gates, references and limitations.

CPU-aware execution adds independently sourced minimum contracts for all
67 active 486+ families and two optional K5 families, guarded discovery,
mandatory feature witnesses and explicit sampled/untested coverage records.
The source-bound matrix generator selects compatible machines, FPU variants,
both runtime engines and optionally both FPU backends.
`make BENCHMARKS=0` builds an accuracy-only image without timed harness
repetition; benchmark-enabled builds remain the default. See
[CPU contracts and coverage](doc/cpu-contracts.md) for commands and boundaries.

The floppy now relocates the runtime to 1 MiB after validating usable extended
RAM, including BSS. Test allocations begin after the runtime; low-memory V86
fixtures and the main stack retain their addresses. The 1.44 MB floppy format
is unchanged. The loader reads the runtime one track at a time through a
low bounce buffer, so the runtime may use the whole floppy.

The optional 86Box runner accepts `--vm` with a VM directory or a direct
configuration path, finds `86Box` on PATH by default, and preserves CPU,
RAM, FPU and engine settings unless explicitly overridden. `--dynarec on|off`
selects an engine for the run. The supplied VM runs in place, without copying
its configuration, NVRAM or media. Floppy/COM1 settings and explicit overrides
are temporary; the original config is restored after exit. NVRAM, disks and
host backends remain live, not rolled back. Logs default to the VM directory;
`--output-dir` selects another location. Generated profiles and
matrix configs remain available through explicit `--profile`/`--config`.

Samples stop once the measured speed settles (a 95% interval and the
first-half/second-half drift both within 2%, or 5% for records above 150%),
timed on the emulation thread's CPU time; each record reports whether the
host delayed 86Box (`contended`). `tools/shard_86box.py` splits a run across
pinned cores and merges the results, and `make QUICK=1` checks costly,
redundant records once instead of sampling them. See
[Running](doc/running.md#faster-runs).

## Known results and limitations

- Default image on TestVM (Pentium II Deschutes 333, Virtual PC 2007,
  dynarec, host FPU backend): 423 records, 312 PASS, 74 FAIL, 37 SKIP,
  0 FAULT. Most failures are x87 checks that 86Box's default host-double
  FPU backend cannot pass; with softfloat five non-FPU failures remain.
  See [known results](README.md#known-results) for the list and the
  86Box settings and fixes that clear them. Failures are findings, not
  accepted baselines.
- MediaGX MMX (`mediagxm`) runs on the ECS P5GX-M source-bound matrix.
  The CPU identity contract, GXm CPUID, Cyrix CPUID controls, disabled-SMM
  checks and the EMMI records pass in both engines. Earlier full runs
  faulted in `fpu_stack` and `fpu_p6`; this has not been rechecked with the
  current image.
- Baseline 3DNow! workloads pass on 86Box K6-2 in both engines. K6-2+
  also passes enhanced horizontal arithmetic and PSWAPD; its PI2FW/PF2IW
  lane/sign-extension checks fail in both engines, consistent with AMD
  22466D (see [results](doc/results.md)).
- All twelve EMMI opcodes pass the selected 6x86MX cases in both engines.
  Intel MMX, 486DX and 486SX correctly skip unavailable vendor features.
- Host-side throughput numbers require matched builds/configurations and
  sufficient samples. Short correctness runs are not precision benchmarks.
- The floppy is host-independent; the optional 86Box automation/profiling
  runner requires Linux, Xvfb, local ROMs and a compatible AUTOMATION-enabled
  emulator. ROMs and emulator binaries are not included.
- No fresh physical AMD/Cyrix CPU, Raspberry Pi host, native ARM64/LoongArch
  host execution, Windows/macOS runner or full current-image KVM validation is claimed.
  Real hardware and other hypervisors are not certified.
- SSE/SSE2, later SIMD, long mode, SMP, SMM entry/exit and exhaustive model-specific
  MSR behavior are outside this preview's checked coverage.

## Local release artifacts

`make release` creates `dist/cpuburn-test-0.1.0/` with the bootable image,
BUILD.txt, a source archive, documentation, MIT license and SHA256SUMS.
BUILD.txt records release settings and actual compiler/tool versions;
SHA256SUMS identifies the packaged image and sources. The guest banner
identifies version and compiler. Build and packaging steps are ordinary
Makefile recipes, without helper scripts or generated provenance headers.
Run `make clean` when changing compilers, flags or test settings.

No artifact or repository is uploaded automatically. Verify checksums with
`sha256sum -c SHA256SUMS` inside the release directory.

## aarch64 build hosts

The image defaults to Clang on aarch64 Linux with plain `make`
(Clang's explicit i386 target plus ld.lld/llvm-objcopy), or a GNU i386/i686
cross-toolchain using `make CROSS_COMPILE=i686-linux-gnu-`.
NASM, make and GNU utilities run natively on the build host.
Python is only needed for optional tests and host runner/reporting tools,
not image compilation or release packaging. No x86 sysroot/libc or
guest emulator is needed for compilation. Clang is the checked path; the
GNU cross-toolchain option is not tested, and no native-ARM build has been
validated.
