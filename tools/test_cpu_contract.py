"""CPU control protocol and correctness/performance separation."""
import copy
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from perf_report import correctness_changes, measurement_notes, runtime_breakdown, summarize, validate_run
from qemu_smoke import parse_log
from run_86box import cpu_control_accepted, cpu_control_frame, execute
from test_perf_report import example


class CpuContractTests(unittest.TestCase):
    def test_configured_identity_is_not_derived_from_cpuid(self):
        frame = cpu_control_frame({"cpu_family": "k6_3", "fpu_type": "internal"}, True)
        self.assertEqual(frame, b"@cpu1 k6_3 internal accuracy\n")

    def test_defaults_do_not_guess_a_model_or_fpu(self):
        self.assertEqual(cpu_control_frame({}), b"@cpu1 unknown auto benchmark\n")

    def test_invalid_identifiers_are_rejected(self):
        for value in ("", "k6_3\n", "a" * 48, "a b", "k6_3\N{NO-BREAK SPACE}"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                cpu_control_frame({"cpu_family": value})
        with self.assertRaises(ValueError):
            cpu_control_frame({"fpu_type": "a" * 16})

    def test_acknowledgement_must_match_model_fpu_and_mode(self):
        run = parse_log(
            "CONTROL protocol=cpu1 ready=1\r\n"
            "CONTROL protocol=cpu1 accepted=1 model=k6_3 fpu=internal mode=accuracy\r\n"
            "CONFORMANCE model=k6_3 source=configured catalogue=known coverage=partial "
            "required=0400140f mode=accuracy\r\n")
        machine = {"cpu_family": "k6_3", "fpu_type": "internal"}
        self.assertTrue(cpu_control_accepted(run, machine, True))
        self.assertFalse(cpu_control_accepted(run, machine, False))
        self.assertFalse(cpu_control_accepted(run, {**machine, "cpu_family": "k6_2"}, True))
        self.assertFalse(cpu_control_accepted(run, {**machine, "fpu_type": "none"}, True))
        self.assertFalse(cpu_control_accepted({}, machine, True))
        self.assertEqual(run["conformance"][0]["coverage"], "partial")

    def test_accuracy_results_are_not_benchmark_samples(self):
        run = example()
        for test in run["tests"]:
            test["mode"] = "accuracy"
        report = summarize([run], 0)
        workload = report["workloads"]["heavy"]
        self.assertIsNone(workload["counter_per_op"])
        self.assertIsNone(workload["ms_per_op"])
        self.assertEqual(workload["statuses"], {"PASS": 4})
        self.assertEqual(measurement_notes(run["tests"][0]), ["accuracy_only"])
        failed = {**run["tests"][0], "status": "FAIL"}
        self.assertEqual(measurement_notes(failed), ["accuracy_only", "correctness_fail"])

    def test_different_contracts_cannot_be_pooled(self):
        a = example()
        a["conformance"] = [{"model": "k6_3", "source": "configured", "coverage": "partial"}]
        b = copy.deepcopy(a)
        b["conformance"][0]["model"] = "k6_2"
        with self.assertRaisesRegex(ValueError, "mixed environments"):
            summarize([a, b], 0)

    def test_comparison_retains_changed_failure_and_unexpected_skip_without_policy(self):
        baseline = {"correctness_samples": [[
            {"r": "1", "name": "a", "status": "FAIL", "first": "x", "got": "1", "want": "2"},
            {"r": "1", "name": "b", "status": "PASS"},
        ]]}
        current = copy.deepcopy(baseline)
        current["correctness_samples"][0][0]["got"] = "3"
        current["correctness_samples"][0][1]["status"] = "SKIP"
        changes = correctness_changes(current, baseline)
        self.assertEqual([r["name"] for r in changes["changes"]], ["a", "b"])
        self.assertEqual(changes["changes"][0]["before"][0]["got"], "1")
        self.assertEqual(changes["changes"][0]["after"][0]["got"], "3")
        self.assertFalse(correctness_changes(current, {})["available"])

    def run_capture(self, chunks, expect=(), step_error=None, accuracy=True):
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(accuracy=accuracy, wall_seconds=10,
                                   emulated_ms=len(chunks) * 250, step_ms=250,
                                   expect=list(expect), reject=["FATAL"], label="",
                                   key=[], bios_f1=False)
            commands = []
            state = {"ms": 0, "serial": 0, "cpu": 0}
            machine = {"cpu_family": "k6_3", "fpu_type": "internal"}

            def request(command, allow_failure=False):
                commands.append(command)
                if command.startswith("step "):
                    if step_error:
                        raise step_error
                    state["ms"] += int(command.split()[1])
                    return {"ok": True, "ms": state["ms"]}
                if command.startswith("serial read "):
                    data = chunks[state["serial"]].encode()
                    state["serial"] += 1
                    return {"ok": True, "data": data.hex(), "next": state["serial"]}
                return {"ok": True}

            def cpu_seconds(pid):
                state["cpu"] += .01
                return state["cpu"]

            process = Mock(pid=42)
            process.poll.return_value = None
            api = Mock()
            api.request.side_effect = request
            with patch("run_86box.start_display", return_value=(Mock(), ":999")), \
                    patch("run_86box.subprocess.Popen", return_value=process), \
                    patch("run_86box.socket.socket", return_value=Mock()), \
                    patch("run_86box.Automation", return_value=api), \
                    patch("run_86box.stop_process"), \
                    patch("run_86box.host_metadata", return_value={}), \
                    patch("run_86box.process_cpu_seconds", side_effect=cpu_seconds):
                result = execute(args, Path(directory), ["86Box"], machine)
            return result, commands

    def test_accuracy_handshake_survives_fragmentation_and_crlf(self):
        result, commands = self.run_capture([
            "CONTROL protocol=cpu1 rea",
            "dy=1\r\n",
            "CONTROL protocol=cpu1 accepted=1 model=k6_3 fpu=internal mode=accuracy\r\n"
            "WORK phase=warmup r=1 id=1 name=cpu_model_contract\r\n"
            "WORK phase=sample r=1 id=1 name=cpu_model_contract\r\n",
            "TEST r=1 name=cpu_model_contract status=PASS\r\n"
            "SUMMARY tests=1 pass=1 fail=0 skip=0 fault=0\r\nDONE\r\n",
        ], expect=["ready=1"])
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["emulated_ms"], 1000)
        writes = [c for c in commands if c.startswith("serial write ")]
        self.assertEqual(writes, ["serial write 1 " +
                                 b"@cpu1 k6_3 internal accuracy\n".hex()])

    def test_accuracy_guest_failure_is_not_observation_success(self):
        result, _ = self.run_capture([
            "CONTROL protocol=cpu1 ready=1\r\n",
            "CONTROL protocol=cpu1 accepted=1 model=k6_3 fpu=internal mode=accuracy\r\n"
            "TEST r=1 name=cpu_model_contract status=FAIL\r\n"
            "SUMMARY tests=1 pass=0 fail=1 skip=0 fault=0\r\nDONE\r\n",
        ])
        self.assertEqual(result["status"], "failed")
        self.assertFalse(result["correctness"]["passed"])

    def test_timeout_remains_incomplete_not_architectural_failure(self):
        result, _ = self.run_capture(["BIOS still starting\r\n"])
        self.assertEqual(result["status"], "timeout")
        self.assertFalse(result["correctness"]["complete"])
        self.assertNotIn("error", result)

    def test_legacy_image_cannot_silently_accept_accuracy_mode(self):
        result, _ = self.run_capture([
            "TEST r=1 name=cpu_model_contract status=PASS\r\n"
            "SUMMARY tests=1 pass=1 fail=0 skip=0 fault=0\r\nDONE\r\n",
        ])
        self.assertEqual(result["status"], "error")
        self.assertIn("did not acknowledge", result["error"])
        self.assertFalse(result["correctness"]["passed"])

    def test_wall_deadline_is_not_an_architectural_failure(self):
        result, _ = self.run_capture([""], step_error=TimeoutError("wall-clock deadline exceeded"))
        self.assertEqual(result["status"], "timeout")
        self.assertFalse(result["correctness"]["complete"])
        self.assertFalse(result["correctness"]["passed"])
        self.assertEqual(result["tests"], [])

    def test_correctness_only_build_is_recognized_without_cli_override(self):
        result, commands = self.run_capture([
            'BUILD version=0.1.0 compiler="GCC 16" benchmarks=0\r\n'
            "CONTROL protocol=cpu1 ready=1\r\n",
            "CONTROL protocol=cpu1 accepted=1 model=k6_3 fpu=internal mode=accuracy\r\n"
            "WORK phase=warmup r=1 id=1 name=cpu_model_contract\r\n"
            "WORK phase=sample r=1 id=1 name=cpu_model_contract\r\n",
            "TEST r=1 name=cpu_model_contract status=PASS\r\n"
            "SUMMARY tests=1 pass=1 fail=0 skip=0 fault=0\r\nDONE\r\n",
        ], expect=["ready=1"], accuracy=False)
        self.assertEqual(result["mode"], "accuracy")
        self.assertEqual(result["emulated_ms"], 750)
        self.assertTrue(result["correctness"]["complete"])
        self.assertEqual([c for c in commands if c.startswith("serial write ")],
                         ["serial write 1 " + b"@cpu1 k6_3 internal accuracy\n".hex()])


@unittest.skipUnless(os.environ.get("CPUBURN_CONTRACT_IMAGE"),
                     "set CPUBURN_CONTRACT_IMAGE to the documented focused guest image")
class CpuContractGuestTests(unittest.TestCase):
    def run_guest(self, cpu, frame):
        executable = shutil.which("qemu-system-i386")
        if not executable:
            self.fail("qemu-system-i386 is required for requested guest tests")
        variable = getattr(self, "image_variable", "CPUBURN_CONTRACT_IMAGE")
        image = Path(os.environ[variable]).resolve(strict=True)
        with tempfile.TemporaryDirectory(prefix="cpu-contract-") as directory:
            serial = Path(directory) / "serial"
            with (Path(directory) / "qemu.log").open("w+") as log:
                process = subprocess.Popen([
                    executable, "-accel", "tcg", "-cpu", cpu, "-m", "16",
                    "-drive", f"file={image},format=raw,if=floppy,readonly=on",
                    "-boot", "a", "-display", "none", "-monitor", "none",
                    "-serial", f"unix:{serial},server=on,wait=on",
                    "-no-reboot", "-no-shutdown",
                ], stdout=log, stderr=log)
                data, sent, text = b"", False, ""
                deadline = time.monotonic() + 120
                try:
                    while not serial.exists():
                        if process.poll() is not None or time.monotonic() >= deadline:
                            log.seek(0)
                            self.fail("QEMU startup failed: " + log.read())
                        time.sleep(.02)
                    with socket.socket(socket.AF_UNIX) as connection:
                        connection.connect(str(serial))
                        while time.monotonic() < deadline:
                            if not select.select([connection], [], [], .1)[0]:
                                self.assertIsNone(process.poll(), "QEMU exited during guest tests")
                                continue
                            chunk = connection.recv(65536)
                            self.assertTrue(chunk, "guest serial disconnected")
                            data += chunk
                            text = data.decode("cp437").replace("\r", "")
                            if not sent and frame and "CONTROL protocol=cpu1 ready=1\n" in text:
                                connection.sendall(frame.encode())
                                sent = True
                            if "\nDONE\n" in text:
                                break
                        else:
                            self.fail("guest test incomplete at host deadline:\n" + text)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
        run = parse_log(data.decode("cp437"))
        run["done"] = True
        validate_run(run)
        runtime_breakdown(run)
        return run

    def check_statuses(self, run, contract, mmx, now):
        self.assertEqual({t["name"]: t["status"] for t in run["tests"]}, {
            "cpu_model_contract": contract, "int_partial_boundaries": "PASS",
            "mmx_alu": mmx, "3dnow_float": now,
        }, "use the documented focused image; unexpected guest outcomes must not be hidden")

    def test_known_model(self):
        run = self.run_guest("pentium2", "@cpu1 pentium2_deschutes internal accuracy\n")
        # TCG masks VME even when +vme is requested. That is a genuine
        # missing Pentium II feature, not a reason to weaken its contract.
        self.check_statuses(run, "FAIL", "PASS", "SKIP")
        self.assertIn("vme", [f["name"] for f in run["features"]
                             if f["required"] == "1" and f["detected"] == "0"])
        mmx = next(f for f in run["features"] if f["name"] == "mmx")
        self.assertEqual(mmx["skip"], "0")

    def test_known_486_does_not_run_mmx(self):
        run = self.run_guest("486", "@cpu1 idx4 internal accuracy\n")
        self.check_statuses(run, "PASS", "SKIP", "SKIP")

    def test_signature_is_checked_independently_of_family(self):
        run = self.run_guest("486", "@cpu1 idx4 internal accuracy 00000481\n")
        self.check_statuses(run, "FAIL", "SKIP", "SKIP")
        contract = run["tests"][0]
        self.assertEqual(contract["got"], "00000480")
        self.assertEqual(contract["want"], "00000481")
        self.assertEqual(run["cpu_control"][-1]["signature"], "00000481")

    def test_hidden_feature_still_executes_and_identification_fails(self):
        run = self.run_guest("pentium2,-mmx", "@cpu1 pentium2_deschutes internal accuracy\n")
        # QEMU still executes MMX when its CPUID bit is hidden.
        self.check_statuses(run, "FAIL", "PASS", "SKIP")

    def test_required_unsupported_extension_faults_instead_of_skipping(self):
        run = self.run_guest("pentium2", "@cpu1 k6_3 internal accuracy\n")
        self.check_statuses(run, "FAIL", "PASS", "FAULT")

    def test_unknown_cpu_remains_unverified(self):
        run = self.run_guest("486,-fpu", "@cpu1 future_cpu auto accuracy\n")
        # TCG's 486 also executes MMX despite not advertising it. Unknown
        # models use guarded availability probes, not its masked CPUID bit.
        self.check_statuses(run, "SKIP", "PASS", "SKIP")
        self.assertTrue(any(f["name"] == "mmx" and f["probed"] == "1"
                            for f in run["features"]))
        self.assertEqual(run["conformance"][0]["catalogue"], "unknown")

    def test_standalone_autodetection(self):
        run = self.run_guest("pentium2", None)
        self.check_statuses(run, "FAIL", "PASS", "SKIP")
        self.assertEqual(run["conformance"][0]["source"], "detected")

    def test_build_controls_whether_benchmark_mode_is_available(self):
        run = self.run_guest("pentium2", "@cpu1 pentium2_deschutes internal benchmark\n")
        self.check_statuses(run, "FAIL", "PASS", "SKIP")
        expected = "benchmark" if run["build_info"][0]["benchmarks"] == "1" else "accuracy"
        self.assertEqual(run["conformance"][0]["mode"], expected)
        self.assertEqual(run["cpu_control"][-1]["mode"], expected)


@unittest.skipUnless(os.environ.get("CPUBURN_EXTENSION_IMAGE"),
                     "set CPUBURN_EXTENSION_IMAGE to ONLY=pm_syscall,mmx_extended")
class CpuExtensionGuestTests(unittest.TestCase):
    image_variable = "CPUBURN_EXTENSION_IMAGE"
    run_guest = CpuContractGuestTests.run_guest

    def test_amd_legacy_syscall_and_mmx_extensions(self):
        run = self.run_guest("phenom", "@cpu1 unknown auto accuracy\n")
        self.assertEqual({t["name"]: t["status"] for t in run["tests"]}, {
            "cpu_model_contract": "SKIP", "pm_syscall": "PASS", "mmx_extended": "PASS",
        })
        self.assertTrue(all(int(t.get("iterations", 0)) <= 1 for t in run["tests"]))
        self.assertEqual({f["name"]: f["status"] for f in run["features"]
                          if f["name"] in ("syscall", "mmxext")},
                         {"syscall": "PASS", "mmxext": "PASS"})

    def test_unrelated_intel_cpu_skips_vendor_extensions(self):
        run = self.run_guest("pentium2", "@cpu1 pentium2_deschutes internal accuracy\n")
        self.assertEqual({t["name"]: t["status"] for t in run["tests"]}, {
            "cpu_model_contract": "FAIL", "pm_syscall": "SKIP", "mmx_extended": "SKIP",
        })


if __name__ == "__main__":
    unittest.main()
