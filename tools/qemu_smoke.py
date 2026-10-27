#!/usr/bin/env python3
"""Boot the actual image, require DONE, and preserve serial logs and JSON data."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import time


def parse_log(text):
    tests = []
    details = []
    for line in text.splitlines():
        if line.startswith("BEGIN "):
            details = []
        elif line.startswith("FAILDETAIL "):
            details.append(line)
        if line.startswith("TEST "):
            fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
            if details:
                fields["failure_details"] = details
            tests.append(fields)
            details = []
    summary = next((x for x in text.splitlines() if x.startswith("SUMMARY ")), "")
    counts = dict(re.findall(r"(\w+)=(\d+)", summary))
    config_line = next((x for x in text.splitlines() if x.startswith("CFG ")), "")
    return {"tests": tests, "summary": summary, "counts": counts,
            "build_info": [dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
                           for line in text.splitlines() if line.startswith("BUILD ")],
            "runtime_records": [dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
                                for line in text.splitlines()
                                if line.startswith("RUNTIME ")],
            "config": dict(re.findall(r"(\w+)=(\S+)", config_line)),
            "fatal": any(x.startswith("FATAL") for x in text.splitlines()),
            "cpu_info": [x for x in text.splitlines()
                         if x.startswith("CPU ") and "mhz=" not in x]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", nargs="+", default=["486", "pentium", "pentium2", "qemu32"])
    parser.add_argument("--machine", default="pc")
    parser.add_argument("--accel", choices=["tcg", "kvm"], default="tcg")
    parser.add_argument("--memory", type=int, default=32)
    parser.add_argument("--timeout", type=int, default=28800,
                        help="host deadline in seconds (default: 8 hours for sustained workloads)")
    parser.add_argument("--image", default="floppy.img")
    parser.add_argument("--output", default="build/smoke")
    parser.add_argument("--label", default="", help="emulator build/mode identifier")
    args = parser.parse_args()
    if args.memory < 1 or args.timeout < 1:
        parser.error("memory and timeout must be positive")
    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    results = []
    image_sha256 = hashlib.sha256(pathlib.Path(args.image).read_bytes()).hexdigest()
    emulator_version = subprocess.check_output(
        ["qemu-system-i386", "--version"], text=True).splitlines()[0]
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
            try:
                while proc.poll() is None and time.monotonic() - start < args.timeout:
                    text = log.read_text(errors="replace")
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
        print(f"{cpu}: {'OK' if result['ok'] else 'FAILED'} "
              f"tests={len(result['tests'])} bad={len(bad)} "
              f"host_seconds={result['host_seconds']}", flush=True)
        for test in bad:
            print(" ", test)
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    raise SystemExit(0 if all(r["ok"] for r in results) else 1)


if __name__ == "__main__":
    main()
