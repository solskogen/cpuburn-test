"""Run the actual CPUID feature decoder on synthetic vendor/model inputs."""
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == "Linux" and
                     platform.machine().lower() in ("x86_64", "amd64", "i386", "i686"),
                     "requires Linux with i386 execution")
class CyrixDecodeTests(unittest.TestCase):
    def test_vendor_specific_extended_feature_aliases(self):
        source = r'''
#include "kernel.h"
cpuinfo_t cpu;
int kmemcmp(const void *a, const void *b, u32 n) {
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
void kmemcpy(void *d, const void *s, u32 n) {
    u8 *x = d; const u8 *y = s;
    for (u32 i = 0; i < n; i++) x[i] = y[i];
}
void kprintf(const char *format, ...) { (void)format; }
#include "cpu_contract.c"
void _start(void) {
    u32 failed = 0;
    const u32 aliases = (1u << 16) | (1u << 24);
    kmemcpy(cpu.vendor, "CyrixInstead", 13);
    cpu.has_cpuid = 1; cpu.family = 5; cpu.model = 4;
    cpu.ext_edx = 0x01808131u | (1u << 16);
    cpu.has_emmi = 1;
    failed |= (cpu_extended_standard_edx() & aliases) != 0;
    failed |= (cpu_detected_features() & (REQ_FXSR | REQ_PAT)) != 0;
    failed |= !(cpu_detected_features() & REQ_EMMI);
    configured = 1;
    kmemcpy(configured_model, "mediagxm", sizeof "mediagxm");
    cpu_contract_init();
    failed |= !equal(cpu_model_name(), "mediagxm");
    failed |= cpu_raw_required(2) != 0x01808131u;
    failed |= (cpu_required_features() & (REQ_FXSR | REQ_PAT | REQ_DE | REQ_PSE)) != 0;
    failed |= (cpu_required_features() & (REQ_MMX | REQ_EMMI | REQ_CMOV)) !=
              (REQ_MMX | REQ_EMMI | REQ_CMOV);
    cpu.has_fxsr = 1; cpu.has_pat = 1;
    failed |= (cpu_detected_features() & (REQ_FXSR | REQ_PAT)) != (REQ_FXSR | REQ_PAT);
    cpu.has_fxsr = 0; cpu.has_pat = 0; cpu.has_emmi = 0;
    cpu.ext_edx = aliases;
    kmemcpy(cpu.vendor, "AuthenticAMD", 13);
    failed |= cpu_extended_standard_edx() != aliases;
    failed |= (cpu_detected_features() & (REQ_FXSR | REQ_PAT)) != (REQ_FXSR | REQ_PAT);
    kmemcpy(cpu.vendor, "GenuineIntel", 13);
    failed |= cpu_extended_standard_edx() != aliases;
    failed |= (cpu_detected_features() & (REQ_FXSR | REQ_PAT)) != (REQ_FXSR | REQ_PAT);
    __asm__ volatile("int $0x80" :: "a"(1), "b"(failed != 0) : "memory");
    __builtin_unreachable();
}
'''
        compilers = [cc for cc in ("gcc", "clang") if shutil.which(cc)]
        if not compilers:
            self.skipTest("requires GCC or Clang")
        with tempfile.TemporaryDirectory(prefix="cyrix-decode-") as directory:
            path = Path(directory)
            fixture = path / "fixture.c"
            fixture.write_text(source)
            for cc in compilers:
                for optimization in ("-O2", "-O3"):
                    with self.subTest(compiler=cc, optimization=optimization):
                        binary = path / "fixture"
                        build = subprocess.run([
                            cc, optimization, "-Wall", "-Wextra", "-Werror", "-m32",
                            "-march=i486", "-ffreestanding", "-fno-builtin",
                            "-fno-pie", "-fno-stack-protector", "-nostdlib", "-static",
                            "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
                            "-I", str(ROOT), str(fixture), "-Wl,--gc-sections",
                            "-o", str(binary),
                        ], capture_output=True, text=True)
                        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                        subprocess.run([str(binary)], check=True,
                                       capture_output=True, timeout=30)
