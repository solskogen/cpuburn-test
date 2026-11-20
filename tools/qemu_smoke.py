#!/usr/bin/env python3
"""Boot the actual image, require DONE, and preserve serial logs and JSON data."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import time


REQ_FPU = 1 << 2  # kernel.h


def x87_failures(tests):
    """Names of failed tests that need an x87 FPU (REQ_FPU in their req mask)."""
    return [t["name"] for t in tests if t.get("status") == "FAIL"
            and int(t.get("req", "0"), 16) & REQ_FPU]


def fpu_backend(machine):
    """FPU backend the configuration selects. Boards flagged SoftFloat-only in
    86Box use SoftFloat even when the configuration does not ask for it."""
    if machine.get("fpu_type") == "none":
        return "none"
    return "softfloat" if machine.get("fpu_softfloat", "0") == "1" else "host"


def host_fpu_note(tests, backend):
    """Explain x87 failures from 86Box's host FPU backend, which is not bit-exact."""
    if backend != "host":
        return None
    failed = x87_failures(tests)
    if not failed:
        return None
    return (f"{len(failed)} failing x87 tests ran on 86Box's host FPU backend, which "
            "does not round and flag like a real x87. Rerun with --fpu-backend softfloat "
            "to check x87 conformance.")


# key=value fields; a quoted value may contain spaces and \" or \\ escapes.
FIELD = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\S+)')
CFG_TIMING_FIELDS = ("timing_scope", "ms_resolution")


def fields_of(line):
    return dict(FIELD.findall(line))


def unquote(value):
    """Value of a quoted field without its quotes and escapes."""
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return re.sub(r'\\(.)', r'\1', value[1:-1])
    return value


def parse_log(text):
    """Parse a guest serial log. Accepts both the current format (runtime
    totals in TEST/SUMMARY, timing constants in CFG, one FILTER summary) and
    the older one (BEGIN, RUNTIME and per-record FILTER lines)."""
    lines = text.splitlines()
    tests = []
    details = []
    for line in lines:
        if line.startswith("BEGIN ") or line.startswith("WORK phase=warmup "):
            details = []
        elif line.startswith("FAILDETAIL "):
            details.append(line)
        if line.startswith("TEST "):
            fields = fields_of(line)
            if details:
                fields["failure_details"] = details
            tests.append(fields)
            details = []
    summary = next((x for x in lines if x.startswith("SUMMARY ")), "")
    counts = dict(re.findall(r"(\w+)=(\d+)", summary))
    config = {}
    for line in lines:
        if line.startswith("CFG "):  # the first line, then only=/quick= lists
            config.update(re.findall(r"(\w+)=(\S+)", line))
    # Constant per-record fields moved from TEST to CFG: put them back on the
    # records (as the old format had them) and keep config comparable.
    timing = {k: config.pop(k) for k in CFG_TIMING_FIELDS if k in config}
    for test in tests:
        if "op_unit" in test:  # an executed record, not a missing-feature SKIP
            for key, value in timing.items():
                test.setdefault(key, value)
        if "nfail" in test and "seed" in config:
            test.setdefault("seed", config["seed"])
    runtime = [fields_of(line) for line in lines if line.startswith("RUNTIME ")]
    if not runtime and summary and tests and all("total_ms" in t for t in tests):
        runtime = runtime_from_records(tests, summary, timing.get("ms_resolution", "10"))
    filters = [dict(re.findall(r"(\w+)=(\S+)", line))
               for line in lines if line.startswith("FILTER ")]
    control = [dict(re.findall(r"(\w+)=(\S+)", line))
               for line in lines if line.startswith("CONTROL ")]
    selection = next((c for c in control if c.get("protocol") == "sel1"
                      and c.get("accepted") == "1"), None)
    return {"tests": tests, "summary": summary, "counts": counts,
            "build_info": [fields_of(line) for line in lines if line.startswith("BUILD ")],
            "runtime_records": runtime,
            "config": config,
            "cpu_control": control,
            "selection": selection,
            "conformance": [dict(re.findall(r"(\w+)=(\S+)", line))
                            for line in lines if line.startswith("CONFORMANCE ")],
            "features": [dict(re.findall(r"(\w+)=(\S+)", line))
                         for line in lines if line.startswith("FEATURE ")],
            "probes": [dict(re.findall(r"(\w+)=(\S+)", line))
                       for line in lines if line.startswith("PROBE ")],
            "filtered_tests": [f for f in filters if "name" in f],
            "filter_summary": next((f for f in filters if "name" not in f), None),
            "fatal": any(x.startswith("FATAL") for x in lines),
            "cpu_info": [x for x in lines
                         if x.startswith("CPU ") and "mhz=" not in x]}


def runtime_from_records(tests, summary, resolution):
    """Old-style RUNTIME records rebuilt from TEST total_ms/overhead_ms and
    the SUMMARY runtime totals."""
    records = [{"scope": "test", "r": t.get("r"), "id": t.get("id"), "name": t.get("name"),
                "total_ms": t["total_ms"], "warmup_ms": t.get("warmup_ms", "0"),
                "sample_ms": t.get("ms", "0"), "overhead_ms": t.get("overhead_ms", "0"),
                "resolution_ms": resolution} for t in tests]
    totals = dict(re.findall(r"(\w+)=(\d+)", summary))
    keys = ("warmup_ms", "sample_ms", "overhead_ms", "suite_overhead_ms")
    if "ms" in totals and all(k in totals for k in keys):
        records.append({"scope": "suite", "total_ms": totals["ms"],
                        **{k: totals[k] for k in keys}, "resolution_ms": resolution})
    return records


SUSTAINED_TIMEOUT = 8 * 3600
ACCURACY_TIMEOUT = 30 * 60
ACCURACY_BUILD = re.compile(r"(?m)^BUILD [^\n]*\bbenchmarks=0\r?$")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", nargs="+", default=["486", "pentium", "pentium2", "qemu32"])
    parser.add_argument("--machine", default="pc")
    parser.add_argument("--accel", choices=["tcg", "kvm"], default="tcg")
    parser.add_argument("--memory", type=int, default=32)
    parser.add_argument("--timeout", type=int, default=None,
                        help="host deadline per CPU in seconds (default: 30 minutes once the "
                             "guest reports an accuracy build, otherwise 8 hours for "
                             "sustained benchmark workloads)")
    parser.add_argument("--image", default="floppy.img")
    parser.add_argument("--output", default="build/smoke")
    parser.add_argument("--label", default="", help="emulator build/mode identifier")
    args = parser.parse_args()
    if args.memory < 1 or (args.timeout is not None and args.timeout < 1):
        parser.error("memory and timeout must be positive")
    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    results = []
    image_sha256 = hashlib.sha256(pathlib.Path(args.image).read_bytes()).hexdigest()
    emulator_version = subprocess.check_output(
        ["qemu-system-i386", "--version"], text=True).splitlines()[0]
    results_path = out / "results.json"

    def save():
        # Rewritten after every CPU, so an interrupt or hang keeps earlier results.
        staging = results_path.with_suffix(".json.tmp")
        staging.write_text(json.dumps(results, indent=2) + "\n")
        staging.replace(results_path)

    for cpu in args.cpus:
        stem = re.sub(r"[^a-zA-Z0-9_-]", "_", cpu)
        log = out / f"{stem}.serial.log"
        errors = out / f"{stem}.qemu.log"
        log.write_text("")
        cmd = [
            "qemu-system-i386", "-machine", args.machine, "-accel", args.accel,
            "-cpu", cpu, "-m", str(args.memory), "-drive",
            f"file={pathlib.Path(args.image).resolve()},format=raw,if=floppy,readonly=on",
            "-boot", "a", "-display", "none", "-monitor", "none",
            "-serial", f"file:{log}", "-no-reboot", "-no-shutdown",
        ]
        start = time.monotonic()
        elapsed = None
        done = False
        with errors.open("w") as err:
            proc = subprocess.Popen(cmd, stdout=err, stderr=err)
            timeout = args.timeout or SUSTAINED_TIMEOUT
            try:
                while proc.poll() is None and time.monotonic() - start < timeout:
                    text = log.read_text(errors="replace")
                    if args.timeout is None and ACCURACY_BUILD.search(text):
                        timeout = ACCURACY_TIMEOUT
                    if "\nDONE\n" in text or text.startswith("DONE\n"):
                        done = True
                        elapsed = time.monotonic() - start
                        break
                    time.sleep(0.1)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
        text = log.read_text(errors="replace")
        if elapsed is None:
            elapsed = time.monotonic() - start
        result = parse_log(text)
        bad = [t for t in result["tests"] if t.get("status") not in ("PASS", "SKIP")]
        counts = result["counts"]
        consistent = counts.get("tests") == str(len(result["tests"]))
        for key, status in (("pass", "PASS"), ("skip", "SKIP"),
                            ("fail", "FAIL"), ("fault", "FAULT")):
            consistent &= counts.get(key) == str(sum(
                t.get("status") == status for t in result["tests"]))
        result.update(cpu=cpu, machine=args.machine, accel=args.accel,
                      memory_mib=args.memory, image_sha256=image_sha256,
                      label=args.label, emulator_version=emulator_version,
                      host_seconds=round(elapsed, 3), done=done,
                      ok=done and bool(result["tests"]) and bool(result["summary"])
                      and consistent and not bad and "FATAL" not in text)
        results.append(result)
        save()
        print(f"{cpu}: {'OK' if result['ok'] else 'FAILED'} "
              f"tests={len(result['tests'])} bad={len(bad)} "
              f"host_seconds={result['host_seconds']}", flush=True)
        for test in bad:
            print(" ", test)
    save()
    raise SystemExit(0 if all(r["ok"] for r in results) else 1)


if __name__ == "__main__":
    main()
