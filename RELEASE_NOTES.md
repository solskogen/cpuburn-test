# 0.1.0 preview

First public-release candidate of the bare-metal i486-and-newer CPU/FPU
correctness and benchmark-data suite for 86Box. This is a preview, not an
exhaustive x86 conformance certification.

The default image selects 281 workload records: one round, scale 8,
TEST_SECONDS=5 and BURN_SECONDS=10. Optional features produce explicit SKIP
records. The suite preserves failures during warm-up and measured execution,
reports coarse guest runtime and first-failure context, and leaves comparison
and regression policy to its users.

Coverage includes integer/defined flags, x87, MMX, protected-mode exceptions,
paging/restarts, translated-code boundaries and self-modification, AMD
baseline/enhanced 3DNow!, and Cyrix EMMI. See README.md for exact workloads,
feature gates, references and limitations.

## Known results and limitations

- The 281-record default MMX/dynarec integration image completed with 187 PASS,
  72 FAIL, 22 SKIP and no FAULT. All 271 pre-vendor outcomes were unchanged;
  the ten vendor records skipped correctly. All original 266 outcomes were
  also unchanged by the boundary additions. These are observed emulator results, not expected
  results to substitute for architectural checks.
- Focused baseline 3DNow! workloads pass on 86Box K6-2 in both engines.
  K6-2+ additionally passes enhanced horizontal arithmetic and PSWAPD;
  PI2FW/PF2IW lane/sign-extension checks fail in both engines, consistent with
  the discrepancies described in README.md and AMD 22466D.
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
- SSE/SSE2, later SIMD, long mode, SMP, SMM and exhaustive model-specific
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
guest emulator is needed for compilation. CI contains both aarch64 build
paths; configured jobs do not constitute a completed native-ARM validation.
