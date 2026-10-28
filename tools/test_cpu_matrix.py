"""Source inventory parsing and compatible CPU matrix generation."""
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

from cpu_matrix import eligible, fields, make_matrix, number, preprocess, run_matrix, split_c, table, write_configs
from qemu_smoke import parse_log
from run_86box import cpu_control_accepted, cpu_control_frame

ROOT = Path(__file__).resolve().parents[1]


class CpuMatrixTests(unittest.TestCase):
    def cpu(self, **kw):
        return {
            "family": "i486dx", "family_name": "i486DX", "model_name": "33", "index": 0,
            "package": "CPU_PKG_SOCKET1", "type": "CPU_i486DX", "speed": 33333333,
            "multi": 1, "voltage": 5000, "flags": "CPU_SUPPORTS_DYNAREC",
            "fpus": "fpus_internal", "cyrix_id": 0, "signature": 0x401, **kw,
        }

    def board(self, **kw):
        return {
            "name": "486sp3", "init": "machine_at_486sp3_init",
            "cpu": {"package": "CPU_PKG_SOCKET3", "min_bus": "25000000",
                    "max_bus": "33333333", "min_voltage": "5000",
                    "max_voltage": "5000", "min_multi": "0", "max_multi": "0"},
            "ram": {"min": "1024", "max": "131072", "step": "1024"}, **kw,
        }

    def test_balanced_initializer_preserves_commas_and_parentheses(self):
        parts = split_c('.a = "a,b", .b = (const X[]) {{.a=1}, {.a=2}},')
        self.assertEqual(len(parts), 2)
        self.assertEqual(fields("{" + ",".join(parts) + "} ")["a"], '"a,b"')

    def test_table_ends_before_following_function_and_table(self):
        text = 'X cpus[] = {{.name="one"}, {0}}; int f() {return 1;} X other[]={0};'
        self.assertEqual(table(text, "cpus"), [{"name": '"one"'}, {}])

    def test_preprocessor_excludes_disabled_and_build_conditional_cpus(self):
        text = '#if 0\nbad\n#endif\n#ifdef USE_AMD_K5\nk5\n#endif\nalways\n'
        self.assertEqual(preprocess(text).strip(), "always")
        self.assertEqual(preprocess(text, ["USE_AMD_K5"]).split(), ["k5", "always"])

    def test_numeric_parser_does_not_execute_code(self):
        self.assertAlmostEqual(number("7.0 / 3.0"), 7 / 3)
        for value in ('__import__("os").system("true")', "CPU_FLAGS", "1 << 2"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                number(value)

    def test_package_intersection_and_implicit_socket_compatibility(self):
        cpu, board = self.cpu(), self.board()
        self.assertTrue(eligible(cpu, board))
        cpu["package"] += " | CPU_PKG_SOCKET3_PC330"
        self.assertTrue(eligible(cpu, board))
        cpu["package"] = "CPU_PKG_SLOT1"
        self.assertFalse(eligible(cpu, board))

    def test_bus_and_voltage_limits_are_not_silently_overridden(self):
        self.assertFalse(eligible(self.cpu(speed=50000000), self.board()))
        self.assertFalse(eligible(self.cpu(voltage=3300), self.board()))
        self.assertTrue(eligible(self.cpu(speed=33333333 + 840907), self.board()))
        self.assertFalse(eligible(self.cpu(speed=33333333 + 840908), self.board()))

    def test_small_ram_board_does_not_hide_a_working_candidate(self):
        board = self.board(name="a-small", ram={"min": "1024", "max": "4096", "step": "1024"})
        rows = make_matrix([self.cpu()], [board, self.board()])
        self.assertEqual(len(rows), 2)
        self.assertTrue(all(r["machine"] == "486sp3" for r in rows))

    def test_required_dynarec_and_external_fpu_variants(self):
        cpu = self.cpu(flags="CPU_SUPPORTS_DYNAREC | CPU_REQUIRES_DYNAREC", fpus="fpus_486sx")
        rows = make_matrix([cpu], [self.board()])
        self.assertEqual({r["fpu"] for r in rows}, {"none", "487sx"})
        self.assertEqual({r["dynarec"] for r in rows}, {"on", "off"})
        self.assertTrue(all(r["gui_requires_dynarec"] for r in rows))

    def test_fpu_backends_expand_only_where_applicable(self):
        rows = make_matrix([self.cpu(fpus="fpus_486sx")], [self.board()], fpu_backends="both")
        self.assertEqual(len(rows), 6)
        self.assertEqual({r["fpu_backend"] for r in rows if r["fpu"] == "none"}, {"none"})
        self.assertEqual({r["fpu_backend"] for r in rows if r["fpu"] == "487sx"},
                         {"host", "softfloat"})
        board = self.board(flags="MACHINE_SOFTFLOAT_ONLY")
        rows = make_matrix([self.cpu()], [board], fpu_backends="both")
        self.assertEqual({r["fpu_backend"] for r in rows}, {"softfloat"})

    def test_default_and_all_speed_selection_do_not_drop_unrunnable_entries(self):
        a = self.cpu()
        b = self.cpu(speed=50000000, index=1)
        default = make_matrix([a, b], [self.board()])
        self.assertEqual(len(default), 2)
        all_rows = make_matrix([a, b], [self.board()], True)
        self.assertEqual(len(all_rows), 3)
        self.assertEqual(all_rows[-1]["status"], "blocked")
        self.assertEqual(all_rows[-1]["index"], 1)

    def test_missing_board_and_missing_contract_are_explicit(self):
        rows = make_matrix([self.cpu(family="future_cpu")], [])
        self.assertEqual(rows[0]["status"], "blocked")
        rows = make_matrix([self.cpu(family="future_cpu")], [self.board()])
        self.assertFalse(rows[0]["contract"])

    def test_config_output_is_safe_and_preserves_identity(self):
        rows = make_matrix([self.cpu()], [self.board()])
        with tempfile.TemporaryDirectory() as directory:
            write_configs(rows, Path(directory))
            text = Path(rows[0]["config"]).read_text()
            self.assertIn("cpu_family = i486dx", text)
            self.assertIn("cpu_speed = 33333333", text)
            self.assertNotIn("hdd", text)

    def test_matrix_records_infrastructure_failure_and_refuses_to_overwrite_results(self):
        rows = make_matrix([self.cpu()], [self.board()])[:1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image, binary = root / "floppy.img", root / "86Box"
            image.write_bytes(b"fixture")
            binary.write_bytes(b"fixture")
            write_configs(rows, root)
            args = SimpleNamespace(output=root, image=image, emulator=binary,
                                   family=[], engine=None, wall_seconds=10, rompath=None)
            with patch("cpu_matrix.subprocess.run") as run:
                run.return_value = SimpleNamespace(returncode=1, stdout="",
                                                   stderr="missing ROM tree")
                result = run_matrix(rows, args)
            self.assertEqual(result["rows"][0]["outcome"], "infrastructure_error")
            self.assertIn("missing ROM", result["rows"][0]["stderr"])
            command = run.call_args.args[0]
            self.assertIn("--expected-family-name", command)
            self.assertIn("--expected-model-name", command)
            self.assertIn("--expected-signature", command)
            self.assertEqual(json.loads((root / "results.json").read_text()), result)
            with self.assertRaisesRegex(ValueError, "already exists"):
                run_matrix(rows, args)

    def test_optional_signature_must_be_acknowledged_exactly(self):
        machine = {"cpu_family": "k6_2", "fpu_type": "internal"}
        self.assertEqual(cpu_control_frame(machine, True, 0x58c),
                         b"@cpu1 k6_2 internal accuracy 0000058c\n")
        run = parse_log("CONTROL protocol=cpu1 accepted=1 model=k6_2 "
                        "fpu=internal mode=accuracy signature=0000058c\n")
        self.assertTrue(cpu_control_accepted(run, machine, True, 0x58c))
        self.assertFalse(cpu_control_accepted(run, machine, True, 0x580))
        for sig in (-1, 0x100000000):
            with self.assertRaises(ValueError):
                cpu_control_frame(machine, True, sig)

    def test_benchmark_matrix_requests_benchmark_and_rejects_accuracy_fallback(self):
        for mode in ("benchmark", "accuracy", None):
            with self.subTest(guest_mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                image = root / "floppy.img"
                image.write_bytes(b"fixture")
                (root / "result.json").write_text(json.dumps({
                    "status": "pass", "mode": mode,
                    "tests": [{"name": "cpu_model_contract", "status": "PASS"}],
                }))
                rows = make_matrix([self.cpu()], [self.board()])[:1]
                write_configs(rows, root)
                args = SimpleNamespace(output=root, image=image, emulator=image, mode="benchmark",
                                       family=[], engine=None, wall_seconds=10, rompath=None)
                with patch("cpu_matrix.subprocess.run") as run:
                    run.return_value = SimpleNamespace(
                        returncode=0, stdout=json.dumps({"directory": str(root)}), stderr="")
                    result = run_matrix(rows, args)
                self.assertNotIn("--accuracy", run.call_args.args[0])
                self.assertEqual(result["mode"], "benchmark")
                self.assertEqual(result["rows"][0]["outcome"],
                                 "pass" if mode == "benchmark" else "benchmark_unavailable")

    def test_blocked_rows_are_retained_with_engine_selection_and_saved(self):
        rows = make_matrix([self.cpu()], [])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "floppy.img"
            image.write_bytes(b"fixture")
            args = SimpleNamespace(output=root, image=image, emulator=image,
                                   family=[], engine="on", wall_seconds=10, rompath=None)
            result = run_matrix(rows, args)
            self.assertEqual(result["rows"][0]["outcome"], "blocked")
            self.assertEqual(json.loads((root / "results.json").read_text()), result)

    def test_corrupt_runner_result_is_explicit_and_does_not_stop_matrix(self):
        rows = make_matrix([self.cpu()], [self.board()])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "floppy.img"
            image.write_bytes(b"fixture")
            (root / "result.json").write_text("{broken")
            write_configs(rows, root)
            args = SimpleNamespace(output=root, image=image, emulator=image,
                                   family=[], engine=None, wall_seconds=10, rompath=None)
            with patch("cpu_matrix.subprocess.run") as run:
                run.return_value = SimpleNamespace(
                    returncode=1, stdout=json.dumps({"directory": str(root)}), stderr="")
                result = run_matrix(rows, args)
            self.assertEqual(len(result["rows"]), 2)
            self.assertTrue(all(r["outcome"] == "infrastructure_error" and r["result_error"]
                                for r in result["rows"]))

    def test_additive_coverage_and_filtered_inventory_are_retained(self):
        run = parse_log(
            "PROBE feature=00000008 available=1 rc=0\n"
            "FILTER name=mmx_alu id=4 req=00000008 reason=build_selection\n"
            "FEATURE name=mmx required=1 detected=0 probed=0 status=UNTESTED "
            "witness=mmx_alu pass=0 fail=0 fault=0 skip=0 filtered=1 scope=sampled\n")
        self.assertEqual(run["features"][0]["status"], "UNTESTED")
        self.assertEqual(run["filtered_tests"][0]["name"], "mmx_alu")
        self.assertEqual(run["probes"][0]["available"], "1")

    def test_catalogue_and_behavioural_witnesses_are_unique_and_registered(self):
        names = re.findall(r'^CPU_MODEL\("([^"]+)"', (ROOT / "cpu_models.h").read_text(), re.M)
        self.assertEqual(len(names), len(set(names)))
        features = re.findall(r'CPU_FEATURE\((REQ_\w+), "([^"]+)", "([^"]*)"\)',
                              (ROOT / "cpu_features.h").read_text())
        self.assertEqual(len(features), len(set(f[0] for f in features)))
        registry = "\n".join(p.read_text() for p in ROOT.glob("tests_*.c"))
        identity = (ROOT / "cpu_contract.c").read_text().split("static u32 cpu_identity(void) {")[1]
        identity = identity.split("const test_t tests_cpu_contract[]")[0]
        for requirement, name, witness in features:
            if requirement != "REQ_486":
                self.assertIn(requirement, identity, name)
            if witness:
                self.assertRegex(registry, r'\{\s*"' + re.escape(witness) + r'"', name)
        claims = re.findall(r'CPU_CLAIM\((\d+), (\d+), "([^"]+)", "([^"]+)"\)',
                            (ROOT / "cpu_features.h").read_text())
        for _, _, name, witness in claims:
            self.assertRegex(registry, r'\{\s*"' + re.escape(witness) + r'"', name)


if __name__ == "__main__":
    unittest.main()
