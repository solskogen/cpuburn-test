"""Deterministic host capture tests; no emulator/perf privilege is required."""

from pathlib import Path
import signal
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from qemu_smoke import parse_log
from run_86box import arguments, complete_counts, execute, start_display, write_config
from workload_capture import WorkloadCapture, PerfRecorder, parse_perf_script, process_cpu_seconds


def marker(phase="sample", identifier=1, round_number=1, name="heavy"):
    return f"WORK phase={phase} r={round_number} id={identifier} name={name}\n"


class CaptureTests(unittest.TestCase):
    def step(self, capture, before, after, data=""):
        capture.observe_step(before, after)
        return capture.feed(data, after[2], after[0])

    def test_interior_cpu_wall_and_capacity(self):
        capture = WorkloadCapture(2, 250)
        self.assertEqual(self.step(capture, (10, 1, 0), (11, 1.2, 250), marker()),
                         ["+00000001\n"])
        self.assertEqual(self.step(capture, (20, 1.3, 250), (20.5, 1.8, 500)), [])
        self.step(capture, (30, 2, 500), (31, 2.5, 750), marker("end"))
        report = capture.report()
        workload = report["workloads"][0]
        self.assertAlmostEqual(workload["process_cpu_seconds"], .5)
        self.assertEqual(workload["step_wall_seconds"], .5)
        self.assertEqual(workload["emulated_ms"], 250)
        self.assertEqual(workload["full_speed_capacity_percent"], 50)
        self.assertAlmostEqual(workload["guest_seconds_per_process_cpu_second"], .5)
        self.assertEqual(report["unattributed"]["steps"], 2)
        self.assertEqual(capture.windows, [(20, 20.5, 1)])
        self.assertIn("under_sampled:1", report["warnings"])
        self.assertEqual(workload["end_boundary"]["step_monotonic_start"], 30)

    def test_lease_heartbeat_and_release(self):
        capture = WorkloadCapture(2)
        self.assertEqual(capture.feed(marker(identifier=255), 0, 1), ["+000000ff\n"])
        self.assertEqual(self.step(capture, (1, 0, 0), (2, .5, 1000)), ["+000000ff\n"])
        self.assertEqual(self.step(capture, (2, .5, 1000), (4, 2.5, 2000)), ["-000000ff\n"])
        self.assertEqual(self.step(capture, (4, 2.5, 2000), (5, 3, 3000)), ["-000000ff\n"])
        self.assertEqual(capture.feed(marker("end", 255), 3000, 5), [])
        self.assertFalse(capture.report()["workloads"][0]["under_sampled"])

    def test_stale_end_and_new_round_ids(self):
        capture = WorkloadCapture(.1)
        capture.feed(marker(identifier=1), 0)
        capture.feed(marker("end", 0), 100)
        self.assertEqual(capture.active["id"], 1)
        capture.feed(marker("end", 1), 250)
        self.assertEqual(capture.feed(marker(identifier=1), 250), [])
        self.assertEqual(capture.feed(marker(identifier=2, round_number=2), 500),
                         ["+00000002\n"])
        capture.feed(marker("end", 1), 500)
        self.assertEqual(capture.active["id"], 2)
        capture.feed(marker("end", 2, round_number=2), 750)
        self.assertIsNone(capture.active)
        self.assertEqual(len(capture.report()["workloads"]), 2)

    def test_fragmented_boundaries_discard_partial_steps(self):
        capture = WorkloadCapture(1)
        start = marker()
        self.assertEqual(self.step(capture, (1, 0, 0), (2, .1, 250), start[:20]), [])
        self.assertEqual(self.step(capture, (2, .1, 250), (3, .2, 500), start[20:]),
                         ["+00000001\n"])
        self.step(capture, (3, .2, 500), (4, .4, 750))
        end = marker("end")
        self.step(capture, (4, .4, 750), (5, .6, 1000), end[:15])
        self.step(capture, (5, .6, 1000), (6, .8, 1250), end[15:])
        self.assertEqual(capture.report()["workloads"][0]["steps"], 1)
        self.assertEqual(capture.windows, [(3, 4, 1)])

    def test_warmup_skip_fault_and_sample(self):
        capture = WorkloadCapture()
        self.assertEqual(capture.feed(marker("warmup"), 0), [])
        self.step(capture, (1, 0, 0), (2, .5, 250))
        self.assertEqual(capture.feed(marker("sample"), 250), ["+00000001\n"])
        self.assertEqual(capture.feed(marker("end"), 500), [])
        self.assertEqual(capture.feed(marker("warmup", 2), 750), [])
        self.assertEqual(capture.feed(marker("end", 2) + "TEST r=1 name=heavy status=FAULT\n",
                                      1000), [])
        report = capture.report()
        self.assertEqual([p["phase"] for p in report["phases"]], ["warmup", "sample", "warmup"])
        self.assertEqual(len(report["workloads"]), 1)
        self.assertEqual(report["phases"][0]["process_cpu_seconds"], .5)

    def test_multiple_markers_in_step_never_attribute_or_extend_ended_work(self):
        capture = WorkloadCapture(.1)
        commands = self.step(capture, (1, 0, 0), (2, 1, 250),
                             marker() + marker("end") + marker(identifier=2))
        self.assertEqual(commands, ["+00000001\n", "+00000002\n"])
        self.assertEqual(capture.windows, [])
        self.assertEqual(capture.report()["workloads"][0]["steps"], 0)

    def test_invalid_clocks(self):
        for after in ((1, 2, 250), (2, -1, 250), (2, 1, -1), (float("nan"), 1, 250)):
            with self.subTest(after=after):
                capture = WorkloadCapture()
                capture.feed(marker(), 0)
                self.step(capture, (1, 0, 0), after)
                self.assertEqual(capture.windows, [])
                self.assertEqual(capture.report()["workloads"][0]["invalid_steps"], 1)

    def test_invalid_protocol_and_target(self):
        for target in (0, -1, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                WorkloadCapture(target)
        capture = WorkloadCapture()
        self.assertEqual(capture.feed(marker(identifier=0x100000000), 0), [])
        self.assertEqual(capture.feed("WORK phase=sample r=1 id=ff name=heavy\n", 0), [])
        self.assertEqual(capture.records, [])
        self.assertEqual(capture.report()["availability"], "unavailable_no_sample_markers")

    def test_fail_diagnostics_do_not_end_sample(self):
        capture = WorkloadCapture(.1)
        capture.feed(marker(), 0)
        self.assertEqual(self.step(capture, (1, 0, 0), (2, .2, 250),
                                   "FAILDETAIL mismatch\nNOTE profiling continues\n"),
                         ["-00000001\n"])
        capture.feed(marker("end") + "TEST r=1 name=heavy status=FAIL\n", 500)
        self.assertEqual(capture.report()["aggregates"]["heavy"]["samples"], 1)

    def test_unclosed_sample_is_reported_under_sampled(self):
        capture = WorkloadCapture()
        capture.feed(marker(), 0)
        capture.finish(100, 1)
        report = capture.report()["workloads"][0]
        self.assertTrue(report["under_sampled"])
        self.assertEqual(report["end_reason"], "capture_stopped")

    def test_proc_stat_includes_process_utime_and_stime(self):
        fields = ["S"] + ["0"] * 10 + ["123", "77"] + ["0"] * 10
        with patch("workload_capture.Path.read_text", return_value="42 (odd ) name) " + " ".join(fields)), \
                patch("workload_capture.os.sysconf", return_value=100):
            self.assertEqual(process_cpu_seconds(42), 2)


class PerfTests(unittest.TestCase):
    def test_unknown_jit_addresses_are_not_collapsed(self):
        report = parse_perf_script([
            " 10.100000000: abc [unknown] (/tmp/perf-42.map)\n",
            " 10.200000000: def [unknown] (/tmp/perf-42.map)\n",
        ], [(10, 11, 1)])
        self.assertEqual(report["1"]["leaf_hotpaths"], [
            ("abc [unknown] (/tmp/perf-42.map)", 1),
            ("def [unknown] (/tmp/perf-42.map)", 1)])

    def test_perf_time_windows_and_callchains(self):
        script = [
            " 9.900000000: outside (86Box)\n",
            " 10.100000000: leaf (86Box)\n",
            "    abc caller (86Box)\n",
            "    abd caller (86Box)\n",
            " 10.500000000: boundary (86Box)\n",
            " 11.100000000: \n",
            "    ccc second (86Box)\n",
            "    ddd caller (86Box)\n",
            " 12.000000000: excluded (86Box)\n",
        ]
        report = parse_perf_script(script, [(10, 10.5, 1), (11, 12, 2)])
        self.assertEqual(report["1"]["samples"], 1)
        self.assertEqual(report["1"]["leaf_hotpaths"], [("leaf (86Box)", 1)])
        self.assertEqual(dict(report["1"]["inclusive_hotpaths"])["caller (86Box)"], 1)
        self.assertEqual(report["2"]["leaf_hotpaths"], [("second (86Box)", 1)])

    def test_missing_perf_explicit(self):
        with patch("workload_capture.shutil.which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "not installed"):
                PerfRecorder(Path(__file__).parent).start(42)

    def test_permission_failure_explicit(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            recorder = PerfRecorder(directory)
            (Path(directory) / "perf-record.log").write_text("No permission to enable cycles event\n")
            process = Mock()
            process.poll.return_value = 255
            # Popen's log redirection truncates the file, so inject the diagnostic at launch.
            def launch(*args, **kwargs):
                kwargs["stderr"].write(b"Permission denied: perf_event_paranoid\n")
                kwargs["stderr"].flush()
                return process
            with patch("workload_capture.shutil.which", return_value="/usr/bin/perf"), \
                    patch("workload_capture.subprocess.Popen", side_effect=launch), \
                    patch("workload_capture.time.sleep"):
                with self.assertRaisesRegex(RuntimeError, "Permission denied"):
                    recorder.start(42)

    def test_stop_flushes_specific_recorder_and_checks_exit(self):
        recorder = PerfRecorder(Path(__file__).parent)
        recorder.process = Mock()
        recorder.process.poll.return_value = None
        recorder.process.wait.return_value = 0
        recorder.stop()
        recorder.process.send_signal.assert_called_once_with(signal.SIGINT)
        recorder.process.wait.return_value = 1
        with self.assertRaisesRegex(RuntimeError, "perf record failed"):
            recorder.stop()

    def test_missing_recording_is_not_success(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            with self.assertRaisesRegex(RuntimeError, "no nonempty perf.data"):
                PerfRecorder(directory).report([])

    def test_script_requests_ip_to_enable_real_perf_callchains(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            recorder = PerfRecorder(directory)
            recorder.command = ["/usr/bin/perf", "record"]
            (Path(directory) / "perf.data").write_bytes(b"recording")

            def script(command, stdout, stderr):
                self.assertEqual(command[-1], "time,ip,sym,dso")
                stdout.write(" 10.100000000: \n\t abc leaf (86Box)\n\t def caller (86Box)\n\n")
                return SimpleNamespace(returncode=0)

            with patch("workload_capture.subprocess.run", side_effect=script):
                report = recorder.report([(10, 11, 1)])
            self.assertEqual(report["workloads_by_id"]["1"]["leaf_hotpaths"],
                             [("leaf (86Box)", 1)])


class RunnerTests(unittest.TestCase):
    def test_display_readiness_waits_for_newline(self):
        process = Mock()
        process.poll.return_value = None
        with tempfile.TemporaryDirectory() as directory, \
                patch("run_86box.os.pipe", return_value=(10, 11)), \
                patch("run_86box.os.close") as close, \
                patch("run_86box.subprocess.Popen", return_value=process), \
                patch("run_86box.time.monotonic", return_value=1), \
                patch("run_86box.select.select", return_value=([10], [], [])) as select, \
                patch("run_86box.os.read", side_effect=[b"123", b"\n"]) as read:
            self.assertEqual(start_display(Path(directory), 10), (process, ":123"))
            self.assertEqual(read.call_count, 2)
            self.assertEqual(select.call_count, 2)
            close.assert_any_call(10)
            close.assert_any_call(11)

    def test_display_incomplete_readiness_fails_and_stops_child(self):
        process = Mock()
        with tempfile.TemporaryDirectory() as directory, \
                patch("run_86box.os.pipe", return_value=(10, 11)), \
                patch("run_86box.os.close"), \
                patch("run_86box.subprocess.Popen", return_value=process), \
                patch("run_86box.time.monotonic", return_value=1), \
                patch("run_86box.select.select", return_value=([10], [], [])), \
                patch("run_86box.os.read", side_effect=[b"123", b""]), \
                patch("run_86box.stop_process") as stop:
            with self.assertRaisesRegex(RuntimeError, "readiness pipe"):
                start_display(Path(directory), 10)
            stop.assert_called_once_with(process)

    def test_default_done_executes_single_combined_capture_and_retains_failures(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            run_dir = Path(directory)
            args = SimpleNamespace(wall_seconds=10, emulated_ms=10000, step_ms=250,
                                   expect=[], reject=["FATAL"], label="", key=[], bios_f1=False)
            chunks = [
                marker("warmup") + marker(),
                "",
                marker("end") + "TEST r=1 name=heavy status=FAIL\n"
                "SUMMARY tests=1 pass=0 fail=1 skip=0 fault=0\nDONE\n",
            ]
            commands = []
            state = {"ms": 0, "serial": 0}

            def request(command, allow_failure=False):
                commands.append(command)
                if command.startswith("step "):
                    state["ms"] += int(command.split()[1])
                    return {"ok": True, "ms": state["ms"]}
                if command.startswith("serial read "):
                    data = chunks[state["serial"]].encode()
                    state["serial"] += 1
                    return {"ok": True, "data": data.hex(), "next": state["serial"]}
                return {"ok": True}

            process = Mock(pid=42)
            process.poll.return_value = None
            api = Mock()
            api.request.side_effect = request
            with patch("run_86box.start_display", return_value=(Mock(), ":999")), \
                    patch("run_86box.subprocess.Popen", return_value=process), \
                    patch("run_86box.socket.socket", return_value=Mock()), \
                    patch("run_86box.Automation", return_value=api), \
                    patch("run_86box.stop_process"), \
                    patch("run_86box.host_metadata", return_value={"architecture": "test"}), \
                    patch("run_86box.process_cpu_seconds",
                          side_effect=[0, .1, .1, .3, .3, .4, .4]):
                result = execute(args, run_dir, ["86Box"], {})
            self.assertEqual(result["status"], "observed")
            self.assertEqual(result["emulated_ms"], 750)
            self.assertTrue(result["correctness"]["complete"])
            self.assertFalse(result["correctness"]["passed"])
            self.assertEqual(result["workload_capture"]["workloads"][0]["steps"], 1)
            self.assertEqual(sum(c.startswith("serial write ") for c in commands), 1)
            self.assertTrue((run_dir / "result.json").is_file())

    def test_defaults_and_mmx_233_and_pii(self):
        with patch("sys.argv", ["run_86box.py"]):
            args = arguments()
        self.assertEqual((args.profile, args.sample_cpu_seconds, args.emulated_ms,
                          args.wall_seconds), ("mmx", 30, 86400000, 28800))
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            options = SimpleNamespace(profile="mmx", config=None, memory_mb=16, dynarec=None)
            machine = write_config(options, Path(directory))
            self.assertEqual((machine["cpu_speed"], machine["cpu_multi"]), ("233333333", "3.5"))
            options.profile = "pii"
            self.assertEqual(write_config(options, Path(directory))["cpu_family"], "pentium2_klamath")

    def test_default_completion_checks_nonzero_consistent_counts_not_zero_failures(self):
        run = parse_log("TEST r=1 name=heavy status=FAIL\n"
                        "SUMMARY tests=1 pass=0 fail=1 skip=0 fault=0\nDONE\n")
        self.assertTrue(complete_counts(run))
        run["counts"]["fail"] = "0"
        self.assertFalse(complete_counts(run))
        self.assertFalse(complete_counts(parse_log(
            "SUMMARY tests=0 pass=0 fail=0 skip=0 fault=0\nDONE\n")))


if __name__ == "__main__":
    unittest.main()
