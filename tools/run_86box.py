#!/usr/bin/env python3
"""Run one combined correctness/unthrottled-capacity benchmark in isolated 86Box."""

import argparse
import configparser
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
        "Never downloads firmware. All mutable state stays in a new build/86box-* "
        "directory. No original config or image is modified. Without --expect, "
        "DONE completes observation with strict nonzero summary counts, not a "
        "guest-test PASS (FAIL/FAULT remain visible). Default limits: 24 guest "
        "hours and 8 host hours. Ratios measure unthrottled speed capacity, not "
        "the GUI's actual-speed percentage. All emulator threads are charged; "
        "boundary steps and non-step RPC work are excluded from workload ratios. COM1 is "
        "captured; port 0xE9 is not an automation capture source. Fresh CMOS "
        "may require --bios-f1. Graphical BIOS prompts cannot be read by 'text'; "
        "AL440LX needs --key 20000:0x3b to resume its fresh-CMOS F1 prompt and "
        "may leave COM1 inaccessible (use screen.log). --config imports only Machine and Video keys; "
        "disks, networking, host backends and other peripheral settings are discarded."
    ))
    parser.add_argument("--emulator", type=Path,
                        default=ROOT.parent / "86Box/build/src/86Box")
    parser.add_argument("--image", type=Path, default=ROOT / "floppy.img")
    parser.add_argument("--rompath", type=Path,
                        help="local ROM directory (normally containing machines/ and video/)")
    parser.add_argument("--profile", choices=PROFILES, default="mmx",
                        help="generated machine/CPU profile; ignored with --config")
    parser.add_argument("--config", type=Path,
                        help="explicit source config; snapshot and import safe machine/video keys only")
    parser.add_argument("--dynarec", choices=("on", "off"),
                        help="override the profile/config execution mode")
    parser.add_argument("--label", default="", help="emulator build/mode identifier")
    parser.add_argument("--memory-mb", type=int, choices=(16, 32), default=16)
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
                        help="press F1 when the BIOS screen requests it")
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


def write_config(args, run_dir):
    machine, family, speed, multiplier, fpu = PROFILES[args.profile]
    config = configparser.ConfigParser(interpolation=None)
    config["Machine"] = {
        "machine": machine, "cpu_family": family, "cpu_speed": str(speed),
        "cpu_multi": str(multiplier), "fpu_type": fpu,
        "cpu_use_dynarec": "1", "time_sync": "disabled",
    }
    config["Video"] = {"gfxcard": "et4000ax"}
    if args.config:
        snapshot = run_dir / "source.cfg"
        shutil.copyfile(args.config.resolve(strict=True), snapshot)
        original = configparser.ConfigParser(interpolation=None, strict=False)
        original.read(snapshot)
        if not original.has_option("Machine", "machine"):
            raise RuntimeError("--config needs a [Machine] machine setting")
        config["Machine"].clear()
        for key in ("machine", "cpu_family", "cpu_speed", "cpu_multi", "fpu_type",
                    "cpu_use_dynarec", "cpu_waitstates"):
            if original.has_option("Machine", key):
                config["Machine"][key] = original["Machine"][key]
        if original.has_option("Video", "gfxcard"):
            config["Video"]["gfxcard"] = original["Video"]["gfxcard"]
    config["Machine"]["mem_size"] = str(args.memory_mb * 1024)
    if args.dynarec is not None:
        config["Machine"]["cpu_use_dynarec"] = "1" if args.dynarec == "on" else "0"
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
    result = {"status": "error", "expected": args.expect, "matched": [],
              "machine": machine, "label": args.label, "step_ms": args.step_ms,
              "emulated_ms": 0, "directory": str(run_dir), "host": host_metadata(),
              "emulator_executable": command[0],
              "limits": {"emulated_ms": args.emulated_ms, "wall_seconds": args.wall_seconds},
              "perf": {"status": "requested" if recorder else "disabled"}}
    try:
        display, display_name = start_display(run_dir, deadline)
        environment = os.environ.copy()
        # Keep the session runtime directory so OpenAL can reach PipeWire.
        environment.update(DISPLAY=display_name, QT_QPA_PLATFORM="xcb",
                           XDG_CONFIG_HOME=str(run_dir / "config"),
                           XDG_CACHE_HOME=str(run_dir / "cache"),
                           XDG_DATA_HOME=str(run_dir / "data"),
                           TMPDIR=str(run_dir / "scratch"))
        for name in ("config", "cache", "data", "scratch"):
            (run_dir / name).mkdir(mode=0o700)
        with (run_dir / "process.log").open("wb") as output:
            emulator = subprocess.Popen(command, cwd=run_dir, env=environment,
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
                for lease in capture.feed(data, now, time.monotonic()):
                    api.request(f"serial write 1 {lease.encode('ascii').hex()}")
                if recorder is not None:
                    recorder.check()
                serial_tail += data.decode("cp437")
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
                if args.expect and all(marker in seen for marker in args.expect):
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
            result["final_info"] = api.request("info")
            result["registers"] = api.request("regs")
            screenshot = api.request(f"screenshot {run_dir / 'screen.png'}",
                                     allow_failure=True)
            if not screenshot.get("ok"):
                result["screenshot_note"] = screenshot.get("error")
            result["status"] = ("failed" if result.get("rejected") else
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


def main():
    args = arguments()
    try:
        emulator = args.emulator.resolve(strict=True)
        image = args.image.resolve(strict=True)
        if not os.access(emulator, os.X_OK):
            raise RuntimeError(f"not executable: {emulator}")
        if image.stat().st_size != 1474560:
            raise RuntimeError("expected a raw 1.44 MB floppy image (1474560 bytes)")
        if not args.prepare_only and shutil.which("Xvfb") is None:
            raise RuntimeError("Xvfb is required")
        roms = rom_directory(args.rompath)
        (ROOT / "build").mkdir(exist_ok=True)
        run_dir = ROOT / "build" / (
            f"86box-{args.profile}-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:6]}")
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
    sys.exit(main())
