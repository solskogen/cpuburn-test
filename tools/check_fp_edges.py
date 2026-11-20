#!/usr/bin/env python3
"""Independently verify the raw reference constants of the FPU/MMX tests.

tests_fp_edges.c: FDIV/FPREM tables against exact rationals.
tests_fpu.c: the backend-probe constants, and the fpu_mac, fpu_mandel,
fpu_nbody and fpu_trig known answers.
tests_mmx.c / tests_vendor.c: the register-resident kernel checksums,
by simulating the exact block strings compiled into the guest.
"""

from decimal import Decimal, getcontext
from fractions import Fraction
from math import isqrt
from pathlib import Path
import re
import struct


SRC = Path(__file__).resolve().parents[1] / "src"
SOURCE = SRC / "tests_fp_edges.c"
E_RE = re.compile(
    r"E\(\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\)"
)


def decode(raw):
    lo, hi, se = raw
    sig = (hi << 32) | lo
    exp = se & 0x7FFF
    value = Fraction(sig)
    value *= Fraction(2) ** ((exp - 16383 - 63) if exp else (1 - 16383 - 63))
    return -value if se & 0x8000 else value


def floor_log2(value):
    exp = value.numerator.bit_length() - value.denominator.bit_length()
    if value < Fraction(2) ** exp:
        exp -= 1
    return exp


def encode_ext(value, negative_zero=False):
    if not value:
        return (0, 0, 0x8000 if negative_zero else 0)
    negative = value < 0
    value = abs(value)
    exp = floor_log2(value)
    scaled = value / (Fraction(2) ** (exp - 63))
    significand, remainder = divmod(scaled.numerator, scaled.denominator)
    if remainder:
        raise ValueError("rounded result was not an exact integer significand")
    if significand == 1 << 64:
        significand >>= 1
        exp += 1
    biased = exp + 16383
    if not 0 < biased < 0x7FFF or not (1 << 63) <= significand < (1 << 64):
        raise ValueError("expected table result is outside normal extended range")
    return (significand & 0xFFFFFFFF, significand >> 32,
            biased | (0x8000 if negative else 0))


def round_ext(value, mode):
    if not value:
        return (0, 0, 0)
    negative = value < 0
    magnitude = abs(value)
    exp = floor_log2(magnitude)
    scaled = magnitude / (Fraction(2) ** (exp - 63))
    significand, remainder = divmod(scaled.numerator, scaled.denominator)
    if remainder:
        if mode == 0:
            twice = remainder * 2
            if twice > scaled.denominator or (
                twice == scaled.denominator and significand & 1
            ):
                significand += 1
        elif mode == 1 and negative:
            significand += 1
        elif mode == 2 and not negative:
            significand += 1
    if significand == 1 << 64:
        significand >>= 1
        exp += 1
    biased = exp + 16383
    return (significand & 0xFFFFFFFF, significand >> 32,
            biased | (0x8000 if negative else 0))


def parse_exts(block):
    return [tuple(int(word, 16) for word in match)
            for match in E_RE.findall(block)]


def quotient_nearest_even(value):
    negative = value < 0
    magnitude = abs(value)
    q, r = divmod(magnitude.numerator, magnitude.denominator)
    twice = 2 * r
    if twice > magnitude.denominator or (twice == magnitude.denominator and q & 1):
        q += 1
    return -q if negative else q


def check_divisions(source):
    start = source.index("static const div_vector div_vectors[] = {")
    end = source.index("\n};\n\ntypedef struct", start)
    entries = parse_exts(source[start:end])
    if len(entries) != 15 * 6:
        raise AssertionError(f"FDIV table has {len(entries) // 6} vectors, expected 15")
    for index in range(15):
        numerator, denominator, *expected = entries[index * 6:(index + 1) * 6]
        exact = decode(numerator) / decode(denominator)
        for mode in range(4):
            got = round_ext(exact, mode)
            if got != expected[mode]:
                raise AssertionError(
                    f"FDIV vector {index}, RC={mode}: "
                    f"expected {expected[mode]!r}, exact rational gives {got!r}"
                )
    print("FDIV: verified 15 raw vectors × all 4 x87 rounding controls (60 results)")


def check_remainders(source):
    start = source.index("static const rem_vector rem_vectors[] = {")
    end = source.index("\n};\n\n#undef E", start)
    block = source[start:end]
    starts = list(re.finditer(r'\{\s*"([^"]+)"', block))
    if len(starts) != 11:
        raise AssertionError(f"FPREM table has {len(starts)} vectors, expected 11")
    negative_q_checks = 0
    for index, match in enumerate(starts):
        stop = starts[index + 1].start() if index + 1 < len(starts) else len(block)
        entry = block[match.start():stop]
        fields = parse_exts(entry)
        qfields = re.search(r",\s*(\d+)\s*,\s*(\d+)\s*\},?\s*$", entry)
        if len(fields) != 4 or not qfields:
            raise AssertionError(f"could not parse FPREM vector {match.group(1)!r}")
        dividend, divisor, trunc_result, nearest_result = fields
        qtrunc, qnearest = map(int, qfields.groups())
        ratio = decode(dividend) / decode(divisor)

        trunc_q = abs(ratio.numerator) // ratio.denominator
        if ratio < 0:
            trunc_q = -trunc_q
        trunc_rem = decode(dividend) - trunc_q * decode(divisor)
        nearest_q = quotient_nearest_even(ratio)
        nearest_rem = decode(dividend) - nearest_q * decode(divisor)

        dividend_negative = bool(dividend[2] & 0x8000)
        if encode_ext(trunc_rem, dividend_negative) != trunc_result:
            raise AssertionError(f"FPREM vector {match.group(1)!r}: wrong exact remainder")
        if encode_ext(nearest_rem, dividend_negative) != nearest_result:
            raise AssertionError(f"FPREM1 vector {match.group(1)!r}: wrong exact remainder")
        # C0/C3/C1 hold the low three bits of |Q|: the remainder steps work
        # on magnitudes, so operand signs only reach the remainder's sign.
        if qtrunc != abs(trunc_q) % 8:
            raise AssertionError(f"FPREM vector {match.group(1)!r}: wrong quotient bits")
        if qnearest != abs(nearest_q) % 8:
            raise AssertionError(f"FPREM1 vector {match.group(1)!r}: wrong quotient bits")
        negative_q_checks += (trunc_q < 0) + (nearest_q < 0)
    print(
        "FPREM/FPREM1: verified 11 exact remainder vectors and 22 quotient-bit "
        f"fields (|Q| mod 8; {negative_q_checks} with a negative quotient)"
    )


def csum_mix(s, v):
    s ^= v & 0xFFFFFFFF
    s = ((s << 5 | s >> 27) & 0xFFFFFFFF) * 0x9E3779B1 & 0xFFFFFFFF
    return s


def c_function(source, name):
    start = source.index(f"static u32 {name}(void)")
    return source[start:source.index("\n}\n", start)]


def c_hex(pattern, text):
    match = re.search(pattern, text)
    if not match:
        raise AssertionError(f"could not find {pattern!r}")
    return int(match.group(1), 16)


def ext_words(source, name):
    match = re.search(name + r"\s*=\s*\{\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}\}", source)
    if not match:
        raise AssertionError(f"could not find ext80 {name}")
    return tuple(int(word, 16) for word in match.groups())


# Fixed-point helpers for the transcendental references (P fraction bits).
P = 320
ONE = 1 << P


def fx_atan_inv(n):
    total, term, k, sign = 0, ONE // n, 1, 1
    while term:
        total += sign * (term // k)
        term //= n * n
        k += 2
        sign = -sign
    return total


def fx_mul(a, b):
    return a * b >> P


def fx_sin_cos(x):
    s, t, k = 0, x, 1
    while t:
        s += t
        t = -fx_mul(fx_mul(t, x), x) // ((k + 1) * (k + 2))
        k += 2
    c, t, k = 0, ONE, 0
    while t:
        c += t
        t = -fx_mul(fx_mul(t, x), x) // ((k + 1) * (k + 2))
        k += 2
    return s, c


def fx_sqrt(x):
    return isqrt(x << P)


def fx_atan(x):
    halvings = 0
    while x > ONE // 8:
        x = (x << P) // (ONE + fx_sqrt(ONE + fx_mul(x, x)))
        halvings += 1
    total, term, k, x2 = 0, x, 1, fx_mul(x, x)
    while term:
        total += term // k
        term = -fx_mul(term, x2)
        k += 2
    return total << halvings


def fx_round_ext(v):
    negative, v = v < 0, abs(v)
    exp = v.bit_length() - 1 - P
    shift = v.bit_length() - 64
    sig, rem = v >> shift, v & ((1 << shift) - 1)
    half = 1 << (shift - 1)
    if rem > half or (rem == half and sig & 1):
        sig += 1
    if sig == 1 << 64:
        sig >>= 1
        exp += 1
    return (sig & 0xFFFFFFFF, sig >> 32, (exp + 16383) | (0x8000 if negative else 0))


def check_probe(source):
    third = ext_words(source, "probe_third64")
    if round_ext(Fraction(1, 3), 0) != third:
        raise AssertionError("probe_third64 is not 1/3 rounded to 64 bits")
    arg = c_hex(r"probe_sin_arg\s*=\s*(0x[0-9a-fA-F]+)ll", source)
    if float(arg) != arg:
        raise AssertionError("probe_sin_arg is not exact as a double")
    pi = 16 * fx_atan_inv(5) - 4 * fx_atan_inv(239)
    # The x87 (and softfloat without BETTER_THAN_PENTIUM) reduces with a
    # 66-bit pi: c90fdaa2 2168c234 c.
    pi66 = (0xC90FDAA22168C234 << 2 | 3) << (P - 64)

    def sin_reduced(pi_value):
        quotient, rem = divmod(arg << P, pi_value)
        s, _ = fx_sin_cos(rem if rem <= pi_value // 2 else pi_value - rem)
        return -s if quotient & 1 else s

    for name, pi_value in (("probe_sin_x87", pi66), ("probe_sin_exact", pi)):
        if fx_round_ext(sin_reduced(pi_value)) != ext_words(source, name):
            raise AssertionError(f"{name} does not match the reference")
    print(f"probe: 1/3 and FSIN({arg:#x}) under 66-bit and exact pi verified")


def check_fpu_workloads(source):
    mac = 0
    for i in range(24):
        for j in range(24):
            mac = csum_mix(mac, sum(((i + k) % 7 - 3) * ((k * 3 + j) % 5 - 2) for k in range(24)))
    if c_hex(r"CHECK_EQ\(sum,(0x[0-9a-fA-F]+)u\)", c_function(source, "fpu_mac")) != mac:
        raise AssertionError("fpu_mac checksum constant is wrong")

    def low(x):
        return struct.unpack("<Q", struct.pack("<d", x))[0] & 0xFFFFFFFF

    mandel, escaped, iterations = 0, 0, 0
    for py in range(32):
        for px in range(48):
            cr, ci, x, y, k = (px - 32) / 16, (py - 16) / 16, 0.0, 0.0, 0
            while k < 64:
                xx, yy = x * x, y * y
                if xx + yy > 4.0:
                    break
                x, y = (xx - yy) + cr, (x * y) * 2 + ci
                k += 1
            escaped += k < 64
            iterations += k
            mandel = csum_mix(csum_mix(mandel, k), low(x) ^ low(y))
    body = c_function(source, "fpu_mandel")
    want = (int(re.search(r"CHECK_EQ\(escaped,(\d+)u\)", body).group(1)),
            int(re.search(r"CHECK_EQ\(iterations,(\d+)u\)", body).group(1)),
            c_hex(r"CHECK_EQ\(sum,(0x[0-9a-fA-F]+)u\)", body))
    if want != (escaped, iterations, mandel):
        raise AssertionError(f"fpu_mandel constants {want} != {(escaped, iterations, mandel)}")

    def nbody(num, sqrt):
        one = num(1)
        x = [num(i - 4) / num(8) for i in range(8)]
        v = [num(0)] * 8
        dt = one / num(1024)
        for _ in range(128):
            force = []
            for i in range(8):
                f = num(0)
                for j in range(8):
                    dx = x[j] - x[i]
                    r2 = dx * dx + one
                    f = f + dx / (r2 * sqrt(r2))
                force.append(f)
            for i in range(8):
                v[i] = v[i] + force[i] * dt
                x[i] = x[i] + v[i] * dt
        return x

    def bits(x):
        return struct.unpack("<Q", struct.pack("<d", x))[0]

    getcontext().prec = 60
    exact = [bits(float(x)) for x in nbody(Decimal, Decimal.sqrt)]
    ieee = [bits(x) for x in nbody(float, lambda a: a ** 0.5)]
    body = c_function(source, "fpu_nbody")
    table = [int(w, 16) for w in re.findall(r"(0x[0-9a-fA-F]+)ull", body.split("want[8]=")[1].split("};")[0])]
    tolerance = int(re.search(r"double_near\(x\[i\],want\[i\],(\d+)\)", body).group(1))
    if table != exact:
        raise AssertionError("fpu_nbody reference is not the 60-digit result rounded to double")
    worst = max(abs(a - b) for a, b in zip(ieee, exact))
    if worst > tolerance:
        raise AssertionError(f"fpu_nbody IEEE double result is {worst} ulps away")

    total = 0
    for i in range(1, 1025):
        x = (i << P) // 1024
        s, c = fx_sin_cos(x)
        total += fx_mul(s, c) + fx_atan(x) + fx_sqrt(x)
    if fx_round_ext(total) != ext_words(source, "want"):
        raise AssertionError("fpu_trig reference sum is wrong")
    print(f"FPU workloads: fpu_mac/fpu_mandel checksums, fpu_nbody (IEEE {worst} ulps, "
          f"limit {tolerance}) and fpu_trig sum verified")


def asm_lines(source, macro):
    start = source.index(f"#define {macro} \\")
    lines = []
    for line in source[start:].splitlines()[1:]:
        lines.extend(re.findall(r'"([^"]*)"', line))
        if not line.rstrip().endswith("\\"):
            break
    text = "".join(lines).replace("%%", "%").replace("\\n\\t", ";")
    return [op.strip() for op in text.split(";") if op.strip()]


M64 = (1 << 64) - 1


def lanes(v, w):
    return [(v >> (i * w)) & ((1 << w) - 1) for i in range(64 // w)]


def join(values, w):
    return sum((x & ((1 << w) - 1)) << (i * w) for i, x in enumerate(values))


def sx(x, w):
    return x - (1 << w) if x >> (w - 1) else x


def sat(v, w, unsigned):
    lo, hi = (0, (1 << w) - 1) if unsigned else (-(1 << (w - 1)), (1 << (w - 1)) - 1)
    return min(max(v, lo), hi)


def mmx_op(op, d, s):
    sizes = {"b": 8, "w": 16, "d": 32, "q": 64}
    if op == "movq":
        return s
    if op in ("pand", "pandn", "por", "pxor"):
        return {"pand": d & s, "pandn": ~d & s & M64, "por": d | s, "pxor": d ^ s}[op]
    match = re.fullmatch(r"p(add|sub)(s|us)?([bwd])", op)
    if match:
        kind, saturate, w = match.group(1), match.group(2), sizes[match.group(3)]
        out = []
        for a, b in zip(lanes(d, w), lanes(s, w)):
            if saturate == "s":
                a, b = sx(a, w), sx(b, w)
            r = a + b if kind == "add" else a - b
            out.append(sat(r, w, saturate == "us") if saturate else r)
        return join(out, w)
    match = re.fullmatch(r"pcmp(eq|gt)([bwd])", op)
    if match:
        w = sizes[match.group(2)]
        test = (lambda a, b: a == b) if match.group(1) == "eq" else (lambda a, b: sx(a, w) > sx(b, w))
        return join([-1 if test(a, b) else 0 for a, b in zip(lanes(d, w), lanes(s, w))], w)
    if op in ("pmullw", "pmulhw"):
        shift = 0 if op == "pmullw" else 16
        return join([(sx(a, 16) * sx(b, 16)) >> shift for a, b in zip(lanes(d, 16), lanes(s, 16))], 16)
    if op == "pmaddwd":
        a, b = [sx(x, 16) for x in lanes(d, 16)], [sx(x, 16) for x in lanes(s, 16)]
        return join([a[0] * b[0] + a[1] * b[1], a[2] * b[2] + a[3] * b[3]], 32)
    match = re.fullmatch(r"pack(ss|us)(wb|dw)", op)
    if match:
        w = 16 if match.group(2) == "wb" else 32
        return join([sat(sx(x, w), w // 2, match.group(1) == "us") for x in lanes(d, w) + lanes(s, w)], w // 2)
    match = re.fullmatch(r"punpck([lh])(bw|wd|dq)", op)
    if match:
        w = {"bw": 8, "wd": 16, "dq": 32}[match.group(2)]
        n = 32 // w
        part = slice(0, n) if match.group(1) == "l" else slice(n, 2 * n)
        return join([x for pair in zip(lanes(d, w)[part], lanes(s, w)[part]) for x in pair], w)
    raise AssertionError(f"unsupported MMX op {op}")


def mmx_shift(op, d, count):
    kind, w = op[2:4], {"w": 16, "d": 32, "q": 64}[op[4]]
    if kind == "ll":
        return join([x << count if count < w else 0 for x in lanes(d, w)], w)
    if kind == "rl":
        return join([x >> count if count < w else 0 for x in lanes(d, w)], w)
    return join([sx(x, w) >> min(count, w - 1) for x in lanes(d, w)], w)


def run_block(ops, regs, step):
    for line in ops:
        op, args = line.split(None, 1)
        src, dst = [a.strip() for a in args.split(",")]
        d = int(dst[3:])
        if src.startswith("$"):
            regs[d] = mmx_shift(op, regs[d], int(src[1:], 0))
        else:
            regs[d] = step(op, regs[d], regs[int(src[3:])]) & M64


def register_checksum(regs):
    total = 0
    for r in regs:
        total = csum_mix(csum_mix(total, r & 0xFFFFFFFF), r >> 32)
    return total


def u64_table(source, name):
    block = source[source.index(name):]
    block = block[:block.index("};")]
    values = re.findall(r"\b(0x[0-9a-fA-F]+|0)(?:ull)?\b", block.split("=", 1)[1])
    if len(values) != 8:
        raise AssertionError(f"{name}: expected 8 entries")
    return [int(v, 16) if v.startswith("0x") else 0 for v in values]


def check_mmx_kernels():
    source = (SRC / "tests_mmx.c").read_text()
    init = u64_table(source, "mmx_kernel_init[8]")
    iterations = int(re.search(r"#define MMX_KERNEL_ITERS (\d+)", source).group(1))
    kernels = re.findall(r"^MMX_KERNEL\((\w+),\s*(\w+),\s*(\w+)\)", source, re.M)
    wants = dict(re.findall(r"mmx_kernel_run\((\w+),\s*(0x[0-9a-fA-F]+)u\)", source))
    if len(kernels) != 3 or set(wants) != {k[0] for k in kernels}:
        raise AssertionError("could not match MMX kernels to their checksums")
    for body, first, second in kernels:
        a, b = asm_lines(source, first), asm_lines(source, second)
        ops = a + b + a + b
        if len(ops) != 64:
            raise AssertionError(f"{body}: {len(ops)} ops per iteration, expected 64")
        regs = list(init)
        for _ in range(iterations):
            run_block(ops, regs, mmx_op)
        if register_checksum(regs) != int(wants[body], 16):
            raise AssertionError(f"{body}: checksum {register_checksum(regs):#x} != {wants[body]}")

    source = (SRC / "tests_vendor.c").read_text()
    words = u64_table(source, "now_kernel_init[8]")

    def to_float(bits32):
        return Fraction(struct.unpack("<f", struct.pack("<I", bits32))[0])

    def to_bits(value):
        if value == 0:
            return 0
        bits32 = struct.unpack("<I", struct.pack("<f", float(value)))[0]
        if to_float(bits32) != value or not 0 < (bits32 >> 23) & 0xFF < 0xFF:
            raise AssertionError(f"3DNow! kernel value {value} is not an exact normal float32")
        return bits32

    regs = [[to_float(w & 0xFFFFFFFF), to_float(w >> 32)] for w in words]
    ops = asm_lines(source, "NOW_KERNEL_A") * 4
    iterations = int(re.search(r"#define NOW_KERNEL_ITERS (\d+)", source).group(1))
    for _ in range(iterations):
        for line in ops:
            op, args = line.split(None, 1)
            s, d = [int(a.strip()[3:]) for a in args.split(",")]
            a, b = regs[d], regs[s]
            regs[d] = {"movq": lambda: list(b),
                       "pfadd": lambda: [x + y for x, y in zip(a, b)],
                       "pfsub": lambda: [x - y for x, y in zip(a, b)],
                       "pfsubr": lambda: [y - x for x, y in zip(a, b)],
                       "pfmul": lambda: [x * y for x, y in zip(a, b)]}[op]()
            for value in regs[d]:
                to_bits(value)
    total = 0
    for lo, hi in regs:
        total = csum_mix(csum_mix(total, to_bits(lo)), to_bits(hi))
    want = c_hex(r"CHECK_EQ\(sum, (0x[0-9a-fA-F]+)u\)", c_function(source, "now_kernel"))
    if total != want:
        raise AssertionError(f"3dnow_kernel checksum {total:#x} != {want:#x}")
    print(f"MMX/3DNow! kernels: {len(kernels)} MMX checksums and the exact-float 3DNow! checksum verified")


def main():
    source = SOURCE.read_text()
    check_divisions(source)
    check_remainders(source)
    fpu = (SRC / "tests_fpu.c").read_text()
    check_probe(fpu)
    check_fpu_workloads(fpu)
    check_mmx_kernels()
    print(f"Independent reference check passed: {SRC.relative_to(SRC.parents[1])}")


if __name__ == "__main__":
    main()
