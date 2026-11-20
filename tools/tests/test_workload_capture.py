"""Deterministic host capture tests; no emulator/perf privilege is required."""

import gzip
import io
import json
import re
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from qemu_smoke import parse_log
import run_86box
from run_86box import arguments, complete_counts, display_environment, execute, write_config
from workload_capture import (EmulationClock, WorkloadCapture, PerfRecorder,
                              batch_means_interval, find_thread, parse_perf_script,
                              parse_schedstat, process_cpu_seconds, settle_state)


def marker(phase="sample", identifier=1, round_number=1, name="heavy"):
    return f"WORK phase={phase} r={round_number} id={identifier} name={name}\n"


class CaptureTests(unittest.TestCase):
    def step(self, capture, before, after, data=""):
        capture.observe_step(before, after)
        return capture.feed(data, after[2], after[0])

    def test_interior_cpu_wall_and_capacity(self):
        capture = WorkloadCapture(250)
        self.assertEqual(self.step(capture, (10, 1, 0), (11, 1.2, 250), marker()),
                         ["+00000001\n"])
        self.assertEqual(self.step(capture, (20, 1.3, 250), (20.5, 1.8, 500)),
                         ["+00000001\n"])
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

    def test_lease_heartbeat_and_release_when_settled(self):
        capture = WorkloadCapture(min_seconds=2, block_seconds=.5, min_blocks=2)
        self.assertEqual(capture.feed(marker(identifier=255), 0, 1), ["+000000ff\n"])
        self.assertEqual(self.step(capture, (1, 0, 0), (2, 1, 1000)), ["+000000ff\n"])
        self.assertEqual(self.step(capture, (2, 1, 1000), (3, 2, 2000)), ["+000000ff\n"])
        self.assertEqual(self.step(capture, (3, 2, 2000), (4, 3, 3000)), ["-000000ff\n"])
        self.assertEqual(self.step(capture, (4, 3, 3000), (5, 4, 3100)), [])
        self.assertEqual(self.step(capture, (5, 4, 3100), (6, 5, 3250)), ["-000000ff\n"])
        self.assertEqual(capture.feed(marker("end", 255), 3250, 6), [])
        workload = capture.report()["workloads"][0]
        self.assertEqual(workload["sample_stop"], "settled")
        self.assertFalse(workload["under_sampled"])
        self.assertEqual(workload["settle"]["discarded_blocks"], 1)

    def test_unsettled_sample_is_capped(self):
        capture = WorkloadCapture(min_seconds=1, max_seconds=4, block_seconds=.5, min_blocks=2)
        capture.feed(marker(), 0, 0)
        speeds = (250, 1000, 250, 1000)
        commands = [self.step(capture, (i, i, i * 1000), (i + 1, i + 1, i * 1000 + guest))
                    for i, guest in enumerate(speeds)]
        self.assertEqual(commands[-1], ["-00000001\n"])
        self.assertFalse(any("-00000001\n" in c for c in commands[:-1]))
        report = capture.report()
        workload = report["workloads"][0]
        self.assertEqual(workload["sample_stop"], "capped")
        self.assertFalse(workload["under_sampled"])
        self.assertIn("capped:1", report["warnings"])
        self.assertEqual(report["aggregates"]["heavy"]["capped"], 1)
        self.assertEqual(workload["block_speed_percent"], [25, 100, 25, 100])

    def test_short_steps_accumulate_into_blocks(self):
        capture = WorkloadCapture(block_seconds=.5)
        capture.feed(marker(), 0, 0)
        for i in range(5):
            self.step(capture, (i * .2, 0, i * 100), ((i + 1) * .2, 0, (i + 1) * 100))
        [(wall, guest)] = capture.active["_blocks"]
        self.assertAlmostEqual(wall, .6)
        self.assertEqual(guest, 300)
        self.assertAlmostEqual(capture.active["_block"][0], .4)

    def test_stale_end_and_new_round_ids(self):
        capture = WorkloadCapture()
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
        capture = WorkloadCapture()
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
        capture = WorkloadCapture()
        commands = self.step(capture, (1, 0, 0), (2, 1, 250),
                             marker() + marker("end") + marker(identifier=2))
        self.assertEqual(commands, ["+00000002\n"])
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

    def test_invalid_protocol_and_settings(self):
        for name in ("min_seconds", "max_seconds", "tolerance", "block_seconds"):
            for value in (0, -1, float("nan"), float("inf")):
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    WorkloadCapture(**{name: value})
        for options in ({"min_seconds": 3, "max_seconds": 2}, {"min_blocks": 1},
                        {"min_blocks": 2.5}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                WorkloadCapture(**options)
        capture = WorkloadCapture()
        self.assertEqual(capture.feed(marker(identifier=0x100000000), 0), [])
        self.assertEqual(capture.feed("WORK phase=sample r=1 id=ff name=heavy\n", 0), [])
        self.assertEqual(capture.records, [])
        self.assertEqual(capture.report()["availability"], "unavailable_no_sample_markers")

    def test_fail_diagnostics_do_not_end_sample(self):
        capture = WorkloadCapture(min_seconds=.5, max_seconds=1)
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

    def test_settle_state(self):
        steady = [(.25, 250)] * 8
        state = settle_state(steady, .01, 5)
        self.assertTrue(state["settled"])
        self.assertEqual((state["discarded_blocks"], state["window_blocks"]), (2, 6))
        self.assertEqual(state["window_speed_percent"], 100)
        self.assertTrue(settle_state(steady[:6], .01, 5)["settled"])
        self.assertFalse(settle_state(steady[:5], .01, 5)["settled"])
        slow_start = [(.25, 50)] + steady
        self.assertTrue(settle_state(slow_start, .01, 5)["settled"])
        noisy = [(.25, 250 + (25 if i % 2 else -25)) for i in range(12)]
        state = settle_state(noisy, .01, 5)
        self.assertFalse(state["settled"])
        self.assertGreater(state["relative_interval"], .01)
        drifting = [(.25, 240 + 2 * i) for i in range(40)]
        state = settle_state(drifting, .05, 5)
        # A trend is autocorrelation: batch means widen the interval too.
        self.assertGreater(state["relative_interval"], .05)
        self.assertGreater(state["relative_drift"], .05)
        self.assertFalse(state["settled"])
        empty = settle_state([], .01, 5)
        self.assertEqual((empty["window_blocks"], empty["settled"]), (0, False))

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

            def script(command, stdout, stderr, **options):
                self.assertIn("time,ip,sym,dso", command)
                self.assertEqual(stdout, subprocess.PIPE)
                if command[-1] == "--hide-call-graph":
                    output = io.StringIO(" 10.100000000:  abc leaf (86Box)\n")
                    return SimpleNamespace(stdout=output, wait=lambda: 0)
                self.assertEqual(command[-1], "time,ip,sym,dso")
                output = io.StringIO(" 10.100000000: \n\t abc leaf (86Box)\n"
                                     "\t def caller (86Box)\n\n")
                return SimpleNamespace(stdout=output, wait=lambda: 0)

            with patch("workload_capture.subprocess.Popen", side_effect=script):
                report = recorder.report([(10, 11, 1)])
            self.assertEqual(report["workloads_by_id"]["1"]["leaf_hotpaths"],
                             [("leaf (86Box)", 1)])
            self.assertFalse((Path(directory) / "perf.script").exists())


class RunnerTests(unittest.TestCase):
    def test_display_is_the_session_native_one_or_offscreen(self):
        with patch.dict("run_86box.os.environ", {"WAYLAND_DISPLAY": "wayland-1", "DISPLAY": ":0"}, clear=True):
            self.assertEqual(display_environment(), {"QT_QPA_PLATFORM": "wayland"})
        with patch.dict("run_86box.os.environ", {"DISPLAY": ":5"}, clear=True):
            self.assertEqual(display_environment(), {"QT_QPA_PLATFORM": "xcb"})
        with patch.dict("run_86box.os.environ", {}, clear=True):
            self.assertEqual(display_environment(), {"QT_QPA_PLATFORM": "offscreen"})

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
            with patch("run_86box.subprocess.Popen", return_value=process), \
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
            self.assertEqual(sum(c.startswith("serial write ") for c in commands), 2)
            self.assertTrue((run_dir / "result.json").is_file())

    def fake_run(self, args, chunks, process):
        """Run execute() against scripted serial chunks; return (result, commands)."""
        commands = []
        state = {"ms": 0, "serial": 0}

        def request(command, allow_failure=False):
            commands.append(command)
            if command.startswith("step "):
                state["ms"] += int(command.split()[1])
                return {"ok": True, "ms": state["ms"]}
            if command.startswith("serial read "):
                index = state["serial"]
                state["serial"] += 1
                data = (chunks[index] if index < len(chunks) else "").encode()
                return {"ok": True, "data": data.hex(), "next": state["serial"]}
            return {"ok": True}

        api = Mock()
        api.request.side_effect = request
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory, \
                patch("run_86box.subprocess.Popen", return_value=process), \
                patch("run_86box.socket.socket", return_value=Mock()), \
                patch("run_86box.Automation", return_value=api), \
                patch("run_86box.stop_process"), \
                patch("run_86box.host_metadata", return_value={"architecture": "test"}), \
                patch("run_86box.process_cpu_seconds", return_value=0):
            return execute(args, Path(directory), ["86Box"], {}), commands

    def test_accuracy_expect_without_done_is_not_pass(self):
        args = SimpleNamespace(wall_seconds=10, emulated_ms=1000, step_ms=250,
                               expect=["SUMMARY"], reject=["FATAL"], label="", key=[],
                               bios_f1=False, accuracy=True)
        process = Mock(pid=42)
        process.poll.return_value = None
        result, _ = self.fake_run(
            args, ["TEST r=1 name=heavy status=FAIL\n"
                   "SUMMARY tests=1 pass=0 fail=1 skip=0 fault=0\n"], process)
        self.assertIn("SUMMARY", result["matched"])
        self.assertFalse(result["done"])
        self.assertEqual(result["status"], "timeout")

    def test_quit_waits_for_emulator_before_signalling(self):
        args = SimpleNamespace(wall_seconds=10, emulated_ms=10000, step_ms=250,
                               expect=[], reject=["FATAL"], label="", key=[], bios_f1=False)
        process = Mock(pid=42)
        process.poll.return_value = None
        result, commands = self.fake_run(
            args, ["SUMMARY tests=0 pass=0 fail=0 skip=0 fault=0\nDONE\n"], process)
        self.assertIn("quit", commands)
        process.wait.assert_any_call(timeout=30)
        self.assertNotIn("quit_note", result)

    def test_defaults_and_mmx_233_and_pii(self):
        with patch("sys.argv", ["run_86box.py", "--profile", "mmx"]):
            args = arguments()
        self.assertEqual((args.profile, args.min_sample_seconds, args.max_sample_seconds,
                          args.settle_tolerance, args.emulated_ms, args.wall_seconds),
                         ("mmx", 1, 20, .01, 86400000, 28800))
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


class ClockTests(unittest.TestCase):
    def test_schedstat_is_nanoseconds(self):
        self.assertEqual(parse_schedstat("1500000000 2000 7\n"), 1.5)
        with self.assertRaises(ValueError):
            parse_schedstat("12\n")

    def test_emulation_thread_found_by_name(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            tasks = Path(directory) / "proc/42/task"
            for tid, comm in ((42, "86Box"), (43, "main_thread"), (44, "QThread")):
                (tasks / str(tid)).mkdir(parents=True)
                (tasks / str(tid) / "comm").write_text(comm + "\n")
                (tasks / str(tid) / "schedstat").write_text(f"{tid}000000000 0 1\n")
            real = Path

            def rooted(path, *rest):
                text = str(path)
                return real(directory + text) if text.startswith("/proc/") else real(path, *rest)

            with patch("workload_capture.Path", side_effect=rooted), \
                    patch("workload_capture.is_linux", return_value=True):
                self.assertEqual(find_thread(42), 43)
                clock = EmulationClock(42, fallback=lambda: 99)
                self.assertEqual(clock.locate(), "emulation_thread_schedstat_ns")
                self.assertEqual(clock.cpu_seconds(), 43)
                (tasks / "43" / "comm").write_text("renamed\n")
                fallback = EmulationClock(42, fallback=lambda: 99)
                self.assertEqual(fallback.locate(), "process_all_threads_stat_ticks")
                self.assertIn("main_thread", fallback.note)
                self.assertEqual(fallback.cpu_seconds(), 99)

    def test_other_operating_systems_report_unavailable(self):
        with patch("workload_capture.is_linux", return_value=False):
            clock = EmulationClock(42, fallback=lambda: 99)
            self.assertEqual(clock.locate(), "unavailable")
            self.assertEqual(clock.cpu_seconds(), 0.0)
            self.assertIn("Linux", clock.note)
        with patch("workload_capture.sys.platform", "darwin"):
            with self.assertRaisesRegex(RuntimeError, "requires Linux"):
                process_cpu_seconds(42)

    def test_batch_means_widen_correlated_intervals(self):
        alternating = [100 + (1 if i % 2 else -1) for i in range(64)]
        runs = [100 + (1 if (i // 16) % 2 else -1) for i in range(64)]
        # Same values, same naive standard error; long runs are correlated.
        self.assertLess(batch_means_interval(alternating)[1], batch_means_interval(runs)[1])


class PerfJitTests(unittest.TestCase):
    def test_jit_share_counts_anonymous_leaves(self):
        report = parse_perf_script([
            " 10.1: 7f00 [unknown] ([unknown])\n",
            " 10.2: 7f01 [unknown] (//anon)\n",
            " 10.3: 4000 exec386 (/usr/bin/86Box)\n",
            " 10.4: 5000 codegen_block (/tmp/perf-42.map)\n",
        ], [(10, 11, 1)])
        self.assertEqual(report["1"]["jit_samples"], 3)
        self.assertEqual(report["1"]["jit_share"], .75)

    def test_leaf_stream_supplies_ip_for_empty_callchains(self):
        # DWARF unwinding yields no callchain rows for samples in JIT code.
        stacks = [" 10.1: \n", "\n",
                  " 10.2: \n", "\t4000 exec386 (/usr/bin/86Box)\n",
                  "\t3000 main (/usr/bin/86Box)\n", "\n"]
        leaves = [" 10.1:  7f00 [unknown] (/tmp/perf-42.map)\n",
                  " 10.2:  4000 exec386 (/usr/bin/86Box)\n"]
        without = parse_perf_script(stacks, [(10, 11, 1)])
        self.assertEqual(without["1"]["jit_samples"], 0)
        report = parse_perf_script(stacks, [(10, 11, 1)], leaves)
        self.assertEqual(report["1"]["samples"], 2)
        self.assertEqual(report["1"]["jit_samples"], 1)
        self.assertEqual(dict(report["1"]["leaf_hotpaths"])["exec386 (/usr/bin/86Box)"], 1)

    def test_record_targets_thread_and_starts_disabled(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            recorder = PerfRecorder(directory)
            process = Mock()
            process.poll.return_value = None
            with patch("workload_capture.shutil.which", return_value="/usr/bin/perf"), \
                    patch("workload_capture.subprocess.Popen", return_value=process), \
                    patch("workload_capture.time.sleep"):
                recorder.start(42, 43)
            command = recorder.command
            self.assertEqual(command[command.index("-F") + 1], "999")
            self.assertEqual(command[command.index("-t") + 1], "43")
            self.assertIn("dwarf,8192", command)
            self.assertIn("--control", command)
            self.assertEqual(command[command.index("--delay") + 1], "-1")
            self.assertFalse(recorder.enabled)
            # perf acknowledges each control command on the ack FIFO.
            os.write(recorder.ack, b"ack\n")
            recorder.enable()
            self.assertTrue(recorder.enabled)
            process.wait.return_value = 0
            recorder.stop()
            self.assertFalse((Path(directory) / "perf-ctl.fifo").exists())


class SerialGuest:
    """Fake 86Box automation: guest output scheduled by emulated ms, with
    `step`, `wait serial` (1 ms quanta) and `serial read` over one stream."""

    def __init__(self, events, wait_serial=True):
        self.events = sorted(events)
        self.ms = 0
        self.wait_serial = wait_serial
        self.commands = []
        self.writes = []
        self.cpu = 0.0

    def emitted(self):
        # Like a UART at 115200 baud: about 11 bytes per emulated ms.
        return b"".join(data.encode()[:max(0, (self.ms - ms + 1) * 11)]
                        for ms, data in self.events if ms <= self.ms)

    def request(self, command, allow_failure=False):
        self.commands.append(command)
        words = command.split(" ")
        if command.startswith("step "):
            self.ms += int(words[1])
            return {"ok": True, "ms": self.ms}
        if command.startswith("wait serial ") and self.wait_serial:
            timeout, start, text = int(words[2]), int(words[4]), " ".join(words[5:]).encode()
            for _ in range(timeout):
                self.ms += 1
                found = self.emitted().find(text, start)
                if found >= 0:
                    return {"ok": True, "ms": self.ms, "matched": True, "at": found,
                            "next": len(self.emitted())}
            return {"ok": True, "ms": self.ms, "matched": False, "next": len(self.emitted())}
        if command.startswith("wait serial "):
            return {"ok": False, "error": "unknown command"}
        if command.startswith("serial read "):
            start = int(words[3])
            data = self.emitted()
            return {"ok": True, "data": data[start:].hex(), "next": len(data)}
        if command.startswith("serial write "):
            self.writes.append(bytes.fromhex(words[3]))
        if command == "regs":
            return {"ok": True, "eip": 0x1234, "eflags": 0x2}
        return {"ok": True}

    def cpu_seconds(self, pid):
        self.cpu += .001
        return self.cpu


class RunLoopTests(unittest.TestCase):
    def run_guest(self, guest, **options):
        values = dict(wall_seconds=10, emulated_ms=10000, step_ms=250, expect=[],
                      reject=["FATAL"], label="", key=[], bios_f1=False)
        values.update(options)
        args = SimpleNamespace(**values)
        process = Mock(pid=42)
        process.poll.return_value = None
        api = Mock()
        api.request.side_effect = guest.request
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory, \
                patch("run_86box.subprocess.Popen", return_value=process), \
                patch("run_86box.socket.socket", return_value=Mock()), \
                patch("run_86box.Automation", return_value=api), \
                patch("run_86box.stop_process"), \
                patch("run_86box.host_metadata", return_value={}), \
                patch("run_86box.process_cpu_seconds", side_effect=guest.cpu_seconds):
            result = execute(args, Path(directory), ["86Box"], {})
            files = {path.name for path in Path(directory).iterdir()}
        return result, files

    SUITE = [(100, "CONTROL protocol=cpu1 ready=1\r\n"),
             (300, "CONTROL protocol=cpu1 accepted=1 model=unknown fpu=auto mode=benchmark\r\n"),
             (1010, marker("warmup")), (1515, marker("sample")), (3020, marker("end")),
             (3030, "TEST r=1 name=heavy status=PASS\r\n"
                    "SUMMARY tests=1 pass=1 fail=0 skip=0 fault=0\r\nDONE\r\n")]

    def test_wait_serial_stops_steps_at_work_lines(self):
        guest = SerialGuest(self.SUITE)
        result, _ = self.run_guest(guest)
        self.assertEqual(result["status"], "observed")
        self.assertEqual(result["boundaries"], "wait-serial")
        workload = result["workload_capture"]["workloads"][0]
        # 1515..3020 ms: only the 1 ms steps finishing each line are lost.
        self.assertGreaterEqual(workload["emulated_ms"], 1490)
        self.assertEqual(sum(row[2] for row in workload["step_series"]),
                         workload["emulated_ms"])
        self.assertIn("step 1", guest.commands)
        self.assertEqual(guest.writes[0], b"@cpu1 unknown auto benchmark\n")

    def test_fixed_steps_lose_boundary_steps_and_fallback_is_automatic(self):
        guest = SerialGuest(self.SUITE, wait_serial=False)
        result, _ = self.run_guest(guest)
        self.assertEqual(result["boundaries"], "step")
        self.assertIn("unavailable", result["boundary_note"])
        self.assertLessEqual(result["workload_capture"]["workloads"][0]["emulated_ms"], 1250)

    def test_selection_frame_sent_on_ready_and_verified(self):
        events = self.SUITE[:2] + [
            (400, "CONTROL protocol=sel1 ready=1\r\n"),
            (600, "CONTROL protocol=sel1 accepted=1 only=2 skip=0 from=-\r\n")] + self.SUITE[2:]
        guest = SerialGuest(events)
        result, _ = self.run_guest(guest, only=["heavy", "light"])
        self.assertIn(b"@sel1 heavy,light - -\n", guest.writes)
        self.assertTrue(result["selection"]["accepted"])
        self.assertEqual(result["status"], "observed")
        guest = SerialGuest(events)
        result, _ = self.run_guest(guest, skip=["other"])
        self.assertEqual(result["status"], "error")
        self.assertIn("did not accept", result["error"])

    def test_selection_without_guest_support_is_an_error_only_when_asked(self):
        result, _ = self.run_guest(SerialGuest(self.SUITE), resume_from="heavy")
        self.assertEqual(result["status"], "error")
        self.assertIn("does not offer runtime selection", result["error"])
        result, _ = self.run_guest(SerialGuest(self.SUITE))
        self.assertEqual(result["selection"]["state"], "unsupported")

    def test_silent_guest_is_reported_as_hang(self):
        guest = SerialGuest(self.SUITE[:4])
        result, _ = self.run_guest(guest, hang_seconds=2, emulated_ms=60000)
        self.assertEqual(result["status"], "hang")
        hang = result["hangs"][0]
        self.assertEqual(hang["name"], "heavy")
        self.assertFalse(hang["interrupts_enabled"])
        self.assertLess(result["emulated_ms"], 5000)

    def test_compact_transcript_and_text_polling(self):
        guest = SerialGuest(self.SUITE)
        result, files = self.run_guest(guest)
        self.assertNotIn("automation.jsonl", files)
        steps = sum(c.startswith(("step", "wait")) for c in guest.commands)
        self.assertLess(guest.commands.count("text"), steps)
        self.assertEqual(result["transcript"]["mode"], "compact")

    def test_interrupt_stops_emulator_restores_and_reraises(self):
        guest = SerialGuest(self.SUITE)

        def request(command, allow_failure=False):
            if command.startswith("serial read") and guest.ms > 500:
                raise KeyboardInterrupt
            return SerialGuest.request(guest, command, allow_failure)

        args = SimpleNamespace(wall_seconds=10, emulated_ms=10000, step_ms=250, expect=[],
                               reject=["FATAL"], label="", key=[], bios_f1=False)
        process = Mock(pid=42)
        process.poll.return_value = None
        api = Mock()
        api.request.side_effect = request
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory, \
                patch("run_86box.subprocess.Popen", return_value=process), \
                patch("run_86box.socket.socket", return_value=Mock()), \
                patch("run_86box.Automation", return_value=api), \
                patch("run_86box.os.killpg") as kill, \
                patch("run_86box.host_metadata", return_value={}), \
                patch("run_86box.process_cpu_seconds", side_effect=guest.cpu_seconds):
            with self.assertRaises(KeyboardInterrupt):
                execute(args, Path(directory), ["86Box"], {})
            result = json.loads((Path(directory) / "result.json").read_text())
        self.assertEqual(result["status"], "interrupted")
        self.assertIn("quit", guest.commands)
        kill.assert_called_with(42, signal.SIGTERM)
        self.assertNotIn(process, run_86box.LIVE_EMULATORS)

    def test_stop_all_emulators_kills_registered_process_groups(self):
        process = Mock(pid=77)
        process.poll.return_value = None
        process.wait.side_effect = [subprocess.TimeoutExpired("86Box", 3), 0]
        run_86box.LIVE_EMULATORS.add(process)
        with patch("run_86box.os.killpg") as kill:
            run_86box.stop_all_emulators()
        self.assertEqual(kill.call_args_list[-1].args, (77, signal.SIGKILL))
        self.assertNotIn(process, run_86box.LIVE_EMULATORS)

    def test_request_timeout_is_distinct_from_wall_deadline(self):
        connection = Mock()
        connection.recv.side_effect = TimeoutError("timed out")
        api = run_86box.Automation(connection, None, time.monotonic() + 60, request_seconds=0.01)
        with patch("run_86box.time.monotonic", side_effect=[0, 0, 0, 1, 1]):
            with self.assertRaisesRegex(run_86box.RequestTimeout, "request-seconds"):
                api.request("step 250")


class TranscriptTests(unittest.TestCase):
    def records(self, mode, max_bytes=1 << 20):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            transcript = run_86box.Transcript(Path(directory), mode, max_bytes)
            screen = {"ok": True, "chars": ["41" * 80] * 25, "attrs": ["07" * 80] * 25}
            for _ in range(3):
                transcript.record("text", screen)
                transcript.record("serial read 1 0", {"ok": True, "data": "41" * 100, "next": 100})
            transcript.record("text", {"ok": True, "chars": ["42" * 80] * 25})
            transcript.record("bad", {"ok": False, "error": "unknown command"})
            stats = transcript.close()
            rows = []
            for name in ("automation.1.jsonl.gz", "automation.jsonl.gz"):
                if (Path(directory) / name).exists():
                    with gzip.open(Path(directory) / name, "rt") as handle:
                        rows += [json.loads(line) for line in handle]
        return rows, stats

    def test_modes(self):
        full, _ = self.records("full")
        self.assertEqual(len(full), 8)
        compact, stats = self.records("compact")
        self.assertEqual([r["command"] for r in compact].count("text"), 2)
        self.assertNotIn("chars", compact[0]["reply"])
        self.assertEqual(compact[0]["reply"]["chars_bytes"], 2000)
        self.assertEqual(compact[1]["reply"]["data_bytes"], 100)
        self.assertEqual(stats["suppressed"], 2)
        self.assertLess(stats["uncompressed_bytes"] * 10, sum(len(json.dumps(r)) for r in full))
        errors, _ = self.records("errors")
        self.assertEqual([r["command"] for r in errors], ["bad"])
        off, stats = self.records("off")
        self.assertEqual((off, stats["path"]), ([], None))

    def test_rotation_caps_size(self):
        rows, stats = self.records("full", max_bytes=9000)
        self.assertGreater(stats["rotations"], 0)
        self.assertLess(len(rows), 8)


class SelectionTests(unittest.TestCase):
    def test_frame(self):
        self.assertEqual(run_86box.selection_frame(), b"@sel1 - - -\n")
        self.assertEqual(run_86box.selection_frame(["a", "b"], ["c"], "d.e"),
                         b"@sel1 a,b c d.e\n")
        with self.assertRaisesRegex(ValueError, "invalid record name"):
            run_86box.selection_frame(["a b"])
        with self.assertRaisesRegex(ValueError, "4096"):
            run_86box.selection_frame(["x" * 40] * 120)

    def test_cli(self):
        with patch("sys.argv", ["run_86box.py", "--profile", "mmx", "--only", "a,b",
                                "--only", "c", "--skip", "d", "--aa"]):
            args = arguments()
        self.assertEqual((args.only, args.skip, args.resume_from, args.repeat),
                         (["a", "b", "c"], ["d"], None, 2))
        self.assertEqual((args.transcript, args.boundaries, args.text_poll_ms),
                         ("compact", "wait-serial", 2000))
        with patch("sys.argv", ["run_86box.py", "--profile", "mmx", "--perf-stat"]), \
                patch("run_86box.is_linux", return_value=False), \
                patch("sys.stderr", io.StringIO()):
            with self.assertRaises(SystemExit):
                arguments()


class SignalCleanupTests(unittest.TestCase):
    """A real child process: run_main must stop the 'emulator' it started,
    by PID, when the runner gets SIGTERM, SIGHUP or SIGINT."""

    CHILD = """
import sys, time
import run_86box
from pathlib import Path

def main():
    process = run_86box.launch_emulator([sys.executable, "-c", "import time; time.sleep(60)"])
    try:
        print(process.pid, flush=True)
        time.sleep(60)
    finally:
        Path(sys.argv[1]).write_text("cleaned")
    return 0

sys.exit(run_86box.run_main(main))
"""

    def test_signals_stop_emulator_and_run_cleanup(self):
        tools = str(Path(run_86box.__file__).resolve().parent)
        for number in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            with self.subTest(signal=number.name), \
                    tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
                marker = Path(directory) / "marker"
                runner = subprocess.Popen([sys.executable, "-c", self.CHILD, str(marker)],
                                          stdout=subprocess.PIPE, text=True,
                                          env=dict(os.environ, PYTHONPATH=tools))
                try:
                    emulator = int(runner.stdout.readline())
                    os.kill(runner.pid, number)
                    runner.wait(timeout=10)
                finally:
                    if runner.poll() is None:
                        runner.kill()
                        runner.wait()
                self.assertEqual(marker.read_text(), "cleaned")
                self.assertNotEqual(runner.returncode, 0)
                with self.assertRaises(ProcessLookupError):
                    os.kill(emulator, 0)

    @unittest.skipUnless(Path("/proc/self/status").exists(), "needs /proc")
    def test_emulator_does_not_inherit_blocked_signals(self):
        process = run_86box.launch_emulator([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            time.sleep(.2)
            status = Path(f"/proc/{process.pid}/status").read_text()
            blocked = int(re.search(r"SigBlk:\s*([0-9a-f]+)", status).group(1), 16)
            for number in run_86box.CLEANUP_SIGNALS:
                self.assertFalse(blocked & (1 << (number - 1)), number)
        finally:
            run_86box.stop_all_emulators()
        self.assertEqual(process.returncode, -signal.SIGTERM)
