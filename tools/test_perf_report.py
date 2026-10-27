"""Small deterministic tests for aggregation and regression gating."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

from perf_report import (compare, compare_host, distribution, load_runs, measurement_notes,
                         summarize, validate_run)
from qemu_smoke import parse_log
from run_86box import write_config


def example():
    records = "\n".join(
        f"TEST r={r} name=heavy status=PASS ms=20 cyc={cycles} unit=tsc ops=100 sum=01234567"
        for r, cycles in enumerate((1000, 1200, 1100, 1300), 1))
    run = parse_log("CPU cpuid=1 vendor=\"GenuineIntel\" family=6\n"
                    "CFG rounds=4 heavy_scale=8 seed=86b04a21\n"
                    f"{records}\nSUMMARY tests=4 pass=4 fail=0 skip=0 fault=0\nDONE\n")
    run.update(done=True, cpu="host", machine="pc", accel="kvm", memory_mib=32,
               image_sha256="a" * 64, host_seconds=1.5)
    return run


def options(**changes):
    args = dict(allow_environment_change=False, allow_image_change=False,
                min_samples=3, max_regression_percent=10)
    args.update(changes)
    return SimpleNamespace(**args)


class PerfTests(unittest.TestCase):
    def test_vendor_capability_records(self):
        cpu_line = 'CPU cpuid=1 vendor="AuthenticAMD" family=5 model=13'
        extensions = "CPU ext_edx=c08029bb 3dnow=1 3dnowext=1 cyrix_emmi=0"
        run = parse_log(
            f"{cpu_line}\nCPU mhz=266 tsc_per_ms=266000\n{extensions}\n"
            "TEST r=1 name=3dnow_word_convert status=FAIL req=0c00000c "
            "got=deadbeef want=00000000\n"
            "TEST r=1 name=cyrix_emmi_moves status=SKIP "
            "reason=missing_cyrix_emmi req=10000008\n"
            "SUMMARY tests=2 pass=0 fail=1 skip=1 fault=0\nDONE\n")
        self.assertEqual(run["cpu_info"], [cpu_line, extensions])
        self.assertEqual(run["tests"][0]["req"], "0c00000c")
        self.assertEqual(run["tests"][0]["status"], "FAIL")
        self.assertEqual(run["tests"][1]["req"], "10000008")
        self.assertEqual(run["tests"][1]["reason"], "missing_cyrix_emmi")

    def test_failure_detail_capture(self):
        run = parse_log(
            "BEGIN r=1 name=a phase=first\n"
            "FAILDETAIL test=a line=10 check=x == y got=1 want=2\n"
            'TEST r=1 name=a status=FAIL first="x == y" got=1 want=2 '
            "failure_iteration=2 failure_phase=sample seed=86b04a21\n"
            "TEST r=1 name=b status=SKIP reason=missing_mmx\n"
            "BEGIN r=1 name=c phase=first\nTEST r=1 name=c status=PASS\n")
        a, b, c = run["tests"]
        self.assertEqual(len(a["failure_details"]), 1)
        self.assertEqual(a["failure_iteration"], "2")
        self.assertEqual(a["got"], "1")
        self.assertNotIn("failure_details", b)
        self.assertNotIn("failure_details", c)

    def test_measurement_notes(self):
        test = example()["tests"][0]
        self.assertEqual(measurement_notes(test), [])
        test.update(iterations="2", iteration_ops="50")
        self.assertEqual(measurement_notes(test), [])
        test["iteration_ops"] = "0"
        self.assertIn("nonconstant_or_inconsistent_operations", measurement_notes(test))
        test.update(ms="0", cyc="0", timing_invalid="counter_regressed")
        notes = measurement_notes(test)
        for reason in ("guest_ms_unresolved", "counter_unresolved", "counter_regressed"):
            self.assertIn(reason, notes)
        test["status"] = "FAIL"
        self.assertEqual(measurement_notes(test), ["correctness_fail"])
        report = summarize([example()], 0)
        self.assertEqual(report["measurement_notes"], [])

    def test_distribution(self):
        self.assertEqual(distribution([4, 1, 3, 2]),
                         dict(n=4, min=1, median=2.5, p95=4, max=4))
        self.assertIsNone(distribution([]))

    def test_warmup_exclusion(self):
        report = summarize([example()], 1)
        self.assertEqual(report["workloads"]["heavy"]["counter_per_op"]["n"], 3)
        self.assertEqual(report["workloads"]["heavy"]["counter_per_op"]["median"], 12)
        self.assertEqual(report["host_boot_to_done_seconds"]["median"], 1.5)

    def test_no_samples(self):
        with self.assertRaisesRegex(ValueError, "no samples"):
            summarize([example()], 4)

    def test_incomplete(self):
        run = example()
        run["done"] = False
        with self.assertRaises(ValueError):
            validate_run(run)
        run["done"] = True
        run["counts"]["tests"] = "3"
        with self.assertRaisesRegex(ValueError, "test count"):
            validate_run(run)

    def test_fatal(self):
        run = example()
        run["fatal"] = True
        with self.assertRaises(ValueError):
            validate_run(run)

    def test_duplicate(self):
        run = example()
        run["tests"][1]["r"] = "1"
        with self.assertRaisesRegex(ValueError, "duplicate"):
            validate_run(run)

    def test_mixed_environment(self):
        a, b = example(), example()
        b["cpu"] = "486"
        with self.assertRaisesRegex(ValueError, "mixed environments"):
            summarize([a, b], 0)

    def test_host_sampling_provenance(self):
        a, b = example(), example()
        for run in (a, b):
            run["host"] = {"architecture": "aarch64", "cpu": "Pi 5"}
            run["workload_capture"] = {"sample_cpu_seconds_target": 30}
            run["perf"] = {"status": "disabled"}
        for key, value in (("host", {"architecture": "x86_64"}),
                           ("workload_capture", {"sample_cpu_seconds_target": .05}),
                           ("perf", {"status": "captured"})):
            other = copy.deepcopy(b)
            other[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "mixed environments"):
                summarize([a, other], 0)

    def test_regression(self):
        baseline = summarize([example()], 1)
        current = copy.deepcopy(baseline)
        current["workloads"]["heavy"]["counter_per_op"]["median"] *= 1.2
        self.assertEqual(compare(current, baseline, options())[0]["status"], "regression")
        current["workloads"]["heavy"]["counter_per_op"]["n"] = 1
        self.assertEqual(compare(current, baseline, options())[0]["status"], "insufficient_samples")

    def test_host_regression(self):
        baseline = summarize([example(), example(), example()], 0)
        current = copy.deepcopy(baseline)
        current["host_boot_to_done_seconds"]["median"] *= 1.2
        self.assertEqual(compare_host(current, baseline, options())["status"], "regression")
        current["host_boot_to_done_seconds"]["n"] = 1
        self.assertEqual(compare_host(current, baseline, options())["status"],
                         "insufficient_samples")

    def test_failures_survive_discard(self):
        run = example()
        run["tests"][0]["status"] = "FAIL"
        run["counts"]["pass"], run["counts"]["fail"] = "3", "1"
        self.assertEqual(len(summarize([run], 1)["failures"]), 1)

    def test_timing_invalid(self):
        run = example()
        run["tests"][0]["timing_invalid"] = "counter_regressed"
        self.assertEqual(summarize([run], 0)["workloads"]["heavy"]["counter_per_op"]["n"], 3)

    def test_baseline_guards(self):
        baseline = summarize([example()], 0)
        for key, value in (("image_sha256", "b" * 64),
                           ("environment", {"cpu": "different"}),
                           ("config", {"seed": "different"})):
            current = copy.deepcopy(baseline)
            current[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                compare(current, baseline, options())

    def test_work_mismatch(self):
        baseline = summarize([example()], 0)
        current = copy.deepcopy(baseline)
        current["workloads"]["heavy"]["ops"] = [200]
        self.assertEqual(compare(current, baseline, options())[0]["status"], "not_comparable")

    def test_repeated_work_normalization(self):
        a, b = example(), example()
        for run, repetitions in ((a, 2), (b, 3)):
            run["config"]["protocol"] = "work1"
            for test in run["tests"]:
                test.update(iterations=str(repetitions), iteration_ops="100",
                            ops=str(100 * repetitions),
                            cyc=str(int(test["cyc"]) * repetitions))
        current, baseline = summarize([a], 0), summarize([b], 0)
        self.assertNotEqual(current["workloads"]["heavy"]["ops"],
                            baseline["workloads"]["heavy"]["ops"])
        self.assertEqual(compare(current, baseline, options())[0]["status"], "ok")
        a["tests"][0]["iteration_ops"] = "99"
        current = summarize([a], 0)
        self.assertEqual(compare(current, baseline, options())[0]["status"], "not_comparable")

    def test_cli_and_json(self):
        with tempfile.TemporaryDirectory() as directory:
            source, report = Path(directory) / "input.json", Path(directory) / "report.json"
            source.write_text(json.dumps([example()]))
            self.assertEqual(len(load_runs(source)), 1)
            command = [sys.executable, str(Path(__file__).with_name("perf_report.py")),
                       str(source), "--output", str(report), "--discard-rounds", "1"]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            other = Path(directory) / "comparison.json"
            result = subprocess.run(command + ["--baseline", str(report),
                                               "--output", str(other)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(command + ["--baseline", str(report),
                                               "--output", str(other), "--min-samples", "5"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stderr)

    def test_runtime_breakdown(self):
        run = example()
        self.assertEqual(summarize([run], 0)["guest_runtime"], [None])
        lines = "\n".join(
            f"RUNTIME scope=test r={r} id={r} name=heavy total_ms=50 warmup_ms=10 "
            "sample_ms=20 overhead_ms=20 resolution_ms=10" for r in range(1, 5))
        text = (f"CPU cpuid=1\nCFG rounds=4\n{lines}\n"
                "SUMMARY tests=4 pass=4 fail=0 skip=0 fault=0 ms=210\n"
                "RUNTIME scope=suite total_ms=210 warmup_ms=40 sample_ms=80 "
                "overhead_ms=80 suite_overhead_ms=10 resolution_ms=10\nDONE\n")
        run["runtime_records"] = parse_log(text)["runtime_records"]
        run["counts"]["ms"] = "210"
        runtime = summarize([run], 0)["guest_runtime"][0]
        self.assertEqual(runtime["suite"]["sample_ms"], 80)
        self.assertEqual(len(runtime["tests"]), 4)
        for mutate, message in (
                (lambda r: r["runtime_records"].pop(0), "incomplete"),
                (lambda r: r["runtime_records"][0].update(name="other"), "inventory"),
                (lambda r: r["runtime_records"][0].update(overhead_ms="21"), "add up"),
                (lambda r: r["runtime_records"][0].update(overhead_ms="-1"), "invalid"),
                (lambda r: r["runtime_records"][0].update(resolution_ms="1"), "invalid"),
                (lambda r: r["tests"][0].update(id="99"), "id differs"),
                (lambda r: r["tests"][0].update(ms="21"), "invocation times"),
                (lambda r: r["runtime_records"][-1].update(sample_ms="81", total_ms="211"),
                 "suite totals"),
                (lambda r: r["counts"].update(ms="200"), "SUMMARY")):
            broken = copy.deepcopy(run)
            mutate(broken)
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                summarize([broken], 0)

    def test_execution_mode_override(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory)
            config = run_dir / "original.cfg"
            config.write_text("[Machine]\nmachine=486sp3\ncpu_use_dynarec=1\n")
            args = SimpleNamespace(profile="486dx", config=config, memory_mb=16,
                                   dynarec="off")
            machine = write_config(args, run_dir)
            self.assertEqual(machine["cpu_use_dynarec"], "0")
            self.assertEqual(machine["mem_size"], "16384")
            self.assertEqual(config.read_text(),
                             "[Machine]\nmachine=486sp3\ncpu_use_dynarec=1\n")
            args.config, args.dynarec = None, "on"
            self.assertEqual(write_config(args, run_dir)["cpu_use_dynarec"], "1")

    def test_k6_plus_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(profile="k6-2+", config=None, memory_mb=16,
                                   dynarec="off")
            machine = write_config(args, Path(directory))
            self.assertEqual(machine["machine"], "p55t2p4")
            self.assertEqual(machine["cpu_family"], "k6_2p")
            self.assertEqual(machine["cpu_speed"], "266666666")
            self.assertEqual(machine["cpu_use_dynarec"], "0")


if __name__ == "__main__":
    unittest.main()
