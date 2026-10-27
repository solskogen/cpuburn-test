"""Step-only, whole-emulator CPU accounting and optional Linux perf capture.

WORK boundaries are observed after a step, not at the guest instruction that
printed them. Mixed/partial boundary steps are therefore never attributed.
Ratios describe unthrottled speed capacity, not the GUI's reported actual speed.
"""

from collections import Counter
import bisect
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import time


WORK = re.compile(r"WORK phase=(warmup|sample|end) r=([1-9][0-9]*) "
                  r"id=([0-9]+) name=(\S+)")


def process_cpu_seconds(pid):
    """Linux /proc/PID/stat utime+stime includes all threads, not just main."""
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def host_metadata():
    cpu = platform.processor()
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith(("model name", "Model", "Hardware")) and ":" in line:
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    return {"platform": platform.platform(), "kernel": platform.release(),
            "architecture": platform.machine(), "python": platform.python_version(),
            "cpu": cpu, "logical_cpus": os.cpu_count(),
            "process_cpu_scope": "total_process_all_threads",
            "clock": "CLOCK_MONOTONIC", "clock_ticks_per_second": os.sysconf("SC_CLK_TCK")}


class WorkloadCapture:
    def __init__(self, sample_cpu_seconds=30.0, step_ms=250):
        if not math.isfinite(sample_cpu_seconds) or sample_cpu_seconds <= 0:
            raise ValueError("sample CPU seconds must be finite and positive")
        self.target = sample_cpu_seconds
        self.step_ms = step_ms
        self.records = []
        self.active = None
        self.buffer = ""
        self.pending = None
        self.last_id = -1
        self.last_lease_ms = None
        self.warnings = []
        self.unattributed = {"steps": 0, "step_wall_seconds": 0.0,
                             "process_cpu_seconds": 0.0, "emulated_ms": 0}
        self.windows = []

    def observe_step(self, before, after):
        """Snapshots are (monotonic seconds, process CPU seconds, guest ms)."""
        if self.pending is not None:
            raise RuntimeError("feed serial after each observed step")
        self.pending = (before, after, self.active)

    def _close(self, boundary, reason):
        if self.active is not None:
            self.active["end_boundary"] = boundary
            self.active["end_reason"] = reason
            self.active = None
            self.last_lease_ms = None

    def feed(self, data, emulated_ms, monotonic=None):
        """Consume fragmented COM1 bytes and return lease commands as ASCII."""
        commands = []
        boundary = {"monotonic_seconds": monotonic, "emulated_ms": emulated_ms,
                    "uncertainty_emulated_ms": self.step_ms}
        if self.pending is not None:
            boundary["step_monotonic_start"] = self.pending[0][0]
            boundary["step_monotonic_end"] = self.pending[1][0]
            boundary["uncertainty_emulated_ms"] = self.pending[1][2] - self.pending[0][2]
        self.buffer += data.decode("cp437") if isinstance(data, bytes) else data
        changed = False
        while "\n" in self.buffer:
            line, self.buffer = self.buffer.split("\n", 1)
            line = line.rstrip("\r")
            match = WORK.fullmatch(line)
            if match:
                phase, round_number, identifier, name = match.groups()
                identifier, round_number = int(identifier), int(round_number)
                if identifier > 0xFFFFFFFF:
                    self.warnings.append("invalid_workload_id")
                    continue
                if phase in ("warmup", "sample"):
                    following_warmup = (
                        phase == "sample" and self.active is not None
                        and self.active["phase"] == "warmup"
                        and (identifier, round_number, name) ==
                        (self.active["id"], self.active["r"], self.active["name"]))
                    if identifier <= self.last_id and not following_warmup:
                        self.warnings.append(f"stale_sample_id:{identifier}")
                        continue
                    changed = True
                    self._close(boundary, "sample_start" if following_warmup
                                else "new_workload_without_end")
                    self.last_id = identifier
                    self.active = {
                        "id": identifier, "r": round_number, "name": name, "phase": phase,
                        "start_boundary": boundary, "end_boundary": None,
                        "end_reason": None, "steps": 0, "step_wall_seconds": 0.0,
                        "process_cpu_seconds": 0.0, "emulated_ms": 0,
                        "lease_released": False, "invalid_steps": 0,
                    }
                    self.records.append(self.active)
                    if phase == "sample":
                        self.last_lease_ms = emulated_ms
                        commands.append(f"+{identifier:08x}\n")
                elif (self.active is not None and identifier == self.active["id"]
                      and round_number == self.active["r"] and name == self.active["name"]):
                    changed = True
                    self._close(boundary, "end")
                else:
                    self.warnings.append(f"stale_end_id:{identifier}")
            elif line.startswith("WORK "):
                self.warnings.append("malformed_work_marker")
                changed = True
                self._close(boundary, "malformed_marker")
            elif line == "DONE" or line.startswith(("FATAL", "BEGIN ", "FAULT")):
                changed = True
                self._close(boundary, line.split()[0].lower())
            elif line.startswith("TEST ") and self.active is not None:
                changed = True
                self._close(boundary, "test_without_end")
        if len(self.buffer) > 65536:
            raise ValueError("unterminated COM1 line exceeds 65536 bytes")
        if self.pending is not None:
            before, after, original = self.pending
            self.pending = None
            wall, cpu, guest = (after[i] - before[i] for i in range(3))
            valid = (all(math.isfinite(x) for x in (*before, *after))
                     and wall > 0 and cpu >= 0 and guest > 0)
            if not valid:
                self.warnings.append("invalid_step_clock")
                if original is not None:
                    original["invalid_steps"] += 1
            else:
                attributable = (original is not None and original is self.active
                                and not changed and not self.buffer)
                entry = original if attributable else self.unattributed
                entry["steps"] += 1
                entry["step_wall_seconds"] += wall
                entry["process_cpu_seconds"] += cpu
                entry["emulated_ms"] += guest
                if attributable and original["phase"] == "sample":
                    self.windows.append((before[0], after[0], original["id"]))
        if self.active is not None and self.active["phase"] == "sample":
            identifier = self.active["id"]
            if self.active["process_cpu_seconds"] >= self.target:
                if (not self.active["lease_released"] or self.last_lease_ms is None
                        or emulated_ms - self.last_lease_ms >= 1000):
                    commands.append(f"-{identifier:08x}\n")
                    self.last_lease_ms = emulated_ms
                self.active["lease_released"] = True
            elif self.last_lease_ms is None or emulated_ms - self.last_lease_ms >= 1000:
                commands.append(f"+{identifier:08x}\n")
                self.last_lease_ms = emulated_ms
        return commands

    def finish(self, emulated_ms, monotonic):
        self._close({"monotonic_seconds": monotonic, "emulated_ms": emulated_ms,
                     "uncertainty_emulated_ms": self.step_ms}, "capture_stopped")

    def report(self):
        records = []
        aggregates = {}
        for entry in self.records:
            entry = dict(entry)
            guest_seconds = entry["emulated_ms"] / 1000
            entry["guest_seconds_per_step_wall_second"] = (
                guest_seconds / entry["step_wall_seconds"] if entry["step_wall_seconds"] else None)
            entry["guest_seconds_per_process_cpu_second"] = (
                guest_seconds / entry["process_cpu_seconds"] if entry["process_cpu_seconds"] else None)
            entry["full_speed_capacity_percent"] = (
                entry["guest_seconds_per_step_wall_second"] * 100
                if entry["step_wall_seconds"] else None)
            entry["cpu_full_speed_capacity_percent"] = (
                entry["guest_seconds_per_process_cpu_second"] * 100
                if entry["process_cpu_seconds"] else None)
            entry["under_sampled"] = (entry["process_cpu_seconds"] < self.target
                                      if entry["phase"] == "sample" else None)
            records.append(entry)
            if entry["phase"] == "sample":
                group = aggregates.setdefault(entry["name"], {
                    "samples": 0, "under_sampled": 0, "step_wall_seconds": 0.0,
                    "process_cpu_seconds": 0.0, "emulated_ms": 0})
                group["samples"] += 1
                group["under_sampled"] += int(entry["under_sampled"])
                for key in ("step_wall_seconds", "process_cpu_seconds", "emulated_ms"):
                    group[key] += entry[key]
        for group in aggregates.values():
            group["guest_seconds_per_step_wall_second"] = (
                group["emulated_ms"] / 1000 / group["step_wall_seconds"]
                if group["step_wall_seconds"] else None)
            group["guest_seconds_per_process_cpu_second"] = (
                group["emulated_ms"] / 1000 / group["process_cpu_seconds"]
                if group["process_cpu_seconds"] else None)
        return {
            "sample_cpu_seconds_target": self.target,
            "cpu_scope": "total_process_all_threads",
            "measurement": "unthrottled_step_only_speed_capacity_not_actual_speed",
            "boundary_policy": "exclude_mixed_and_partial_line_steps",
            "boundary_uncertainty_emulated_ms": self.step_ms,
            "workloads": [entry for entry in records if entry["phase"] == "sample"],
            "phases": records, "aggregates": aggregates,
            "unattributed": dict(self.unattributed),
            "warnings": list(self.warnings) + [
                f"under_sampled:{entry['id']}" for entry in records if entry["under_sampled"]],
            "availability": ("captured" if any(e["phase"] == "sample" for e in records)
                             else "unavailable_no_sample_markers"),
        }


def parse_perf_script(lines, windows):
    """Attribute leaf and inclusive stacks only to interior step intervals."""
    starts = [window[0] for window in windows]
    counts = {}
    current = None
    stack = set()
    leaf_pending = False

    def symbol_text(text):
        text = text.strip()
        if "[unknown]" in text:
            return text
        return re.sub(r"^[0-9a-fA-F]+\s+", "", text)

    def flush():
        if current is not None:
            counts[current]["inclusive"].update(stack)

    for line in lines:
        match = re.match(r"^\s*(\d+\.\d+):\s+(.*)$", line)
        if match:
            flush()
            current, stack, leaf_pending = None, set(), False
            stamp = float(match[1])
            symbol = symbol_text(match[2])
            index = bisect.bisect_right(starts, stamp) - 1
            if index >= 0 and stamp < windows[index][1]:
                current = windows[index][2]
                entry = counts.setdefault(current, {"samples": 0, "leaf": Counter(),
                                                    "inclusive": Counter()})
                entry["samples"] += 1
                if symbol:
                    entry["leaf"][symbol] += 1
                    stack.add(symbol)
                else:
                    leaf_pending = True
        elif current is not None and line.strip():
            # perf's callchain rows start with an IP, followed by symbol/DSO.
            symbol = symbol_text(line)
            stack.add(symbol)
            if leaf_pending:
                counts[current]["leaf"][symbol] += 1
                leaf_pending = False
    flush()
    return {str(key): {"samples": value["samples"],
                      "leaf_hotpaths": value["leaf"].most_common(30),
                      "inclusive_hotpaths": value["inclusive"].most_common(30)}
            for key, value in counts.items()}


class PerfRecorder:
    def __init__(self, run_dir):
        self.directory = Path(run_dir)
        self.process = None
        self.command = None

    def _error(self, reason):
        log = self.directory / "perf-record.log"
        details = log.read_text(errors="replace") if log.exists() else ""
        return RuntimeError(f"{reason}; perf permissions/events must be available. "
                            f"See {log}: {details[-4000:]}")

    def start(self, pid):
        executable = shutil.which("perf")
        if executable is None:
            raise RuntimeError("--perf requested but perf is not installed")
        self.command = [executable, "record", "-F", "99", "-g", "-p", str(pid),
                        "--mmap-pages", "64",
                        "-o", str(self.directory / "perf.data"), "--timestamp",
                        "--clockid", "mono"]
        with (self.directory / "perf-record.log").open("wb") as log:
            self.process = subprocess.Popen(self.command, stdout=log, stderr=log)
        time.sleep(0.25)
        self.check()

    def check(self):
        if self.process is not None and self.process.poll() is not None:
            raise self._error(f"perf record exited unexpectedly ({self.process.returncode})")

    def stop(self):
        if self.process is None:
            return
        self.check()
        self.process.send_signal(signal.SIGINT)
        try:
            code = self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise self._error("perf did not flush within 15 seconds")
        if code not in (0, -signal.SIGINT, 128 + signal.SIGINT):
            raise self._error(f"perf record failed ({code})")

    def report(self, windows):
        data = self.directory / "perf.data"
        if not data.is_file() or data.stat().st_size == 0:
            raise self._error("perf record produced no nonempty perf.data")
        script = self.directory / "perf.script"
        command = [self.command[0], "script", "--ns", "-i",
                   str(self.directory / "perf.data"), "-F", "time,ip,sym,dso"]
        with script.open("w") as output, \
                (self.directory / "perf-script.log").open("w") as errors:
            result = subprocess.run(command, stdout=output, stderr=errors)
        if result.returncode:
            raise RuntimeError("perf script failed; see perf-script.log")
        with script.open() as source:
            hotpaths = parse_perf_script(source, windows)
        return {"status": "captured" if hotpaths else "no_attributed_samples",
                "command": self.command, "script_command": command,
                "clock": "CLOCK_MONOTONIC", "workloads_by_id": hotpaths,
                "interior_step_windows": windows,
                "data": str(self.directory / "perf.data"), "script": str(script)}
