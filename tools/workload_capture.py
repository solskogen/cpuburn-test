"""Step-only CPU accounting of the emulation thread and optional Linux perf capture.

WORK boundaries are observed after a step, not at the guest instruction that
printed them. Mixed/partial boundary steps are therefore never attributed; the
runner stops its steps at WORK lines (automation `wait serial`) so that they
are short. Ratios describe unthrottled speed capacity, not the GUI's reported
actual speed.
"""

from collections import Counter
import bisect
import ctypes
import math
import os
from pathlib import Path
import platform
import re
import select
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import time


WORK = re.compile(r"WORK phase=(warmup|sample|end) r=([1-9][0-9]*) "
                  r"id=([0-9]+) name=(\S+)")
# Lease frames go out at most once per feed, only for the sample still active
# then, and are repeated well inside the guest's 1 s minimum: a stale frame
# queued with the current one can overrun the 16-byte UART FIFO.
LEASE_RENEW_MS = 250
# 86Box names its emulation thread (src/qt/qt_main.cpp, main_thread_fn).
EMULATION_THREAD = "main_thread"


def is_linux():
    return sys.platform.startswith("linux")


def require_linux(feature):
    """Fail clearly where a feature depends on Linux /proc, perf or affinity."""
    if not is_linux():
        raise RuntimeError(f"{feature} requires Linux (/proc, perf_event, sched_setaffinity); "
                           f"this host is {sys.platform}")


def process_cpu_seconds(pid):
    """Linux /proc/PID/stat utime+stime includes all threads, not just main."""
    require_linux("process CPU accounting")
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def find_thread(pid, name=EMULATION_THREAD):
    """The TID of the first thread of PID whose comm is `name`, or None."""
    if not is_linux():
        return None
    try:
        tasks = sorted(Path(f"/proc/{pid}/task").iterdir(), key=lambda p: int(p.name))
    except (OSError, ValueError):
        return None
    for task in tasks:
        try:
            if (task / "comm").read_text().strip() == name:
                return int(task.name)
        except (OSError, ValueError):
            continue
    return None


def parse_schedstat(text):
    """CPU seconds from /proc/.../schedstat: on-CPU nanoseconds, run delay, slices."""
    fields = text.split()
    if len(fields) < 3:
        raise ValueError(f"malformed schedstat: {text!r}")
    return int(fields[0]) / 1e9


def thread_cpu_seconds(pid, tid):
    """(seconds, source) for one thread: schedstat in ns, else stat in ticks."""
    base = Path(f"/proc/{pid}/task/{tid}")
    try:
        return parse_schedstat((base / "schedstat").read_text()), "schedstat_ns"
    except (OSError, ValueError):
        fields = (base / "stat").read_text().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"), "stat_ticks"


class EmulationClock:
    """CPU time of 86Box's emulation thread, with clean fallbacks.

    locate() is called once, after the first automation reply (requests run on
    the emulation thread, so it exists by then); the scope is then fixed for the
    whole run, so no step mixes two clocks. Without the named thread the whole
    process's /proc/PID/stat (all threads, 10 ms ticks) is used; on hosts
    without /proc CPU time is unavailable and reads as a constant 0."""

    def __init__(self, pid, fallback=None):
        self.pid = pid
        self.tid = None
        self.fallback = fallback or (lambda: process_cpu_seconds(pid))
        self.scope = "process_all_threads_stat_ticks" if is_linux() else "unavailable"
        self.note = None if is_linux() else "CPU time needs Linux /proc; capacity per CPU second unavailable"

    def locate(self):
        if not is_linux():
            return self.scope
        tid = find_thread(self.pid)
        if tid is not None:
            try:
                _, source = thread_cpu_seconds(self.pid, tid)
            except (OSError, ValueError):
                tid = None
        if tid is None:
            self.note = (f"no thread named {EMULATION_THREAD}; charging all emulator "
                         "threads at 10 ms resolution")
            return self.scope
        self.tid = tid
        self.scope = "emulation_thread_" + source
        return self.scope

    def cpu_seconds(self):
        if self.scope == "unavailable":
            return 0.0
        if self.tid is not None:
            return thread_cpu_seconds(self.pid, self.tid)[0]
        return self.fallback()


def _read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def cpu_boost_state():
    """'on'/'off' for AMD/acpi-cpufreq boost or intel_pstate turbo, else None."""
    boost = _read("/sys/devices/system/cpu/cpufreq/boost")
    if boost in ("0", "1"):
        return "on" if boost == "1" else "off"
    no_turbo = _read("/sys/devices/system/cpu/intel_pstate/no_turbo")
    if no_turbo in ("0", "1"):
        return "off" if no_turbo == "1" else "on"
    return None


def core_state(core):
    """Volatile frequency state of one logical CPU (kHz), or {} without sysfs."""
    base = f"/sys/devices/system/cpu/cpu{int(core)}/cpufreq/"
    state = {"core": int(core)}
    for key in ("scaling_cur_freq", "scaling_max_freq", "cpuinfo_max_freq"):
        value = _read(base + key)
        if value and value.isdigit():
            state[key + "_khz"] = int(value)
    governor = _read(base + "scaling_governor")
    if governor:
        state["governor"] = governor
    return state


def host_metadata(pin=None):
    """Stable host description; volatile frequencies go in host_state()."""
    cpu = platform.processor()
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith(("model name", "Model", "Hardware")) and ":" in line:
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    core = pin if pin is not None else 0
    return {"platform": platform.platform(), "kernel": platform.release(),
            "architecture": platform.machine(), "python": platform.python_version(),
            "cpu": cpu, "logical_cpus": os.cpu_count(), "pinned_core": pin,
            "governor": core_state(core).get("governor") if is_linux() else None,
            "boost": cpu_boost_state() if is_linux() else None,
            "clock": "CLOCK_MONOTONIC",
            "clock_ticks_per_second": os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else None}


def host_state(pin=None):
    """Frequencies of the pinned (or first) core at this moment."""
    if not is_linux():
        return {}
    return core_state(pin if pin is not None else 0)


PERF_SYSCALL = {"x86_64": 298, "amd64": 298, "i386": 336, "i686": 336,
                "aarch64": 241, "arm64": 241, "riscv64": 241, "armv7l": 364,
                "ppc64le": 319, "ppc64": 319}


class ThreadCounters:
    """Hardware cycles and instructions of one thread, read at step boundaries.

    Uses perf_event_open(2) directly, so per-step deltas line up exactly with
    the CPU-time snapshots. Counts user and kernel mode where
    perf_event_paranoid allows it, otherwise user mode only."""

    EVENTS = (("cycles", 0), ("instructions", 1))

    def __init__(self, tid):
        require_linux("--perf-stat")
        number = PERF_SYSCALL.get(platform.machine().lower())
        if number is None:
            raise RuntimeError(f"--perf-stat: unknown perf_event_open syscall on {platform.machine()}")
        libc = ctypes.CDLL(None, use_errno=True)
        self.fds, self.scope = [], "user+kernel"
        for _, config in self.EVENTS:
            fd = -1
            for exclude_kernel in (0, 1):
                flags = (1 << 6) | (exclude_kernel << 5)  # exclude_hv, exclude_kernel
                attr = struct.pack("<IIQQQQQIIQ", 0, 64, config, 0, 0, 3, flags, 0, 0, 0)
                buffer = ctypes.create_string_buffer(attr, 64)
                fd = libc.syscall(ctypes.c_long(number), buffer, ctypes.c_long(tid),
                                  ctypes.c_long(-1), ctypes.c_long(-1), ctypes.c_ulong(8))
                if fd >= 0:
                    if exclude_kernel:
                        self.scope = "user"
                    break
            if fd < 0:
                error = ctypes.get_errno()
                self.close()
                raise RuntimeError(f"--perf-stat: perf_event_open failed: {os.strerror(error)} "
                                   "(check /proc/sys/kernel/perf_event_paranoid)")
            self.fds.append(fd)

    def read(self):
        values = []
        for fd in self.fds:
            value, enabled, running = struct.unpack("<QQQ", os.read(fd, 24))
            values.append(value * enabled / running if running else 0.0)
        return tuple(values)

    def close(self):
        for fd in self.fds:
            os.close(fd)
        self.fds = []


T975 = (12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
        2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086)


def t975(df):
    """Two-sided 95% Student t quantile."""
    return T975[df - 1] if df <= len(T975) else 1.96 + 2.4 / df


def batch_means_interval(values):
    """(mean, 95% half-width) by the method of batch means.

    Adjacent values are averaged into batches (sqrt(n) of them once there are
    16 or more values) before the standard error is taken, so positively
    autocorrelated blocks widen the interval instead of narrowing it."""
    n = len(values)
    mean = statistics.fmean(values)
    batches = n if n < 16 else max(4, math.isqrt(n))
    size = n // batches
    start = n - batches * size  # drop the oldest remainder
    means = [statistics.fmean(values[start + i * size:start + (i + 1) * size])
             for i in range(batches)]
    return mean, t975(batches - 1) * statistics.stdev(means) / math.sqrt(batches)


def settle_state(blocks, tolerance, min_blocks):
    """Judge whether per-block speeds (wall seconds, guest ms) have settled.

    The leading quarter of the blocks (at least one) is left out, so a slow
    first block cannot hold a sample open. The remaining window has settled
    when it holds at least min_blocks blocks, twice the standard error of the
    mean block speed (an approximate 95% interval) is within tolerance of the
    mean, and the means of its first and second halves differ by at most
    tolerance (no drift). All ratios are relative to the window mean. The
    interval is a batch-means 95% interval (see batch_means_interval), which
    stays honest when consecutive blocks are correlated.
    """
    discard = min(len(blocks), max(1, len(blocks) // 4))
    window = blocks[discard:]
    speeds = [guest / 1000 / wall for wall, guest in window]
    state = {"blocks": len(blocks), "discarded_blocks": discard,
             "window_blocks": len(speeds), "window_speed_percent": None,
             "relative_interval": None, "relative_drift": None, "settled": False}
    if speeds:
        state["window_speed_percent"] = (
            sum(guest for _, guest in window) / 10 / sum(wall for wall, _ in window))
    if len(speeds) >= 2:
        mean, interval = batch_means_interval(speeds)
        half = len(speeds) // 2
        state["relative_interval"] = interval / mean
        state["relative_drift"] = (
            statistics.fmean(speeds[-half:]) - statistics.fmean(speeds[:half])) / mean
        state["settled"] = (len(speeds) >= min_blocks
                            and state["relative_interval"] <= tolerance
                            and abs(state["relative_drift"]) <= tolerance)
    return state


class WorkloadCapture:
    """Hold each sample open until its speed settles, or until max_seconds.

    Speed is guest time per host wall time of interior steps. Steps are grouped
    into blocks of at least block_seconds; see settle_state for the rule. The
    guest's own TEST_SECONDS window is a floor that a release cannot shorten.
    """

    def __init__(self, step_ms=250, *, min_seconds=1.0, max_seconds=20.0,
                 tolerance=0.01, block_seconds=0.2, min_blocks=5):
        for name, value in (("min_seconds", min_seconds), ("max_seconds", max_seconds),
                            ("tolerance", tolerance), ("block_seconds", block_seconds)):
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"{name} must be finite and positive")
        if min_seconds > max_seconds:
            raise ValueError("min_seconds must not exceed max_seconds")
        if int(min_blocks) != min_blocks or min_blocks < 2:
            raise ValueError("min_blocks must be an integer of at least 2")
        self.settings = {"min_sample_seconds": min_seconds, "max_sample_seconds": max_seconds,
                         "settle_tolerance": tolerance, "block_seconds": block_seconds,
                         "min_window_blocks": int(min_blocks)}
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
        self.cpu_scope = "total_process_all_threads"

    def _settle(self, entry):
        return settle_state(entry["_blocks"], self.settings["settle_tolerance"],
                            self.settings["min_window_blocks"])

    def observe_step(self, before, after):
        """Snapshots are (monotonic seconds, process CPU seconds, guest ms)."""
        if self.pending is not None:
            raise RuntimeError("feed serial after each observed step")
        self.pending = (before, after, self.active)

    def observe_unattributed(self, before, after):
        """Charge a step known to straddle a boundary (e.g. the 1 ms steps
        that complete a WORK line) to the unattributed totals."""
        wall, cpu, guest = (after[i] - before[i] for i in range(3))
        if all(math.isfinite(x) for x in (wall, cpu, guest)) and wall >= 0 and cpu >= 0 and guest >= 0:
            self.unattributed["steps"] += 1
            self.unattributed["step_wall_seconds"] += wall
            self.unattributed["process_cpu_seconds"] += cpu
            self.unattributed["emulated_ms"] += guest

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
                        "lease_released": False, "sample_stop": None, "invalid_steps": 0,
                        "_blocks": [], "_block": [0.0, 0],
                    }
                    self.records.append(self.active)
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
                    block = original["_block"]
                    block[0] += wall
                    block[1] += guest
                    if block[0] >= self.settings["block_seconds"]:
                        original["_blocks"].append(tuple(block))
                        original["_block"] = [0.0, 0]
        if self.active is not None and self.active["phase"] == "sample":
            identifier = self.active["id"]
            if not self.active["lease_released"]:
                sampled = self.active["step_wall_seconds"]
                if sampled >= self.settings["max_sample_seconds"]:
                    self.active["sample_stop"] = "capped"
                elif (sampled >= self.settings["min_sample_seconds"]
                      and self._settle(self.active)["settled"]):
                    self.active["sample_stop"] = "settled"
            if self.active["sample_stop"] is not None:
                if (not self.active["lease_released"] or self.last_lease_ms is None
                        or emulated_ms - self.last_lease_ms >= LEASE_RENEW_MS):
                    commands.append(f"-{identifier:08x}\n")
                    self.last_lease_ms = emulated_ms
                self.active["lease_released"] = True
            elif (self.last_lease_ms is None
                  or emulated_ms - self.last_lease_ms >= LEASE_RENEW_MS):
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
            blocks = entry["_blocks"]
            entry = {key: value for key, value in entry.items() if not key.startswith("_")}
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
            if entry["phase"] == "sample":
                entry["settle"] = settle_state(blocks, self.settings["settle_tolerance"],
                                               self.settings["min_window_blocks"])
                entry["block_speed_percent"] = [round(guest / 10 / wall, 2)
                                                for wall, guest in blocks]
                entry["under_sampled"] = entry["sample_stop"] is None
            else:
                entry["under_sampled"] = None
            records.append(entry)
            if entry["phase"] == "sample":
                group = aggregates.setdefault(entry["name"], {
                    "samples": 0, "under_sampled": 0, "capped": 0, "step_wall_seconds": 0.0,
                    "process_cpu_seconds": 0.0, "emulated_ms": 0})
                group["samples"] += 1
                group["under_sampled"] += int(entry["under_sampled"])
                group["capped"] += int(entry["sample_stop"] == "capped")
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
            "sampling": dict(self.settings),
            "cpu_scope": self.cpu_scope,
            "measurement": "unthrottled_step_only_speed_capacity_not_actual_speed",
            "boundary_policy": "exclude_mixed_and_partial_line_steps",
            "boundary_uncertainty_emulated_ms": self.step_ms,
            "workloads": [entry for entry in records if entry["phase"] == "sample"],
            "phases": records, "aggregates": aggregates,
            "unattributed": dict(self.unattributed),
            "warnings": list(self.warnings) + [
                f"under_sampled:{entry['id']}" for entry in records if entry["under_sampled"]] + [
                f"capped:{entry['id']}" for entry in records
                if entry.get("sample_stop") == "capped"],
            "availability": ("captured" if any(e["phase"] == "sample" for e in records)
                             else "unavailable_no_sample_markers"),
        }


JIT_DSO = re.compile(r"\[unknown\]|perf-\d+\.map|\[anon|//anon|memfd:|\[jit")


def _dso(text):
    match = re.search(r"\(([^()]*)\)\s*$", text)
    return match[1] if match else ""


def parse_perf_script(lines, windows, leaves=None):
    """Attribute leaf and inclusive stacks only to interior step intervals.

    A leaf in an anonymous executable mapping (perf shows `[unknown]`, or the
    JIT's /tmp/perf-PID.map when 86Box writes one) counts as dynarec-generated
    code; jit_share is the fraction of a workload's samples there.

    leaves, when given, is the same perf.data printed with --hide-call-graph
    (one `time: ip sym (dso)` line per sample, in the same order): each
    sample's leaf is then its own IP. Without it the leaf is the first
    callchain row, and a sample whose callchain is empty (DWARF unwinding
    cannot start in generated code, which has no CFI) has no leaf."""
    starts = [window[0] for window in windows]
    leaves = iter(leaves) if leaves is not None else None

    def sample_leaf(stamp):
        for line in leaves:
            match = re.match(r"^\s*(\d+\.\d+):\s+(.*)$", line)
            if match and match[1] == stamp:
                return symbol_text(match[2]) or None
        return None
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

    def leaf(symbol):
        entry = counts[current]
        entry["leaf"][symbol] += 1
        if JIT_DSO.search(_dso(symbol)) or symbol.startswith("[unknown]"):
            entry["jit"] += 1

    for line in lines:
        match = re.match(r"^\s*(\d+\.\d+):\s+(.*)$", line)
        if match:
            flush()
            current, stack, leaf_pending = None, set(), False
            stamp = float(match[1])
            symbol = symbol_text(match[2])
            if leaves is not None:
                own = sample_leaf(match[1])
                if own and not symbol:
                    stack.add(own)
                symbol = own or symbol
            index = bisect.bisect_right(starts, stamp) - 1
            if index >= 0 and stamp < windows[index][1]:
                current = windows[index][2]
                entry = counts.setdefault(current, {"samples": 0, "leaf": Counter(),
                                                    "inclusive": Counter(), "jit": 0})
                entry["samples"] += 1
                if symbol:
                    leaf(symbol)
                    stack.add(symbol)
                else:
                    leaf_pending = True
        elif current is not None and line.strip():
            # perf's callchain rows start with an IP, followed by symbol/DSO.
            symbol = symbol_text(line)
            stack.add(symbol)
            if leaf_pending:
                leaf(symbol)
                leaf_pending = False
    flush()
    return {str(key): {"samples": value["samples"],
                      "jit_samples": value["jit"],
                      "jit_share": value["jit"] / value["samples"] if value["samples"] else None,
                      "leaf_hotpaths": value["leaf"].most_common(30),
                      "inclusive_hotpaths": value["inclusive"].most_common(30)}
            for key, value in counts.items()}


class PerfRecorder:
    """perf record on the emulation thread, enabled only during sample windows.

    Records at `frequency` Hz with the given call-graph mode (dwarf by default,
    fp or none optional) on one TID when known, starting with events disabled
    and switched on/off through perf's control FIFO, so perf.data holds sample
    windows only. The report streams `perf script` instead of saving it."""

    def __init__(self, run_dir, frequency=999, call_graph="dwarf"):
        self.directory = Path(run_dir)
        self.process = None
        self.command = None
        self.frequency = int(frequency)
        self.call_graph = call_graph
        self.control = self.ack = None
        self.enabled = False
        self.pid = None
        self.tid = None

    def _error(self, reason):
        log = self.directory / "perf-record.log"
        details = log.read_text(errors="replace") if log.exists() else ""
        return RuntimeError(f"{reason}; perf permissions/events must be available. "
                            f"See {log}: {details[-4000:]}")

    def start(self, pid, tid=None):
        executable = shutil.which("perf")
        if executable is None:
            raise RuntimeError("--perf requested but perf is not installed")
        self.pid, self.tid = pid, tid
        target = ["-t", str(tid)] if tid is not None else ["-p", str(pid)]
        graph = ([] if self.call_graph == "none" else
                 ["--call-graph", "dwarf,8192" if self.call_graph == "dwarf" else self.call_graph,
                  "--user-callchains"])
        control = []
        if hasattr(os, "mkfifo"):
            ctl, ack = self.directory / "perf-ctl.fifo", self.directory / "perf-ack.fifo"
            for fifo in (ctl, ack):
                if fifo.exists():
                    fifo.unlink()
                os.mkfifo(fifo, 0o600)
            # O_RDWR keeps both ends open, so neither side blocks on open.
            self.control = os.open(ctl, os.O_RDWR | os.O_NONBLOCK)
            self.ack = os.open(ack, os.O_RDWR | os.O_NONBLOCK)
            control = ["--control", f"fifo:{ctl},{ack}", "--delay", "-1"]
        self.command = [executable, "record", "-F", str(self.frequency), *graph, *target,
                        *control, "-o", str(self.directory / "perf.data"),
                        "--clockid", "mono"]
        with (self.directory / "perf-record.log").open("wb") as log:
            self.process = subprocess.Popen(self.command, stdout=log, stderr=log)
        time.sleep(0.25)
        self.check()
        self.enabled = not control

    def _command(self, word):
        if self.control is None or self.process is None:
            return
        os.write(self.control, word.encode() + b"\n")
        ready, _, _ = select.select([self.ack], [], [], 5)
        if not ready:
            self.check()
            raise self._error(f"perf did not acknowledge '{word}'")
        os.read(self.ack, 64)

    def enable(self):
        if not self.enabled:
            self._command("enable")
            self.enabled = True

    def disable(self):
        if self.enabled and self.control is not None:
            self._command("disable")
            self.enabled = False

    def check(self):
        if self.process is not None and self.process.poll() is not None:
            raise self._error(f"perf record exited unexpectedly ({self.process.returncode})")

    def _close_fifos(self):
        for name in ("control", "ack"):
            fd = getattr(self, name)
            if fd is not None:
                os.close(fd)
                setattr(self, name, None)
        for fifo in ("perf-ctl.fifo", "perf-ack.fifo"):
            (self.directory / fifo).unlink(missing_ok=True)

    def stop(self):
        if self.process is None:
            return
        try:
            self.check()
            self.process.send_signal(signal.SIGINT)
            try:
                code = self.process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise self._error("perf did not flush within 60 seconds")
            if code not in (0, -signal.SIGINT, 128 + signal.SIGINT):
                raise self._error(f"perf record failed ({code})")
        finally:
            self._close_fifos()

    def report(self, windows):
        data = self.directory / "perf.data"
        if not data.is_file() or data.stat().st_size == 0:
            raise self._error("perf record produced no nonempty perf.data")
        command = [self.command[0], "script", "--ns", "-i", str(data), "-F", "time,ip,sym,dso"]
        # A second stream gives each sample's own IP as its leaf: callchains
        # are empty for samples in generated code, the JIT share's subject.
        leaf_command = command + ["--hide-call-graph"]
        with (self.directory / "perf-script.log").open("w") as errors:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors,
                                       text=True, errors="replace")
            leaf_process = None
            if self.call_graph != "none":
                leaf_process = subprocess.Popen(leaf_command, stdout=subprocess.PIPE,
                                                stderr=errors, text=True, errors="replace")
            try:
                hotpaths = parse_perf_script(process.stdout, windows,
                                             leaf_process.stdout if leaf_process else None)
            finally:
                process.stdout.close()
                code = process.wait()
                if leaf_process is not None:
                    leaf_process.stdout.close()
                    leaf_code = leaf_process.wait()
                    # Closing the pipe early ends it with SIGPIPE: not a failure.
                    if leaf_code not in (0, -signal.SIGPIPE, 128 + signal.SIGPIPE):
                        code = code or leaf_code
        if code:
            raise RuntimeError("perf script failed; see perf-script.log")
        samples = sum(v["samples"] for v in hotpaths.values())
        jit = sum(v["jit_samples"] for v in hotpaths.values())
        perf_map = Path(f"/tmp/perf-{self.pid}.map")
        return {"status": "captured" if hotpaths else "no_attributed_samples",
                "command": self.command, "script_command": command,
                "frequency_hz": self.frequency, "call_graph": self.call_graph,
                "thread": self.tid, "clock": "CLOCK_MONOTONIC", "workloads_by_id": hotpaths,
                "attributed_samples": samples,
                "jit_share": jit / samples if samples else None,
                "perf_map": str(perf_map) if perf_map.exists() else None,
                "interior_step_windows": windows, "data": str(data)}
