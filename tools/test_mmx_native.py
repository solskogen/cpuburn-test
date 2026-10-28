"""Native Linux i386 check of the guest's MMX/x87 transition assertions."""
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == "Linux" and
                     platform.machine().lower() in ("x86_64", "amd64", "i386", "i686"),
                     "requires Linux with native i386 MMX/x87 execution")
class MmxNativeTests(unittest.TestCase):
    def test_guest_transition_on_hardware(self):
        source = r'''
#include "kernel.h"
static unsigned failures;
void t_fail(const char *what, int line) {
    (void)what; (void)line; failures++;
}
void t_failv(const char *what, int line, u32 got, u32 want) {
    (void)got; (void)want; t_fail(what, line);
}
void t_note(const char *fmt, ...) { (void)fmt; }
void t_ops(u64 n) { (void)n; }
#include "tests_mmx.c"
void _start(void) {
    mmx_x87_transitions();
    __asm__ volatile("int $0x80" :: "a"(1), "b"(failures != 0) : "memory");
    __builtin_unreachable();
}
'''
        compilers = [cc for cc in ("gcc", "clang") if shutil.which(cc)]
        if not compilers:
            self.skipTest("requires GCC or Clang")
        with tempfile.TemporaryDirectory(prefix="mmx-native-") as directory:
            path = Path(directory)
            fixture = path / "fixture.c"
            fixture.write_text(source)
            for cc in compilers:
                for optimization in ("-O2", "-O3"):
                    with self.subTest(compiler=cc, optimization=optimization):
                        binary = path / "fixture"
                        build = subprocess.run([
                            cc, optimization, "-Wall", "-Wextra", "-Werror",
                            "-m32", "-march=i486", "-ffreestanding",
                            "-fno-pie", "-fno-stack-protector",
                            "-nostdlib", "-static",
                            "-Wno-unused-function", "-mno-sse", "-mno-mmx",
                            "-msoft-float", "-fno-strict-aliasing",
                            "-ffunction-sections", "-fdata-sections",
                            "-DHEAVY_SCALE=8", "-DTEST_SEED=0x86b04a21u",
                            "-I", str(ROOT), str(fixture), "-Wl,--gc-sections",
                            "-o", str(binary),
                        ], capture_output=True, text=True)
                        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                        subprocess.run([str(binary)], check=True,
                                       capture_output=True, timeout=30)
