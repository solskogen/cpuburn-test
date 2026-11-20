#!/usr/bin/env python3
"""Summarize repeated guest workloads and compare matching performance baselines."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import re
import statistics

from qemu_smoke import fpu_backend, host_fpu_note, parse_log


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
    run["fpuprobe"] = fpuprobe(path.read_text(encoding="cp437"))
    return [run]


def environment(run):
    result = {key: run.get(key) for key in
              ("cpu", "machine", "fpu_backend", "accel", "memory_mib", "cpu_info",
               "step_ms")}
    if run.get("conformance"):
        result["conformance"] = run["conformance"]
    if "host" in run:
        result["host"] = run["host"]
    if "workload_capture" in run:
        result["sampling"] = run["workload_capture"].get("sampling")
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
    if test.get("mode") == "accuracy":
        return ["accuracy_only", *(
            ["correctness_" + test["status"].lower()] if test["status"] != "PASS" else [])]
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


def correctness_samples(run):
    fields = ("r", "name", "status", "reason", "first", "got", "want",
              "exc_vec", "exc_err", "seed")
    return [{key: test[key] for key in fields if key in test} for test in run["tests"]]


def correctness_changes(current, baseline):
    if "correctness_samples" not in baseline:
        return {"available": False, "reason": "baseline_lacks_assertion_details", "changes": []}

    def observations(report):
        result = {}
        for sample in report["correctness_samples"]:
            for test in sample:
                key = (test.get("r"), test["name"])
                result.setdefault(key, []).append(test)
        return result

    now, old = observations(current), observations(baseline)
    changes = []
    for key in sorted(set(now) | set(old)):
        if now.get(key) != old.get(key):
            changes.append({"r": key[0], "name": key[1],
                            "before": old.get(key, []), "after": now.get(key, [])})
    return {"available": True, "changes": changes,
            "note": "Descriptive observations only; no accepted-failure or regression policy."}


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
                                                    "work_units": set(), "timing_scopes": set(),
                                                    "ops": set(), "iteration_ops": set(),
                                                    "cycles": [], "ms": []})
            status = test["status"]
            entry["statuses"][status] = entry["statuses"].get(status, 0) + 1
            if status != "PASS" or "timing_invalid" in test or test.get("mode") == "accuracy":
                continue
            ops, cycles = int(test.get("ops", 0)), int(test.get("cyc", 0))
            if ops <= 0:
                continue
            entry["units"].add(test["unit"])
            entry["work_units"].add(test.get("op_unit", "unspecified"))
            entry["timing_scopes"].add(test.get("timing_scope", "unspecified"))
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
        if len(entry["work_units"]) > 1 or len(entry["timing_scopes"]) > 1:
            raise ValueError(f"{name}: mixed operation units or timing scopes")
        workloads[name] = {
            "statuses": entry["statuses"], "unit": next(iter(entry["units"]), None),
            "op_unit": next(iter(entry["work_units"]), None),
            "timing_scope": next(iter(entry["timing_scopes"]), None),
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
            "feature_coverage": [run.get("features", []) for run in runs],
            "correctness_samples": [correctness_samples(run) for run in runs],
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
        if (healthy and now["unit"] == old["unit"] and matching_work and
                now.get("op_unit") == old.get("op_unit") and
                now.get("timing_scope") == old.get("timing_scope")):
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


CAPACITY_DEFINITION = ("guest seconds advanced per host CPU second of the 86Box emulation "
                       "thread x 100 (speed % per host CPU second), from interior sample "
                       "steps after the leading quarter (warm-in) of each sample")
STATUS_ORDER = ("HANG", "FAULT", "FAIL", "PASS", "SKIP")


def warm_in_steps(count):
    """Leading steps of a sample left out of every capacity figure."""
    return max(1, count // 4) if count >= 4 else 0


def _percentile(values, fraction):
    values = sorted(values)
    if not values:
        return None
    position = (len(values) - 1) * fraction
    low = math.floor(position)
    high = min(low + 1, len(values) - 1)
    return values[low] + (values[high] - values[low]) * (position - low)


def block_bootstrap(guest, cpu, replicates, rng):
    """Moving-block bootstrap replicates of sum(guest)/sum(cpu) x 100 / 1000.

    Blocks of ceil(sqrt(n)) consecutive steps keep the autocorrelation of
    neighbouring steps inside each resampled unit."""
    n = len(guest)
    length = max(1, math.ceil(math.sqrt(n)))
    blocks = math.ceil(n / length)
    starts = n - length + 1
    prefix_g, prefix_c = [0.0], [0.0]
    for g, c in zip(guest, cpu):
        prefix_g.append(prefix_g[-1] + g)
        prefix_c.append(prefix_c[-1] + c)
    result = []
    for _ in range(replicates):
        total_g = total_c = 0.0
        for _ in range(blocks):
            start = rng.randrange(starts)
            total_g += prefix_g[start + length] - prefix_g[start]
            total_c += prefix_c[start + length] - prefix_c[start]
        if total_c > 0:
            result.append(total_g / 10 / total_c)
    return result


def sample_series(workload):
    """Post-warm-in steps of one sampled workload: lists of wall, cpu, guest ms
    and, with --perf-stat, cycles and instructions."""
    rows = [row for row in workload.get("step_series") or [] if len(row) >= 3]
    rows = rows[warm_in_steps(len(rows)):]
    return rows


def capacity_samples(runs, replicates=1000, seed=1):
    """{name: [sample]} for every sampled workload of every run."""
    rng = random.Random(seed)
    tests = {}
    for index, run in enumerate(runs):
        capture = run.get("workload_capture") or {}
        for workload in capture.get("workloads", []):
            rows = sample_series(workload)
            sample = {"run": index, "r": workload.get("r"), "id": workload.get("id"),
                      "steps": len(rows)}
            if rows:
                guest = [row[2] for row in rows]
                cpu = [row[1] for row in rows]
                if sum(cpu) <= 0:
                    continue
                sample["capacity"] = sum(guest) / 10 / sum(cpu)
                sample["wall_capacity"] = (sum(guest) / 10 / sum(row[0] for row in rows)
                                           if sum(row[0] for row in rows) > 0 else None)
                sample["replicates"] = block_bootstrap(guest, cpu, replicates, rng) or [
                    sample["capacity"]]
                if all(len(row) >= 5 for row in rows) and sum(row[3] for row in rows) > 0:
                    sample["cycles"] = sum(row[3] for row in rows)
                    sample["instructions"] = sum(row[4] for row in rows)
            elif workload.get("cpu_full_speed_capacity_percent"):
                # Older results: one whole-sample figure, no within-sample CI.
                sample["capacity"] = workload["cpu_full_speed_capacity_percent"]
                sample["wall_capacity"] = workload.get("full_speed_capacity_percent")
                sample["replicates"] = [sample["capacity"]]
            else:
                continue
            tests.setdefault(workload["name"], []).append(sample)
    return tests


def test_replicates(samples, replicates, rng):
    """Two-level bootstrap of the median sample capacity: resample samples
    (rounds and repeated runs), then a block-bootstrap replicate of each."""
    result = []
    for _ in range(replicates):
        picks = [rng.choice(rng.choice(samples)["replicates"]) for _ in samples]
        result.append(statistics.median(picks))
    return result


def capacity_estimates(runs, replicates=1000, seed=1):
    """{name: (median, replicates, samples)}."""
    rng = random.Random(seed + 1)
    return {name: (statistics.median(s["capacity"] for s in samples),
                   test_replicates(samples, replicates, rng), samples)
            for name, samples in sorted(capacity_samples(runs, replicates, seed).items())}


def capacity_section(runs, replicates=1000, seed=1, estimates=None):
    estimates = estimates if estimates is not None else capacity_estimates(runs, replicates, seed)
    tests = {}
    for name, (median, reps, samples) in estimates.items():
        cycles = sum(s.get("cycles", 0) for s in samples)
        instructions = sum(s.get("instructions", 0) for s in samples)
        walls = [s["wall_capacity"] for s in samples if s.get("wall_capacity")]
        tests[name] = {
            "median_percent": median,
            "ci95_percent": [_percentile(reps, .025), _percentile(reps, .975)],
            "samples": len(samples), "steps": sum(s["steps"] for s in samples),
            "per_sample_percent": [round(s["capacity"], 3) for s in samples],
            "wall_median_percent": statistics.median(walls) if walls else None,
            "ipc": instructions / cycles if cycles else None}
    ranked = sorted(tests, key=lambda name: tests[name]["median_percent"])
    logs = [math.log(tests[n]["median_percent"]) for n in ranked if tests[n]["median_percent"] > 0]
    mean = statistics.fmean(logs) if logs else 0
    spread = statistics.pstdev(logs) if len(logs) > 1 else 0
    for rank, name in enumerate(ranked, 1):
        entry = tests[name]
        entry["rank"] = rank
        entry["z"] = ((math.log(entry["median_percent"]) - mean) / spread
                      if spread and entry["median_percent"] > 0 else 0.0)
    return {"definition": CAPACITY_DEFINITION, "bootstrap_replicates": replicates,
            "seed": seed, "ci": "two-level bootstrap: samples, then moving blocks of steps",
            "rank_order": "1 = lowest capacity (slowest) in this suite",
            "z": "standardised log capacity against this suite's tests",
            "tests": tests}


def verdict(low, high, floor):
    if low is None or high is None:
        return "not_comparable"
    if low > floor:
        return "faster"
    if high < -floor:
        return "slower"
    return "noise"


def compare_estimates(current, baseline, floor=0.0):
    """Faster/slower/noise per test: the bootstrap CI of current/baseline - 1
    (percent) must clear +/- floor percent."""
    tests = {}
    for name in sorted(set(current) | set(baseline)):
        if name not in current or name not in baseline:
            tests[name] = {"verdict": "missing_in_" + ("current" if name not in current else "baseline")}
            continue
        (a, ra, _), (b, rb, _) = current[name], baseline[name]
        ratios = [(x / y - 1) * 100 for x, y in zip(ra, rb) if y > 0]
        low, high = _percentile(ratios, .025), _percentile(ratios, .975)
        tests[name] = {"current_percent": a, "baseline_percent": b,
                       "delta_percent": (a / b - 1) * 100 if b > 0 else None,
                       "ci95_percent": [low, high], "verdict": verdict(low, high, floor)}
    counts = {}
    for entry in tests.values():
        counts[entry["verdict"]] = counts.get(entry["verdict"], 0) + 1
    return {"noise_floor_percent": floor, "counts": counts, "tests": tests,
            "rule": "faster if the 95% CI of the capacity change lies above +floor, "
                    "slower if below -floor, otherwise noise"}


def aa_noise_floor(runs, replicates=1000, seed=1):
    """A/A: alternate runs of one binary into two groups and compare them.
    The floor is the 95th percentile of |change| over tests."""
    if len(runs) < 2:
        raise ValueError("A/A noise floor needs at least two runs of the same binary")
    first = capacity_estimates(runs[0::2], replicates, seed)
    second = capacity_estimates(runs[1::2], replicates, seed + 7)
    comparison = compare_estimates(first, second, 0.0)
    deltas = [abs(t["delta_percent"]) for t in comparison["tests"].values()
              if t.get("delta_percent") is not None]
    comparison["noise_floor_percent"] = _percentile(deltas, .95) if deltas else None
    comparison["rule"] = ("A/A: even- versus odd-numbered runs of the same binary; "
                          "noise_floor_percent is the 95th percentile of |change|")
    return comparison


def fpuprobe(text):
    """FPUPROBE lines (fpu backend probe) as dicts."""
    return [dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line)) for line in text.splitlines()
            if line.startswith("FPUPROBE ")]


def _number(value):
    try:
        return int(value, 0)
    except (TypeError, ValueError):
        try:
            return float(value)
        except (TypeError, ValueError):
            return value


def build_summary(runs, replicates=1000, seed=1, compare=None, noise_floor=None,
                  estimates=None):
    """AI-facing per-test summary of one or more runner results (summary.json)."""
    section = capacity_section(runs, replicates, seed, estimates)
    names, tests = [], {}

    def entry(name):
        if name not in tests:
            names.append(name)
            tests[name] = {"name": name, "statuses": {}, "tags": None, "exc": None,
                           "irq": None, "harness_cyc": None, "fpu_backend": None}
        return tests[name]

    global_fpu = None
    for run in runs:
        probes = run.get("fpuprobe") or []
        for probe in probes:
            target = probe.get("name") or probe.get("test")
            backend = probe.get("backend")
            if target:
                entry(target)["fpu_backend"] = backend
            elif backend:
                global_fpu = backend
        for test in run.get("tests", []):
            item = entry(test["name"])
            status = test.get("status", "UNKNOWN")
            item["statuses"][status] = item["statuses"].get(status, 0) + 1
            if test.get("tags"):
                item["tags"] = test["tags"].strip('"').split(",")
            for key in ("exc", "irq", "harness_cyc"):
                if key in test:
                    item[key] = _number(test[key])
        for hang in run.get("hangs", []):
            item = entry(hang.get("name") or "unknown")
            item["statuses"]["HANG"] = item["statuses"].get("HANG", 0) + 1
            item["hang"] = hang
        perf = run.get("perf") or {}
        by_id = perf.get("workloads_by_id") or {}
        for workload in (run.get("workload_capture") or {}).get("workloads", []):
            data = by_id.get(str(workload.get("id")))
            if not data:
                continue
            item = entry(workload["name"])
            merged = item.setdefault("_perf", {"samples": 0, "jit": 0, "leaf": {}})
            merged["samples"] += data.get("samples", 0)
            merged["jit"] += data.get("jit_samples", 0)
            for symbol, count in data.get("leaf_hotpaths", []):
                merged["leaf"][symbol] = merged["leaf"].get(symbol, 0) + count
    for name in section["tests"]:
        entry(name)
    result = []
    for name in names:
        item = tests[name]
        statuses = item["statuses"]
        item["status"] = next((s for s in STATUS_ORDER if s in statuses),
                              next(iter(statuses), "NOT_RUN"))
        if item["fpu_backend"] is None:
            item["fpu_backend"] = global_fpu
        capacity = section["tests"].get(name)
        item["capacity"] = ({key: capacity[key] for key in
                             ("median_percent", "ci95_percent", "samples", "steps",
                              "wall_median_percent")} if capacity else None)
        item["rank"] = capacity["rank"] if capacity else None
        item["z"] = capacity["z"] if capacity else None
        item["ipc"] = capacity["ipc"] if capacity else None
        merged = item.pop("_perf", None)
        if merged:
            item["perf"] = {"samples": merged["samples"],
                            "jit_share": merged["jit"] / merged["samples"] if merged["samples"] else None,
                            "top_symbols": sorted(merged["leaf"].items(),
                                                  key=lambda kv: -kv[1])[:10]}
        result.append(item)
    result.sort(key=lambda t: (t["rank"] is None, t["rank"] or 0, t["name"]))
    first = runs[0] if runs else {}
    summary = {"format": "cpuburn-summary/1", "runs": len(runs),
               "directories": [r.get("directory") for r in runs],
               "statuses": [r.get("status") for r in runs],
               "image_sha256": first.get("image_sha256"),
               "emulator_sha256": first.get("emulator_sha256"),
               "machine": first.get("machine"), "host": first.get("host"),
               "cpu_scope": (first.get("workload_capture") or {}).get("cpu_scope"),
               "fpu_backend": global_fpu, "capacity_definition": CAPACITY_DEFINITION,
               "bootstrap_replicates": replicates, "tests": result}
    if compare is not None:
        summary["compare"] = compare
    if noise_floor is not None:
        summary["noise_floor"] = noise_floor
    return summary


def noise_floor_value(text):
    """--noise-floor: a percentage, or a report/summary JSON with an A/A floor."""
    try:
        return float(text)
    except ValueError:
        data = json.loads(Path(text).read_text())
        for key in ("capacity_noise_floor", "noise_floor"):
            if isinstance(data.get(key), dict) and data[key].get("noise_floor_percent") is not None:
                return float(data[key]["noise_floor_percent"])
        raise ValueError(f"{text}: no noise_floor_percent found") from None


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
    parser.add_argument("--compare-capacity", nargs="+", type=Path, metavar="BASELINE",
                        help="runner result JSON of the baseline binary; A/B capacity verdicts "
                             "(faster/slower/noise) from bootstrap CIs")
    parser.add_argument("--aa", action="store_true",
                        help="A/A noise floor: compare alternate input runs of one binary")
    parser.add_argument("--noise-floor", type=noise_floor_value, default=0.0,
                        help="percent change a CI must clear to count as faster/slower, or a "
                             "report/summary JSON holding an A/A noise floor (default 0)")
    parser.add_argument("--bootstrap", type=int, default=1000,
                        help="bootstrap replicates for capacity CIs (default 1000)")
    parser.add_argument("--seed", type=int, default=1, help="bootstrap seed (default 1)")
    parser.add_argument("--summary", type=Path,
                        help="also write the AI-facing summary.json (see doc/results.md)")
    parser.add_argument("--gate-capacity", action="store_true",
                        help="exit 1 when --compare-capacity finds a slower test")
    args = parser.parse_args()
    if (args.discard_rounds < 0 or args.min_samples < 1 or
            not math.isfinite(args.max_regression_percent) or args.max_regression_percent < 0):
        parser.error("invalid round/sample/regression limit")
    if args.gate_host_time and not args.baseline:
        parser.error("--gate-host-time requires --baseline")
    if args.gate_capacity and not args.compare_capacity:
        parser.error("--gate-capacity requires --compare-capacity")
    if args.bootstrap < 10:
        parser.error("--bootstrap needs at least 10 replicates")
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
            report["correctness_comparison"] = correctness_changes(report, baseline)
            report["host_comparison"] = compare_host(report, baseline, args)
        estimates = capacity_estimates(runs, args.bootstrap, args.seed)
        report["capacity"] = capacity_section(runs, args.bootstrap, args.seed, estimates)
        if args.aa:
            report["capacity_noise_floor"] = aa_noise_floor(runs, args.bootstrap, args.seed)
        if args.compare_capacity:
            baseline_runs = [run for path in args.compare_capacity for run in load_runs(path)
                             if args.cpu is None or run.get("cpu") == args.cpu]
            report["capacity_comparison"] = compare_estimates(
                estimates, capacity_estimates(baseline_runs, args.bootstrap, args.seed + 101),
                args.noise_floor)
        if args.summary:
            args.summary.parent.mkdir(parents=True, exist_ok=True)
            args.summary.write_text(json.dumps(build_summary(
                runs, args.bootstrap, args.seed, report.get("capacity_comparison"),
                report.get("capacity_noise_floor"), estimates), indent=2) + "\n")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(f"report={args.output} workloads={len(report['workloads'])} "
          f"failures={len(report['failures'])}")
    for contract in report["environment"].get("conformance", []):
        print("CPU conformance: " + " ".join(
            f"{key}={contract.get(key, 'unknown')}"
            for key in ("model", "source", "catalogue", "coverage", "mode")))
    for index, features in enumerate(report["feature_coverage"]):
        if features:
            print(f"Run {index + 1} sampled feature coverage: " + " ".join(
                f"{feature['name']}={feature['status']}" for feature in features
                if feature["status"] != "NA"))
    if "correctness_comparison" in report:
        comparison = report["correctness_comparison"]
        if not comparison["available"]:
            print("Correctness comparison unavailable: baseline lacks assertion details.")
        for change in comparison["changes"]:
            print(f"Correctness observations changed: r={change['r']} {change['name']} "
                  f"before={change['before']} after={change['after']}")
    for index, run in enumerate(runs):
        backend = run.get("fpu_backend")
        if backend is None and isinstance(run.get("machine"), dict):
            backend = fpu_backend(run["machine"])  # results from before fpu_backend
        if note := host_fpu_note(run["tests"], backend):
            print(f"Run {index + 1}: {note}")
    print("name samples min median p95 counter/op timer_unit op_unit timing_scope comparison")
    comparisons = {c["name"]: c for c in report.get("comparison", [])}
    for name, entry in report["workloads"].items():
        stats = entry["counter_per_op"]
        if stats:
            change = comparisons.get(name, {})
            label = change.get("status", "-")
            if "percent" in change:
                label += f"({change['percent']:+.1f}%)"
            print(f"{name} {stats['n']} {stats['min']:.3f} {stats['median']:.3f} "
                  f"{stats['p95']:.3f} {entry['unit']} "
                  f"{entry['op_unit']} {entry['timing_scope']} {label}")
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
    capacity = report["capacity"]["tests"]
    if capacity:
        print("Capacity (speed % per host CPU second of the emulation thread, warm-in excluded):")
        print("rank name median ci95_low ci95_high samples")
        for name, entry in sorted(capacity.items(), key=lambda kv: kv[1]["rank"]):
            low, high = entry["ci95_percent"]
            print(f"{entry['rank']} {name} {entry['median_percent']:.1f} {low:.1f} {high:.1f} "
                  f"{entry['samples']}")
    print("Guest counter/op follows emulated time (86Box's timing model), not host cost; "
          "capacity is the host-cost measure.")
    if "capacity_noise_floor" in report:
        floor = report["capacity_noise_floor"]["noise_floor_percent"]
        print("A/A capacity noise floor: " + ("unavailable" if floor is None else f"{floor:.2f}%"))
    if "capacity_comparison" in report:
        comparison = report["capacity_comparison"]
        print(f"Capacity comparison (noise floor {comparison['noise_floor_percent']}%): "
              + " ".join(f"{k}={v}" for k, v in sorted(comparison["counts"].items())))
        for name, entry in comparison["tests"].items():
            if entry.get("delta_percent") is not None:
                low, high = entry["ci95_percent"]
                print(f"  {name} {entry['verdict']} {entry['delta_percent']:+.1f}% "
                      f"[{low:+.1f}, {high:+.1f}]")
            else:
                print(f"  {name} {entry['verdict']}")
    if "host_comparison" in report:
        host = report["host_comparison"]
        print(f"Host boot-to-DONE comparison: {host['status']}"
              + (f" ({host['percent']:+.1f}%)" if "percent" in host else ""))
    bad = bool(report["failures"]) or any(
        c["status"] == "regression" for c in report.get("comparison", []))
    if args.gate_host_time:
        bad |= report["host_comparison"]["status"] == "regression"
    if args.gate_capacity:
        bad |= report["capacity_comparison"]["counts"].get("slower", 0) > 0
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
