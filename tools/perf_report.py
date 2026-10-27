#!/usr/bin/env python3
"""Summarize repeated guest workloads and compare matching performance baselines."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics

from qemu_smoke import parse_log


def distribution(values):
    if not values:
        return None
    values = sorted(values)
    return {"n": len(values), "min": values[0], "median": statistics.median(values),
            "p95": values[max(0, math.ceil(len(values) * .95) - 1)],
            "max": values[-1]}


def load_runs(path):
    if path.suffix == ".json":
        value = json.loads(path.read_text())
        runs = value if isinstance(value, list) else [value]
        if not runs or any(not isinstance(r, dict) or "tests" not in r for r in runs):
            raise ValueError(f"{path}: expected runner results with tests")
        return runs
    run = parse_log(path.read_text(encoding="cp437"))
    run["done"] = any(line == "DONE" for line in
                      path.read_text(encoding="cp437").splitlines())
    return [run]


def environment(run):
    result = {key: run.get(key) for key in
              ("cpu", "machine", "accel", "memory_mib", "cpu_info", "step_ms")}
    if "host" in run:
        result["host"] = run["host"]
    if "workload_capture" in run:
        result["sample_cpu_seconds_target"] = run["workload_capture"].get(
            "sample_cpu_seconds_target")
    if "perf" in run:
        result["perf_requested"] = run["perf"].get("status") != "disabled"
    return result


def validate_run(run):
    tests = run["tests"]
    counts = run.get("counts", {})
    if not run.get("done") or not tests or run.get("fatal"):
        raise ValueError("run lacks complete serial results/DONE (VGA snapshots are not complete logs)")
    if int(counts.get("tests", -1)) != len(tests):
        raise ValueError("SUMMARY test count does not match records")
    for name, status in (("pass", "PASS"), ("fail", "FAIL"),
                         ("skip", "SKIP"), ("fault", "FAULT")):
        if int(counts.get(name, -1)) != sum(t.get("status") == status for t in tests):
            raise ValueError(f"SUMMARY {name} count does not match records")
    keys = [(t.get("r"), t.get("name")) for t in tests]
    if any(t.get("status") not in ("PASS", "FAIL", "FAULT", "SKIP") for t in tests):
        raise ValueError("unknown test status")
    if len(set(keys)) != len(keys):
        raise ValueError("duplicate round/test records")
    if not run.get("config") or not run.get("cpu_info"):
        raise ValueError("missing CPU/CFG metadata; use a full serial log")


def runtime_breakdown(run):
    records = run.get("runtime_records", [])
    if not records:
        return None
    tests = [r for r in records if r.get("scope") == "test"]
    suites = [r for r in records if r.get("scope") == "suite"]
    if len(suites) != 1 or len(tests) != len(run["tests"]) or len(records) != len(tests) + 1:
        raise ValueError("incomplete runtime breakdown")
    keys = [(r["r"], r["name"]) for r in tests]
    if len(set(keys)) != len(keys) or set(keys) != {
            (t["r"], t["name"]) for t in run["tests"]}:
        raise ValueError("runtime workload inventory differs")
    fields = ("total_ms", "warmup_ms", "sample_ms", "overhead_ms", "resolution_ms")
    parsed = [{**{k: r[k] for k in ("r", "id", "name")},
               **{k: int(r[k]) for k in fields}} for r in tests]
    by_test = {(t["r"], t["name"]): t for t in run["tests"]}
    for row in parsed:
        test = by_test[row["r"], row["name"]]
        if "id" in test and row["id"] != test["id"]:
            raise ValueError("runtime workload id differs")
        if any(key in test and int(test[key]) != row[field]
               for key, field in (("ms", "sample_ms"), ("warmup_ms", "warmup_ms"))):
            raise ValueError("runtime invocation times differ from TEST")
    suite = {k: int(suites[0][k]) for k in (*fields, "suite_overhead_ms")}
    for row in [*parsed, suite]:
        if any(row[k] < 0 for k in fields) or row["resolution_ms"] != 10:
            raise ValueError("invalid runtime duration or resolution")
        extra = row.get("suite_overhead_ms", 0)
        if extra < 0 or row["total_ms"] != sum(
                row[k] for k in ("warmup_ms", "sample_ms", "overhead_ms")) + extra:
            raise ValueError("runtime durations do not add up")
    if any(suite[k] != sum(t[k] for t in parsed)
           for k in ("warmup_ms", "sample_ms", "overhead_ms")):
        raise ValueError("runtime suite totals differ from test records")
    if suite["total_ms"] != int(run["counts"]["ms"]):
        raise ValueError("runtime total differs from SUMMARY")
    return {"suite": suite,
            "tests": sorted(parsed, key=lambda r: r["total_ms"], reverse=True)}


def measurement_notes(test):
    notes = []
    if test["status"] != "PASS":
        return ["correctness_" + test["status"].lower()]
    if "timing_invalid" in test:
        notes.append(test["timing_invalid"])
    if int(test.get("ops", 0)) <= 0:
        notes.append("no_operation_count")
    if int(test.get("cyc", 0)) <= 0:
        notes.append("counter_unresolved")
    if int(test.get("ms", 0)) <= 0:
        notes.append("guest_ms_unresolved")
    if "iterations" in test:
        iterations = int(test["iterations"])
        ops = int(test.get("iteration_ops", 0))
        if iterations <= 0:
            notes.append("no_measured_invocations")
        elif ops <= 0 or int(test.get("ops", 0)) != iterations * ops:
            notes.append("nonconstant_or_inconsistent_operations")
    return notes


def summarize(runs, discard_rounds):
    if not runs:
        raise ValueError("no runs selected")
    for run in runs:
        validate_run(run)
    first = runs[0]
    for run in runs[1:]:
        if (environment(run) != environment(first) or
                run.get("config") != first.get("config") or
                run.get("image_sha256") != first.get("image_sha256") or
                run.get("build_info") != first.get("build_info") or
                run.get("label") != first.get("label") or
                run.get("emulator_version") != first.get("emulator_version") or
                run.get("emulator_sha256") != first.get("emulator_sha256")):
            raise ValueError("mixed environments/images/configs; create a separate report for each")
    groups, failures = {}, []
    for run_index, run in enumerate(runs):
        for test in run["tests"]:
            if test.get("status") in ("FAIL", "FAULT"):
                failures.append({"run": run_index, **test})
            if int(test["r"]) <= discard_rounds:
                continue
            entry = groups.setdefault(test["name"], {"statuses": {}, "units": set(),
                                                    "ops": set(), "iteration_ops": set(),
                                                    "cycles": [], "ms": []})
            status = test["status"]
            entry["statuses"][status] = entry["statuses"].get(status, 0) + 1
            if status != "PASS" or "timing_invalid" in test:
                continue
            ops, cycles = int(test.get("ops", 0)), int(test.get("cyc", 0))
            if ops <= 0:
                continue
            entry["units"].add(test["unit"])
            entry["ops"].add(ops)
            if "iterations" in test:
                iterations = int(test["iterations"])
                per_iteration = int(test.get("iteration_ops", 0))
                if iterations > 0 and per_iteration > 0 and ops == iterations * per_iteration:
                    entry["iteration_ops"].add(per_iteration)
                else:
                    entry["iteration_ops"].add(0)
            if cycles > 0:
                entry["cycles"].append(cycles / ops)
            if int(test.get("ms", 0)) > 0:
                entry["ms"].append(int(test["ms"]) / ops)
    if not groups:
        raise ValueError("no samples remain after discarding rounds")
    workloads = {}
    for name, entry in sorted(groups.items()):
        if len(entry["units"]) > 1:
            raise ValueError(f"{name}: mixed timer units")
        workloads[name] = {
            "statuses": entry["statuses"], "unit": next(iter(entry["units"]), None),
            "ops": sorted(entry["ops"]), "iteration_ops": sorted(entry["iteration_ops"]),
            "counter_per_op": distribution(entry["cycles"]),
            "ms_per_op": distribution(entry["ms"])}
    return {"format": 1, "environment": environment(first),
            "config": first["config"], "image_sha256": first.get("image_sha256"),
            "build_info": first.get("build_info", []),
            "label": first.get("label", ""), "discard_rounds": discard_rounds,
            "emulator_version": first.get("emulator_version"),
            "emulator_sha256": first.get("emulator_sha256"),
            "runs": len(runs), "failures": failures, "workloads": workloads,
            "measurement_notes": [
                {"run": index, "r": test["r"], "name": test["name"],
                 "notes": notes}
                for index, run in enumerate(runs) for test in run["tests"]
                if (notes := measurement_notes(test))],
            "guest_runtime": [runtime_breakdown(run) for run in runs],
            "host_boot_to_done_seconds": distribution(
                [float(r["host_seconds"]) for r in runs if r.get("host_seconds") is not None])}


def compare(current, baseline, args):
    for key in ("format", "config", "discard_rounds"):
        if current.get(key) != baseline.get(key):
            raise ValueError(f"baseline {key} differs")
    if current["environment"] != baseline["environment"] and not args.allow_environment_change:
        raise ValueError("baseline environment differs; --allow-environment-change is explicit opt-in")
    images = (current.get("image_sha256"), baseline.get("image_sha256"))
    if (None in images or images[0] != images[1]) and not args.allow_image_change:
        raise ValueError("baseline image differs or is unverified; --allow-image-change is explicit opt-in")
    if set(current["workloads"]) != set(baseline["workloads"]):
        raise ValueError("baseline workload inventory differs")
    changes = []
    for name, now in current["workloads"].items():
        old = baseline["workloads"][name]
        change = {"name": name, "status": "not_comparable"}
        healthy = all(set(x["statuses"]) == {"PASS"} for x in (now, old))
        if current["config"].get("protocol") == "work1":
            signature = now.get("iteration_ops", [])
            matching_work = (len(signature) == 1 and signature[0] > 0 and
                             signature == old.get("iteration_ops"))
        else:
            matching_work = now["ops"] == old["ops"] and len(now["ops"]) == 1
        if healthy and now["unit"] == old["unit"] and matching_work:
            a, b = now["counter_per_op"], old["counter_per_op"]
            if a and b and b["median"] > 0:
                pct = (a["median"] / b["median"] - 1) * 100
                enough = min(a["n"], b["n"]) >= args.min_samples
                change.update(percent=pct, status=(
                    "insufficient_samples" if not enough else
                    "regression" if pct > args.max_regression_percent else "ok"))
        changes.append(change)
    return changes


def compare_host(current, baseline, args):
    a, b = current["host_boot_to_done_seconds"], baseline.get("host_boot_to_done_seconds")
    if not a or not b or b["median"] <= 0:
        return {"status": "not_comparable"}
    pct = (a["median"] / b["median"] - 1) * 100
    enough = min(a["n"], b["n"]) >= args.min_samples
    return {"percent": pct, "status": (
        "insufficient_samples" if not enough else
        "regression" if pct > args.max_regression_percent else "ok")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="runner JSON or complete serial logs")
    parser.add_argument("--cpu", help="select one CPU from a QEMU matrix JSON")
    parser.add_argument("--output", type=Path, default=Path("build/perf-report.json"))
    parser.add_argument("--baseline", type=Path, help="a previous perf-report.json")
    parser.add_argument("--discard-rounds", type=int, default=0,
                        help="exclude the first N rounds from timing, not correctness")
    parser.add_argument("--min-samples", type=int, default=3)
    parser.add_argument("--max-regression-percent", type=float, default=10)
    parser.add_argument("--allow-image-change", action="store_true")
    parser.add_argument("--allow-environment-change", action="store_true",
                        help="explicitly compare different CPUs/machines/interpreter modes")
    parser.add_argument("--gate-host-time", action="store_true",
                        help="also gate host boot-to-DONE median (needs repeated independent boots)")
    args = parser.parse_args()
    if (args.discard_rounds < 0 or args.min_samples < 1 or
            not math.isfinite(args.max_regression_percent) or args.max_regression_percent < 0):
        parser.error("invalid round/sample/regression limit")
    if args.gate_host_time and not args.baseline:
        parser.error("--gate-host-time requires --baseline")
    try:
        runs = [run for path in args.inputs for run in load_runs(path)
                if args.cpu is None or run.get("cpu") == args.cpu]
        report = summarize(runs, args.discard_rounds)
        report["sources"] = [{"path": str(p),
                              "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                             for p in args.inputs]
        if args.baseline:
            baseline = json.loads(args.baseline.read_text())
            report["comparison"] = compare(report, baseline, args)
            report["host_comparison"] = compare_host(report, baseline, args)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(f"report={args.output} workloads={len(report['workloads'])} "
          f"failures={len(report['failures'])}")
    print("name samples min median p95 counter/op unit comparison")
    comparisons = {c["name"]: c for c in report.get("comparison", [])}
    for name, entry in report["workloads"].items():
        stats = entry["counter_per_op"]
        if stats:
            change = comparisons.get(name, {})
            label = change.get("status", "-")
            if "percent" in change:
                label += f"({change['percent']:+.1f}%)"
            print(f"{name} {stats['n']} {stats['min']:.3f} {stats['median']:.3f} "
                  f"{stats['p95']:.3f} {entry['unit']} {label}")
    print("Guest counters are not host throughput. Host boot-to-DONE includes boot/I/O/"
          "automation overhead; ms has 10ms resolution. First round is not guaranteed cold.")
    for note in report["measurement_notes"]:
        print(f"Measurement run={note['run'] + 1} r={note['r']} {note['name']}: "
              + ",".join(note["notes"]))
    for index, runtime in enumerate(report["guest_runtime"]):
        if runtime is None:
            print(f"Run {index + 1}: guest runtime breakdown unavailable (older log).")
            continue
        suite = runtime["suite"]
        print(f"Run {index + 1} guest runtime (ms): "
              + " ".join(f"{key}={suite[key]}" for key in
                         ("total_ms", "warmup_ms", "sample_ms", "overhead_ms",
                          "suite_overhead_ms")))
        print("Slowest tests by guest time (not host cost):")
        for test in runtime["tests"][:10]:
            print(f"  r={test['r']} {test['name']} total={test['total_ms']} "
                  f"warmup={test['warmup_ms']} sample={test['sample_ms']} "
                  f"overhead={test['overhead_ms']}")
        print("Invocation times have 10ms quantization error per invocation; "
              "overhead is a residual, not precise framework profiling.")
    if "host_comparison" in report:
        host = report["host_comparison"]
        print(f"Host boot-to-DONE comparison: {host['status']}"
              + (f" ({host['percent']:+.1f}%)" if "percent" in host else ""))
    bad = bool(report["failures"]) or any(
        c["status"] == "regression" for c in report.get("comparison", []))
    if args.gate_host_time:
        bad |= report["host_comparison"]["status"] == "regression"
    if bad:
        return 1
    if args.gate_host_time and report["host_comparison"]["status"] not in ("ok", "regression"):
        print("Host baseline gate inconclusive: insufficient comparable boots.")
        return 2
    if args.baseline and (any(c["status"] == "insufficient_samples"
                             for c in report["comparison"]) or
                          not any(c["status"] == "ok" for c in report["comparison"])):
        print("Baseline gate inconclusive: insufficient comparable samples.")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
