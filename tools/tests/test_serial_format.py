"""Serial format parsing: the current TEST/CFG/SUMMARY layout, quoted values,
runtime selection and the older BEGIN/RUNTIME/FILTER layout."""
import unittest

from perf_report import runtime_breakdown, summarize
from qemu_smoke import parse_log, unquote

HEAD = ("CPU cpuid=1 vendor=\"GenuineIntel\" family=6\n"
        "CONTROL protocol=cpu1 ready=1\n"
        "CONTROL protocol=sel1 ready=1\n")
NEW = HEAD + (
    "CFG rounds=1 heavy_scale=8 seed=86b04a21 test_seconds=1 protocol=work1 benchmarks=1"
    " suite=full timing_scope=checked_workload ms_resolution=10\n"
    "CONTROL protocol=sel1 accepted=1 only=2 skip=1 from=int_alu\n"
    "WORK phase=warmup r=1 id=1 name=alpha\n"
    "FAILDETAIL test=alpha line=7 check=\"a \\\"b\\\" c\\\\d\" got=00000001 want=00000002\n"
    "WORK phase=sample r=1 id=1 name=alpha\n"
    "WORK phase=end r=1 id=1 name=alpha\n"
    "TEST r=1 name=alpha status=FAIL ms=20 us=20123 cyc=100 unit=tsc ops=1 sum=00000000"
    " iterations=1 invocations=2 warmup_ms=10 id=1 iteration_ops=1 mode=accuracy"
    " op_unit=operation ops_constant=1 nfail=1 first=\"a \\\"b\\\" c\\\\d\" line=7"
    " failure_iteration=0 failure_phase=warmup got=00000001 want=00000002 total_ms=40"
    " overhead_ms=10 harness_cyc=500 harness_full=1 exc=3 excv=13:2,14:1 irq0=4"
    " tags=paging,exc fn=00101000\n"
    "TEST r=1 name=beta status=SKIP reason=missing_fpu id=2 req=00000004 mode=benchmark"
    " total_ms=0 overhead_ms=0 tags=x87 fn=00102000\n"
    "FILTER suite_selection=0 build_selection=3 runtime_selection=5\n"
    "SUMMARY rounds=1 tests=2 pass=0 fail=1 skip=1 fault=0 ms=50 warmup_ms=10"
    " sample_ms=20 overhead_ms=10 suite_overhead_ms=10\nDONE\n")
OLD = HEAD + (
    "CFG rounds=1 heavy_scale=8 seed=86b04a21 test_seconds=1 protocol=work1 benchmarks=1"
    " suite=full\n"
    "FILTER name=gamma reason=build_selection\n"
    "BEGIN r=1 id=1 name=alpha\n"
    "FAILDETAIL test=alpha line=7 check=a b got=00000001 want=00000002\n"
    "TEST r=1 name=alpha status=FAIL ms=20 cyc=100 unit=tsc ops=1 sum=00000000"
    " iterations=1 warmup_ms=10 id=1 iteration_ops=1 mode=accuracy op_unit=operation"
    " timing_scope=checked_workload ops_constant=1 ms_resolution=10 nfail=1 first=\"a b\""
    " line=7 failure_iteration=0 failure_phase=warmup seed=86b04a21\n"
    "RUNTIME scope=test r=1 id=1 name=alpha total_ms=40 warmup_ms=10 sample_ms=20"
    " overhead_ms=10 resolution_ms=10\n"
    "SUMMARY rounds=1 tests=1 pass=0 fail=1 skip=0 fault=0 ms=50\n"
    "RUNTIME scope=suite total_ms=50 warmup_ms=10 sample_ms=20 overhead_ms=10"
    " suite_overhead_ms=10 resolution_ms=10\nDONE\n")


def finished(run):
    run.update(done=True, cpu="host", machine="pc", accel="tcg", memory_mib=32)
    return run


class SerialFormatTests(unittest.TestCase):
    def test_current_format_fields(self):
        run = parse_log(NEW)
        alpha, beta = run["tests"]
        self.assertEqual(alpha["first"], '"a \\"b\\" c\\\\d"')
        self.assertEqual(unquote(alpha["first"]), 'a "b" c\\d')
        self.assertEqual(alpha["line"], "7")
        self.assertEqual(alpha["tags"], "paging,exc")
        self.assertEqual(alpha["excv"], "13:2,14:1")
        self.assertEqual(alpha["failure_details"][0].split()[1], "test=alpha")
        # CFG constants come back on executed records only; seed on failures.
        self.assertEqual((alpha["timing_scope"], alpha["ms_resolution"], alpha["seed"]),
                         ("checked_workload", "10", "86b04a21"))
        self.assertNotIn("timing_scope", beta)
        self.assertNotIn("seed", beta)
        self.assertNotIn("timing_scope", run["config"])
        self.assertEqual(run["filtered_tests"], [])
        self.assertEqual(run["filter_summary"], {"suite_selection": "0",
                                                 "build_selection": "3",
                                                 "runtime_selection": "5"})
        self.assertEqual(run["selection"], {"protocol": "sel1", "accepted": "1",
                                            "only": "2", "skip": "1", "from": "int_alu"})

    def test_runtime_records_rebuilt_from_test_and_summary(self):
        run = finished(parse_log(NEW))
        breakdown = runtime_breakdown(run)
        self.assertEqual(breakdown["suite"]["total_ms"], 50)
        self.assertEqual(breakdown["suite"]["suite_overhead_ms"], 10)
        self.assertEqual({r["name"]: r["total_ms"] for r in breakdown["tests"]},
                         {"alpha": 40, "beta": 0})
        self.assertEqual(summarize([run], 0)["guest_runtime"][0], breakdown)

    def test_old_format_still_parses_the_same_way(self):
        old, new = parse_log(OLD), parse_log(NEW)
        self.assertEqual(old["config"], new["config"])
        self.assertEqual(old["filtered_tests"], [{"name": "gamma", "reason": "build_selection"}])
        self.assertIsNone(old["filter_summary"])
        self.assertIsNone(old["selection"])
        self.assertEqual(old["tests"][0]["failure_details"],
                         ["FAILDETAIL test=alpha line=7 check=a b got=00000001 want=00000002"])
        for key in ("timing_scope", "ms_resolution", "seed"):
            self.assertEqual(old["tests"][0][key], new["tests"][0][key])
        self.assertEqual(runtime_breakdown(finished(old))["suite"]["total_ms"], 50)

    def test_rejected_or_absent_selection(self):
        self.assertIsNone(parse_log(HEAD + "FATAL invalid selection control frame\n")["selection"])


if __name__ == "__main__":
    unittest.main()
