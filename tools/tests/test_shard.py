"""Shard planning and merging; no emulator is started."""

import tempfile
from pathlib import Path
import unittest

from shard_86box import CONTRACT, core_warnings, merge, merge_features, plan


def feature(witness, ran, filtered, status="PASS", **counts):
    row = {"name": "f", "status": status, "witness": witness, "scope": "sampled",
           "pass": "0", "fail": "0", "fault": "0", "skip": "0", "filtered": str(filtered)}
    row.update({key: str(value) for key, value in counts.items()})
    row["pass"] = str(int(row["pass"]) + ran)
    return row


def shard(tests, features, filtered_runtime):
    return {"image_sha256": "i", "emulator_sha256": "e", "machine": {"cpu": "x"},
            "status": "passed", "done": True, "counts_consistent": True,
            "counts": {"rounds": "1", "tests": str(len(tests))},
            "filter_summary": {"suite_selection": "0", "build_selection": "1",
                               "runtime_selection": str(filtered_runtime)},
            "tests": [{"r": "1", "name": n, "id": str(i), "status": "PASS", "sum": "0"}
                      for i, n in tests],
            "features": features, "runtime_records": [],
            "workload_capture": {"workloads": [], "phases": [], "unattributed": {}}}


class ShardTests(unittest.TestCase):
    def test_plan_is_contiguous_and_balanced(self):
        names = [CONTRACT, "a", "b", "c", "d"]
        slices = plan(names, {"a": 4, "b": 1, "c": 1, "d": 2}, 2)
        self.assertEqual([s["only"] for s in slices], [["a"], ["b", "c", "d"]])
        self.assertEqual([s["planned_seconds"] for s in slices], [4, 4])

    def test_plan_rejects_oversized_selection_frames(self):
        names = [f"record_with_a_long_name_{i:04d}" for i in range(200)]
        with self.assertRaises(ValueError):
            plan(names, {}, 1)
        self.assertEqual(len(plan(names, {}, 2)), 2)

    def test_merge_features_removes_cross_shard_filtering(self):
        rows = merge_features([{"features": [feature("a", 1, 1)]},
                               {"features": [feature("a", 1, 1)]}],
                              {"a": {"status": "PASS"}})
        self.assertEqual((rows[0]["status"], rows[0]["pass"], rows[0]["filtered"]),
                         ("PASS", "2", "0"))
        rows = merge_features([{"features": [feature("a", 1, 2)]},
                               {"features": [feature("a", 1, 2)]}],
                              {"a": {"status": "PASS"}})
        self.assertEqual((rows[0]["status"], rows[0]["filtered"]), ("UNTESTED", "1"))
        with self.assertRaises(ValueError):
            merge_features([{"features": [feature("a", 1, 0)]},
                            {"features": [feature("a", 1, 0)]}], {})

    def test_merge_keeps_contract_once_and_rejects_duplicates(self):
        a = shard([(1, CONTRACT), (2, "a")], [feature("a", 1, 1)], 1)
        b = shard([(1, CONTRACT), (3, "b")], [feature("a", 0, 2)], 1)
        merged = merge([a, b])
        self.assertEqual([t["name"] for t in merged["tests"]], [CONTRACT, "a", "b"])
        self.assertEqual(merged["coverage"]["not_run"], 0)
        self.assertTrue(merged["correctness"]["passed"])
        with self.assertRaises(ValueError):
            merge([a, shard([(1, CONTRACT), (2, "a")], [feature("a", 1, 1)], 1)])
        with self.assertRaises(ValueError):
            merge([a, dict(b, emulator_sha256="other")])

    def test_core_warnings_read_topology(self):
        with tempfile.TemporaryDirectory() as root:
            for cpu, siblings, l3 in ((0, "0,2", "0-3"), (1, "1,3", "0-3"), (4, "4", "4-7")):
                base = Path(root, f"cpu{cpu}")
                (base / "topology").mkdir(parents=True)
                (base / "topology/thread_siblings_list").write_text(siblings + "\n")
                (base / "cache/index3").mkdir(parents=True)
                (base / "cache/index3/shared_cpu_list").write_text(l3 + "\n")
            self.assertEqual(core_warnings([0, 2], root), ["cores 0 and 2 are SMT siblings"])
            self.assertEqual(core_warnings([0, 1], root), ["cores 0 and 1 share an L3 cache"])
            self.assertEqual(core_warnings([0, 4], root), [])


if __name__ == "__main__":
    unittest.main()
