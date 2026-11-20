#!/usr/bin/env python3
"""Split one suite run across several pinned 86Box instances and merge the results.

  plan:  contiguous shards of the suite, balanced by a previous run's host seconds
  run:   run tools/run_86box.py --only <shard> --pin <core> in parallel (cloning
         --vm once per core; generated profiles need no clone), then merge
  merge: combine shard result.json files into one result.json and summary.json

Each record runs in exactly one shard (cpu_model_contract, which sel1 never
filters, runs in all of them and is kept once). Shards are contiguous slices
of the suite order, so every record except the first of each shard keeps its
usual predecessor. Feature rows are recombined from the shards' counters: a
record that ran in one shard is FILTERED in the others, and those artefacts
are removed again (see merge_features)."""
import argparse
import json
import shutil
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

from perf_report import build_summary

TOOLS = Path(__file__).resolve().parent
CONTRACT = "cpu_model_contract"
STATUS_RANK = {"error": 3, "failed": 2, "observed": 1, "passed": 0}
SEL1_MAX = 4096


def _cpu_list(path):
    try:
        text = Path(path).read_text().strip()
    except OSError:
        return None
    cpus = set()
    for part in text.split(","):
        low, _, high = part.partition("-")
        cpus.update(range(int(low), int(high or low) + 1))
    return frozenset(cpus)


def core_warnings(cores, sysfs="/sys/devices/system/cpu"):
    """Workers that share a physical core or a last-level cache slow each other
    down unevenly (two on one Zen CCD cost footprint-heavy records ~25%), so
    their capacities are not comparable with a solo run."""
    warnings = []
    if len(set(cores)) != len(cores):
        warnings.append("a core is listed twice")
    for i, a in enumerate(cores):
        for b in cores[i + 1:]:
            if a == b:
                continue
            siblings = _cpu_list(f"{sysfs}/cpu{a}/topology/thread_siblings_list")
            if siblings and b in siblings:
                warnings.append(f"cores {a} and {b} are SMT siblings")
                continue
            for index in (3, 2):
                shared = _cpu_list(f"{sysfs}/cpu{a}/cache/index{index}/shared_cpu_list")
                if shared is not None:
                    if b in shared:
                        warnings.append(f"cores {a} and {b} share an L{index} cache")
                    break
    return warnings


def load(path):
    return json.loads(Path(path).read_text())


def record_costs(history):
    """{name: host seconds} from a previous run: its workload phases' step time."""
    costs = {test["name"]: 0.0 for test in history.get("tests", [])}
    for phase in (history.get("workload_capture") or {}).get("phases", []):
        costs[phase["name"]] = costs.get(phase["name"], 0.0) + phase.get("step_wall_seconds", 0.0)
    return costs


def plan(names, costs, shards):
    """Contiguous slices of names minimising the largest slice cost."""
    names = [n for n in names if n != CONTRACT]
    default = statistics.median(costs.values()) if costs else 1.0
    weights = [max(costs.get(n, default), 0.01) for n in names]
    shards = max(1, min(shards, len(names)))

    def cut(limit):
        slices, current, load_ = [[]], 0.0, 0.0
        for name, weight in zip(names, weights):
            if slices[-1] and load_ + weight > limit:
                slices.append([])
                load_ = 0.0
            slices[-1].append(name)
            load_ += weight
        return slices

    low, high = max(weights), sum(weights)
    for _ in range(60):
        middle = (low + high) / 2
        if len(cut(middle)) <= shards:
            high = middle
        else:
            low = middle
    slices = cut(high)
    for names_ in slices:
        frame = len("@sel1  - -\n") + len(",".join(names_))
        if frame > SEL1_MAX:
            raise ValueError(f"a shard's selection frame is {frame} bytes (> {SEL1_MAX}); use more shards")
    return [{"only": s, "planned_seconds": sum(costs.get(n, default) for n in s)} for s in slices]


def merge_features(runs, tests_by_name):
    """Recombine FEATURE rows. In each shard a record counted for a feature is
    either run there (pass/fail/fault/skip) or FILTERED; with N shards a record
    run once adds N-1 spurious FILTERED counts, a record filtered everywhere
    (suite/build selection) adds N."""
    shards = len(runs)
    merged = []
    for rows in zip(*(run.get("features", []) for run in runs)):
        names = {row["name"] for row in rows}
        if len(names) != 1:
            raise ValueError(f"feature rows differ between shards: {sorted(names)}")
        row = dict(rows[0])
        total = {key: sum(int(r.get(key, 0)) for r in rows) for key in ("pass", "fail", "fault", "skip", "filtered")}
        ran = total["pass"] + total["fail"] + total["fault"] + total["skip"]
        spare = total["filtered"] - (shards - 1) * ran
        if row.get("scope") == "sampled" and (spare < 0 or spare % shards):
            raise ValueError(f"feature {row['name']}: shard counters are inconsistent")
        if row.get("scope") == "sampled":
            filtered = spare // shards
            witness = tests_by_name.get(row.get("witness"), {}).get("status")
            if row.get("status") != "NA":
                if total["fail"] or total["fault"]:
                    row["status"] = "FAIL"
                elif witness != "PASS" or total["skip"] or filtered:
                    row["status"] = "UNTESTED"
                else:
                    row["status"] = "PASS"
            row.update({key: str(total[key]) for key in ("pass", "fail", "fault", "skip")})
            row["filtered"] = str(filtered)
        merged.append(row)
    return merged


def shard_coverage(runs, merged_tests):
    """Every shard's FILTER and SUMMARY lines count the whole image: records
    run + filtered by suite, build and runtime selection. Records filtered only
    by runtime selection must have run in some shard."""
    totals, fixed = set(), set()
    for run in runs:
        f = {k: int(v) for k, v in (run.get("filter_summary") or {}).items()}
        tests = int((run.get("counts") or {}).get("tests", len(run["tests"])))
        totals.add(tests + f.get("suite_selection", 0) + f.get("build_selection", 0) +
                   f.get("runtime_selection", 0))
        fixed.add(f.get("suite_selection", 0) + f.get("build_selection", 0))
    if len(totals) != 1 or len(fixed) != 1:
        raise ValueError(f"shards report different record totals: {sorted(totals)}")
    records, selection = totals.pop(), fixed.pop()
    return {"records_in_image": records, "selection_filtered": selection,
            "not_run": records - selection - merged_tests}


def by_id(rows):
    return sorted(rows, key=lambda row: int(row.get("id", 0)))


def merge(runs, shard_meta=None):
    """One result.json-like dict for N shard results of the same image and VM."""
    for key in ("image_sha256", "emulator_sha256"):
        if len({run.get(key) for run in runs}) != 1:
            raise ValueError(f"shards differ in {key}")
    if len({json.dumps(run.get("machine"), sort_keys=True) for run in runs}) != 1:
        raise ValueError("shards ran different machines")
    contract = [t for run in runs for t in run["tests"] if t["name"] == CONTRACT]
    if len({(t["status"], t.get("sum")) for t in contract}) > 1:
        raise ValueError("cpu_model_contract differs between shards")
    tests, seen = [], set()
    for run in runs:
        for test in run["tests"]:
            key = (test.get("r"), test["name"])
            if key in seen:
                if test["name"] != CONTRACT:
                    raise ValueError(f"record {test['name']} ran in more than one shard")
                continue
            seen.add(key)
            tests.append(test)
    tests = by_id(tests)
    tests_by_name = {t["name"]: t for t in tests}
    first = runs[0]
    coverage = shard_coverage(runs, len(tests))
    merged = {key: first.get(key) for key in (
        "machine", "label", "step_ms", "directory", "host", "emulator_executable", "mode",
        "limits", "initial_info", "build_info", "config", "conformance", "cpu_info",
        "image_sha256", "emulator_sha256")}
    merged["coverage"] = coverage
    merged["status"] = max((run.get("status", "error") for run in runs),
                           key=lambda s: STATUS_RANK.get(s, 3))
    merged["tests"] = tests
    counts = {"rounds": first.get("counts", {}).get("rounds", "1"), "tests": str(len(tests))}
    for key, status in (("pass", "PASS"), ("fail", "FAIL"), ("skip", "SKIP"), ("fault", "FAULT")):
        counts[key] = str(sum(t["status"] == status for t in tests))
    merged["counts"] = counts
    merged["done"] = all(run.get("done") for run in runs)
    merged["fatal"] = any(run.get("fatal") for run in runs)
    merged["features"] = merge_features(runs, tests_by_name)
    merged["conformance_coverage"] = dict(first.get("conformance_coverage") or {})
    merged["conformance_coverage"].update({
        "untested_features": [f["name"] for f in merged["features"] if f.get("status") == "UNTESTED"],
        "failed_features": [f["name"] for f in merged["features"] if f.get("status") == "FAIL"]})
    records = [r for run in runs for r in run.get("runtime_records", []) if r.get("scope") == "test"]
    merged["runtime_records"] = by_id(list({(r.get("r"), r["name"]): r for r in records}.values()))
    captures = [run.get("workload_capture") or {} for run in runs]
    capture = dict(captures[0])
    for key in ("workloads", "phases"):
        rows = [w for c in captures for w in c.get(key, [])
                if w["name"] != CONTRACT or c is captures[0]]
        capture[key] = sorted(rows, key=lambda w: (int(w["id"]), w.get("phase", "")))
    capture["aggregates"] = {}
    for c in captures:
        for name, value in (c.get("aggregates") or {}).items():
            capture["aggregates"].setdefault(name, value)
    capture["unattributed"] = {key: sum((c.get("unattributed") or {}).get(key, 0) for c in captures)
                               for key in (captures[0].get("unattributed") or {})}
    capture["warnings"] = [w for c in captures for w in c.get("warnings", [])]
    merged["workload_capture"] = capture
    merged["emulated_ms"] = max(run.get("emulated_ms", 0) for run in runs)
    merged["host_seconds"] = max(run.get("host_seconds", 0) for run in runs)
    merged["process_cpu_seconds_total"] = sum(run.get("process_cpu_seconds_total", 0) for run in runs)
    merged["shards"] = [{"directory": run.get("directory"), "status": run.get("status"),
                         "tests": len(run["tests"]), "host_seconds": run.get("host_seconds"),
                         "startup_wall_seconds": run.get("startup_wall_seconds"),
                         "emulated_ms": run.get("emulated_ms"), **(meta or {})}
                        for run, meta in zip(runs, shard_meta or [None] * len(runs))]
    complete = (merged["done"] and coverage["not_run"] == 0 and
                all(run.get("counts_consistent") for run in runs))
    merged["correctness"] = {"complete": complete,
                             "passed": complete and not merged["fatal"] and
                             counts["fail"] == "0" and counts["fault"] == "0",
                             "note": f"merged from {len(runs)} shards"}
    return merged


def write_merged(out, runs, meta=None, wall=None):
    merged = merge(runs, meta)
    merged["directory"] = str(out)
    if wall is not None:
        merged["host_seconds"] = wall
    (out / "result.json").write_text(json.dumps(merged, indent=2) + "\n")
    summary = build_summary([merged])
    summary["shards"] = merged["shards"]
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return merged


def cmd_plan(args):
    history = load(args.history)
    names = [t["name"] for t in by_id(history["tests"])]
    print(json.dumps(plan(names, record_costs(history), args.shards), indent=2))


def cmd_run(args):
    """A pool of one worker per core pulls contiguous chunks, largest planned
    first: per-record host time varies run to run (unsettled samples run to
    --max-sample-seconds), so more chunks than cores balance the wall time at
    the price of one boot (~3 s) per chunk."""
    if args.plan:
        slices = load(args.plan)
    else:
        if not args.history:
            raise SystemExit("run needs --history or --plan")
        history = load(args.history)
        names = [t["name"] for t in by_id(history["tests"])]
        slices = plan(names, record_costs(history), args.chunks or len(args.cores))
    for warning in core_warnings(args.cores):
        print(f"warning: {warning}; capacities will not match a solo run", file=sys.stderr)
    out = args.output_dir.expanduser().resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "plan.json").write_text(json.dumps(slices, indent=2) + "\n")
    source = None
    if args.vm:
        source = args.vm.expanduser().resolve()
        source = source if source.is_dir() else source.parent
    queue = sorted(range(len(slices)), key=lambda i: -slices[i]["planned_seconds"])
    lock = threading.Lock()
    meta = [None] * len(slices)

    def worker(core):
        vm_args = []
        if source is not None:
            # Two 86Box processes must not share one VM's NVR, disks or config.
            vm = out / f"vm-core{core}"
            shutil.copytree(source, vm, symlinks=True)
            vm_args = ["--vm", str(vm)]
        while True:
            with lock:
                if not queue:
                    return
                index = queue.pop(0)
            command = [sys.executable, str(TOOLS / "run_86box.py"), *vm_args,
                       "--output-dir", str(out / f"shard-{index}"), "--pin", str(core),
                       "--only", ",".join(slices[index]["only"]), *args.passthrough]
            started = time.monotonic()
            with open(out / f"shard-{index}.log", "w") as log:
                code = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT)
            meta[index] = {"core": core, "planned_seconds": slices[index]["planned_seconds"],
                           "exit": code, "started": started, "wall": time.monotonic() - started}

    started = time.monotonic()
    threads = [threading.Thread(target=worker, args=(core,)) for core in args.cores]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    wall = time.monotonic() - started
    runs = [load(out / f"shard-{i}" / "result.json") for i in range(len(slices))]
    merged = write_merged(out, runs, meta, wall)
    print(json.dumps({"status": merged["status"], "directory": str(out),
                      "wall_seconds": wall, "chunks": len(slices),
                      "shard_seconds": [round(s["host_seconds"], 1) for s in merged["shards"]]}))
    return max(m["exit"] for m in meta)


def cmd_merge(args):
    out = args.output_dir.expanduser().resolve()
    out.mkdir(parents=True, exist_ok=True)
    merged = write_merged(out, [load(p) for p in args.results])
    print(json.dumps({"status": merged["status"], "directory": str(out),
                      "tests": len(merged["tests"]), "host_seconds": merged["host_seconds"]}))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("plan", help="print the shard plan")
    p.add_argument("--history", type=Path, required=True, help="a previous full result.json of this suite")
    p.add_argument("--shards", type=int, required=True)
    p.set_defaults(func=cmd_plan)
    r = sub.add_parser("run", help="run the shards in parallel and merge")
    r.add_argument("--history", type=Path, help="a previous full result.json to balance shards by")
    r.add_argument("--plan", type=Path,
                   help="reuse a plan.json (use the same one for both sides of an A/B comparison)")
    r.add_argument("--vm", type=Path,
                   help="VM directory, cloned once per core; omit for run_86box generated profiles")
    r.add_argument("--cores", type=lambda s: [int(c) for c in s.split(",")], required=True,
                   help="one logical CPU per worker, e.g. 7,9; use one per L3 cache (CCD) for "
                        "capacity numbers comparable with a solo run")
    r.add_argument("--chunks", type=int, default=0,
                   help="contiguous chunks to queue over the cores (default: one per core)")
    r.add_argument("--output-dir", type=Path, required=True)
    r.add_argument("passthrough", nargs=argparse.REMAINDER,
                   help="-- then options for run_86box.py (--image, --emulator, --headless, ...)")
    r.set_defaults(func=cmd_run)
    m = sub.add_parser("merge", help="merge shard result.json files")
    m.add_argument("results", nargs="+", type=Path)
    m.add_argument("--output-dir", type=Path, required=True)
    m.set_defaults(func=cmd_merge)
    args = parser.parse_args()
    if getattr(args, "passthrough", None) and args.passthrough[0] == "--":
        args.passthrough = args.passthrough[1:]
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
