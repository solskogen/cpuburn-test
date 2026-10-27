#!/usr/bin/env python3
"""Independently verify the raw x87 reference tables in tests_fp_edges.c."""

from fractions import Fraction
from pathlib import Path
import re


SOURCE = Path(__file__).resolve().parents[1] / "tests_fp_edges.c"
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
    positive_q_checks = 0
    negative_q_unasserted = 0
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
        # Intel specifies Q2/Q1/Q0 but leaves negative-integer bit encoding
        # unstated in the published operation pseudocode. The runtime table
        # currently asserts the magnitude convention (abs(Q) modulo 8).
        negative_trunc_q = trunc_q < 0
        negative_nearest_q = nearest_q < 0
        if negative_trunc_q:
            negative_q_unasserted += 1
            if qtrunc != 0:
                raise AssertionError(
                    f"FPREM vector {match.group(1)!r}: negative-Q field must remain an inert placeholder"
                )
        elif qtrunc != trunc_q % 8:
            raise AssertionError(f"FPREM vector {match.group(1)!r}: wrong positive quotient bits")
        else:
            positive_q_checks += 1
        if negative_nearest_q:
            negative_q_unasserted += 1
            if qnearest != 0:
                raise AssertionError(
                    f"FPREM1 vector {match.group(1)!r}: negative-Q field must remain an inert placeholder"
                )
        elif qnearest != nearest_q % 8:
            raise AssertionError(f"FPREM1 vector {match.group(1)!r}: wrong positive quotient bits")
        else:
            positive_q_checks += 1
    print(
        "FPREM/FPREM1: verified 11 exact remainder vectors, "
        f"{positive_q_checks} positive/zero quotient-bit references; "
        f"left {negative_q_unasserted} negative-quotient cases unasserted"
    )


def main():
    source = SOURCE.read_text()
    check_divisions(source)
    check_remainders(source)
    print(f"Independent exact-rational table check passed: {SOURCE.relative_to(SOURCE.parents[1])}")


if __name__ == "__main__":
    main()
