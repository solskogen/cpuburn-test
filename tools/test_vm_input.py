import configparser
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from run_86box import arguments, emulator_binary, vm_config, write_config


CONFIG = """[Machine]
machine = vpc2007
cpu_family = pentium2_deschutes
cpu_speed = 333333333
cpu_multi = 5
mem_size = 65536
cpu_use_dynarec = 0
fpu_type = internal
fpu_softfloat = 1
cpu_debug_regs_486 = 1
time_sync = local

[Video]
gfxcard = et4000ax

[Virtual PC 2007]
bios = vpc2007

[Hard disks]
hdd_01_fn = ../disks/private.vhd

[Floppy and CD-ROM drives]
fdd_01_fn = /missing/original.img
cdrom_01_image_path = ../private.iso

[Network]
net_01_card = ne2000
net_01_net_type = pcap

[Ports (COM & LPT)]
serial1_device = passthrough

[General]
gdbstub_enabled = 1
vid_renderer = opengl

[Dangerous passthrough]
path = /dev/sda
"""


class VMInputTests(unittest.TestCase):
    def options(self, source, **overrides):
        values = dict(vm=source, config=None, profile=None, memory_mb=None,
                      dynarec=None, fpu_backend=None)
        values.update(overrides)
        return SimpleNamespace(**values)

    def test_directory_and_file_preserve_hardware_and_private_nvram(self):
        for direct_file in (False, True):
            with self.subTest(direct_file=direct_file), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                vm = root / "VM with spaces"
                vm.mkdir()
                source = vm / ("custom.cfg" if direct_file else "86box.cfg")
                source.write_text(CONFIG)
                (vm / "nvr").mkdir()
                (vm / "nvr/rtc.nvr").write_bytes(b"original RTC")
                run = root / "run"
                run.mkdir()
                machine = write_config(self.options(source if direct_file else vm), run)
                self.assertEqual(machine["cpu_family"], "pentium2_deschutes")
                self.assertEqual(machine["cpu_speed"], "333333333")
                self.assertEqual(machine["mem_size"], "65536")
                self.assertEqual(machine["cpu_use_dynarec"], "0")
                self.assertEqual(machine["fpu_softfloat"], "1")
                self.assertEqual(machine["cpu_debug_regs_486"], "1")
                self.assertEqual(machine["time_sync"], "disabled")
                config = configparser.ConfigParser()
                config.read(run / "86box.cfg")
                self.assertEqual(config["Video"]["gfxcard"], "et4000ax")
                self.assertEqual(config["Virtual PC 2007"]["bios"], "vpc2007")
                self.assertNotIn("Hard disks", config)
                self.assertNotIn("Dangerous passthrough", config)
                self.assertNotIn("gdbstub_enabled", config["General"])
                self.assertEqual(config["Network"]["net_01_card"], "none")
                self.assertEqual(config["Storage controllers"]["hdc"], "none")
                self.assertEqual(config["Ports (COM & LPT)"]["serial1_device"], "automation")
                self.assertEqual(config["Floppy and CD-ROM drives"]["fdd_01_fn"],
                                 str(run / "floppy.img"))
                self.assertEqual((run / "source.cfg").read_text(), CONFIG)
                self.assertEqual((run / "nvr/rtc.nvr").read_bytes(), b"original RTC")
                (run / "nvr/rtc.nvr").write_bytes(b"modified RTC")
                self.assertEqual((vm / "nvr/rtc.nvr").read_bytes(), b"original RTC")
                self.assertEqual(source.read_text(), CONFIG)

    def test_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "custom.cfg"
            source.write_text(CONFIG)
            for mode in ("on", "off"):
                run = root / mode
                run.mkdir()
                machine = write_config(self.options(source, dynarec=mode,
                                                    memory_mb=32, fpu_backend="host"), run)
                self.assertEqual(machine["cpu_use_dynarec"], str(int(mode == "on")))
                self.assertEqual(machine["mem_size"], "32768")
                self.assertEqual(machine["fpu_softfloat"], "0")

    def test_missing_config_and_required_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "not found"):
                vm_config(root)
            for key in ("machine", "cpu_family", "cpu_speed", "mem_size"):
                source = root / "86box.cfg"
                source.write_text("\n".join(line for line in CONFIG.splitlines()
                                            if not line.startswith(key + " =")))
                with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, key):
                    write_config(self.options(root), root)

    def test_nvram_symlinks_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "86box.cfg").write_text(CONFIG)
            (root / "nvr").mkdir()
            (root / "nvr/external").symlink_to(root / "86box.cfg")
            run = root / "run"
            run.mkdir()
            with self.assertRaisesRegex(RuntimeError, "NVRAM entry"):
                write_config(self.options(root), run)

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
