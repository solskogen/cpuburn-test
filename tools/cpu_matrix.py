#!/usr/bin/env python3
"""Enumerate 86Box CPU configurations; CPU tables are inventory, never an oracle."""
import argparse
import ast
import configparser
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PREFERRED = ("486sp3", "486sp3g", "amis76", "p55t2p4", "p5a", "bf6",
             "ap61", "p6nat", "spc7700plw", "deskpro386")
EXCLUDED = {"CPU_8088", "CPU_80C88", "CPU_8086", "CPU_8086_MAZOVIA",
            "CPU_188", "CPU_186", "CPU_V20", "CPU_V30", "CPU_286",
            "CPU_386SX", "CPU_386DX", "CPU_IBM386SLC", "CPU_RAPIDCAD"}


def preprocess(text, defines=()):
    # Preprocess only the table's conditional sections; no project headers,
    # executable expressions, generated helpers or emulator build are involved.
    text = re.sub(r"(?m)^\s*#\s*include[^\n]*", "", text)
    command = ["cpp", "-P", "-undef", "-nostdinc", *("-D" + d for d in defines)]
    return subprocess.run(command, input=text, text=True, capture_output=True,
                          check=True).stdout


def split_c(text):
    """Split commas at the outer level, retaining C string/initializer contents."""
    pieces, start, depth, quoted, escape = [], 0, 0, False, False
    for i, c in enumerate(text):
        if quoted:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
        elif c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
            if depth < 0:
                raise ValueError("unbalanced C initializer")
        elif c == "," and not depth:
            pieces.append(text[start:i].strip())
            start = i + 1
    if quoted or depth:
        raise ValueError("unterminated C initializer")
    pieces.append(text[start:].strip())
    return [p for p in pieces if p]


def fields(entry):
    entry = entry.strip()
    if not entry.startswith("{") or not entry.endswith("}"):
        raise ValueError("expected designated C initializer")
    result = {}
    for field in split_c(entry[1:-1]):
        match = re.fullmatch(r"\.(\w+)\s*=\s*(.*)", field, re.S)
        if match:
            if match[1] in result:
                raise ValueError("duplicate C field: " + match[1])
            result[match[1]] = match[2].strip()
        elif field != "0":
            raise ValueError("unsupported C field: " + field[:80])
    return result


def table(text, name):
    match = re.search(r"\b" + re.escape(name) + r"\[\]\s*=\s*\{", text)
    if not match:
        raise ValueError("missing table " + name)
    depth, quoted, escape, end = 1, False, False, None
    for i in range(match.end(), len(text)):
        c = text[i]
        if quoted:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if not depth:
                end = i
                break
    if end is None:
        raise ValueError("unterminated table " + name)
    return [fields(p) for p in split_c(text[match.end():end])]


def number(text):
    """Evaluate the numeric subset used by tables, not arbitrary C/Python."""
    node = ast.parse(text.strip(), mode="eval").body

    def value(n):
        if isinstance(n, ast.Constant) and type(n.value) in (int, float):
            return n.value
        if isinstance(n, ast.BinOp) and isinstance(n.op, (ast.Div, ast.Mult, ast.Add, ast.Sub)):
            a, b = value(n.left), value(n.right)
            if isinstance(n.op, ast.Div):
                return a / b
            if isinstance(n.op, ast.Mult):
                return a * b
            if isinstance(n.op, ast.Add):
                return a + b
            return a - b
        raise ValueError("unsupported numeric expression: " + text)
    return value(node)


def string(text):
    if not re.fullmatch(r'"[^"\\]*"', text):
        raise ValueError("unsupported C string: " + text)
    return text[1:-1]


def contracts():
    return set(re.findall(r'^CPU_MODEL\("([^"]+)"', (ROOT / "cpu_models.h").read_text(), re.M))


def read_inventory(source, defines=()):
    source = Path(source)
    cpu_path, machine_path = source / "src/cpu/cpu_table.c", source / "src/machine/machine_table.c"
    cpu_text = preprocess(cpu_path.read_text(), defines)
    machine_text = preprocess(machine_path.read_text(), defines)
    cpus = []
    for family in table(cpu_text, "cpu_families"):
        if "internal_name" not in family:
            continue
        entries = family["cpus"]
        start = entries.index("{")
        rows = [fields(p) for p in split_c(entries[start + 1:-1])]
        for index, row in enumerate(rows):
            if not row.get("name") or string(row["name"]) == "":
                continue
            if row["cpu_type"] in EXCLUDED:
                continue
            cpus.append({
                "family": string(family["internal_name"]),
                "family_name": string(family["name"]), "model_name": string(row["name"]),
                "index": index, "package": family["package"],
                "type": row["cpu_type"], "speed": int(number(row["rspeed"])),
                "multi": number(row["multi"]), "voltage": int(number(row["voltage"])),
                "flags": row["cpu_flags"], "fpus": row["fpus"],
                "cyrix_id": int(number(row["cyrix_id"])),
                "signature": int(number(row["cpuid_model"])),
            })
    boards = []
    for m in table(machine_text, "machines"):
        if m.get("internal_name", "NULL") == "NULL" or "cpu" not in m or "ram" not in m:
            continue
        if m.get("available_flag") != "MACHINE_AVAILABLE":
            continue
        cpu = fields(m["cpu"])
        if cpu.get("block") != "CPU_BLOCK_NONE":
            continue  # choose an unrestricted board; never ignore its blocklist
        boards.append({
            "name": string(m["internal_name"]), "cpu": cpu, "ram": fields(m["ram"]),
            "init": m.get("init"), "flags": m.get("flags", ""),
        })
    revision = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(source), "status", "--porcelain",
                            "--", "src/cpu/cpu_table.c", "src/machine/machine_table.c"],
                           check=True, capture_output=True, text=True).stdout.strip()
    return cpus, boards, {
        "revision": revision, "tables_modified": bool(dirty),
        "defines": list(defines),
        "table_sha256": {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in (cpu_path, machine_path)},
    }


def eligible(cpu, board):
    spec = board["cpu"]
    packages = set(re.findall(r"CPU_PKG_\w+", spec["package"]))
    if "CPU_PKG_SOCKET3" in packages:
        packages.add("CPU_PKG_SOCKET1")
    if "CPU_PKG_SLOT1" in packages:
        packages.update(("CPU_PKG_SOCKET370", "CPU_PKG_SOCKET8"))
    if not packages.intersection(re.findall(r"CPU_PKG_\w+", cpu["package"])):
        return False
    if board["init"] in ("machine_at_cobalt3k_init", "machine_at_tandy_vis_init"):
        return False
    if board["init"] == "machine_at_nupro592_init" and cpu["cyrix_id"] & 0xff00 == 0x400:
        return False
    bus = int(cpu["speed"] / cpu["multi"])
    for key, actual, tolerance in (("bus", bus, 840907), ("voltage", cpu["voltage"], 100)):
        low, high = number(spec.get("min_" + key, "0")), number(spec.get("max_" + key, "0"))
        if (low and actual < low - tolerance) or (high and actual > high + tolerance):
            return False
    if "CPU_FIXED_MULTIPLIER" in cpu["flags"]:
        return True
    multi = cpu["multi"]
    low, high = number(spec.get("min_multi", "0")), number(spec.get("max_multi", "0"))
    if cpu["package"] == "CPU_PKG_SOCKET5_7":
        if multi == 1.5 and cpu["type"] == "CPU_5K86" and low > 1.5:
            multi = 2
        elif multi == 1.75:
            multi = 2.5
        elif multi == 2:
            if cpu["type"] == "CPU_5K86":
                multi = 3
            elif cpu["type"] in ("CPU_K6_2P", "CPU_K6_3P"):
                multi = 2.5
            elif cpu["type"] in ("CPU_WINCHIP", "CPU_WINCHIP2") and low > 2:
                multi = 2.5
        elif multi in (7 / 3, 8 / 3, 10 / 3):
            multi = {7 / 3: 5, 8 / 3: 5.5, 10 / 3: 2}[multi]
        elif multi == 3 and cpu["type"] in ("CPU_Cx6x86", "CPU_Cx6x86L"):
            multi = 1.5
        elif multi == 3.5:
            multi = 1.5
        elif multi == 4:
            if cpu["type"] in ("CPU_WINCHIP", "CPU_WINCHIP2"):
                if low >= 1.5:
                    multi = 1.5
                elif low >= 3.5:
                    multi = 3.5
                elif low >= 4.5:
                    multi = 4.5
            elif cpu["type"] in ("CPU_Cx6x86", "CPU_Cx6x86L"):
                multi = 3
        elif multi == 5 and cpu["type"] in ("CPU_WINCHIP", "CPU_WINCHIP2") and low > 5:
            multi = 5.5
        elif multi == 6:
            multi = 2
    return multi >= low and (not high or multi <= high)


def make_matrix(cpus, boards, all_speeds=False, fpu_backends="auto"):
    known, rows, selected = contracts(), [], set()
    boards = sorted(boards, key=lambda b: (
        PREFERRED.index(b["name"]) if b["name"] in PREFERRED else len(PREFERRED),
        b["name"]))
    for cpu in cpus:
        # Default covers each named family; --all-speeds covers every entry.
        if not all_speeds and cpu["family"] in selected:
            continue
        candidates = [b for b in boards if eligible(cpu, b) and number(b["ram"]["max"]) >= 16384]
        if not candidates:
            if all_speeds:
                rows.append({**cpu, "status": "blocked", "reason": "no_compatible_board"})
            continue
        board = candidates[0]
        ram = board["ram"]
        memory = max(16384, int(number(ram["min"])))
        step = int(number(ram["step"]))
        if step:
            base = int(number(ram["min"]))
            memory = base + ((memory - base + step - 1) // step) * step
        if memory > number(ram["max"]):
            continue
        selected.add(cpu["family"])
        if cpu["fpus"] == "fpus_internal":
            fpus = ["internal"]
        elif cpu["fpus"] == "fpus_486sx":
            fpus = ["none", "487sx"]
        elif cpu["fpus"] == "fpus_80386":
            fpus = ["none", "387"]
        else:
            raise ValueError("unhandled FPU inventory: " + cpu["fpus"])
        # REQUIRES_DYNAREC constrains the GUI checkbox, not the runtime
        # interpreter selector. Regression runs cover both available engines.
        engines = ["off"]
        if "CPU_SUPPORTS_DYNAREC" in cpu["flags"] and "on" not in engines:
            engines.append("on")
        for fpu in fpus:
            if fpu == "none":
                backends = ["none"]
            elif "MACHINE_SOFTFLOAT_ONLY" in board.get("flags", ""):
                backends = ["softfloat"]
            elif fpu_backends == "both":
                backends = ["host", "softfloat"]
            else:
                backends = ["host" if fpu_backends == "auto" else fpu_backends]
            for backend in backends:
                for engine in engines:
                    rows.append({**cpu, "machine": board["name"], "memory_kb": memory,
                                 "fpu": fpu, "fpu_backend": backend, "dynarec": engine,
                                 "gui_requires_dynarec": "CPU_REQUIRES_DYNAREC" in cpu["flags"],
                                 "contract": cpu["family"] in known, "status": "ready"})
    for family in sorted({c["family"] for c in cpus} - selected):
        rows.append({"family": family, "status": "blocked", "reason": "no_compatible_board"})
    return rows


def write_configs(rows, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for row in rows:
        if row["status"] != "ready":
            continue
        name = f"{row['family']}-{row['index']}-{row['fpu']}-{row['fpu_backend']}-{row['dynarec']}"
        row["id"] = name
        config = configparser.ConfigParser()
        config["Machine"] = {
            "machine": row["machine"], "cpu_family": row["family"],
            "cpu_speed": str(row["speed"]), "cpu_multi": str(row["multi"]),
            "fpu_type": row["fpu"], "cpu_use_dynarec": "1" if row["dynarec"] == "on" else "0",
            "mem_size": str(row["memory_kb"]),
            "fpu_softfloat": "1" if row["fpu_backend"] == "softfloat" else "0",
        }
        path = destination / (name + ".cfg")
        with path.open("w") as stream:
            config.write(stream)
        row["config"] = str(path.resolve())


def run_matrix(rows, args):
    results_path = args.output / "results.json"
    if results_path.exists():
        raise ValueError(f"{results_path} already exists; choose a fresh --output directory")
    image = args.image.resolve(strict=True)
    binary = args.emulator.resolve(strict=True)
    mode = getattr(args, "mode", "accuracy")
    results = {
        "format": 1, "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "mode": mode,
        "emulator_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "engine_verification": "requested_configuration_only",
        "fpu_backend_verification": "requested_configuration_only",
        "rows": [],
    }
    for row in rows:
        if args.family and row["family"] not in args.family:
            continue
        if args.engine and row["status"] == "ready" and row.get("dynarec") != args.engine:
            continue
        if row["status"] != "ready":
            results["rows"].append({**row, "outcome": "blocked"})
        else:
            command = [
                sys.executable, str(ROOT / "tools/run_86box.py"), "--bios-f1",
                "--emulator", str(binary), "--image", str(image), "--config", row["config"],
                "--memory-mb", str(row["memory_kb"] // 1024),
                "--expected-family-name", row["family_name"],
                "--expected-model-name", row["model_name"],
                "--expected-signature", hex(row["signature"]),
                "--wall-seconds", str(args.wall_seconds), "--label", row["id"],
            ]
            if mode == "accuracy":
                command.append("--accuracy")
            if args.rompath:
                command += ["--rompath", str(args.rompath)]
            process = subprocess.run(command, capture_output=True, text=True)
            output = process.stdout.strip().splitlines()
            entry = {**row, "returncode": process.returncode,
                     "stderr": process.stderr, "stdout": process.stdout}
            try:
                payload = json.loads(output[-1]) if output else None
            except json.JSONDecodeError:
                payload = None
            if isinstance(payload, dict) and "directory" in payload:
                result_path = Path(payload["directory"]) / "result.json"
                if result_path.is_file():
                    entry["result"] = str(result_path)
                    try:
                        result = json.loads(result_path.read_text())
                        if not isinstance(result, dict) or not isinstance(result.get("status"), str):
                            raise ValueError("runner result lacks a status")
                    except (OSError, ValueError) as error:
                        entry["result_error"] = str(error)
                    else:
                        entry["outcome"] = result["status"]
                        entry["counts"] = result.get("counts", {})
                        entry["features"] = result.get("features", [])
                        entry["contract_tests"] = [t for t in result.get("tests", [])
                                                   if t.get("name") == "cpu_model_contract"]
                        entry["conformance_coverage"] = result.get("conformance_coverage", {})
                        if mode == "benchmark" and result.get("mode") != "benchmark":
                            entry["outcome"] = "benchmark_unavailable"
                            entry["result_error"] = "guest did not confirm benchmark mode; use BENCHMARKS=1"
                        if not any(t.get("status") == "PASS" for t in entry["contract_tests"]):
                            if entry["outcome"] in ("pass", "observed"):
                                entry["outcome"] = "unverified_identity"
            if "outcome" not in entry:
                entry["outcome"] = "infrastructure_error"
            results["rows"].append(entry)
            print(f"{row.get('id', row['family'])}: {results['rows'][-1]['outcome']}", flush=True)
        results_path.write_text(json.dumps(results, indent=2) + "\n")
    if not results["rows"]:
        raise ValueError("matrix selection is empty")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="local 86Box source checkout")
    parser.add_argument("--output", type=Path, default=ROOT / "build/cpu-matrix")
    parser.add_argument("--all-speeds", action="store_true", help="every CPU-table entry, not one per family")
    parser.add_argument("--define", action="append", default=[], help="enabled source-table preprocessor option")
    parser.add_argument("--run", action="store_true", help="execute selected configurations sequentially")
    parser.add_argument("--mode", choices=("accuracy", "benchmark"), default="accuracy",
                        help="matrix run mode; benchmarks require a BENCHMARKS=1 image")
    parser.add_argument("--family", action="append", default=[], help="select a family; repeatable")
    parser.add_argument("--engine", choices=("on", "off"), help="select requested dynarec mode")
    parser.add_argument("--fpu-backend", choices=("auto", "host", "softfloat", "both"), default="auto")
    parser.add_argument("--image", type=Path, default=ROOT / "floppy.img")
    parser.add_argument("--emulator", type=Path, default=ROOT.parent / "86Box/build/src/86Box")
    parser.add_argument("--rompath", type=Path)
    parser.add_argument("--wall-seconds", type=float, default=1800)
    args = parser.parse_args()
    try:
        if (args.output / "results.json").exists():
            raise ValueError("output already contains execution results; choose a fresh --output directory")
        if any(not re.fullmatch(r"[A-Z][A-Z0-9_]*", d) for d in args.define):
            raise ValueError("defines must be simple uppercase identifiers")
        cpus, boards, provenance = read_inventory(args.source, args.define)
        if set(args.family) - {c["family"] for c in cpus}:
            raise ValueError("family selection is absent from the active source inventory")
        rows = make_matrix(cpus, boards, args.all_speeds, args.fpu_backend)
        if not rows:
            raise ValueError("no 486+ CPUs found")
        write_configs(rows, args.output)
        result = {"format": 1, "source": provenance, "all_speeds": args.all_speeds,
                  "rows": rows, "families": sorted({c["family"] for c in cpus})}
        (args.output / "matrix.json").write_text(json.dumps(result, indent=2) + "\n")
        blocked = [r for r in rows if r["status"] != "ready" or not r.get("contract")]
        print(f"families={len(result['families'])} configurations={len(rows)} blocked={len(blocked)} "
              f"manifest={args.output / 'matrix.json'}")
        if args.run:
            results = run_matrix(rows, args)
            return 1 if any(row["outcome"] not in ("pass", "observed")
                            for row in results["rows"]) else 0
        return 2 if blocked else 0
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"CPU matrix: {error}\n")


if __name__ == "__main__":
    sys.exit(main())
