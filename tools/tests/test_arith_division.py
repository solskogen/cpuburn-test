"""Host check of the guest's 64-bit division helpers (src/arith.c) against
Python integers: the 32-bit divisor fast path and the general bit loop."""
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RENAMES = ("__udivmoddi4", "__udivdi3", "__umoddi3", "__divdi3", "__moddi3",
           "memset", "memcpy", "memmove", "memcmp")
DRIVER = r"""
#include <stdio.h>
typedef unsigned long long u64;
typedef long long s64;
u64 cb___udivmoddi4(u64, u64, u64 *);
s64 cb___divdi3(s64, s64);
s64 cb___moddi3(s64, s64);
int main(void) {
    u64 n, d, r;
    while (scanf("%llx %llx", &n, &d) == 2) {
        u64 q = cb___udivmoddi4(n, d, &r);
        printf("%llx %llx %llx %llx\n", q, r,
               (u64)cb___divdi3((s64)n, (s64)d), (u64)cb___moddi3((s64)n, (s64)d));
    }
    return 0;
}
"""
MASK = (1 << 64) - 1


def signed(x):
    return x - (1 << 64) if x >> 63 else x


def reference(n, d):
    sn, sd = signed(n), signed(d)
    q = abs(sn) // abs(sd)
    q = -q if (sn < 0) != (sd < 0) else q
    r = sn - q * sd
    return n // d, n % d, q & MASK, r & MASK


def cases():
    rng = random.Random(1)
    edge = [1, 2, 3, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
            0x100000000, 0x100000001, 0xFFFFFFFFFFFF, 1 << 63, MASK, MASK - 1]
    pairs = [(n, d) for n in [0, *edge] for d in edge]
    for _ in range(4000):
        n = rng.getrandbits(rng.choice((16, 32, 33, 48, 63, 64)))
        d = rng.getrandbits(rng.choice((1, 8, 16, 31, 32, 33, 48, 64))) or 1
        pairs.append((n, d))
    return pairs


class ArithDivisionTests(unittest.TestCase):
    def test_division_matches_reference(self):
        cc = shutil.which(os.environ.get("CC", "gcc")) or shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build) as work:
            work = Path(work)
            (work / "driver.c").write_text(DRIVER)
            renames = [f"-D{name}=cb_{name}" for name in RENAMES]
            exe = None
            for arch in (["-m32"], []):
                out = work / f"div{len(arch)}"
                cmd = [cc, *arch, "-O2", "-fno-builtin", "-I", str(ROOT / "src"), *renames,
                       "-o", str(out), str(ROOT / "src" / "arith.c"), str(work / "driver.c")]
                if subprocess.run(cmd, capture_output=True).returncode == 0:
                    exe = out
                    break
            if exe is None:
                self.skipTest("cannot build src/arith.c for the host")
            pairs = cases()
            text = "".join(f"{n:x} {d:x}\n" for n, d in pairs)
            result = subprocess.run([str(exe)], input=text, capture_output=True,
                                    text=True, check=True).stdout.split("\n")
        for (n, d), line in zip(pairs, result):
            with self.subTest(n=hex(n), d=hex(d)):
                self.assertEqual(tuple(int(x, 16) for x in line.split()), reference(n, d))
        self.assertGreaterEqual(len(result), len(pairs))


if __name__ == "__main__":
    unittest.main()
