#!/usr/bin/env python3
"""Run CPU checks and capacity measurements using an existing 86Box VM."""

import argparse
import configparser
from contextlib import contextmanager
import fcntl
import re
import hashlib
import json
import math
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid
from qemu_smoke import parse_log
from workload_capture import WorkloadCapture, PerfRecorder, host_metadata, process_cpu_seconds


ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "486dx": ("486sp3", "i486dx", 33333333, 1, "internal"),
    "486sx": ("486sp3", "i486sx", 25000000, 1, "none"),
    "pentium": ("p55t2p4", "pentium_p54c", 100000000, 1.5, "internal"),
    "mmx": ("p55t2p4", "pentium_p55c", 233333333, 3.5, "internal"),
    "pii": ("al440lx", "pentium2_klamath", 233333333, 3.5, "internal"),
    "pii-bf6": ("bf6", "pentium2_deschutes", 300000000, 4.5, "internal"),
    "k6-2": ("p55t2p4", "k6_2", 266666666, 4.0, "internal"),
    "k6-2+": ("p55t2p4", "k6_2p", 266666666, 4.0, "internal"),
    "6x86mx": ("p55t2p4", "cx6x86mx", 166666666, 2.5, "internal"),
}


def positive(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return number


def positive_int(value):
    number = int(value, 0)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def boot_key(value):
    try:
        when, code = value.split(":")
        when, code = int(when, 0), int(code, 0)
        if when < 0 or not 1 <= code <= 0x1FF:
            raise ValueError
        return when, code
    except ValueError:
        raise argparse.ArgumentTypeError("use EMULATED_MS:SET1_SCANCODE") from None


def arguments():
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Requires an AUTOMATION-enabled 86Box, Xvfb, and locally installed ROMs. "
        "Never downloads firmware. --vm runs the supplied VM in place, temporarily "
        "inserting the test floppy and enabling COM1, then restores its config. Without --expect, "
        "DONE completes observation with strict nonzero summary counts, not a "
        "guest-test PASS (FAIL/FAULT remain visible). Default limits: 24 guest "
        "hours and 8 host hours. Ratios measure unthrottled speed capacity, not "
        "the GUI's actual-speed percentage. All emulator threads are charged; "
        "boundary steps and non-step RPC work are excluded from workload ratios. COM1 is "
        "captured; port 0xE9 is not an automation capture source. Fresh CMOS "
        "may require --bios-f1. Graphical BIOS prompts cannot be read by 'text'; "
        "AL440LX needs --key 20000:0x3b to resume its fresh-CMOS F1 prompt and "
        "may leave COM1 inaccessible (use screen.log). --vm accepts a VM directory "
        "(containing 86box.cfg) or a configuration file. NVRAM, disks and devices "
        "remain in place and may be updated by the VM. Stop the VM before running."
    ))
    parser.add_argument("--emulator", default="86Box",
                        help="executable name or path (default: 86Box on PATH)")
    parser.add_argument("--image", type=Path, default=ROOT / "floppy.img")
    parser.add_argument("--output-dir", type=Path,
                        help="results directory (default: supplied VM directory)")
    parser.add_argument("--rompath", type=Path,
                        help="local ROM directory (normally containing machines/ and video/)")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--vm", type=Path, help="existing VM directory or configuration file")
    source.add_argument("--profile", choices=PROFILES,
                        help="generate a fixture instead of using an existing VM")
    source.add_argument("--config", type=Path,
                        help="source config for generated matrix fixtures (legacy interface)")
    parser.add_argument("--dynarec", choices=("on", "off"),
                        help="override the profile/config execution mode")
    parser.add_argument("--accuracy", action="store_true",
                        help="run checked invocations without benchmark sampling extensions; "
                             "require CPU control acknowledgement and fail on guest failures")
    parser.add_argument("--expected-signature", type=lambda s: int(s, 0),
                        help="configured CPUID signature, e.g. 0x580 (matrix runner supplies it)")
    parser.add_argument("--expected-family-name", help="expected automation CPU family display name")
    parser.add_argument("--expected-model-name", help="expected automation CPU speed/model label")
    parser.add_argument("--fpu-backend", choices=("host", "softfloat"),
                        help="request host FPU or software floating-point implementation")
    parser.add_argument("--label", default="",
                        help="descriptive result label only; does not change CPU engine/settings")
    parser.add_argument("--memory-mb", type=positive_int,
                        help="override RAM; otherwise preserve VM RAM (generated profiles: 16 MiB)")
    parser.add_argument("--emulated-ms", type=positive_int, default=86400000)
    parser.add_argument("--wall-seconds", type=positive, default=28800)
    parser.add_argument("--step-ms", type=positive_int, default=250,
                        help="capture interval in emulated milliseconds (default: 250)")
    parser.add_argument("--sample-cpu-seconds", type=positive, default=30.0,
                        help="minimum interior-step process CPU seconds per workload (default: 30)")
    parser.add_argument("--perf", action="store_true",
                        help="attach perf record once; require working perf permissions, save hotpaths")
    parser.add_argument("--expect", action="append", default=[],
                        help="literal marker required in COM1 or screen; repeat for all markers")
    parser.add_argument("--reject", action="append", default=["FATAL"],
                        help="fail immediately on a literal marker (default: FATAL); repeatable")
    parser.add_argument("--bios-f1", action="store_true",
                        help="press F1 at recognized text-mode resume/continue BIOS prompts; "
                             "does not configure firmware or detect graphical prompts")
    parser.add_argument("--key", type=boot_key, action="append", default=[],
                        help="press/release set-1 key at EMULATED_MS:SCANCODE; repeatable")
    parser.add_argument("--prepare-only", action="store_true",
                        help="write isolated inputs and launch.json without starting 86Box")
    return parser.parse_args()


def rom_directory(explicit):
    candidates = [explicit] if explicit else [
        ROOT.parent / "86Box/roms",
        ROOT.parent / "86Box/build/roms",
        Path("/usr/share/86Box/roms"),
        Path("/usr/share/86box/roms"),
        Path("/usr/local/share/86Box/roms"),
        Path("/usr/local/share/86box/roms"),
        Path.home() / ".local/share/86Box/roms",
        Path.home() / ".local/share/86box/roms",
    ]
    for path in candidates:
        if path.is_dir() and (path / "machines").is_dir():
            return path.resolve()
    raise RuntimeError("No local ROM tree found; provide --rompath. Firmware is not downloaded.")


def vm_config(path):
    path = path.expanduser().absolute()
    if path.is_dir():
        path /= "86box.cfg"
    if not path.is_file():
        raise RuntimeError(f"VM configuration not found: {path}")
    return path


def emulator_binary(value):
    executable = shutil.which(value)
    if executable is None:
        raise RuntimeError(f"86Box executable not found or not executable: {value}; "
                           "install 86Box on PATH or supply --emulator /path/to/86Box")
    return Path(executable).resolve(strict=True)


@contextmanager
def inplace_vm(args, image):
    source = vm_config(args.vm)
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            command = (process / "cmdline").read_bytes().split(b"\0")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if not command or "86box" not in Path(os.fsdecode(command[0])).name.lower():
            continue
        cwd = None
        try:
            cwd = (process / "cwd").resolve(strict=True)
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
        for index, word in enumerate(command):
            if word in (b"--vmpath", b"-P") and index + 1 < len(command):
                path = Path(os.fsdecode(command[index + 1]))
                if not path.is_absolute():
                    if cwd is None:
                        continue
                    path = cwd / path
                if path.resolve() == source.parent.resolve():
                    raise RuntimeError(f"VM is already running (PID {process.name}): {source}")
            elif os.fsdecode(word) == str(source):
                raise RuntimeError(f"VM is already running (PID {process.name}): {source}")
    with source.open("r+b") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError(f"VM configuration is already in use: {source}") from None
        original = handle.read()
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.read_string(original.decode("utf-8-sig"))
        if not config.has_section("Machine"):
            raise RuntimeError("VM configuration needs a [Machine] section")
        for key in ("machine", "cpu_family", "cpu_speed", "mem_size"):
            if not config.has_option("Machine", key):
                raise RuntimeError(f"VM configuration needs an explicit [Machine] {key}")
        for key in ("cpu_speed", "mem_size"):
            if int(config["Machine"][key]) <= 0:
                raise RuntimeError(f"VM [Machine] {key} must be positive")
        if args.dynarec is not None:
            config["Machine"]["cpu_use_dynarec"] = str(int(args.dynarec == "on"))
        if args.memory_mb is not None:
            config["Machine"]["mem_size"] = str(args.memory_mb * 1024)
        if args.fpu_backend is not None:
            config["Machine"]["fpu_softfloat"] = str(int(args.fpu_backend == "softfloat"))
        for section in ("Ports (COM & LPT)", "Floppy and CD-ROM drives"):
            if not config.has_section(section):
                config.add_section(section)
        config["Ports (COM & LPT)"].update(serial1_enabled="1", serial1_device="automation")
        config["Floppy and CD-ROM drives"].update(
            fdd_01_type="35_2hd", fdd_01_fn=str(image), fdd_01_writeprot="1")
        try:
            with source.open("w", encoding="utf-8") as output:
                config.write(output)
            yield source, dict(config["Machine"]), hashlib.sha256(original).hexdigest()
        finally:
            with source.open("wb") as output:
                output.write(original)


def write_config(args, run_dir):
    source = getattr(args, "vm", None) or args.config
    config = configparser.ConfigParser(interpolation=None)
    config["Video"] = {"gfxcard": "et4000ax"}
    if source:
        source = vm_config(source)
        snapshot = run_dir / "source.cfg"
        shutil.copyfile(source, snapshot)
        original = configparser.ConfigParser(interpolation=None, strict=False)
        with snapshot.open() as handle:
            original.read_file(handle)
        # Never inherit DEFAULT values: an arbitrary host backend/path could
        # otherwise leak into the isolated configuration.
        original.defaults().clear()
        if not original.has_option("Machine", "machine"):
            raise RuntimeError("VM configuration needs a [Machine] machine setting")
        config["Machine"] = {}
        for key in ("machine", "cpu_family", "cpu_speed", "cpu_multi", "fpu_type",
                    "cpu_use_dynarec", "cpu_waitstates", "fpu_softfloat", "mem_size",
                    "cpu", "cpu_manufacturer", "cpu_override", "cpu_debug_regs_486"):
            if original.has_option("Machine", key):
                config["Machine"][key] = original["Machine"][key]
        if original.has_option("Video", "gfxcard"):
            config["Video"]["gfxcard"] = original["Video"]["gfxcard"]
        for section in original.sections():
            if section not in config and original.has_option(section, "bios"):
                bios = original[section]["bios"]
                if not re.fullmatch(r"[a-zA-Z0-9_.-]+", bios):
                    raise RuntimeError(f"unsupported BIOS selector in [{section}]: {bios!r}")
                config[section] = {"bios": bios}
    else:
        machine, family, speed, multiplier, fpu = PROFILES[args.profile]
        config["Machine"] = {
            "machine": machine, "cpu_family": family, "cpu_speed": str(speed),
            "cpu_multi": str(multiplier), "fpu_type": fpu, "cpu_use_dynarec": "1",
        }
    if args.memory_mb is not None:
        config["Machine"]["mem_size"] = str(args.memory_mb * 1024)
    elif "mem_size" not in config["Machine"]:
        config["Machine"]["mem_size"] = "16384"
    if args.dynarec is not None:
        config["Machine"]["cpu_use_dynarec"] = "1" if args.dynarec == "on" else "0"
    if getattr(args, "fpu_backend", None) is not None:
        config["Machine"]["fpu_softfloat"] = "1" if args.fpu_backend == "softfloat" else "0"
    config["Machine"]["time_sync"] = "disabled"
    config["General"] = {"vid_renderer": "qt_software"}
    config["Ports (COM & LPT)"] = {
        "serial1_enabled": "1", "serial1_device": "automation",
        "serial2_enabled": "0", "lpt1_enabled": "0",
    }
    config["Floppy and CD-ROM drives"] = {
        "fdd_01_type": "35_2hd", "fdd_01_fn": str(run_dir / "floppy.img"),
        "fdd_01_writeprot": "1", "fdd_02_type": "none",
    }
    config["Storage controllers"] = {"hdc": "none", "fdc": "internal"}
    config["Sound"] = {"sndcard": "none"}
    config["Network"] = {f"net_{i:02d}_card": "none" for i in range(1, 5)}
    with (run_dir / "86box.cfg").open("w") as handle:
        config.write(handle)
    return dict(config["Machine"])


def cpu_control_frame(machine, accuracy=False, signature=None):
    family = machine.get("cpu_family", "unknown")
    fpu = machine.get("fpu_type", "auto")
    if any(not re.fullmatch(r"[a-zA-Z0-9_-]+", value) for value in (family, fpu)):
        raise ValueError("CPU family/FPU identifiers must be nonempty ASCII tokens")
    if len(family) >= 48 or len(fpu) >= 16:
        raise ValueError("CPU family/FPU identifier exceeds guest control limit")
    mode = "accuracy" if accuracy else "benchmark"
    if signature is not None and not 0 <= signature <= 0xffffffff:
        raise ValueError("CPU signature must fit an unsigned 32-bit value")
    suffix = f" {signature:08x}" if signature is not None else ""
    return f"@cpu1 {family} {fpu} {mode}{suffix}\n".encode("ascii")


def cpu_control_accepted(run, machine, accuracy, signature=None):
    return any(row.get("protocol") == "cpu1" and row.get("accepted") == "1"
               and row.get("model") == machine.get("cpu_family", "unknown")
               and row.get("fpu") == machine.get("fpu_type", "auto")
               and row.get("mode") == ("accuracy" if accuracy else "benchmark")
               and (signature is None or row.get("signature") == f"{signature:08x}")
               for row in run.get("cpu_control", []))


class Automation:
    def __init__(self, connection, transcript, deadline):
        self.connection = connection
        self.buffer = b""
        self.transcript = transcript
        self.deadline = deadline

    def request(self, command, allow_failure=False):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("wall-clock deadline exceeded")
        self.connection.settimeout(remaining)
        self.connection.sendall(command.encode("utf-8") + b"\n")
        while b"\n" not in self.buffer:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("wall-clock deadline exceeded")
            self.connection.settimeout(remaining)
            chunk = self.connection.recv(65536)
            if not chunk:
                raise RuntimeError("86Box closed automation connection; see emulator.log")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        reply = json.loads(line)
        self.transcript.write(json.dumps({"command": command, "reply": reply}) + "\n")
        self.transcript.flush()
        if not reply.get("ok") and not allow_failure:
            raise RuntimeError(f"Automation {command!r}: {reply}")
        return reply


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=3)


def start_display(run_dir, deadline):
    # -displayfd allocates a free server number; no shared display is reused.
    read_fd, write_fd = os.pipe()
    log = (run_dir / "xvfb.log").open("wb")
    try:
        process = subprocess.Popen(
            ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1024x768x24",
             "-nolisten", "tcp", "-ac"],
            pass_fds=(write_fd,), stdout=log, stderr=log, start_new_session=True)
    finally:
        os.close(write_fd)
        log.close()
    try:
        limit = min(deadline, time.monotonic() + 10)
        display_line = b""
        # Xvfb can write the display number and newline separately.
        while b"\n" not in display_line:
            ready, _, _ = select.select([read_fd], [], [], max(0, limit - time.monotonic()))
            if not ready:
                raise RuntimeError("Xvfb did not become ready")
            chunk = os.read(read_fd, 64)
            if not chunk or len(display_line) + len(chunk) > 64:
                raise RuntimeError("Xvfb readiness pipe closed or returned an invalid display")
            display_line += chunk
        display = display_line.decode().strip()
        if not display.isdigit() or process.poll() is not None:
            raise RuntimeError("Xvfb startup failed; see xvfb.log")
        return process, ":" + display
    except BaseException:
        stop_process(process)
        raise
    finally:
        os.close(read_fd)


def complete_counts(result):
    tests, counts = result["tests"], result["counts"]
    if not tests or counts.get("tests") != str(len(tests)):
        return False
    if any(test.get("status") not in ("PASS", "FAIL", "SKIP", "FAULT") for test in tests):
        return False
    if len({(test.get("r"), test.get("name")) for test in tests}) != len(tests):
        return False
    return all(counts.get(key) == str(sum(test.get("status") == status for test in tests))
               for key, status in (("pass", "PASS"), ("fail", "FAIL"),
                                   ("skip", "SKIP"), ("fault", "FAULT")))


def execute(args, run_dir, command, machine):
    started = time.monotonic()
    deadline = started + args.wall_seconds
    display = emulator = connection = None
    capture = WorkloadCapture(getattr(args, "sample_cpu_seconds", 30.0), args.step_ms)
    recorder = PerfRecorder(run_dir) if getattr(args, "perf", False) else None
    perf_stopped = False
    accuracy = getattr(args, "accuracy", False)
    control_sent = False
    signature = getattr(args, "expected_signature", None)
    control_frame = cpu_control_frame(machine, accuracy, signature)
    result = {"status": "error", "expected": args.expect, "matched": [],
              "machine": machine, "label": args.label, "step_ms": args.step_ms,
              "emulated_ms": 0, "directory": str(run_dir), "host": host_metadata(),
              "emulator_executable": command[0],
              "mode": "accuracy" if accuracy else "benchmark",
              "limits": {"emulated_ms": args.emulated_ms, "wall_seconds": args.wall_seconds},
              "perf": {"status": "requested" if recorder else "disabled"}}
    try:
        display, display_name = start_display(run_dir, deadline)
        environment = os.environ.copy()
        # Keep the session runtime directory so OpenAL can reach PipeWire.
        environment.update(DISPLAY=display_name, QT_QPA_PLATFORM="xcb")
        if not getattr(args, "vm", None):
            environment.update(
                           XDG_CONFIG_HOME=str(run_dir / "config"),
                           XDG_CACHE_HOME=str(run_dir / "cache"),
                           XDG_DATA_HOME=str(run_dir / "data"),
                           TMPDIR=str(run_dir / "scratch"))
            for name in ("config", "cache", "data", "scratch"):
                (run_dir / name).mkdir(mode=0o700)
        with (run_dir / "process.log").open("wb") as output:
            emulator = subprocess.Popen(command, cwd=(vm_config(args.vm).parent
                                        if getattr(args, "vm", None) else run_dir), env=environment,
                                        stdout=output, stderr=output, start_new_session=True)
        socket_path = run_dir / "automation.sock"
        while time.monotonic() < deadline:
            if emulator.poll() is not None:
                raise RuntimeError(f"86Box exited {emulator.returncode}; see process.log/emulator.log")
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                connection.connect(str(socket_path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                connection.close()
                connection = None
                time.sleep(0.05)
        if connection is None:
            raise TimeoutError("86Box automation socket startup timed out")
        result["startup_wall_seconds"] = time.monotonic() - started
        if recorder is not None:
            recorder.start(emulator.pid)
        with (run_dir / "automation.jsonl").open("w") as transcript, \
                (run_dir / "serial.log").open("wb") as serial_log, \
                (run_dir / "screen.log").open("w") as screen_log:
            api = Automation(connection, transcript, deadline)
            api.request("status")
            api.request("ff on")
            result["initial_info"] = api.request("info")
            info = result["initial_info"]
            for expected, actual in (("machine", "machine"), ("mem_size", "mem_kb"),
                                     ("cpu_speed", "cpu_hz")):
                if expected in machine and str(info[actual]) != machine[expected]:
                    raise RuntimeError(f"hardware mismatch: requested {expected}="
                                       f"{machine[expected]}, got {info[actual]}")
            for expected, actual in (("expected_family_name", "cpu_family"),
                                     ("expected_model_name", "cpu")):
                wanted = getattr(args, expected, None)
                if wanted is not None and wanted != info.get(actual):
                    raise RuntimeError(f"CPU substitution: requested {actual}={wanted!r}, "
                                       f"got {info.get(actual)!r}")
            serial_from = 0
            serial_tail = ""
            seen = set()
            keys = sorted(args.key)
            last_screen = None
            last_f1 = -5000
            while result["emulated_ms"] < args.emulated_ms:
                now = result["emulated_ms"]
                while keys and keys[0][0] <= now:
                    _, code = keys.pop(0)
                    api.request(f"key down {code}")
                    now = api.request(f"key up {code}")["ms"]
                target = min(args.emulated_ms, now + args.step_ms)
                if keys and now < keys[0][0] < target:
                    target = keys[0][0]
                if target > now:
                    cpu_before = process_cpu_seconds(emulator.pid)
                    step_start = time.monotonic()
                    before = (step_start, cpu_before, now)
                    now = api.request(f"step {target - now}")["ms"]
                    step_end = time.monotonic()
                    after = (step_end, process_cpu_seconds(emulator.pid), now)
                    capture.observe_step(before, after)
                result["emulated_ms"] = now
                serial = api.request(f"serial read 1 {serial_from}")
                if serial.get("lost"):
                    raise RuntimeError("COM1 capture overflowed; decrease --step-ms")
                serial_from = serial["next"]
                data = bytes.fromhex(serial["data"])
                serial_log.write(data)
                serial_log.flush()
                serial_tail += data.decode("cp437")
                if re.search(r"(?m)^BUILD [^\n]*\bbenchmarks=0\r?$", serial_tail):
                    accuracy = True
                    result["mode"] = "accuracy"
                    control_frame = cpu_control_frame(machine, True, signature)
                leases = capture.feed(data, now, time.monotonic())
                if not accuracy:
                    for lease in leases:
                        api.request(f"serial write 1 {lease.encode('ascii').hex()}")
                if recorder is not None:
                    recorder.check()
                if (not control_sent and
                        "CONTROL protocol=cpu1 ready=1\n" in serial_tail.replace("\r", "")):
                    api.request(f"serial write 1 {control_frame.hex()}")
                    control_sent = True
                for marker in args.expect:
                    if marker in serial_tail:
                        seen.add(marker)
                rejected = [marker for marker in args.reject if marker in serial_tail]
                longest = max(map(len, args.expect + args.reject), default=1)
                serial_tail = serial_tail[-max(65536, longest):]
                screen = api.request("text", allow_failure=True)
                text = "\n".join(bytes.fromhex(row).decode("cp437")
                                 for row in screen.get("chars", []))
                if text != last_screen:
                    screen_log.write(f"\n--- {now} ms ---\n{text}\n")
                    screen_log.flush()
                    last_screen = text
                for marker in args.expect:
                    if marker in text:
                        seen.add(marker)
                rejected.extend(marker for marker in args.reject if marker in text)
                result["matched"] = sorted(seen)
                if rejected:
                    result["rejected"] = sorted(set(rejected))
                    break
                f1_prompt = any(
                    "press" in line.lower() and
                    ("<f1>" in line.lower() or "f1 to" in line.lower()) and
                    ("resume" in line.lower() or "continue" in line.lower())
                    for line in text.splitlines())
                if args.bios_f1 and f1_prompt and now - last_f1 >= 5000:
                    api.request("key down 0x3b")
                    api.request("key up 0x3b")
                    last_f1 = now
                if not accuracy and args.expect and all(marker in seen for marker in args.expect):
                    break
                if any(line == "DONE" for line in serial_tail.replace("\r", "").splitlines()):
                    break
            result["host_seconds"] = time.monotonic() - started
            serial_text = (run_dir / "serial.log").read_text(encoding="cp437")
            result.update(parse_log(serial_text))
            result["done"] = any(line == "DONE" for line in serial_text.splitlines())
            result["counts_consistent"] = complete_counts(result)
            result["correctness"] = {
                "complete": result["done"] and result["counts_consistent"],
                "passed": (result["done"] and result["counts_consistent"] and
                           not result["fatal"] and
                           not any(t.get("status") in ("FAIL", "FAULT") for t in result["tests"])),
                "note": "Observed guest failures are retained; status observed is not correctness PASS.",
            }
            result["conformance_coverage"] = {
                "scope": "sampled",
                "identity_verified": any(t.get("name") == "cpu_model_contract"
                                         and t.get("status") == "PASS"
                                         for t in result["tests"]),
                "untested_features": [f["name"] for f in result.get("features", [])
                                      if f.get("status") == "UNTESTED"],
                "failed_features": [f["name"] for f in result.get("features", [])
                                    if f.get("status") == "FAIL"],
            }
            if (accuracy and result["done"] and not result["fatal"]
                    and not cpu_control_accepted(result, machine, True, signature)):
                result["correctness"]["passed"] = False
                raise RuntimeError("guest did not acknowledge the requested accuracy/CPU contract")
            if (accuracy and result["done"] and not result["fatal"]
                    and not any(t.get("name") == "cpu_model_contract"
                                for t in result["tests"])):
                result["correctness"]["passed"] = False
                raise RuntimeError("accuracy run lacks the CPU contract test")
            result["final_info"] = api.request("info")
            result["registers"] = api.request("regs")
            screenshot = api.request(f"screenshot {run_dir / 'screen.png'}",
                                     allow_failure=True)
            if not screenshot.get("ok"):
                result["screenshot_note"] = screenshot.get("error")
            result["status"] = ("failed" if result.get("rejected") else
                                "failed" if accuracy and result["done"]
                                and not result["correctness"]["passed"] else
                                "failed" if result["done"] and not result["counts_consistent"] else
                                "pass" if args.expect and
                                all(marker in seen for marker in args.expect)
                                else "observed" if result["done"] else "timeout")
            if not serial_text:
                result["capture_note"] = (
                    "No COM1 output; no reliable workload attribution/completion. "
                    "Some profiles (including AL440LX) may require BIOS/COM1 setup; see screen.log.")
            result["process_cpu_seconds_total"] = process_cpu_seconds(emulator.pid)
            if recorder is not None:
                recorder.stop()
                perf_stopped = True
                result["perf"] = recorder.report(capture.windows)
            api.request("quit")
    except TimeoutError as error:
        serial_path = run_dir / "serial.log"
        if serial_path.exists():
            result.update(parse_log(serial_path.read_text(encoding="cp437")))
        result["status"] = "timeout"
        result["error"] = str(error)
        result["correctness"] = {
            "complete": False, "passed": False,
            "note": "Host deadline interrupted the run; not an architectural failure.",
        }
    except (OSError, RuntimeError, ValueError) as error:
        result["status"] = "error"
        result["error"] = str(error)
    finally:
        if recorder is not None and not perf_stopped and recorder.process is not None:
            try:
                recorder.stop()
            except (OSError, RuntimeError) as error:
                result["perf"] = {"status": "error", "error": str(error)}
                result["error"] = result.get("error", str(error))
                result["status"] = "error"
        if recorder is not None and "error" in result and result["perf"]["status"] == "requested":
            result["perf"] = {"status": "error", "error": result["error"]}
        capture.finish(result["emulated_ms"], time.monotonic())
        result["workload_capture"] = capture.report()
        phases = result["workload_capture"]["phases"]
        result["step_wall_seconds"] = (sum(p["step_wall_seconds"] for p in phases) +
                                       capture.unattributed["step_wall_seconds"])
        result["step_process_cpu_seconds"] = (
            sum(p["process_cpu_seconds"] for p in phases) +
            capture.unattributed["process_cpu_seconds"])
        result["total_host_seconds"] = time.monotonic() - started
        result.setdefault("host_seconds", result["total_host_seconds"])
        result["non_step_wall_seconds"] = max(
            0, result["host_seconds"] - result["step_wall_seconds"])
        if "process_cpu_seconds_total" not in result and emulator is not None and emulator.poll() is None:
            try:
                result["process_cpu_seconds_total"] = process_cpu_seconds(emulator.pid)
            except OSError as error:
                result["cpu_accounting_note"] = str(error)
        if "process_cpu_seconds_total" in result:
            result["non_step_process_cpu_seconds"] = max(
                0, result["process_cpu_seconds_total"] - result["step_process_cpu_seconds"])
        if connection is not None:
            connection.close()
        stop_process(emulator)
        stop_process(display)
        (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def run_existing_vm(args, emulator, image, roms):
    source = vm_config(args.vm)
    run_dir = (args.output_dir.expanduser().absolute() if args.output_dir else source.parent)
    run_dir.mkdir(parents=True, exist_ok=True)
    artifacts = ("serial.log", "result.json", "launch.json", "automation.sock",
                 "automation.jsonl", "screen.log", "screen.png", "emulator.log",
                 "process.log", "xvfb.log", "perf.data", "perf-record.log")
    for name in artifacts:
        if (run_dir / name).exists():
            raise RuntimeError(f"output already exists: {run_dir / name}; "
                               "use --output-dir for another run")
    if len(os.fsencode(run_dir / "automation.sock")) >= 108:
        raise RuntimeError("output path is too long for a UNIX automation socket; use --output-dir")
    if args.prepare_only:
        raise RuntimeError("--prepare-only is for generated fixtures, not in-place --vm runs")
    print("Running VM in place: disks, NVRAM and host-connected devices remain active. "
          "The VM must not already be running. Configuration is restored after exit.",
          file=sys.stderr)
    with inplace_vm(args, image) as (source, machine, source_hash):
        command = [str(emulator), "--automation", f"unix:{run_dir / 'automation.sock'}",
                   "--automation-hold", "--noconfirm", "--vmpath", str(source.parent),
                   "--rompath", str(roms), "--logfile", str(run_dir / "emulator.log"),
                   str(source)]
        image_hash = hashlib.sha256(image.read_bytes()).hexdigest()
        (run_dir / "launch.json").write_text(json.dumps({
            "command": command, "source_vm": str(source), "machine": machine,
            "source_config_sha256": source_hash, "image": str(image),
            "image_sha256": image_hash, "in_place": True,
        }, indent=2) + "\n")
        result = execute(args, run_dir, command, machine)
        result["image_sha256"] = image_hash
        result["emulator_sha256"] = hashlib.sha256(emulator.read_bytes()).hexdigest()
        (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in
                      ("status", "directory", "emulated_ms", "matched")}))
    if "error" in result:
        print(result["error"], file=sys.stderr)
    return 0 if result["status"] in ("pass", "observed") else 1


def main():
    args = arguments()
    try:
        emulator = emulator_binary(args.emulator)
        image = args.image.resolve(strict=True)
        if not os.access(emulator, os.X_OK):
            raise RuntimeError(f"not executable: {emulator}")
        if image.stat().st_size != 1474560:
            raise RuntimeError("expected a raw 1.44 MB floppy image (1474560 bytes)")
        if not args.prepare_only and shutil.which("Xvfb") is None:
            raise RuntimeError("Xvfb is required")
        roms = rom_directory(args.rompath)
        if args.vm:
            return run_existing_vm(args, emulator, image, roms)
        (ROOT / "build").mkdir(exist_ok=True)
        run_dir = ROOT / "build" / (
            f"86box-{args.profile or 'vm'}-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:6]}")
        run_dir.mkdir(mode=0o700)
        if len(os.fsencode(run_dir / "automation.sock")) >= 108:
            raise RuntimeError("workspace path is too long for a UNIX automation socket")
        shutil.copyfile(image, run_dir / "floppy.img")
        machine = write_config(args, run_dir)
        (run_dir / "global.cfg").write_text("")
        command = [str(emulator), "--automation", f"unix:{run_dir / 'automation.sock'}",
                   "--automation-hold", "--automation-seed", "1", "--noconfirm",
                   "--vmpath", str(run_dir), "--global", str(run_dir / "global.cfg"),
                   "--rompath", str(roms), "--logfile", str(run_dir / "emulator.log"),
                   str(run_dir / "86box.cfg")]
        manifest = {"command": command, "image": str(image),
                    "image_sha256": hashlib.sha256((run_dir / "floppy.img").read_bytes()).hexdigest(),
                    "rompath": str(roms), "machine": machine,
                    "options": {"emulated_ms": args.emulated_ms,
                                "wall_seconds": args.wall_seconds,
                                "step_ms": args.step_ms, "keys": args.key,
                                "bios_f1": args.bios_f1, "expect": args.expect,
                                "reject": args.reject,
                                "sample_cpu_seconds": getattr(args, "sample_cpu_seconds", 30.0),
                                "accuracy": getattr(args, "accuracy", False),
                                "perf": getattr(args, "perf", False)},
                    "host": host_metadata()}
        (run_dir / "launch.json").write_text(json.dumps(manifest, indent=2) + "\n")
        if args.prepare_only:
            print(json.dumps({"status": "prepared", "directory": str(run_dir)}))
            return 0
        result = execute(args, run_dir, command, machine)
        result["image_sha256"] = manifest["image_sha256"]
        result["emulator_sha256"] = hashlib.sha256(emulator.read_bytes()).hexdigest()
        (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({key: result[key] for key in
                          ("status", "directory", "emulated_ms", "matched")}))
        if "error" in result:
            print(result["error"], file=sys.stderr)
        return 0 if result["status"] in ("pass", "observed") else 1
    except (OSError, RuntimeError, ValueError, configparser.Error) as error:
        print(f"86Box smoke: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    def interrupt(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    sys.exit(main())
