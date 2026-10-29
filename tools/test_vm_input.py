import configparser
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from run_86box import arguments, emulator_binary, inplace_vm, run_existing_vm, vm_config


CONFIG = """; preserve this comment
[Machine]
machine = vpc2007
cpu_family = pentium2_deschutes
cpu_speed = 333333333
cpu_multi = 5
mem_size = 65536
cpu_use_dynarec = 0
fpu_type = internal
fpu_softfloat = 1
time_sync = local

[Video]
gfxcard = cl_gd5480_pci

[Hard disks]
hdd_01_fn = ../disks/private.vhd

[Floppy and CD-ROM drives]
fdd_01_fn = original.img
fdd_01_type = 525_2hd
fdd_01_writeprot = 0

[Network]
net_01_card = ne2000

[Ports (COM & LPT)]
serial1_device = passthrough
"""


class VMInputTests(unittest.TestCase):
    def options(self, source, **overrides):
        values = dict(vm=source, output_dir=None, memory_mb=None, dynarec=None,
                      fpu_backend=None, prepare_only=False)
        values.update(overrides)
        return SimpleNamespace(**values)

    def test_inplace_preserves_devices_and_restores_exact_bytes(self):
        for direct_file in (False, True):
            with self.subTest(direct_file=direct_file), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / ("custom.cfg" if direct_file else "86box.cfg")
                source.write_text(CONFIG)
                (root / "nvr").mkdir()
                (root / "nvr/rtc").write_bytes(b"RTC")
                original = source.read_bytes()
                with inplace_vm(self.options(source if direct_file else root),
                                root / "test.img") as (actual, machine, digest):
                    self.assertEqual(actual, source)
                    self.assertEqual(machine["mem_size"], "65536")
                    self.assertEqual(machine["cpu_use_dynarec"], "0")
                    self.assertEqual(machine["time_sync"], "local")
                    config = configparser.ConfigParser()
                    config.read(source)
                    self.assertEqual(config["Hard disks"]["hdd_01_fn"], "../disks/private.vhd")
                    self.assertEqual(config["Network"]["net_01_card"], "ne2000")
                    self.assertEqual(config["Video"]["gfxcard"], "cl_gd5480_pci")
                    self.assertEqual(config["Ports (COM & LPT)"]["serial1_device"], "automation")
                    self.assertEqual(config["Floppy and CD-ROM drives"]["fdd_01_fn"],
                                     str(root / "test.img"))
                    self.assertEqual(config["Floppy and CD-ROM drives"]["fdd_01_writeprot"], "1")
                    (root / "nvr/rtc").write_bytes(b"updated RTC")
                self.assertEqual(source.read_bytes(), original)
                self.assertEqual((root / "nvr/rtc").read_bytes(), b"updated RTC")
                self.assertFalse((root / "source.cfg").exists())

    def test_restore_after_exception_and_interrupt(self):
        for error in (RuntimeError("test"), KeyboardInterrupt()):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "86box.cfg"
                source.write_text(CONFIG)
                with self.assertRaises(type(error)):
                    with inplace_vm(self.options(root, dynarec="on"), root / "test.img"):
                        raise error
                self.assertEqual(source.read_text(), CONFIG)

    def test_output_defaults_and_override(self):
        for override in (False, True):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "86box.cfg"
                source.write_text(CONFIG)
                image = root / "test.img"
                image.write_bytes(b"image")
                output = root / "results" if override else root
                args = self.options(root, output_dir=output if override else None)
                def execute(options, run_dir, command, machine):
                    self.assertEqual(run_dir, output)
                    self.assertEqual(command[command.index("--vmpath") + 1], str(root))
                    self.assertEqual(command[-1], str(source))
                    self.assertNotIn("--global", command)
                    self.assertEqual(machine["cpu_use_dynarec"], "0")
                    (root / "nvr-touched").write_text("no copy")
                    return dict(status="observed", directory=str(output),
                                emulated_ms=100, matched=[])
                with patch("run_86box.execute", side_effect=execute):
                    self.assertEqual(run_existing_vm(args, image, image, root), 0)
                self.assertEqual(source.read_text(), CONFIG)
                self.assertTrue((output / "result.json").exists())
                self.assertFalse((output / "floppy.img").exists())
                with self.assertRaisesRegex(RuntimeError, "output already exists"):
                    run_existing_vm(args, image, image, root)

    def test_missing_config(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "not found"):
                vm_config(Path(directory))

    def test_explicit_source_and_overrides_cli(self):
        with patch("sys.argv", ["run_86box.py", "--vm", "/some VM", "--dynarec", "off"]):
            args = arguments()
        self.assertEqual(args.vm, Path("/some VM"))
        self.assertIsNone(args.profile)
        self.assertIsNone(args.memory_mb)
        self.assertEqual(args.dynarec, "off")
        self.assertEqual(args.emulator, "86Box")

    def test_emulator_path_lookup_and_explicit_path(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "86Box"
            binary.write_text("#!/bin/sh\nexit 0\n")
            binary.chmod(0o700)
            with patch.dict("os.environ", {"PATH": directory}):
                self.assertEqual(emulator_binary("86Box"), binary.resolve())
                self.assertEqual(emulator_binary(str(binary)), binary.resolve())
                with self.assertRaisesRegex(RuntimeError, "--emulator"):
                    emulator_binary("missing")


if __name__ == "__main__":
    unittest.main()
