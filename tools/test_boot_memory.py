"""Exercise the actual bootstrap RAM validator without a BIOS or emulator."""
from pathlib import Path
import os
import platform
import shutil
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == "Linux" and
                     platform.machine().lower() in ("x86_64", "amd64", "i386", "i686"),
                     "requires Linux with i386 execution")
class BootMemoryTests(unittest.TestCase):
    def test_reserved_ranges_and_legacy_memory_sizes(self):
        if not shutil.which("nasm") or not shutil.which("gcc"):
            self.skipTest("requires NASM and GCC")
        assembly = '''
bits 32
section .text
global boot_probe
boot_probe:
    mov eax, [esp + 4]
    push ebx
    mov ebx, [esp + 12]
    call boot_ram_available
    pop ebx
    ret
%include "boot_memory.asm"
section .note.GNU-stack noalloc noexec nowrite progbits
'''
        source = r'''
typedef unsigned int u32;
typedef unsigned long long u64;
extern int boot_probe(u32 end, const u32 *info);
static u32 info[8 + 32 * 6], failures;
static void reset(void) {
    for (u32 i = 0; i < sizeof info / sizeof info[0]; i++) info[i] = 0;
}
static void record(u32 n, u64 base, u64 length, u32 type, u32 enabled) {
    u32 *r = info + 8 + n * 6;
    r[0] = base; r[1] = base >> 32;
    r[2] = length; r[3] = length >> 32;
    r[4] = type; r[5] = enabled;
    if (info[5] <= n) info[5] = n + 1;
}
static void check(u32 end, int want) {
    if (boot_probe(end, info) != want) failures++;
}
void _start(void) {
    reset(); check(0x148000, 0);
    info[2] = 288; check(0x148000, 1); check(0x148001, 0);
    info[2] = 0; info[3] = 288;
    check(0x148000, 1); check(0x148001, 0);
    info[2] = 8192; info[3] = 128; check(0x148000, 0);
    info[3] = 0xffffffffu;
    check(0x1000000, 1); check(0x1000001, 0);
    check(0x100000, 0); check(0xffffffffu, 0);
    reset(); record(0, 0x100000, 0x48000, 1, 1);
    check(0x148000, 1); check(0x148001, 0);
    info[8 + 5] = 0; check(0x148000, 0);
    reset(); record(0, 0, 0xa0000, 1, 1);
    info[2] = 65535; check(0x148000, 0); /* E820 overrides legacy size */
    reset();
    record(0, 0x140000, 0x8000, 1, 1);
    record(1, 0x120000, 0x20000, 1, 1);
    record(2, 0x100000, 0x20000, 1, 1);
    check(0x148000, 1); /* unsorted adjacent usable ranges */
    info[8 + 6]++; check(0x148000, 0); /* one-byte hole */
    reset();
    record(0, 0x100000, 0x700000, 1, 1);
    record(1, 0x147fff, 1, 2, 1);
    check(0x148000, 0); /* reservations win regardless of ordering */
    record(1, 0x148000, 1, 2, 1); check(0x148000, 1);
    record(1, 0xfffff, 1, 2, 1); check(0x148000, 1);
    record(1, 0xfffff, 2, 2, 1); check(0x148000, 0);
    record(1, 0x110000, 0x10000, 2, 0); check(0x148000, 1);
    record(1, 0x100000000ull, 0x100000, 2, 1); check(0x148000, 1);
    reset(); record(0, 0x100000, 0x100000000ull, 1, 1);
    check(0x148000, 1);
    reset(); record(0, 0x100000, 0xfff00000ull, 1, 1);
    check(0x148000, 1); /* exclusive range end at 4 GiB */
    record(1, 0x110000, 0x100000000ull, 2, 1); check(0x148000, 0);
    reset(); record(0, 0x100000, 0xffffffffffffffffull, 1, 1);
    check(0x148000, 0); /* malformed overflowing 64-bit range */
    info[5] = 33; check(0x148000, 0);
    __asm__ volatile("int $0x80" :: "a"(1), "b"(failures != 0) : "memory");
    __builtin_unreachable();
}
'''
        with tempfile.TemporaryDirectory(prefix="boot-memory-") as directory:
            path = Path(directory)
            (path / "probe.asm").write_text(assembly)
            (path / "probe.c").write_text(source)
            subprocess.run(["nasm", "-f", "elf32", "-I", str(ROOT) + "/",
                            str(path / "probe.asm"), "-o", str(path / "probe.o")],
                           check=True, capture_output=True)
            subprocess.run([
                "gcc", "-m32", "-march=i486", "-O2", "-Wall", "-Wextra", "-Werror",
                "-ffreestanding", "-fno-builtin", "-fno-pie", "-fno-stack-protector",
                "-nostdlib", "-static", str(path / "probe.c"), str(path / "probe.o"),
                "-o", str(path / "probe"),
            ], check=True, capture_output=True)
            subprocess.run([str(path / "probe")], check=True,
                           capture_output=True, timeout=10)


class BootLayoutTests(unittest.TestCase):
    def test_extended_bss_does_not_consume_floppy_staging(self):
        nasm = shutil.which("nasm")
        linker = next((shutil.which(tool) for tool in
                       ("i686-linux-gnu-ld", "ld.lld", "ld") if shutil.which(tool)), None)
        objcopy = next((shutil.which(tool) for tool in
                        ("llvm-objcopy", "i686-linux-gnu-objcopy", "objcopy")
                        if shutil.which(tool)), None)
        if not nasm or not linker or not objcopy:
            self.skipTest("requires NASM and i386 linker/objcopy")
        with tempfile.TemporaryDirectory(prefix="boot-layout-") as directory:
            path = Path(directory)
            source = path / "probe.asm"
            source.write_text(
                "bits 32\nsection .text.start\nglobal _start\n_start: hlt\n"
                "section .bss\nresb 524288\n"
                "section .note.GNU-stack noalloc noexec nowrite progbits\n")
            subprocess.run([nasm, "-f", "elf32", str(source), "-o", str(path / "probe.o")],
                           check=True, capture_output=True)
            subprocess.run([linker, "-m", "elf_i386", "-T", str(ROOT / "link.ld"),
                            str(path / "probe.o"), "-o", str(path / "probe.elf")],
                           check=True, capture_output=True)
            self.assertEqual(struct.unpack_from("<I", (path / "probe.elf").read_bytes(), 24)[0],
                             0x100000)
            subprocess.run([objcopy, "-O", "binary", str(path / "probe.elf"),
                            str(path / "probe.bin")], check=True, capture_output=True)
            self.assertLess((path / "probe.bin").stat().st_size, 4096)
            # The same file budget must still reject code larger than the staging area.
            source.write_text(
                "bits 32\nsection .text.start\nglobal _start\n"
                "_start: hlt\ntimes 393216 db 0\n"
                "section .note.GNU-stack noalloc noexec nowrite progbits\n")
            subprocess.run([nasm, "-f", "elf32", str(source), "-o", str(path / "probe.o")],
                           check=True, capture_output=True)
            result = subprocess.run(
                [linker, "-m", "elf_i386", "-T", str(ROOT / "link.ld"),
                 str(path / "probe.o"), "-o", str(path / "probe.elf")],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Kernel exceeds low-memory loader staging area", result.stderr)


@unittest.skipUnless(os.environ.get("CPUBURN_BOOT_IMAGE"),
                     "set CPUBURN_BOOT_IMAGE to a full BENCHMARKS=0 relocated image")
class BootGuestTests(unittest.TestCase):
    def test_relocated_runtime_executes_low_mode_and_paging_cases(self):
        from test_cpu_contract import CpuContractGuestTests
        self.image_variable = "CPUBURN_BOOT_IMAGE"
        run = CpuContractGuestTests.run_guest(self, "486", "@cpu1 idx4 internal accuracy\n")
        statuses = {test["name"]: test["status"] for test in run["tests"]}
        for name in ("cpu_model_contract", "core_fault_watchdog_recovery", "int_alu",
                     "int_smc", "pm_exceptions", "pm_v86", "heavy_pm_16bit",
                     "pg_map_alias", "pg_faults"):
            self.assertEqual(statuses.get(name), "PASS", name)
        # Other architectural failures are not accepted as golden results here.
        self.assertFalse(any(test["status"] == "FAULT" for test in run["tests"]))

    def test_missing_extended_ram_halts_before_kernel_entry(self):
        qemu = shutil.which("qemu-system-i386")
        if not qemu:
            self.fail("requested boot tests require qemu-system-i386")
        image = Path(os.environ["CPUBURN_BOOT_IMAGE"]).resolve(strict=True)
        with tempfile.TemporaryDirectory(prefix="boot-lowram-") as directory:
            root = Path(directory)
            debug, serial = root / "debug.log", root / "serial.log"
            with (root / "qemu.log").open("w+") as log:
                process = subprocess.Popen([
                    qemu, "-accel", "tcg", "-cpu", "486", "-m", "1",
                    "-drive", f"file={image},format=raw,if=floppy,readonly=on",
                    "-boot", "a", "-display", "none", "-monitor", "none",
                    "-debugcon", f"file:{debug}", "-serial", f"file:{serial}",
                    "-no-reboot", "-no-shutdown",
                ], stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        if debug.exists() and "runtime extended RAM unavailable" in debug.read_text():
                            break
                        if process.poll() is not None:
                            break
                        time.sleep(.05)
                    log.seek(0)
                    self.assertIn("runtime extended RAM unavailable",
                                  debug.read_text() if debug.exists() else "", log.read())
                    self.assertIsNone(process.poll(), "bootstrap must halt, not reset or exit")
                    self.assertNotIn("CPUBURN-TEST", serial.read_text())
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
