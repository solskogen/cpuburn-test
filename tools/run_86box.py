#!/usr/bin/env python3
"""Run CPU checks and capacity measurements using an existing 86Box VM."""

import argparse
import configparser
from contextlib import contextmanager
import fcntl
import gzip
import re
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid
from qemu_smoke import fpu_backend, host_fpu_note, parse_log
from workload_capture import (EmulationClock, PerfRecorder, ThreadCounters, WorkloadCapture,
                              host_metadata, host_state, is_linux, process_cpu_seconds)
from perf_report import aa_noise_floor, build_summary, fpuprobe


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


def non_negative_int(value):
    number = int(value, 0)
    if number < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return number


def non_negative(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("must be finite and not negative")
    return number


RECORD_NAME = re.compile(r"[A-Za-z0-9_.:+-]+")
SEL1_MAX = 4096


def record_names(value):
    """A comma-separated list of guest record names (repeatable option)."""
    names = [name.strip() for name in value.split(",") if name.strip()]
    for name in names:
        if not RECORD_NAME.fullmatch(name):
            raise argparse.ArgumentTypeError(f"invalid record name {name!r}")
    return names


def record_name(value):
    if not RECORD_NAME.fullmatch(value):
        raise argparse.ArgumentTypeError(f"invalid record name {value!r}")
    return value


def selection_frame(only=(), skip=(), resume_from=None):
    """The host's half of the sel1 runtime test-selection exchange:
    `@sel1 <only> <skip> <from>`, each field a comma list or `-`."""
    for name in [*only, *skip, *([resume_from] if resume_from else [])]:
        if not RECORD_NAME.fullmatch(name):
            raise ValueError(f"invalid record name {name!r}")
    frame = "@sel1 {} {} {}\n".format(",".join(only) or "-", ",".join(skip) or "-",
                                      resume_from or "-").encode("ascii")
    if len(frame) > SEL1_MAX:
        raise ValueError(f"selection frame is {len(frame)} bytes; the guest accepts {SEL1_MAX}")
    return frame


def selection_accepted(run, only=(), skip=(), resume_from=None):
    return any(row.get("protocol") == "sel1" and row.get("accepted") == "1"
               and row.get("only") == str(len(only)) and row.get("skip") == str(len(skip))
               and row.get("from") == (resume_from or "-")
               for row in run.get("cpu_control", []))


def flatten(lists):
    # The guest marks only the first occurrence of a repeated name as found.
    return list(dict.fromkeys(name for names in lists or [] for name in names))


def sampling_options(args):
    return {"min_seconds": getattr(args, "min_sample_seconds", 1.0),
            "max_seconds": getattr(args, "max_sample_seconds", 10.0),
            "tolerance": getattr(args, "settle_tolerance", 0.02),
            "block_seconds": getattr(args, "block_seconds", 0.2),
            "fast_tolerance": getattr(args, "fast_settle_tolerance", 0.05),
            "fast_percent": getattr(args, "fast_speed_percent", 150.0)}


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
        "Requires an AUTOMATION-enabled 86Box and locally installed ROMs; 86Box opens in the session's Wayland or X display, or offscreen without one. "
        "Never downloads firmware. --vm runs the supplied VM in place, temporarily "
        "inserting the test floppy and enabling COM1, then restores its config. Without --expect, "
        "DONE completes observation with strict nonzero summary counts; guest "
        "FAIL/FAULT results give a nonzero exit. Default limits: 24 guest "
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
    # Sample lengths are counted on the settle clock: the emulation thread's CPU
    # time when /proc gives it in ns, host wall time otherwise.
    parser.add_argument("--min-sample-seconds", type=positive, default=1.0,
                        help="settle-clock seconds a sample must run before it may settle (default: 1)")
    parser.add_argument("--max-sample-seconds", type=positive, default=10.0,
                        help="settle-clock seconds after which an unsettled sample stops and is "
                             "flagged capped (default: 10)")
    parser.add_argument("--settle-tolerance", type=positive, default=0.02,
                        help="relative interval and drift that count as settled (default: 0.02)")
    parser.add_argument("--fast-settle-tolerance", type=positive, default=0.05,
                        help="tolerance for samples at or above --fast-speed-percent (default: 0.05; "
                             "set it equal to --settle-tolerance for fine A/B work on fast records)")
    parser.add_argument("--fast-speed-percent", type=positive, default=150.0,
                        help="capacity from which --fast-settle-tolerance applies (default: 150)")
    parser.add_argument("--block-seconds", type=positive, default=0.2,
                        help="settle-clock seconds per speed block in the settle test (default: 0.2)")
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
    run = parser.add_argument_group("run control")
    run.add_argument("--startup-seconds", type=positive, default=120,
                     help="host seconds for 86Box to open its automation socket (default 120)")
    run.add_argument("--request-seconds", type=positive, default=300,
                     help="host seconds one automation request may take (default 300)")
    run.add_argument("--hang-seconds", type=non_negative, default=600,
                     help="report HANG when no WORK/TEST/BEGIN line arrives for this many "
                          "guest seconds, e.g. a test stuck with IF=0 (default 600, 0 = off)")
    run.add_argument("--on-hang", choices=("stop", "continue"), default="stop",
                     help="after a HANG, stop the run (default) or keep stepping")
    run.add_argument("--boundaries", choices=("wait-serial", "step"), default="wait-serial",
                     help="end steps at WORK lines with automation 'wait serial' (default; "
                          "falls back to fixed steps if 86Box lacks it) or use fixed steps")
    run.add_argument("--text-poll-ms", type=positive_int, default=2000,
                     help="emulated ms between screen 'text' polls while COM1 is busy; polled "
                          "when COM1 is idle at this interval (default 2000)")
    run.add_argument("--transcript", choices=TRANSCRIPT_MODES, default="compact",
                     help="automation.jsonl.gz detail: off, errors (failed requests only), "
                          "compact (default: no screen/serial bodies, screen hashes on change), "
                          "full (every reply)")
    run.add_argument("--transcript-max-mb", type=positive, default=256,
                     help="uncompressed MB before automation.jsonl.gz rotates to "
                          "automation.1.jsonl.gz (default 256)")
    run.add_argument("--headless", action="store_true",
                     help="offscreen Qt, null OpenAL/SDL audio and sound_muted=1 (restored)")
    run.add_argument("--repeat", type=positive_int, default=1,
                     help="run the suite N times (repeat-NN subdirectories) and aggregate")
    run.add_argument("--aa", action="store_true",
                     help="A/A: run at least twice and report a capacity noise floor")
    host = parser.add_argument_group("host measurement (Linux)")
    host.add_argument("--pin", type=non_negative_int, metavar="CORE",
                      help="pin 86Box (all threads) to one logical CPU, like taskset -c CORE")
    host.add_argument("--perf-stat", action="store_true",
                      help="count cycles/instructions of the emulation thread per step (IPC)")
    host.add_argument("--perf-freq", type=positive_int, default=999,
                      help="--perf sampling frequency in Hz (default 999)")
    host.add_argument("--perf-callgraph", choices=("dwarf", "fp", "none"), default="dwarf",
                      help="--perf call-graph mode (default dwarf)")
    selection = parser.add_argument_group("runtime test selection (images with sel1)")
    selection.add_argument("--only", type=record_names, action="append",
                           help="run only these comma-separated records (repeatable)")
    selection.add_argument("--skip", type=record_names, action="append",
                           help="skip these comma-separated records (repeatable)")
    selection.add_argument("--resume-from", type=record_name,
                           help="start at this record, skipping those before it")
    args = parser.parse_args()
    if args.min_sample_seconds > args.max_sample_seconds:
        parser.error("--min-sample-seconds must not exceed --max-sample-seconds")
    if not is_linux():
        for option in ("pin", "perf", "perf_stat"):
            if getattr(args, option) not in (None, False):
                parser.error(f"--{option.replace('_', '-')} requires Linux")
    if args.pin is not None and args.pin not in os.sched_getaffinity(0):
        parser.error(f"--pin {args.pin}: CPU is not available to this process")
    args.only, args.skip = flatten(args.only), flatten(args.skip)
    try:
        selection_frame(args.only, args.skip, args.resume_from)
    except ValueError as error:
        parser.error(str(error))
    if args.aa:
        args.repeat = max(2, args.repeat)
    return args


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


# Signals that end a run through the normal cleanup path (see __main__).
CLEANUP_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
BACKUP_SUFFIX = ".cpuburn-backup"
QUIT_SECONDS = 30


def config_backup(source):
    return source.with_name(source.name + BACKUP_SUFFIX)


def write_durably(path, data):
    with path.open("wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def vm_in_command(command, cwd, source):
    """Whether an 86Box command line (argv bytes, working directory) runs `source`.

    Covers --vmpath/-P, a configuration path given absolutely or relative to
    the process directory, and a bare `86Box` started inside the VM directory,
    whose VM path defaults to the working directory."""
    def resolve(word):
        path = Path(os.fsdecode(word))
        if not path.is_absolute():
            if cwd is None:
                return None
            path = cwd / path
        return path.resolve()

    vm_dir, config = source.parent.resolve(), source.resolve()
    explicit = False
    for index, word in enumerate(command[1:], 1):
        if word in (b"--vmpath", b"-P"):
            explicit = True
            if index + 1 < len(command) and resolve(command[index + 1]) == vm_dir:
                return True
        elif word.endswith(b".cfg") and not word.startswith(b"-"):
            explicit = True
            if resolve(word) == config:
                return True
    return not explicit and cwd is not None and cwd.resolve() == vm_dir


def refuse_running(source):
    """Fail if an 86Box process already has this VM open (Linux /proc only)."""
    if not Path("/proc/self/cmdline").exists():
        print(f"warning: cannot check whether {source} is already open without /proc",
              file=sys.stderr)
        return
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
        if vm_in_command(command, cwd, source):
            raise RuntimeError(f"VM is already running (PID {process.name}): {source}")


@contextmanager
def edited_config(source, edit):
    """The VM's configuration with edit(config) applied for the run, locked
    against a second run, and restored byte-for-byte afterwards.

    The original is also kept as <config>.cpuburn-backup until it has been
    written back, so even an uncatchable termination (SIGKILL, power loss)
    leaves it recoverable; a leftover backup refuses the next run."""
    backup = config_backup(source)
    if backup.exists():
        raise RuntimeError(
            f"{backup} exists: an earlier run was interrupted before it restored "
            f"{source.name}. Compare the two, move the backup over {source.name} "
            "(or delete it if the configuration is already correct), then rerun.")
    with source.open("r+b") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError(f"VM configuration is already in use: {source}") from None
        original = handle.read()
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.read_string(original.decode("utf-8-sig"))
        edit(config)
        staging = backup.with_name(backup.name + ".tmp")
        write_durably(staging, original)
        shutil.copymode(source, staging)
        os.replace(staging, backup)
        try:
            with source.open("w", encoding="utf-8") as output:
                config.write(output)
            yield config, hashlib.sha256(original).hexdigest()
        finally:
            # A second Ctrl+C must not interrupt the truncate-and-write.
            mask = signal.pthread_sigmask(signal.SIG_BLOCK, CLEANUP_SIGNALS)
            try:
                write_durably(source, original)
                backup.unlink()
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, mask)


@contextmanager
def inplace_vm(args, image):
    source = vm_config(args.vm)
    refuse_running(source)

    def edit(config):
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
            fdd_01_type="35_2hd", fdd_01_fn=str(image), fdd_01_writeprot="1",
            fdd_01_turbo="1")
        if getattr(args, "headless", False):
            if not config.has_section("General"):
                config.add_section("General")
            config["General"]["sound_muted"] = "1"
            # Offscreen Qt draws nothing; the OpenGL renderer then skips the software
            # renderer's per-frame host copies. Frames, text and screenshots are unchanged.
            config["General"]["vid_renderer"] = "qt_opengl"

    with edited_config(source, edit) as (config, source_hash):
        yield source, dict(config["Machine"]), source_hash


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
    if getattr(args, "headless", False):
        config["General"]["sound_muted"] = "1"
    config["Ports (COM & LPT)"] = {
        "serial1_enabled": "1", "serial1_device": "automation",
        "serial2_enabled": "0", "lpt1_enabled": "0",
    }
    config["Floppy and CD-ROM drives"] = {
        "fdd_01_type": "35_2hd", "fdd_01_fn": str(run_dir / "floppy.img"),
        "fdd_01_writeprot": "1", "fdd_01_turbo": "1", "fdd_02_type": "none",
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


TRANSCRIPT_MODES = ("off", "errors", "compact", "full")
BODY_KEYS = ("data", "chars", "attrs")


class Transcript:
    """automation.jsonl.gz: the requests and replies of a run.

    full keeps every reply; compact (default) replaces screen and serial
    bodies with sizes and hashes and logs `text` only when the screen changes;
    errors keeps failed requests only; off writes nothing. Output is gzip,
    flushed about once a second, and rotates to automation.1.jsonl.gz once
    max_bytes of uncompressed JSON have been written."""

    def __init__(self, directory, mode="compact", max_bytes=256 << 20, flush_seconds=1.0):
        if mode not in TRANSCRIPT_MODES:
            raise ValueError(f"unknown transcript mode {mode!r}")
        self.directory = Path(directory)
        self.mode = mode
        self.max_bytes = max_bytes
        self.flush_seconds = flush_seconds
        self.path = self.directory / "automation.jsonl.gz"
        self.handle = None
        self.size = 0
        self.total = 0
        self.records = 0
        self.suppressed = 0
        self.rotations = 0
        self.last_screen = None
        self.last_flush = time.monotonic()

    def _entry(self, command, reply):
        failed = not reply.get("ok") or bool(reply.get("fatal"))
        if self.mode == "full":
            return {"command": command, "reply": reply}
        if self.mode == "errors" and not failed:
            return None
        if command == "text" and not failed:
            digest = hashlib.sha256("".join(reply.get("chars", [])).encode()).hexdigest()
            if digest == self.last_screen:
                return None
            self.last_screen = digest
        compact = {}
        for key, value in reply.items():
            if key in BODY_KEYS and isinstance(value, (str, list)):
                text = "".join(value) if isinstance(value, list) else value
                compact[key + "_bytes"] = len(text) // 2
                if key == "chars":
                    compact["chars_sha256"] = self.last_screen
            else:
                compact[key] = value
        return {"command": command, "reply": compact}

    def record(self, command, reply):
        if self.mode == "off":
            return
        entry = self._entry(command, reply)
        if entry is None:
            self.suppressed += 1
            return
        line = json.dumps(entry) + "\n"
        if self.handle is None:
            self.handle = gzip.open(self.path, "wt", encoding="utf-8", compresslevel=6)
        elif self.size + len(line) > self.max_bytes:
            self.handle.close()
            os.replace(self.path, self.directory / "automation.1.jsonl.gz")
            self.rotations += 1
            self.size = 0
            self.handle = gzip.open(self.path, "wt", encoding="utf-8", compresslevel=6)
        self.handle.write(line)
        self.size += len(line)
        self.total += len(line)
        self.records += 1
        now = time.monotonic()
        if now - self.last_flush >= self.flush_seconds:
            self.handle.flush()
            self.last_flush = now

    def close(self):
        if self.handle is not None:
            self.handle.close()
            self.handle = None
        return {"mode": self.mode,
                "path": str(self.path) if self.mode != "off" and self.records else None,
                "records": self.records, "suppressed": self.suppressed,
                "uncompressed_bytes": self.total, "rotations": self.rotations,
                "compressed_bytes": (self.path.stat().st_size if self.path.exists() else 0)}


class RequestTimeout(TimeoutError):
    """One automation request exceeded --request-seconds."""


class Automation:
    def __init__(self, connection, transcript, deadline, request_seconds=None):
        self.connection = connection
        self.buffer = b""
        self.transcript = transcript
        self.deadline = deadline
        self.request_seconds = request_seconds
        self.timing = {}

    def _remaining(self, command, limit):
        now = time.monotonic()
        if self.deadline - now <= 0:
            raise TimeoutError("wall-clock deadline exceeded")
        if limit - now <= 0:
            raise RequestTimeout(f"86Box did not answer {command.split()[0]!r} within "
                                 f"{self.request_seconds:g} s (--request-seconds)")
        return min(self.deadline, limit) - now

    def request(self, command, allow_failure=False):
        began = time.monotonic()
        limit = (began + self.request_seconds if self.request_seconds else math.inf)
        self.connection.settimeout(self._remaining(command, limit))
        self.connection.sendall(command.encode("utf-8") + b"\n")
        while b"\n" not in self.buffer:
            self.connection.settimeout(self._remaining(command, limit))
            try:
                chunk = self.connection.recv(65536)
            except socket.timeout:
                self._remaining(command, limit)
                raise
            if not chunk:
                raise RuntimeError("86Box closed automation connection; see emulator.log")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        verb = " ".join(command.split()[:2 if command.startswith(("serial", "key")) else 1])
        entry = self.timing.setdefault(verb, [0, 0.0])
        entry[0] += 1
        entry[1] += time.monotonic() - began
        reply = json.loads(line)
        if hasattr(self.transcript, "record"):
            self.transcript.record(command, reply)
        elif self.transcript is not None:
            self.transcript.write(json.dumps({"command": command, "reply": reply}) + "\n")
            self.transcript.flush()
        if reply.get("fatal"):
            raise RuntimeError(f"86Box stopped on a fatal error: {reply.get('error')}")
        if not reply.get("ok") and not allow_failure:
            raise RuntimeError(f"Automation {command!r}: {reply}")
        return reply


def shutdown_emulator(api, emulator, seconds):
    """Ask 86Box to quit, so it can save NVRAM and flush disks, and wait for it.

    Returns True if it is no longer running. The caller still signals the
    process group afterwards, which is a no-op once 86Box has exited."""
    if emulator is None or emulator.poll() is not None:
        return True
    if api is not None:
        api.deadline = time.monotonic() + seconds
        try:
            api.request("quit", allow_failure=True)
        except (OSError, RuntimeError, ValueError, TimeoutError):
            pass
    try:
        emulator.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        return False
    return True


# Emulators started by this process, stopped by PID on any exit path.
LIVE_EMULATORS = set()


@contextmanager
def signals_blocked():
    """Hold back CLEANUP_SIGNALS (delivered on exit) around critical sections."""
    mask = signal.pthread_sigmask(signal.SIG_BLOCK, CLEANUP_SIGNALS)
    try:
        yield
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, mask)


def launch_emulator(command, preexec_fn=None, **options):
    """Popen 86Box in its own session; registered before any signal can land.
    The child would inherit the blocked mask, and then ignore stop_process's
    SIGTERM, so it unblocks the signals before exec (after preexec_fn)."""
    def prepare():
        if preexec_fn is not None:
            preexec_fn()
        signal.pthread_sigmask(signal.SIG_UNBLOCK, CLEANUP_SIGNALS)

    with signals_blocked():
        process = subprocess.Popen(command, start_new_session=True, preexec_fn=prepare,
                                   **options)
        LIVE_EMULATORS.add(process)
    return process


def stop_all_emulators():
    for process in list(LIVE_EMULATORS):
        try:
            stop_process(process)
        except (OSError, subprocess.SubprocessError):
            pass
        LIVE_EMULATORS.discard(process)


def run_main(main):
    """main() with SIGTERM and SIGHUP (closed terminal, dropped SSH) raised as
    KeyboardInterrupt like SIGINT, so an in-place VM configuration is restored,
    and every 86Box started here (in its own session) stopped by PID on exit."""
    def interrupt(signum, frame):
        raise KeyboardInterrupt

    for number in CLEANUP_SIGNALS[1:]:
        signal.signal(number, interrupt)
    try:
        return main()
    finally:
        with signals_blocked():
            stop_all_emulators()


def stop_process(process):
    if process is None or process.poll() is not None:
        LIVE_EMULATORS.discard(process)
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        LIVE_EMULATORS.discard(process)
        return
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=3)
    LIVE_EMULATORS.discard(process)


def display_environment():
    """How 86Box shows itself: natively in the session it was started from,
    Wayland's or X's, otherwise with Qt's offscreen platform, which needs no
    display at all."""
    if os.environ.get("WAYLAND_DISPLAY"):
        return {"QT_QPA_PLATFORM": "wayland"}
    if os.environ.get("DISPLAY"):
        return {"QT_QPA_PLATFORM": "xcb"}
    return {"QT_QPA_PLATFORM": "offscreen"}


# 86Box takes audio from OpenAL Soft (and SDL in some builds); null drivers
# keep a headless run off the host's sound server.
HEADLESS_ENVIRONMENT = {"QT_QPA_PLATFORM": "offscreen", "ALSOFT_DRIVERS": "null",
                        "SDL_AUDIO_DRIVER": "dummy"}


def bios_f1_prompt(text):
    """A text-mode BIOS prompt to press F1 to resume or continue."""
    return any(
        "press" in line.lower() and
        ("<f1>" in line.lower() or "f1 to" in line.lower()) and
        ("resume" in line.lower() or "continue" in line.lower())
        for line in text.splitlines())


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


CPU1_READY = "CONTROL protocol=cpu1 ready=1"
SEL1_READY = "CONTROL protocol=sel1 ready=1"
WORK_PATTERN = "WORK phase="
TESTS_STARTED = re.compile(r"(?m)^(BEGIN |WORK |TEST |DONE\r?$)")
PROGRESS = ("WORK ", "TEST ", "BEGIN ")
# 86Box's automation request line limit is 4096 bytes; hex doubles the frame.
SERIAL_WRITE_CHUNK = 1024
TAIL_STEPS = 50


def boundary_pattern(tests_started, control_sent, selection_state):
    """What the next `wait serial` stops at: the cpu1/sel1 handshakes while
    they are due, then WORK lines, so samples end at their boundaries."""
    if tests_started:
        return WORK_PATTERN
    if not control_sent:
        return CPU1_READY
    if selection_state == "waiting":
        return SEL1_READY
    return WORK_PATTERN


def scan_start(serial_from, partial, pattern):
    """`from` for a wait: back far enough to see a pattern split across the
    previous read, not so far that an already-consumed match is found again."""
    return max(0, serial_from - min(len(partial), len(pattern) - 1))


def split_at_match(data, read_from, at):
    """(head, tail) of a read starting at position read_from, split at `at`."""
    cut = max(0, min(len(data), at - read_from))
    return data[:cut], data[cut:]


def execute(args, run_dir, command, machine):
    started = time.monotonic()
    deadline = started + args.wall_seconds
    emulator = connection = api = counters = None
    wait_serial = getattr(args, "boundaries", "wait-serial") == "wait-serial"
    capture = WorkloadCapture(1 if wait_serial else args.step_ms, **sampling_options(args))
    recorder = (PerfRecorder(run_dir, getattr(args, "perf_freq", 999),
                             getattr(args, "perf_callgraph", "dwarf"))
                if getattr(args, "perf", False) else None)
    perf_stopped = False
    accuracy = getattr(args, "accuracy", False)
    control_sent = False
    signature = getattr(args, "expected_signature", None)
    control_frame = cpu_control_frame(machine, accuracy, signature)
    pin = getattr(args, "pin", None)
    only, skip = getattr(args, "only", None) or [], getattr(args, "skip", None) or []
    resume_from = getattr(args, "resume_from", None)
    selection_requested = bool(only or skip or resume_from)
    selection = {"requested": selection_requested, "only": only, "skip": skip,
                 "resume_from": resume_from, "state": "waiting", "accepted": False}
    selection_bytes = selection_frame(only, skip, resume_from)
    transcript = Transcript(run_dir, getattr(args, "transcript", "compact"),
                            int(getattr(args, "transcript_max_mb", 256) * 2 ** 20))
    text_poll_ms = getattr(args, "text_poll_ms", 2000)
    hang_ms = getattr(args, "hang_seconds", 600) * 1000
    on_hang = getattr(args, "on_hang", "stop")
    series = {}
    result = {"status": "error", "expected": args.expect, "matched": [],
              "machine": machine, "fpu_backend": fpu_backend(machine),
              "label": args.label, "step_ms": args.step_ms,
              "emulated_ms": 0, "directory": str(run_dir), "host": host_metadata(pin),
              "host_state": {"start": host_state(pin)},
              "emulator_executable": command[0],
              "mode": "accuracy" if accuracy else "benchmark",
              "boundaries": "wait-serial" if wait_serial else "step",
              "limits": {"emulated_ms": args.emulated_ms, "wall_seconds": args.wall_seconds},
              "perf": {"status": "requested" if recorder else "disabled"}}
    try:
        environment = os.environ.copy()
        # Keep the session runtime directory so OpenAL can reach PipeWire.
        environment.update(display_environment())
        if getattr(args, "headless", False):
            environment.update(HEADLESS_ENVIRONMENT)
        if not getattr(args, "vm", None):
            environment.update(
                           XDG_CONFIG_HOME=str(run_dir / "config"),
                           XDG_CACHE_HOME=str(run_dir / "cache"),
                           XDG_DATA_HOME=str(run_dir / "data"),
                           TMPDIR=str(run_dir / "scratch"))
            for name in ("config", "cache", "data", "scratch"):
                (run_dir / name).mkdir(mode=0o700)
        affinity = None
        if pin is not None:
            def affinity():
                os.sched_setaffinity(0, {pin})
        with (run_dir / "process.log").open("wb") as output:
            emulator = launch_emulator(command, cwd=(vm_config(args.vm).parent
                                       if getattr(args, "vm", None) else run_dir),
                                       env=environment, stdout=output, stderr=output,
                                       preexec_fn=affinity)
        socket_path = run_dir / "automation.sock"
        startup_seconds = getattr(args, "startup_seconds", 120)
        startup_deadline = min(deadline, started + startup_seconds)
        while time.monotonic() < startup_deadline:
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
            raise TimeoutError(f"86Box automation socket startup timed out after "
                               f"{time.monotonic() - started:.0f} s (--startup-seconds)")
        result["startup_wall_seconds"] = time.monotonic() - started
        with (run_dir / "serial.log").open("wb") as serial_log, \
                (run_dir / "screen.log").open("w") as screen_log:
            api = Automation(connection, transcript, deadline,
                             getattr(args, "request_seconds", 300))
            api.request("status")
            # Requests run on the emulation thread, so it exists by now.
            clock = EmulationClock(emulator.pid, lambda: process_cpu_seconds(emulator.pid))
            result["cpu_scope"] = clock.locate()
            capture.cpu_scope = clock.scope
            if clock.note:
                result["cpu_accounting_note"] = clock.note
            if getattr(args, "perf_stat", False):
                if clock.tid is None:
                    raise RuntimeError("--perf-stat needs 86Box's emulation thread "
                                       f"({clock.note})")
                counters = ThreadCounters(clock.tid)
                result["perf_stat"] = {"thread": clock.tid, "scope": counters.scope,
                                       "events": [name for name, _ in counters.EVENTS]}
            if recorder is not None:
                recorder.start(emulator.pid, clock.tid)
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
            partial = b""
            progress_buffer = ""
            last_progress_ms = 0
            last_name = None
            hang_open = False
            tests_started = False
            seen = set()
            keys = sorted(args.key)
            last_screen = None
            last_text_ms = None
            last_f1 = -5000
            text = ""
            boot = result.setdefault("boot_milestones", {})

            def snapshot(ms):
                cpu, delay = clock.sample()
                return (time.monotonic(), cpu, ms, delay)

            def read_serial():
                nonlocal serial_from
                reply = api.request(f"serial read 1 {serial_from}")
                if reply.get("lost"):
                    raise RuntimeError("COM1 capture overflowed; decrease --step-ms")
                start, serial_from = serial_from, reply["next"]
                return start, bytes.fromhex(reply["data"])

            def poll_screen(now):
                nonlocal last_screen, last_text_ms, text
                screen = api.request("text", allow_failure=True)
                last_text_ms = now
                text = "\n".join(bytes.fromhex(row).decode("cp437")
                                 for row in screen.get("chars", []))
                if text != last_screen:
                    screen_log.write(f"\n--- {now} ms ---\n{text}\n")
                    screen_log.flush()
                    last_screen = text

            while result["emulated_ms"] < args.emulated_ms:
                now = result["emulated_ms"]
                while keys and keys[0][0] <= now:
                    _, code = keys.pop(0)
                    api.request(f"key down {code}")
                    now = api.request(f"key up {code}")["ms"]
                target = min(args.emulated_ms, now + args.step_ms)
                if keys and now < keys[0][0] < target:
                    target = keys[0][0]
                if recorder is not None:
                    if capture.active is not None and capture.active["phase"] == "sample":
                        recorder.enable()
                    else:
                        recorder.disable()
                match_at = step_row = before = None
                if target > now:
                    counts_before = counters.read() if counters else None
                    before = snapshot(now)
                    reply = None
                    if wait_serial:
                        pattern = boundary_pattern(tests_started, control_sent,
                                                   selection["state"])
                        reply = api.request(
                            f"wait serial {target - now} 1 "
                            f"{scan_start(serial_from, partial, pattern)} {pattern}",
                            allow_failure=True)
                        if not reply.get("ok") or "ms" not in reply or "next" not in reply:
                            wait_serial = False
                            capture.step_ms = args.step_ms
                            result["boundaries"] = "step"
                            result["boundary_note"] = ("automation 'wait serial' unavailable "
                                                       f"({reply.get('error', 'no ms/next')}); "
                                                       "fixed steps")
                            reply = None
                        elif reply.get("matched") and "at" in reply:
                            match_at = reply["at"]
                    if reply is None:
                        reply = api.request(f"step {target - now}")
                    now = reply["ms"]
                    after = snapshot(now)
                    capture.observe_step(before, after)
                    step_row = [round(after[0] - before[0], 6), round(after[1] - before[1], 9),
                                after[2] - before[2]]
                    if counters:
                        step_row += [round(b - a) for a, b in zip(counts_before, counters.read())]
                result["emulated_ms"] = now
                read_from, data = read_serial()
                tail = b""
                if match_at is not None:
                    data, tail = split_at_match(data, read_from, match_at)
                head_ms = now
                if tail:
                    # Finish the boundary line in 1 ms steps, charged to no workload.
                    for _ in range(TAIL_STEPS):
                        # The boundary line is whole once the tail has a newline; what
                        # follows it belongs to no sample, so later bytes need no 1 ms steps.
                        if b"\n" in tail or now >= args.emulated_ms:
                            break
                        tail_before = snapshot(now)
                        now = api.request("step 1")["ms"]
                        capture.observe_unattributed(tail_before, snapshot(now))
                        tail += read_serial()[1]
                    result["emulated_ms"] = now
                leases = []
                for index, (chunk, chunk_ms) in enumerate(((data, head_ms), (tail, now))):
                    if index and not chunk:
                        continue
                    serial_log.write(chunk)
                    serial_log.flush()
                    decoded = chunk.decode("cp437")
                    for marker in ("CPUBURN-TEST", "WORK phase=", "TEST r="):
                        if marker in decoded and marker not in boot:
                            boot[marker] = {"wall_seconds": round(time.monotonic() - started, 3),
                                            "emulated_ms": chunk_ms}
                    serial_tail += decoded
                    if re.search(r"(?m)^BUILD [^\n]*\bbenchmarks=0\r?$", serial_tail):
                        accuracy = True
                        result["mode"] = "accuracy"
                        control_frame = cpu_control_frame(machine, True, signature)
                    # A tail means the head's sample just ended: drop its leases.
                    leases = capture.feed(chunk, chunk_ms, time.monotonic())
                    if (not index and step_row is not None and capture.windows
                            and capture.windows[-1][0] == before[0]):
                        series.setdefault(capture.windows[-1][2], []).append(step_row)
                    progress_buffer += decoded
                    *lines, progress_buffer = progress_buffer.split("\n")
                    progress_buffer = progress_buffer[-65536:]
                    for line in lines:
                        if line.startswith(PROGRESS):
                            last_progress_ms, hang_open = chunk_ms, False
                            if found := re.search(r"\bname=(\S+)", line):
                                last_name = found[1]
                joined = data + tail
                partial = (partial + joined).rsplit(b"\n", 1)[-1] if joined else partial
                partial = partial[-4096:]
                if not accuracy:
                    for lease in leases:
                        api.request(f"serial write 1 {lease.encode('ascii').hex()}")
                if recorder is not None:
                    recorder.check()
                clean = serial_tail.replace("\r", "")
                if (not control_sent and CPU1_READY + "\n" in clean):
                    api.request(f"serial write 1 {control_frame.hex()}")
                    control_sent = True
                tests_started = tests_started or bool(TESTS_STARTED.search(clean))
                if selection["state"] == "waiting":
                    if SEL1_READY + "\n" in clean:
                        for index in range(0, len(selection_bytes), SERIAL_WRITE_CHUNK):
                            part = selection_bytes[index:index + SERIAL_WRITE_CHUNK]
                            api.request(f"serial write 1 {part.hex()}")
                        selection["state"] = "sent"
                        selection["frame"] = selection_bytes.decode("ascii").strip()
                    elif tests_started:
                        selection["state"] = "unsupported"
                        if selection_requested:
                            raise RuntimeError(
                                "--only/--skip/--resume-from: the image does not offer runtime "
                                f"selection (no '{SEL1_READY}' before the first test)")
                for marker in args.expect:
                    if marker in serial_tail:
                        seen.add(marker)
                rejected = [marker for marker in args.reject if marker in serial_tail]
                longest = max(map(len, args.expect + args.reject), default=1)
                serial_tail = serial_tail[-max(65536, longest):]
                idle = not data and not tail
                if (last_text_ms is None or
                        (now - last_text_ms >= text_poll_ms and
                         (idle or now - last_text_ms >= 5 * text_poll_ms))):
                    poll_screen(now)
                    for marker in args.expect:
                        if marker in text:
                            seen.add(marker)
                    rejected.extend(marker for marker in args.reject if marker in text)
                result["matched"] = sorted(seen)
                if rejected:
                    result["rejected"] = sorted(set(rejected))
                    break
                if args.bios_f1 and bios_f1_prompt(text) and now - last_f1 >= 5000:
                    api.request("key down 0x3b")
                    api.request("key up 0x3b")
                    last_f1 = now
                    last_text_ms = None
                if not accuracy and args.expect and all(marker in seen for marker in args.expect):
                    break
                if any(line == "DONE" for line in clean.splitlines()):
                    break
                if hang_ms and not hang_open and now - last_progress_ms >= hang_ms:
                    hang_open = True
                    name = capture.active["name"] if capture.active else last_name
                    registers = api.request("regs", allow_failure=True)
                    result.setdefault("hangs", []).append({
                        "name": name, "emulated_ms": now, "last_progress_ms": last_progress_ms,
                        "silent_guest_seconds": (now - last_progress_ms) / 1000,
                        "eip": registers.get("eip"), "eflags": registers.get("eflags"),
                        "interrupts_enabled": (bool(registers["eflags"] & 0x200)
                                               if isinstance(registers.get("eflags"), int)
                                               else None)})
                    print(f"HANG {name or 'boot'}: no WORK/TEST/BEGIN line for "
                          f"{(now - last_progress_ms) / 1000:.0f} guest seconds",
                          file=sys.stderr)
                    if on_hang == "stop":
                        break
            if last_text_ms != result["emulated_ms"]:
                poll_screen(result["emulated_ms"])
            result["host_seconds"] = time.monotonic() - started
            serial_text = (run_dir / "serial.log").read_text(encoding="cp437")
            result.update(parse_log(serial_text))
            result["fpuprobe"] = fpuprobe(serial_text)
            result["done"] = any(line == "DONE" for line in serial_text.splitlines())
            result["counts_consistent"] = complete_counts(result)
            result["correctness"] = {
                "complete": result["done"] and result["counts_consistent"],
                "passed": (result["done"] and result["counts_consistent"] and
                           not result["fatal"] and
                           not any(t.get("status") in ("FAIL", "FAULT") for t in result["tests"])),
                "note": "Observed guest failures are retained; status observed is not correctness PASS.",
            }
            if note := host_fpu_note(result["tests"], result["fpu_backend"]):
                result["correctness"]["fpu_note"] = note
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
            if selection["state"] == "sent":
                selection["accepted"] = selection_accepted(result, only, skip, resume_from)
                if selection_requested and not selection["accepted"] and result["done"]:
                    raise RuntimeError("guest did not accept the runtime selection frame "
                                       f"{selection['frame']!r}")
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
                                and (result["done"] or not accuracy)
                                else "observed" if result["done"] else "timeout")
            if result.get("hangs") and not result["done"] and not result.get("rejected"):
                result["status"] = "hang"
                result["correctness"]["note"] = ("The guest stopped reporting progress (HANG); "
                                                 "see hangs.")
            if not serial_text:
                result["capture_note"] = (
                    "No COM1 output; no reliable workload attribution/completion. "
                    "Some profiles (including AL440LX) may require BIOS/COM1 setup; see screen.log.")
            result["process_cpu_seconds_total"] = process_cpu_seconds(emulator.pid)
            if recorder is not None:
                recorder.stop()
                perf_stopped = True
                result["perf"] = recorder.report(capture.windows)
            if not shutdown_emulator(api, emulator, QUIT_SECONDS):
                result["quit_note"] = (f"86Box did not exit within {QUIT_SECONDS}s "
                                       "of quit; it was terminated")
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
    except KeyboardInterrupt:
        result["status"] = "interrupted"
        result["error"] = "interrupted by a signal; 86Box was stopped"
        raise
    finally:
        # A second Ctrl+C/SIGTERM must not cut the cleanup short; it is
        # delivered once 86Box has been stopped and result.json is written.
        with signals_blocked():
            finish_run(result, run_dir, started, capture, recorder, perf_stopped, emulator,
                       connection, api, counters, transcript, series, pin, selection)
    return result


def finish_run(result, run_dir, started, capture, recorder, perf_stopped, emulator,
               connection, api, counters, transcript, series, pin, selection):
    if recorder is not None and not perf_stopped and recorder.process is not None:
        try:
            recorder.stop()
        except (OSError, RuntimeError) as error:
            result["perf"] = {"status": "error", "error": str(error)}
            result["error"] = result.get("error", str(error))
            result["status"] = "error"
    if recorder is not None and "error" in result and result["perf"]["status"] == "requested":
        result["perf"] = {"status": "error", "error": result["error"]}
    if counters is not None:
        counters.close()
    capture.finish(result["emulated_ms"], time.monotonic())
    result["workload_capture"] = capture.report()
    for workload in result["workload_capture"]["workloads"]:
        workload["step_series"] = series.get(workload["id"], [])
    result["workload_capture"]["step_series_fields"] = (
        ["wall_seconds", "cpu_seconds", "emulated_ms"] +
        (["cycles", "instructions"] if counters is not None else []))
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
        except (OSError, RuntimeError) as error:
            result["cpu_accounting_note"] = str(error)
    if ("process_cpu_seconds_total" in result
            and "process" in result["workload_capture"]["cpu_scope"]):
        result["non_step_process_cpu_seconds"] = max(
            0, result["process_cpu_seconds_total"] - result["step_process_cpu_seconds"])
    result["selection"] = selection
    result["host_state"]["end"] = host_state(pin)
    if connection is not None:
        # Error or interrupt paths have not asked 86Box to quit yet.
        try:
            shutdown_emulator(api, emulator, 10)
        finally:
            connection.close()
    stop_process(emulator)
    LIVE_EMULATORS.discard(emulator)
    result["transcript"] = transcript.close()
    if isinstance(getattr(api, "timing", None), dict):
        result["request_timing"] = {verb: {"count": count, "wall_seconds": round(seconds, 6)}
                                    for verb, (count, seconds) in sorted(api.timing.items())}
    (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")


def exit_status(result):
    """0 for a matched --expect run, or a complete run without guest FAIL/FAULT."""
    if result["status"] == "pass":
        return 0
    if result["status"] == "observed":
        return 0 if result.get("correctness", {}).get("passed") else 1
    return 1


ARTIFACTS = ("serial.log", "result.json", "launch.json", "automation.sock",
             "automation.jsonl", "automation.jsonl.gz", "automation.1.jsonl.gz",
             "screen.log", "screen.png", "emulator.log", "process.log", "perf.data",
             "perf-record.log", "perf-script.log", "summary.json")


def repeat_directories(root, count):
    return [root] if count == 1 else [root / f"repeat-{index:02d}" for index in range(1, count + 1)]


def write_summary(root, results, aa=False):
    """summary.json beside the run(s): see doc/results.md."""
    noise = None
    if aa:
        try:
            noise = aa_noise_floor(results)
        except ValueError as error:
            noise = {"noise_floor_percent": None, "error": str(error)}
    summary = build_summary(results, noise_floor=noise)
    path = root / "summary.json"
    path.write_text(json.dumps(summary, indent=2) + "\n")
    return path


def report_runs(root, results, aa=False):
    summary = write_summary(root, results, aa)
    statuses = {result["status"] for result in results}
    line = {"status": results[0]["status"] if len(statuses) == 1 else "mixed",
            "directory": str(root), "emulated_ms": results[-1]["emulated_ms"],
            "matched": results[-1]["matched"], "summary": str(summary)}
    if len(results) > 1:
        line["runs"] = [{key: result[key] for key in ("status", "directory")}
                        for result in results]
    print(json.dumps(line))
    for result in results:
        if "error" in result:
            print(result["error"], file=sys.stderr)
        if note := result.get("correctness", {}).get("fpu_note"):
            print(note, file=sys.stderr)
    return max(exit_status(result) for result in results)


def run_existing_vm(args, emulator, image, roms):
    source = vm_config(args.vm)
    root = (args.output_dir.expanduser().absolute() if args.output_dir else source.parent)
    count = getattr(args, "repeat", 1)
    directories = repeat_directories(root, count)
    for run_dir in directories:
        for name in ARTIFACTS:
            if (run_dir / name).exists():
                raise RuntimeError(f"output already exists: {run_dir / name}; "
                                   "use --output-dir for another run")
        if len(os.fsencode(run_dir / "automation.sock")) >= 108:
            raise RuntimeError("output path is too long for a UNIX automation socket; use --output-dir")
    if (root / "summary.json").exists():
        raise RuntimeError(f"output already exists: {root / 'summary.json'}; "
                           "use --output-dir for another run")
    if args.prepare_only:
        raise RuntimeError("--prepare-only is for generated fixtures, not in-place --vm runs")
    print("Running VM in place: disks, NVRAM and host-connected devices remain active. "
          "The VM must not already be running. Configuration is restored after exit.",
          file=sys.stderr)
    results = []
    with inplace_vm(args, image) as (source, machine, source_hash):
        image_hash = hashlib.sha256(image.read_bytes()).hexdigest()
        emulator_hash = hashlib.sha256(emulator.read_bytes()).hexdigest()
        for run_dir in directories:
            run_dir.mkdir(parents=True, exist_ok=True)
            command = [str(emulator), "--automation", f"unix:{run_dir / 'automation.sock'}",
                       "--automation-hold", "--automation-seed", "1", "--noconfirm",
                       "--vmpath", str(source.parent),
                       "--rompath", str(roms), "--logfile", str(run_dir / "emulator.log"),
                       str(source)]
            (run_dir / "launch.json").write_text(json.dumps({
                "command": command, "source_vm": str(source), "machine": machine,
                "source_config_sha256": source_hash, "image": str(image),
                "image_sha256": image_hash, "in_place": True,
            }, indent=2) + "\n")
            result = execute(args, run_dir, command, machine)
            result["image_sha256"] = image_hash
            result["emulator_sha256"] = emulator_hash
            (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append(result)
    return report_runs(root, results, getattr(args, "aa", False))


def prepare_fixture(args, run_dir, image, emulator, roms):
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
                            "sampling": sampling_options(args),
                            "accuracy": getattr(args, "accuracy", False),
                            "perf": getattr(args, "perf", False),
                            "boundaries": getattr(args, "boundaries", "wait-serial"),
                            "pin": getattr(args, "pin", None),
                            "headless": getattr(args, "headless", False),
                            "selection": {"only": getattr(args, "only", []),
                                          "skip": getattr(args, "skip", []),
                                          "resume_from": getattr(args, "resume_from", None)}},
                "host": host_metadata(getattr(args, "pin", None))}
    (run_dir / "launch.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return command, machine, manifest


def main():
    args = arguments()
    try:
        emulator = emulator_binary(args.emulator)
        image = args.image.resolve(strict=True)
        if not os.access(emulator, os.X_OK):
            raise RuntimeError(f"not executable: {emulator}")
        if image.stat().st_size != 1474560:
            raise RuntimeError("expected a raw 1.44 MB floppy image (1474560 bytes)")
        roms = rom_directory(args.rompath)
        if args.vm:
            return run_existing_vm(args, emulator, image, roms)
        (ROOT / "build").mkdir(exist_ok=True)
        root = ROOT / "build" / (
            f"86box-{args.profile or 'vm'}-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:6]}")
        directories = repeat_directories(root, getattr(args, "repeat", 1))
        if len(directories) > 1:
            root.mkdir(mode=0o700)
        prepared = [prepare_fixture(args, run_dir, image, emulator, roms)
                    for run_dir in directories]
        if args.prepare_only:
            print(json.dumps({"status": "prepared", "directory": str(root)}))
            return 0
        emulator_hash = hashlib.sha256(emulator.read_bytes()).hexdigest()
        results = []
        for run_dir, (command, machine, manifest) in zip(directories, prepared):
            result = execute(args, run_dir, command, machine)
            result["image_sha256"] = manifest["image_sha256"]
            result["emulator_sha256"] = emulator_hash
            (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append(result)
        return report_runs(root, results, getattr(args, "aa", False))
    except (OSError, RuntimeError, ValueError, configparser.Error) as error:
        print(f"86Box smoke: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("86Box smoke: interrupted; 86Box stopped and any VM configuration restored",
              file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(run_main(main))
