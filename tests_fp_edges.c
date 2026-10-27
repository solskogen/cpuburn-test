#include "kernel.h"

typedef struct {
    u32 lo, hi;
    u16 se;
} ext80;

#define E(lo_, hi_, se_) { (lo_), (hi_), (se_) }

typedef struct {
    ext80 numerator, denominator;
    ext80 rounded[4];
} div_vector;

/* Raw results are rounded from exact integer ratios to 64-bit x87 precision. */
static const div_vector div_vectors[] = {
    /* Pentium FDIV erratum's notorious exact-integer operands. */
    { E(0x00000000,0x800bf600,0x4015), E(0x00000000,0xbffffc00,0x4014),
      { E(0xe35a14bd,0xaabaa0e3,0x3fff), E(0xe35a14bc,0xaabaa0e3,0x3fff),
        E(0xe35a14bd,0xaabaa0e3,0x3fff), E(0xe35a14bc,0xaabaa0e3,0x3fff) } },
    { E(0x00000000,0x800bf400,0x4015), E(0x00000000,0xbffffc00,0x4014),
      { E(0x38a1312e,0xaaba9e39,0x3fff), E(0x38a1312e,0xaaba9e39,0x3fff),
        E(0x38a1312f,0xaaba9e39,0x3fff), E(0x38a1312e,0xaaba9e39,0x3fff) } },
    { E(0x00000000,0x800bf800,0x4015), E(0x00000000,0xbffffc00,0x4014),
      { E(0x8e12f84b,0xaabaa38e,0x3fff), E(0x8e12f84b,0xaabaa38e,0x3fff),
        E(0x8e12f84c,0xaabaa38e,0x3fff), E(0x8e12f84b,0xaabaa38e,0x3fff) } },
    { E(0x00000000,0x800bf600,0x4015), E(0x00000000,0xbffff800,0x4014),
      { E(0x7184c4bb,0xaabaa472,0x3fff), E(0x7184c4ba,0xaabaa472,0x3fff),
        E(0x7184c4bb,0xaabaa472,0x3fff), E(0x7184c4ba,0xaabaa472,0x3fff) } },
    { E(0x00000000,0x800bf600,0x4015), E(0x00000000,0xc0000000,0x4014),
      { E(0x55555555,0xaaba9d55,0x3fff), E(0x55555555,0xaaba9d55,0x3fff),
        E(0x55555556,0xaaba9d55,0x3fff), E(0x55555555,0xaaba9d55,0x3fff) } },
    { E(0x00000000,0xbffffc00,0x4014), E(0x00000000,0x800bf600,0x4015),
      { E(0x68bbba95,0xbfee0cad,0x3ffe), E(0x68bbba95,0xbfee0cad,0x3ffe),
        E(0x68bbba96,0xbfee0cad,0x3ffe), E(0x68bbba95,0xbfee0cad,0x3ffe) } },
    { E(0x00000000,0xeb79a2a0,0x4019), E(0x00000000,0xbc614ee0,0x4019),
      { E(0x6f91c578,0xa0000005,0x3fff), E(0x6f91c578,0xa0000005,0x3fff),
        E(0x6f91c579,0xa0000005,0x3fff), E(0x6f91c578,0xa0000005,0x3fff) } },
    { E(0x00000000,0xfffffffe,0x401d), E(0x00000000,0xfffffffc,0x401d),
      { E(0x00000004,0x80000001,0x3fff), E(0x00000004,0x80000001,0x3fff),
        E(0x00000005,0x80000001,0x3fff), E(0x00000004,0x80000001,0x3fff) } },
    { E(0x00000000,0x91a2b380,0x4017), E(0x00000000,0xeca86420,0x4019),
      { E(0xfb74a3a4,0x9d89d852,0x3ffc), E(0xfb74a3a4,0x9d89d852,0x3ffc),
        E(0xfb74a3a5,0x9d89d852,0x3ffc), E(0xfb74a3a4,0x9d89d852,0x3ffc) } },
    { E(0x00000000,0x8c000000,0x4004), E(0x00000000,0x80000000,0x4002),
      { E(0x00000000,0x8c000000,0x4001), E(0x00000000,0x8c000000,0x4001),
        E(0x00000000,0x8c000000,0x4001), E(0x00000000,0x8c000000,0x4001) } },
    { E(0x00000000,0x800bf600,0xc015), E(0x00000000,0xbffffc00,0x4014),
      { E(0xe35a14bd,0xaabaa0e3,0xbfff), E(0xe35a14bd,0xaabaa0e3,0xbfff),
        E(0xe35a14bc,0xaabaa0e3,0xbfff), E(0xe35a14bc,0xaabaa0e3,0xbfff) } },
    { E(0x00000000,0x800bf600,0x4015), E(0x00000000,0xbffffc00,0xc014),
      { E(0xe35a14bd,0xaabaa0e3,0xbfff), E(0xe35a14bd,0xaabaa0e3,0xbfff),
        E(0xe35a14bc,0xaabaa0e3,0xbfff), E(0xe35a14bc,0xaabaa0e3,0xbfff) } },
    { E(0x00000000,0x800bf600,0xc015), E(0x00000000,0xbffffc00,0xc014),
      { E(0xe35a14bd,0xaabaa0e3,0x3fff), E(0xe35a14bc,0xaabaa0e3,0x3fff),
        E(0xe35a14bd,0xaabaa0e3,0x3fff), E(0xe35a14bc,0xaabaa0e3,0x3fff) } },
    /* Same rational quotient at widely separated, still-normal exponents. */
    { E(0x00000000,0x800bf600,0x7aad), E(0x00000000,0xbffffc00,0x7aac),
      { E(0xe35a14bd,0xaabaa0e3,0x3fff), E(0xe35a14bc,0xaabaa0e3,0x3fff),
        E(0xe35a14bd,0xaabaa0e3,0x3fff), E(0xe35a14bc,0xaabaa0e3,0x3fff) } },
    { E(0x00000000,0x800bf600,0x057d), E(0x00000000,0xbffffc00,0x057c),
      { E(0xe35a14bd,0xaabaa0e3,0x3fff), E(0xe35a14bc,0xaabaa0e3,0x3fff),
        E(0xe35a14bd,0xaabaa0e3,0x3fff), E(0xe35a14bc,0xaabaa0e3,0x3fff) } }
};

typedef struct {
    const char *label;
    ext80 dividend, divisor, trunc_rem, nearest_rem;
    u8 trunc_q, nearest_q;
} rem_vector;

/* Negative-Q flag encodings vary/are ambiguous in references; those bits are diagnostic only. */
static const rem_vector rem_vectors[] = {
    { "11/2", E(0x00000000,0xb0000000,0x4002), E(0x00000000,0x80000000,0x4000),
      E(0x00000000,0x80000000,0x3fff), E(0x00000000,0x80000000,0xbfff), 5, 6 },
    { "7/2 tie", E(0x00000000,0xe0000000,0x4001), E(0x00000000,0x80000000,0x4000),
      E(0x00000000,0x80000000,0x3fff), E(0x00000000,0x80000000,0xbfff), 3, 4 },
    { "5/2 tie", E(0x00000000,0xa0000000,0x4001), E(0x00000000,0x80000000,0x4000),
      E(0x00000000,0x80000000,0x3fff), E(0x00000000,0x80000000,0x3fff), 2, 2 },
    { "-11/2", E(0x00000000,0xb0000000,0xc002), E(0x00000000,0x80000000,0x4000),
      E(0x00000000,0x80000000,0xbfff), E(0x00000000,0x80000000,0x3fff), 0, 0 },
    { "11/-2", E(0x00000000,0xb0000000,0x4002), E(0x00000000,0x80000000,0xc000),
      E(0x00000000,0x80000000,0x3fff), E(0x00000000,0x80000000,0xbfff), 0, 0 },
    { "-14/7 signed zero", E(0x00000000,0xe0000000,0xc002), E(0x00000000,0xe0000000,0x4001),
      E(0x00000000,0x00000000,0x8000), E(0x00000000,0x00000000,0x8000), 0, 0 },
    { "-zero/3", E(0x00000000,0x00000000,0x8000), E(0x00000000,0xc0000000,0x4000),
      E(0x00000000,0x00000000,0x8000), E(0x00000000,0x00000000,0x8000), 0, 0 },
    { "max/7", E(0xffffffff,0xffffffff,0x7ffe), E(0x00000000,0xe0000000,0x4001),
      E(0x00000000,0x80000000,0x3fff), E(0x00000000,0x80000000,0x3fff), 1, 1 },
    { "max/11", E(0xffffffff,0xffffffff,0x7ffe), E(0x00000000,0xb0000000,0x4002),
      E(0x00000000,0x80000000,0x4001), E(0x00000000,0x80000000,0x4001), 4, 4 },
    { "2^1000/3", E(0x00000000,0x80000000,0x43e7), E(0x00000000,0xc0000000,0x4000),
      E(0x00000000,0x80000000,0x3fff), E(0x00000000,0x80000000,0x3fff), 5, 5 },
    { "max/minsub", E(0xffffffff,0xffffffff,0x7ffe), E(0x00000001,0x00000000,0x0000),
      E(0x00000000,0x00000000,0x0000), E(0x00000000,0x00000000,0x0000), 0, 0 }
};

#undef E

static int same_ext(const ext80 *a, const ext80 *b) {
    return a->lo == b->lo && a->hi == b->hi && a->se == b->se;
}

static void reset_fp(void) {
    __asm__ volatile("fninit" ::: "memory");
}

static void division_case(const div_vector *v, ext80 *out) {
    __asm__ volatile("fldt %1; fldt %2; fdiv %%st(1),%%st; fstpt %0; fstp %%st(0)"
                     : "=m"(*out) : "m"(v->denominator), "m"(v->numerator) : "memory");
}

static void report_div_mismatch(unsigned index, unsigned mode, const div_vector *v,
                                const ext80 *got, const ext80 *want) {
    t_note("FDIV mismatch seed=%08x vector=%u rc=%u num=%04x:%08x%08x den=%04x:%08x%08x want=%04x:%08x%08x got=%04x:%08x%08x",
           (u32)TEST_SEED, index, mode, v->numerator.se, v->numerator.hi, v->numerator.lo,
           v->denominator.se, v->denominator.hi, v->denominator.lo,
           want->se, want->hi, want->lo, got->se, got->hi, got->lo);
}

static u32 fp_fdiv_vector(u32 index, u32 mode) {
    const u32 iterations = 256u * HEAVY_SCALE;
    u32 sum = 0;
    reset_fp();
    for (u32 i = 0; i < iterations; ++i) {
        const div_vector *v = &div_vectors[index];
        ext80 got;
        u16 cw = (u16)(0x037f | (mode << 10));
        __asm__ volatile("fldcw %0" :: "m"(cw) : "memory");
        division_case(v, &got);
        if (!same_ext(&got, &v->rounded[mode])) {
            report_div_mismatch(index, mode, v, &got, &v->rounded[mode]);
            t_fail("FDIV extended result differs from exact rational reference", __LINE__);
            t_ops(i + 1);
            reset_fp();
            return sum;
        }
        sum = csum_mix(sum, got.lo ^ got.hi ^ got.se ^ index ^ mode);
    }
    t_ops(iterations);
    t_note("FDIV cases=%u vector=%u rounding_control=%u seed=%08x independent_record=1",
           iterations, index, mode, (u32)TEST_SEED);
    reset_fp();
    return sum;
}

static u8 quotient_bits(u16 sw) {
    return (u8)(((sw >> 6) & 4u) | ((sw >> 13) & 2u) | ((sw >> 9) & 1u));
}

static int negative_quotient(const rem_vector *v) {
    return ((v->dividend.se ^ v->divisor.se) & 0x8000) &&
           (v->dividend.lo | v->dividend.hi | (v->dividend.se & 0x7fff));
}

static int remainder_case(const rem_vector *v, int nearest, ext80 *out,
                          u8 *qbits, u32 *steps, u16 *last_sw) {
    u16 sw = 0;
    __asm__ volatile("fldt %0; fldt %1" :: "m"(v->divisor), "m"(v->dividend) : "memory");
    for (*steps = 0; *steps < 2048; ++*steps) {
        if (nearest)
            __asm__ volatile("fprem1; fnstsw %0" : "=m"(sw) :: "memory");
        else
            __asm__ volatile("fprem; fnstsw %0" : "=m"(sw) :: "memory");
        if (!(sw & 0x0400))
            break;
    }
    *last_sw = sw;
    if (sw & 0x0400)
        return 0;
    *qbits = quotient_bits(sw);
    __asm__ volatile("fstpt %0; fstp %%st(0)" : "=m"(*out) :: "memory");
    return 1;
}

static u32 remainder_class_check(u32 which) {
    static const ext80 denormal = { 1, 0, 0 };
    static const ext80 one = { 0, 0x80000000, 0x3fff };
    static const ext80 finite = { 0, 0xb0000000, 0x4002 };
    static const ext80 infinity = { 0, 0x80000000, 0x7fff };
    ext80 got;
    u16 sw;

    reset_fp();
    for (u32 i = 0; i < 1024u * HEAVY_SCALE; i++) {
        const ext80 *x = which < 2 ? &denormal : which == 2 ? &finite : &infinity;
        const ext80 *y = which == 2 ? &infinity : &one;
        if (which & 1)
            __asm__ volatile("fninit; fldt %3; fldt %2; fprem1; fnstsw %0; fstpt %1; fstp %%st(0)"
                             : "=m"(sw), "=m"(got) : "m"(*x), "m"(*y) : "memory");
        else
            __asm__ volatile("fninit; fldt %3; fldt %2; fprem; fnstsw %0; fstpt %1; fstp %%st(0)"
                             : "=m"(sw), "=m"(got) : "m"(*x), "m"(*y) : "memory");
        int valid;
        if (which < 2)
            valid = same_ext(&got, &denormal) && (sw & 2) && !(sw & 0x0401);
        else if (which == 2)
            valid = same_ext(&got, &finite) && !(sw & 0x0401);
        else
            valid = (sw & 1) && (got.se & 0x7fff) == 0x7fff &&
                    (got.hi & 0x40000000) && !(sw & 0x0400);
        t_ops(1);
        if (!valid) {
            t_note("FPREM class=%u seed=%08x case=%u sw=%04x got=%04x:%08x%08x",
                   which, (u32)TEST_SEED, i, sw, got.se, got.hi, got.lo);
            t_fail("independent FPREM classification result/status mismatch", __LINE__);
            reset_fp();
            return i;
        }
    }
    t_note("classification=%u cases=%u independent_record=1", which, 1024u * HEAVY_SCALE);
    reset_fp();
    return 1024u * HEAVY_SCALE;
}

static void report_rem_mismatch(unsigned index, int nearest, const rem_vector *v,
                                const ext80 *got, const ext80 *want, u8 qgot,
                                u8 qwant, int qasserted, u32 steps, u16 sw,
                                int failed_to_finish) {
    t_note("FPREM%s mismatch seed=%08x vector=%u(%s) steps=%u sw=%04x x=%04x:%08x%08x y=%04x:%08x%08x want=%04x:%08x%08x qassert=%u qwant=%u got=%04x:%08x%08x qgot=%u",
           nearest ? "1" : "", (u32)TEST_SEED, index, v->label, steps, sw,
           v->dividend.se, v->dividend.hi, v->dividend.lo,
           v->divisor.se, v->divisor.hi, v->divisor.lo,
           want->se, want->hi, want->lo, qasserted, qwant,
           got->se, got->hi, got->lo, qgot);
    if (failed_to_finish)
        t_note("FPREM C2 remained set at bounded iteration limit; FPU was reset");
}

static u32 fp_fprem_vector(u32 index, int nearest) {
    const u32 iterations = 256u * HEAVY_SCALE;
    u32 sum = 0, total_steps = 0, max_steps = 0, negative_q_count = 0;
    u8 negative_q_flags_xor = 0;
    reset_fp();
    for (u32 i = 0; i < iterations; ++i) {
        const rem_vector *v = &rem_vectors[index];
        const ext80 *want = nearest ? &v->nearest_rem : &v->trunc_rem;
        u8 qwant = nearest ? v->nearest_q : v->trunc_q, qgot = 0;
        int qbits_defined = !negative_quotient(v);
        u32 steps = 0;
        u16 sw = 0;
        ext80 got = {0, 0, 0};
        if (!remainder_case(v, nearest, &got, &qgot, &steps, &sw)) {
            report_rem_mismatch(index, nearest, v, &got, want, qgot, qwant,
                                0, steps, sw, 1);
            t_fail("FPREM C2 did not clear within bounded iterations", __LINE__);
            t_ops(i + total_steps + steps);
            reset_fp();
            return sum;
        }
        total_steps += steps + 1;
        if (steps + 1 > max_steps)
            max_steps = steps + 1;
        if (!qbits_defined) {
            negative_q_count++;
            negative_q_flags_xor ^= qgot;
        }
        if (!same_ext(&got, want) || (qbits_defined && qgot != qwant)) {
            report_rem_mismatch(index, nearest, v, &got, want, qgot, qwant,
                                qbits_defined, steps + 1, sw, 0);
            t_fail(qbits_defined
                       ? "FPREM remainder or positive quotient bits differ from exact reference"
                       : "FPREM remainder differs from exact reference",
                   __LINE__);
            t_ops(i + total_steps);
            reset_fp();
            return sum;
        }
        sum = csum_mix(sum, got.lo ^ got.hi ^ got.se ^ index ^ qgot);
    }
    t_ops(iterations + total_steps);
    t_note("FPREM cases=%u nearest=%u vector=%u iteration_limit=2048 max_steps=%u partial_steps=%u seed=%08x independent_record=1",
           iterations, nearest, index, max_steps, total_steps, (u32)TEST_SEED);
    if (negative_q_count)
        t_note("FPREM negative_quotient_flags_unasserted=%u raw_flag_xor=%02x (encoding unspecified)",
               negative_q_count, negative_q_flags_xor);
    reset_fp();
    return sum;
}

#define DIV_CASE(n) \
    static u32 div_##n##_0(void) { return fp_fdiv_vector(n, 0); } \
    static u32 div_##n##_1(void) { return fp_fdiv_vector(n, 1); } \
    static u32 div_##n##_2(void) { return fp_fdiv_vector(n, 2); } \
    static u32 div_##n##_3(void) { return fp_fdiv_vector(n, 3); }
#define REM_CASE(n) \
    static u32 rem_##n(void) { return fp_fprem_vector(n, 0); } \
    static u32 rem1_##n(void) { return fp_fprem_vector(n, 1); }
DIV_CASE(0) DIV_CASE(1) DIV_CASE(2) DIV_CASE(3) DIV_CASE(4)
DIV_CASE(5) DIV_CASE(6) DIV_CASE(7) DIV_CASE(8) DIV_CASE(9)
DIV_CASE(10) DIV_CASE(11) DIV_CASE(12) DIV_CASE(13) DIV_CASE(14)
REM_CASE(0) REM_CASE(1) REM_CASE(2) REM_CASE(3) REM_CASE(4) REM_CASE(5)
REM_CASE(6) REM_CASE(7) REM_CASE(8) REM_CASE(9) REM_CASE(10)
static u32 rem_denormal(void) { return remainder_class_check(0); }
static u32 rem1_denormal(void) { return remainder_class_check(1); }
static u32 rem_infinity(void) { return remainder_class_check(2); }
static u32 rem1_invalid(void) { return remainder_class_check(3); }

#define DIV_TEST(n) \
    { "fdiv_v" #n "_rn", div_##n##_0, REQ_FPU }, \
    { "fdiv_v" #n "_rd", div_##n##_1, REQ_FPU }, \
    { "fdiv_v" #n "_ru", div_##n##_2, REQ_FPU }, \
    { "fdiv_v" #n "_rz", div_##n##_3, REQ_FPU },
#define REM_TEST(n) \
    { "fprem_v" #n, rem_##n, REQ_FPU }, \
    { "fprem1_v" #n, rem1_##n, REQ_FPU },
const test_t tests_fp_edges[] = {
    DIV_TEST(0) DIV_TEST(1) DIV_TEST(2) DIV_TEST(3) DIV_TEST(4)
    DIV_TEST(5) DIV_TEST(6) DIV_TEST(7) DIV_TEST(8) DIV_TEST(9)
    DIV_TEST(10) DIV_TEST(11) DIV_TEST(12) DIV_TEST(13) DIV_TEST(14)
    REM_TEST(0) REM_TEST(1) REM_TEST(2) REM_TEST(3) REM_TEST(4) REM_TEST(5)
    REM_TEST(6) REM_TEST(7) REM_TEST(8) REM_TEST(9) REM_TEST(10)
    { "fprem_denormal", rem_denormal, REQ_FPU },
    { "fprem1_denormal", rem1_denormal, REQ_FPU },
    { "fprem_finite_inf", rem_infinity, REQ_FPU },
    { "fprem1_inf_invalid", rem1_invalid, REQ_FPU },
    { 0, 0, 0 }
};
