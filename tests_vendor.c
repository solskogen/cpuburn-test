#include "kernel.h"

#ifndef HEAVY_SCALE
#define HEAVY_SCALE 8
#endif
#ifndef TEST_SEED
#define TEST_SEED 0x86B04A21u
#endif

/* Exact finite dyadic inputs keep the oracle integer-only, independent of x87. */
static u32 fp_scaled(s32 v, unsigned fraction) {
    if (!v) return 0;
    u32 sign = v < 0 ? 0x80000000u : 0;
    u32 mag = v < 0 ? 0u - (u32)v : (u32)v, top = 0;
    for (u32 n = mag; n >>= 1;) top++;
    return sign | ((127 + top - fraction) << 23) |
           ((mag << (23 - top)) & 0x7fffff);
}

static u32 random32(u32 *state) {
    u32 x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

static u64 random64(u32 *state) {
    u32 lo = random32(state), hi = random32(state);
    return (u64)hi << 32 | lo;
}

static u64 pair(u32 lo, u32 hi) { return (u64)hi << 32 | lo; }
static unsigned notes_left;

static u32 checked(u32 sum, u64 got, u64 want, const char *op, u32 i, u32 form,
                   u64 a, u64 b) {
    if (got != want) {
        if (notes_left) {
            notes_left--;
            t_note("op=%s case=%u form=%u a=%llx b=%llx got=%llx want=%llx",
                   op, i, form, a, b, got, want);
        }
        CHECK_EQ((u32)got, (u32)want);
        CHECK_EQ((u32)(got >> 32), (u32)(want >> 32));
    }
    return csum_mix(csum_mix(sum, (u32)got), (u32)(got >> 32));
}

#define NOW_OPS(X) \
    X(pfadd) X(pfsub) X(pfsubr) X(pfmul) X(pfacc) \
    X(pfcmpeq) X(pfcmpge) X(pfcmpgt) X(pfmin) X(pfmax) \
    X(pi2fd) X(pf2id) X(pavgusb) X(pmulhrw) \
    X(pfnacc) X(pfpnacc) X(pi2fw) X(pf2iw) X(pswapd)
#define ENUM(n) now_##n,
enum { NOW_OPS(ENUM) now_end };
#undef ENUM
#define NAME(n) #n,
static const char *const now_names[] = { NOW_OPS(NAME) };
#undef NAME

static u64 now_execute(unsigned op, u64 a, u64 b, unsigned form) {
    u64 r = 0;
    switch (op) {
#define DO(n) case now_##n: \
        if (form == 0) \
            __asm__ volatile("movq %1,%%mm0; movq %2,%%mm1; " #n " %%mm1,%%mm0; movq %%mm0,%0; emms" \
                             : "=m"(r) : "m"(a), "m"(b) : "memory"); \
        else if (form == 1) \
            __asm__ volatile("movq %1,%%mm0; " #n " %2,%%mm0; movq %%mm0,%0; emms" \
                             : "=m"(r) : "m"(a), "m"(b) : "memory"); \
        else \
            __asm__ volatile("movq %1,%%mm0; " #n " %%mm0,%%mm0; movq %%mm0,%0; emms" \
                             : "=m"(r) : "m"(a) : "memory"); \
        break;
        NOW_OPS(DO)
#undef DO
    }
    return r;
}

static u64 fp_reference(unsigned op, const s32 a[2], const s32 b[2]) {
    u32 r[2];
    for (unsigned lane = 0; lane < 2; lane++) {
        s32 x = a[lane], y = b[lane], v = 0;
        switch (op) {
        case now_pfadd: v = x + y; break;
        case now_pfsub: v = x - y; break;
        case now_pfsubr: v = y - x; break;
        case now_pfmul: v = x * y; break;
        case now_pfacc: v = lane ? b[0] + b[1] : a[0] + a[1]; break;
        case now_pfnacc: v = lane ? b[0] - b[1] : a[0] - a[1]; break;
        case now_pfpnacc: v = lane ? b[0] + b[1] : a[0] - a[1]; break;
        case now_pfmin: v = x < y ? x : y; break;
        case now_pfmax: v = x > y ? x : y; break;
        }
        if (op == now_pfcmpeq) r[lane] = x == y ? ~0u : 0;
        else if (op == now_pfcmpge) r[lane] = x >= y ? ~0u : 0;
        else if (op == now_pfcmpgt) r[lane] = x > y ? ~0u : 0;
        else {
            r[lane] = fp_scaled(v, 0);
            if (op == now_pfmul && !v && ((x < 0) != (y < 0)))
                r[lane] = 0x80000000u;
        }
    }
    return pair(r[0], r[1]);
}

static u32 float_cases(unsigned first, unsigned end) {
    static const s32 edge[] = { 0, 1, -1, 127, -128, 64, -64, 2 };
    u32 seed = TEST_SEED, sum = 0;
    for (unsigned op = first; op < end; op++) {
        for (u32 i = 0; i < 128u * HEAVY_SCALE; i++) {
            s32 a[2], b[2];
            for (unsigned lane = 0; lane < 2; lane++) {
                a[lane] = i < 64 ? edge[(i / 8 + lane) % 8] : (s32)(random32(&seed) & 255) - 128;
                b[lane] = i < 64 ? edge[(i + lane) % 8] : (s32)(random32(&seed) & 255) - 128;
            }
            u64 av = pair(fp_scaled(a[0], 0), fp_scaled(a[1], 0));
            u64 bv = pair(fp_scaled(b[0], 0), fp_scaled(b[1], 0));
            for (unsigned form = 0; form < 3; form++) {
                u64 want = fp_reference(op, a, form == 2 ? a : b);
                sum = checked(sum, now_execute(op, av, bv, form), want,
                              now_names[op], i, form, av, form == 2 ? av : bv);
                t_ops(1);
            }
        }
    }
    return sum;
}

static u32 now_float(void) {
    notes_left = 8;
    return float_cases(now_pfadd, now_pi2fd);
}

static u32 convert_cases(int enhanced) {
    static const s32 edge[] = {
        0, 1, -1, 2, -2, 65534, -65536, 65535, -65535,
        65536, -65537, 131070, -131072, 131071
    };
    const unsigned edge_count = sizeof edge / sizeof edge[0];
    u32 seed = TEST_SEED, sum = 0;
    for (unsigned kind = 0; kind < 2; kind++) {
        notes_left = 4;
        unsigned op = enhanced ? (kind ? now_pf2iw : now_pi2fw) : (kind ? now_pf2id : now_pi2fd);
        for (u32 i = 0; i < 128u * HEAVY_SCALE; i++) {
            u32 in[2], out[2];
            for (unsigned lane = 0; lane < 2; lane++) {
                s32 x = i < edge_count * edge_count ? edge[(lane ? i / edge_count : i) % edge_count] :
                        (s32)(random32(&seed) & 0x3ffff) - 131072;
                if (!kind && enhanced) x = (s16)x;
                in[lane] = kind ? fp_scaled(x, 1) : enhanced ?
                           (0xa5a50000u | (u16)x) : (u32)x;
                s32 truncated = x / 2;
                if (kind && enhanced) {
                    if (truncated < -32768) truncated = -32768;
                    if (truncated > 32767) truncated = 32767;
                }
                out[lane] = kind ? (u32)truncated : fp_scaled(x, 0);
            }
            u64 input = pair(in[0], in[1]), want = pair(out[0], out[1]);
            for (unsigned form = 0; form < 3; form++) {
                u64 a = form == 2 ? input : 0xdeadbeef12345678ull;
                sum = checked(sum, now_execute(op, a, input, form), want,
                              now_names[op], i, form, a, input);
                t_ops(1);
            }
        }
    }
    return sum;
}

static u32 now_convert(void) {
    notes_left = 8;
    return convert_cases(0);
}

static const u64 integer_edges[] = {
    0, ~0ull, 0x8000800080008000ull, 0x7fff7fff7fff7fffull,
    0x8080808080808080ull, 0x7f7f7f7f7f7f7f7full,
    0x4000c0000001ffffull, 0x0123456789abcdefull
};

static u32 now_integer(void) {
    notes_left = 8;
    u32 sum = 0, seed = TEST_SEED;
    for (unsigned op = now_pavgusb; op <= now_pmulhrw; op++) {
        for (u32 i = 0; i < 128u * HEAVY_SCALE; i++) {
            u64 a = i < 64 ? integer_edges[i / 8] : random64(&seed);
            u64 b = i < 64 ? integer_edges[i % 8] : random64(&seed);
            for (unsigned form = 0; form < 3; form++) {
                u64 src = form == 2 ? a : b, want = 0;
                unsigned width = op == now_pavgusb ? 8 : 16;
                for (unsigned lane = 0; lane < 64 / width; lane++) {
                    u32 x = (u32)(a >> (lane * width)), y = (u32)(src >> (lane * width));
                    u32 v = width == 8 ? ((x & 255) + (y & 255) + 1) / 2 :
                            ((u32)((s32)(s16)x * (s32)(s16)y) + 0x8000u) >> 16;
                    want |= (u64)(v & ((1u << width) - 1)) << (lane * width);
                }
                sum = checked(sum, now_execute(op, a, b, form), want, now_names[op],
                              i, form, a, src);
                t_ops(1);
            }
        }
    }
    return sum;
}

static u64 reciprocal(u64 input, int square, int refined, int memory) {
    u64 result;
#define RECIP(src, refine) \
    __asm__ volatile("movq %1,%%mm0; " src "; movq %%mm1,%%mm2; " refine \
                     "movq %%mm1,%0; emms" : "=m"(result) : "m"(input) : "memory")
#define RCP_REFINE "punpckldq %%mm0,%%mm0; pfrcpit1 %%mm1,%%mm0; pfrcpit2 %%mm2,%%mm0; movq %%mm0,%%mm1; "
#define RSQ_REFINE "punpckldq %%mm0,%%mm0; pfmul %%mm1,%%mm1; pfrsqit1 %%mm0,%%mm1; pfrcpit2 %%mm2,%%mm1; "
    if (square) {
        if (refined) {
            if (memory) RECIP("pfrsqrt %1,%%mm1", RSQ_REFINE);
            else RECIP("pfrsqrt %%mm0,%%mm1", RSQ_REFINE);
        } else {
            if (memory) RECIP("pfrsqrt %1,%%mm1", "");
            else RECIP("pfrsqrt %%mm0,%%mm1", "");
        }
    } else {
        if (refined) {
            if (memory) RECIP("pfrcp %1,%%mm1", RCP_REFINE);
            else RECIP("pfrcp %%mm0,%%mm1", RCP_REFINE);
        } else {
            if (memory) RECIP("pfrcp %1,%%mm1", "");
            else RECIP("pfrcp %%mm0,%%mm1", "");
        }
    }
#undef RSQ_REFINE
#undef RCP_REFINE
#undef RECIP
    return result;
}

static u32 now_reciprocal(void) {
    notes_left = 8;
    u32 sum = 0;
    for (u32 i = 0; i < 32u * HEAVY_SCALE; i++) {
        int exponent = (int)(i % 17) - 8;
        u64 input = pair((u32)(127 + 2 * exponent) << 23, 0x42f60000);
        for (unsigned square = 0; square < 2; square++) {
            u32 want = (u32)(127 - (square ? exponent : 2 * exponent)) << 23;
            for (unsigned refined = 0; refined < 2; refined++) {
                for (unsigned memory = 0; memory < 2; memory++) {
                    u64 got = reciprocal(input, square, refined, memory);
                    for (unsigned lane = 0; lane < 2; lane++) {
                        u32 v = (u32)(got >> (lane * 32));
                        /* At a power of two, spacing below is half the spacing above. */
                        u32 tolerance = refined ? (v < want ? 2 : 1) :
                                        square ? (v < want ? 512 : 256) : (v < want ? 1024 : 512);
                        u32 error = v < want ? want - v : v - want;
                        if (error > tolerance) {
                            if (notes_left) {
                                notes_left--;
                                t_note("op=%s refined=%u memory=%u case=%u lane=%u got=%08x want=%08x tolerance=%u",
                                       square ? "pfrsqrt" : "pfrcp", refined, memory, i, lane, v, want, tolerance);
                            }
                            CHECK_EQ(v, want);
                        }
                        sum = csum_mix(sum, v);
                    }
                    t_ops(1);
                }
            }
        }
    }
    return sum;
}

static u32 now_state(void) {
    u32 sum = 0;
    for (u32 i = 0; i < 128u * HEAVY_SCALE; i++) {
        u16 before[14], after[14];
        s32 input = (s32)i - 512, output = 0;
        u32 flags_before, flags_after;
        u64 hint = pair(i ^ TEST_SEED, ~i);
        __asm__ volatile(
            "fninit; movq %5,%%mm0; fnstenv %0; femms; fnstenv %1; "
            "fildl %4; fistpl %2; "
            "pushfl; popl %3; prefetch %5; prefetchw %5; "
            "pushfl; popl %6; fninit"
            : "=m"(before), "=m"(after), "=m"(output), "=r"(flags_before),
              "+m"(input), "+m"(hint), "=r"(flags_after) :: "memory");
        CHECK_EQ(before[4], 0);
        CHECK_EQ(after[4], 0xffff);
        CHECK_EQ((after[2] >> 11) & 7, 0);
        CHECK_EQ(output, input);
        CHECK_EQ(flags_after, flags_before);
        CHECK_EQ((u32)hint, i ^ TEST_SEED);
        CHECK_EQ((u32)(hint >> 32), ~i);
        sum = csum_mix(sum, (u32)output);
        t_ops(1);
    }
    return sum;
}

static u32 now_enhanced_float(void) {
    notes_left = 8;
    return float_cases(now_pfnacc, now_pi2fw);
}

static u32 now_word_convert(void) { return convert_cases(1); }

static u32 now_swap(void) {
    notes_left = 8;
    u32 seed = TEST_SEED, sum = 0;
    for (u32 i = 0; i < 128u * HEAVY_SCALE; i++) {
        u64 a = random64(&seed), b = random64(&seed);
        for (unsigned form = 0; form < 3; form++) {
            u64 src = form == 2 ? a : b, want = (src << 32) | (src >> 32);
            sum = checked(sum, now_execute(now_pswapd, a, b, form), want,
                          "pswapd", i, form, a, src);
            t_ops(1);
        }
    }
    return sum;
}

#define EMMI_OPS(X) \
    X(paveb, 0x50) X(paddsiw, 0x51) X(pmagw, 0x52) X(pdistib, 0x54) \
    X(psubsiw, 0x55) X(pmulhrwc, 0x59) X(pmulhriw, 0x5d) X(pmachriw, 0x5e) \
    X(pmvzb, 0x58) X(pmvnzb, 0x5a) X(pmvlzb, 0x5b) X(pmvgezb, 0x5c)
#define ENUM(n, code) emmi_##n,
enum { EMMI_OPS(ENUM) emmi_end };
#undef ENUM
#define NAME(n, code) #n,
static const char *const emmi_names[] = { EMMI_OPS(NAME) };
#undef NAME
typedef struct { u64 specified, implied, source; } emmi_result;

static emmi_result emmi_execute(unsigned op, u64 a, u64 b, u64 c,
                                unsigned memory, unsigned odd) {
    emmi_result r;
#define EMIT(code, modrm, first, second) \
    __asm__ volatile("movq %3,%%" first "; movq %4,%%mm2; movq %5,%%" second "; " \
                     ".byte 0x0f," #code "," #modrm "; " \
                     "movq %%" first ",%0; movq %%" second ",%1; movq %%mm2,%2; emms" \
                     : "=m"(r.specified), "=m"(r.implied), "=m"(r.source) \
                     : "m"(a), "m"(b), "m"(c), "S"(&b) : "memory")
    switch (op) {
#define DO(n, code) case emmi_##n: \
        if (memory) { \
            if (odd) EMIT(code, 0x0e, "mm1", "mm0"); \
            else EMIT(code, 0x06, "mm0", "mm1"); \
        } else { \
            if (odd) EMIT(code, 0xca, "mm1", "mm0"); \
            else EMIT(code, 0xc2, "mm0", "mm1"); \
        } break;
        EMMI_OPS(DO)
#undef DO
    }
#undef EMIT
    return r;
}

static u32 abs_word(s32 x) { return x < 0 ? (u32)-x : (u32)x; }
static u16 sat_word(s32 x) {
    return (u16)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
}

static emmi_result emmi_reference(unsigned op, u64 a, u64 b, u64 c) {
    emmi_result r = { a, c, b };
    u64 out = 0;
    unsigned width = op == emmi_paveb || op == emmi_pdistib || op >= emmi_pmvzb ? 8 : 16;
    for (unsigned lane = 0; lane < 64 / width; lane++) {
        u32 x = (u32)(a >> (lane * width)), y = (u32)(b >> (lane * width));
        u32 z = (u32)(c >> (lane * width)), v = 0;
        s32 sx = (s16)x, sy = (s16)y;
        switch (op) {
        case emmi_paveb: v = ((x & 255) + (y & 255)) / 2; break;
        case emmi_paddsiw: v = sat_word(sx + sy); break;
        case emmi_psubsiw: v = sat_word(sx - sy); break;
        case emmi_pmagw: v = abs_word(sy) > abs_word(sx) ? y : x; break;
        case emmi_pmulhrwc: case emmi_pmulhriw: case emmi_pmachriw:
            v = ((u32)(sx * sy) + 0x4000u) >> 15;
            if (op == emmi_pmachriw) v += z;
            break;
        case emmi_pdistib: {
            s32 diff = (s32)(x & 255) - (s32)(y & 255);
            v = (z & 255) + (u32)(diff < 0 ? -diff : diff);
            if (v > 255) v = 255;
            break;
        }
        default: {
            int move = op == emmi_pmvzb ? (z & 255) == 0 :
                       op == emmi_pmvnzb ? (z & 255) != 0 :
                       op == emmi_pmvlzb ? (z & 128) != 0 : (z & 128) == 0;
            v = move ? y : x;
            break;
        }
        }
        out |= (u64)(v & ((1u << width) - 1)) << (lane * width);
    }
    if (op == emmi_paddsiw || op == emmi_psubsiw || op == emmi_pmulhriw ||
        op == emmi_pmachriw || op == emmi_pdistib) r.implied = out;
    else r.specified = out;
    return r;
}

static u32 emmi_sum;
static unsigned emmi_first, emmi_limit;

static void emmi_body(void) {
    u32 seed = TEST_SEED;
    for (unsigned op = emmi_first; op < emmi_limit; op++) {
        for (u32 i = 0; i < 128u * HEAVY_SCALE; i++) {
            u64 a = i < 64 ? integer_edges[i / 8] : random64(&seed);
            u64 b = i < 64 ? integer_edges[i % 8] : random64(&seed);
            u64 c = i < 64 ? integer_edges[(i + i / 8) % 8] : random64(&seed);
            if (op == emmi_pmagw) {
                /* Equal, opposite magnitudes have no tie rule in our reference. */
                for (unsigned lane = 0; lane < 4; lane++) {
                    s32 x = (s16)(a >> (lane * 16)), y = (s16)(b >> (lane * 16));
                    if (x != y && abs_word(x) == abs_word(y)) b ^= 1ull << (lane * 16);
                }
            }
            emmi_result want = emmi_reference(op, a, b, c);
            int memory_only = op == emmi_pdistib || op == emmi_pmachriw || op >= emmi_pmvzb;
            for (unsigned memory = memory_only; memory < 2; memory++) {
                for (unsigned odd = 0; odd < 2; odd++) {
                    emmi_result got = emmi_execute(op, a, b, c, memory, odd);
                    unsigned form = memory * 2 + odd;
                    emmi_sum = checked(emmi_sum, got.specified, want.specified, emmi_names[op], i, form, a, b);
                    emmi_sum = checked(emmi_sum, got.implied, want.implied, emmi_names[op], i, form, c, b);
                    emmi_sum = checked(emmi_sum, got.source, b, emmi_names[op], i, form, a, b);
                    t_ops(1);
                }
            }
        }
    }
}

static u8 cyrix_read(u8 index) { outb(0x22, index); return inb(0x23); }
static void cyrix_write(u8 index, u8 value) { outb(0x22, index); outb(0x23, value); }

static u32 emmi_run(unsigned first, unsigned limit) {
    notes_left = 8;
    u32 flags = rd_eflags();
    cli();
    u8 ccr3 = cyrix_read(0xc3);
    cyrix_write(0xc3, (ccr3 & 15) | 0x10);
    u8 ccr7 = cyrix_read(0xeb);
    cyrix_write(0xeb, ccr7 | 1);
    u8 enabled = cyrix_read(0xeb);
    cyrix_write(0xc3, ccr3);
    wr_eflags(flags);
    CHECK_EQ(enabled & 1, 1);
    emmi_first = first; emmi_limit = limit; emmi_sum = 0;
    int rc = guard_call(emmi_body);
    cli();
    cyrix_write(0xc3, (ccr3 & 15) | 0x10);
    cyrix_write(0xeb, ccr7);
    u8 restored = cyrix_read(0xeb);
    cyrix_write(0xc3, ccr3);
    u8 restored3 = cyrix_read(0xc3);
    wr_eflags(flags);
    CHECK_EQ(restored, ccr7);
    CHECK_EQ(restored3, ccr3);
    if (rc) {
        t_note("rc=%x vec=%u err=%x eip=%x", rc, g_exc.vec, g_exc.err, g_exc.eip);
        guard_recover((u32)rc);
    }
    return emmi_sum;
}

static u32 emmi_arithmetic(void) { return emmi_run(emmi_paveb, emmi_pmvzb); }
static u32 emmi_moves(void) { return emmi_run(emmi_pmvzb, emmi_end); }

#define NOW_REQ (REQ_MMX | REQ_FPU | REQ_3DNOW)
const test_t tests_vendor[] = {
    { "3dnow_float", now_float, NOW_REQ },
    { "3dnow_convert", now_convert, NOW_REQ },
    { "3dnow_integer", now_integer, NOW_REQ },
    { "3dnow_reciprocal", now_reciprocal, NOW_REQ },
    { "3dnow_state", now_state, NOW_REQ },
    { "3dnow_enhanced_float", now_enhanced_float, NOW_REQ | REQ_3DNOWEXT },
    { "3dnow_word_convert", now_word_convert, NOW_REQ | REQ_3DNOWEXT },
    { "3dnow_swap", now_swap, NOW_REQ | REQ_3DNOWEXT },
    { "cyrix_emmi_arithmetic", emmi_arithmetic, REQ_MMX | REQ_EMMI },
    { "cyrix_emmi_moves", emmi_moves, REQ_MMX | REQ_EMMI },
    { NULL, NULL, 0 }
};
