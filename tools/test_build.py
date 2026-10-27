"""Makefile tool selection, local packaging and serial build records."""

import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

from perf_report import summarize
from qemu_smoke import parse_log
from test_perf_report import example

PROJECT_ROOT = Path(__file__).resolve().parents[1]


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("LICENSE", "README.md", "RELEASE_NOTES.md", ".gitignore",
                     "core.c", "arith.c", "kernel.h", "link.ld",
                     "start.asm", "rm.asm", "boot.asm"):
            (self.root / name).write_text(name + "\n")
        (self.root / "VERSION").write_text("0.1.0\n")
        shutil.copy2(PROJECT_ROOT / "Makefile", self.root / "Makefile")
        (self.root / "tools").mkdir()
        (self.root / "tools/test.py").write_text("# source\n")
        (self.root / ".github/workflows").mkdir(parents=True)
        (self.root / ".github/workflows/build.yml").write_text("name: test\n")

    def make(self, *args):
        env = {key: value for key, value in os.environ.items()
               if key not in ("CC", "LD", "OBJCOPY", "CROSS_COMPILE", "MAKEFLAGS", "MFLAGS")}
        return subprocess.run(["make", *args], cwd=self.root, env=env,
                              check=True, capture_output=True, text=True).stdout

    def test_clang_explicit_i386_target_and_llvm_tools(self):
        commands = self.make("-n", "-B", "CC=clang", "IS_CLANG=1")
        self.assertIn("clang --target=i386-unknown-linux-gnu -m32 -march=i486", commands)
        self.assertIn("ld.lld -m elf_i386", commands)
        self.assertIn("llvm-objcopy -O binary", commands)

    def test_arm_hosts_default_to_clang(self):
        for arch in ("aarch64", "arm64"):
            with self.subTest(arch=arch):
                commands = self.make("-n", "-B", f"HOST_ARCH={arch}")
                self.assertIn("clang --target=i386-unknown-linux-gnu -m32", commands)
                self.assertIn("ld.lld -m elf_i386", commands)
                self.assertIn("llvm-objcopy -O binary", commands)

    def test_x86_hosts_still_default_to_gcc(self):
        commands = self.make("-n", "-B", "HOST_ARCH=x86_64")
        self.assertIn("gcc -m32", commands)
        self.assertIn("ld -m elf_i386", commands)

    def test_arm_default_preserves_compiler_and_cross_prefix_overrides(self):
        commands = self.make("-n", "-B", "HOST_ARCH=aarch64",
                             "CC=custom-gcc", "IS_CLANG=")
        self.assertIn("custom-gcc -m32", commands)
        commands = self.make("-n", "-B", "HOST_ARCH=aarch64",
                             "CROSS_COMPILE=i686-linux-gnu-", "IS_CLANG=")
        self.assertIn("i686-linux-gnu-gcc -m32", commands)
        self.assertIn("i686-linux-gnu-ld -m elf_i386", commands)

    def test_gnu_cross_prefix_overrides_make_builtin_defaults(self):
        commands = self.make("-n", "-B", "CROSS_COMPILE=i686-linux-gnu-", "IS_CLANG=")
        self.assertIn("i686-linux-gnu-gcc -m32", commands)
        self.assertIn("i686-linux-gnu-ld -m elf_i386", commands)
        self.assertIn("i686-linux-gnu-objcopy -O binary", commands)

    def test_explicit_cross_tools_take_precedence(self):
        commands = self.make("-n", "-B", "CROSS_COMPILE=i686-elf-", "CC=custom-gcc",
                             "LD=custom-ld", "OBJCOPY=custom-objcopy", "IS_CLANG=")
        self.assertIn("custom-gcc -m32", commands)
        self.assertIn("custom-ld -m elf_i386", commands)
        self.assertIn("custom-objcopy -O binary", commands)

    def test_build_commands_have_no_helper_or_python_dependency(self):
        commands = self.make("-n", "-B", "CC=gcc", "IS_CLANG=", "PYTHON=false", "release")
        self.assertIn("gcc -m32", commands)
        self.assertIn("tar --format=gnu", commands)
        self.assertNotIn("build_release", commands)
        self.assertNotIn("build_info", commands)
        self.assertNotIn("python", commands.lower())
        self.assertNotIn("false", commands)

    def test_clean_removes_all_generated_outputs(self):
        (self.root / "build").mkdir()
        (self.root / "build/kernel.o").write_bytes(b"object")
        (self.root / "floppy.img").write_bytes(b"image")
        (self.root / "dist/release").mkdir(parents=True)
        (self.root / "dist/release/floppy.img").write_bytes(b"packaged image")
        self.make("clean")
        for name in ("build", "floppy.img", "dist"):
            self.assertFalse((self.root / name).exists())
        self.assertTrue((self.root / "core.c").exists())

    def test_package_checksums_and_source_archive(self):
        (self.root / "floppy.img").write_bytes(b"fixture image")
        (self.root / "build").mkdir()
        (self.root / "build/secret.log").write_text("not source")
        args = ("-o", "floppy.img", "release", "CC=echo", "NASM=echo",
                "LD=echo", "OBJCOPY=echo", "IS_CLANG=", "PYTHON=false")
        self.make(*args)
        directory = self.root / "dist/cpuburn-test-0.1.0"
        checksums = (directory / "SHA256SUMS").read_bytes()
        for line in checksums.decode().splitlines():
            digest, name = line.split("  ", 1)
            self.assertEqual(hashlib.sha256((directory / name).read_bytes()).hexdigest(), digest)
        archive_path = directory / "cpuburn-test-0.1.0-source.tar.gz"
        contents = archive_path.read_bytes()
        with tarfile.open(fileobj=io.BytesIO(contents), mode="r:gz") as archive:
            names = archive.getnames()
            self.assertIn("cpuburn-test-0.1.0/tools/test.py", names)
            self.assertIn("cpuburn-test-0.1.0/.github/workflows/build.yml", names)
            self.assertNotIn("cpuburn-test-0.1.0/build/secret.log", names)
        self.make(*args)
        self.assertEqual(contents, archive_path.read_bytes())
        self.assertEqual(checksums, (directory / "SHA256SUMS").read_bytes())

    def test_serial_build_record_preserved(self):
        line = 'BUILD version=0.1.0 compiler="GCC 16"'
        parsed = parse_log(line + "\n")
        self.assertEqual(parsed["build_info"], [{
            "version": "0.1.0", "compiler": '"GCC 16"'}])
        run = example()
        run["build_info"] = parsed["build_info"]
        self.assertEqual(summarize([run], 0)["build_info"], parsed["build_info"])
        other = example()
        other["build_info"] = [{**parsed["build_info"][0], "version": "0.2.0"}]
        with self.assertRaisesRegex(ValueError, "mixed environments"):
            summarize([run, other], 0)


if __name__ == "__main__":
    unittest.main()
