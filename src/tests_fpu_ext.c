/* Timed x87 records aimed at individual 86Box x87 paths (doc/workloads.md,
 * "Targeted x87 and MMX records"). All floating-point work is explicit x87;
 * data are exact dyadic values or small integers so that an emulator using
 * host doubles for the x87 still gets the exact answer, and inexact work is
 * checked against host-computed constants with a relative bound. */
#include "kernel.h"

#define ADD "faddp %%st,%%st(1);"
static void init(void) { __asm__ volatile("fninit" ::: "memory"); }
static void ctl(u16 cw) { __asm__ volatile("fldcw %0" :: "m"(cw) : "memory"); }
/* |got - per*n| <= |per*n| * 2^p, evaluated on the x87. */
static int fp_near(u64 got, u64 per, s32 n, s32 p) {
    u16 sw;
    __asm__ volatile("fldl %[w]; fimull %[n]; fildl %[p]; fld %%st(1); fabs; fscale; fstp %%st(1); fxch %%st(1);"
                     "fsubrl %[g]; fabs; fcompp; fnstsw %[sw]"
                     : [sw]"=m"(sw) : [p]"m"(p), [w]"m"(per), [n]"m"(n), [g]"m"(got) : "memory");
    return (sw & 0x4500) == 0x0100 || (sw & 0x4500) == 0x4000;
}
static u32 done(u64 ops, const char *unit, u32 sum) { t_ops(ops); t_work_unit(unit); init(); return sum; }

/* FLD/FSTP m80 spills (DB /5, /7): 8 values bounce between two buffers. */
static u32 m80_word(unsigned i, unsigned w) {
    u32 h = (TEST_SEED ^ (i * 0x9E3779B1u)) * 0x85EBCA6Bu;
    return w == 0 ? h & ~0x7ffu : w == 1 ? 0x80000000u | h >> 7 : (0x3ff3 + 3 * i) | (i & 1) << 15;
}
static u32 fpu_m80_move(void) {
    static u32 a[8][4] __attribute__((aligned(16))), b[8][4] __attribute__((aligned(16)));
    const s32 n = 256 * HEAVY_SCALE;
    u32 *src = a[0], *dst = b[0], bad = 0;
    s32 left = n;
    for (unsigned i = 0; i < 8; i++)
        for (unsigned w = 0; w < 3; w++) a[i][w] = m80_word(i, w);
    init();
    __asm__ volatile("1:\n.irp o,0,16,32,48,64,80,96,112\n fldt \\o(%%esi); fchs; fstpt \\o(%%edi)\n.endr\n"
                     "xchgl %%esi,%%edi; decl %%ecx; jnz 1b"
                     : "+S"(src), "+D"(dst), "+c"(left) :: "cc", "memory");
    for (unsigned i = 0; i < 8; i++)
        for (unsigned w = 0; w < 3; w++) {
            u32 mask = w == 2 ? 0xffff : ~0u;
            bad += (a[i][w] & mask) != m80_word(i, w) || (b[i][w] & mask) != (m80_word(i, w) ^ (w == 2) << 15);
        }
    CHECK_EQ(bad, 0);
    return done((u64)n * 16, "m80_load_or_store", b[7][1]);
}

/* The same threshold count by FCOMI+Jcc and by FCOMP+FNSTSW AX+SAHF+Jcc. */
static u64 cmp_v[64];
static void cmp_fill(void) {
    static const u32 half = 0x3f000000;
    for (s32 i = 0; i < 64; i++) {
        s32 k = 2 * ((i * 37) & 63) - 63;
        __asm__ volatile("fildl %1; fmuls %2; fstpl %0" : "=m"(cmp_v[i]) : "m"(k), "m"(half) : "memory");
    }
}
static u32 cmp_branch(int fcomi) {
    static const s32 t0 = -32, c64 = 64;
    const s32 nb = HEAVY_SCALE;
    s32 blocks = nb;
    u32 count = 0;
    init();
    cmp_fill();
#define CMP_LOOP(test) \
    "fld1; fildl %[t0]\n1: movl $64,%%ebx\n2: movl $56,%%ecx\n" \
    "3:\n.irp o,0,8,16,24,32,40,48,56\n fldl \\o(%%esi,%%ecx,8); " test "4:\n.endr\n" \
    "subl $8,%%ecx; jns 3b; fadd %%st(1),%%st; decl %%ebx; jnz 2b\n" \
    "fisubl %[c64]; decl %%edx; jnz 1b; fstp %%st(0); fstp %%st(0)"
    if (fcomi)
        __asm__ volatile(CMP_LOOP("fcomip %%st(1),%%st; jbe 4f; incl %%eax; ")
                         : "+a"(count), "+d"(blocks) : "S"(cmp_v), [t0]"m"(t0), [c64]"m"(c64)
                         : "ebx", "ecx", "cc", "memory");
    else
        __asm__ volatile(CMP_LOOP("fcomp %%st(1); fnstsw %%ax; sahf; jbe 4f; incl %%edi; ")
                         : "+D"(count), "+d"(blocks) : "S"(cmp_v), [t0]"m"(t0), [c64]"m"(c64)
                         : "eax", "ebx", "ecx", "cc", "memory");
    /* Thresholds -32..31 against -31.5..31.5: 32-t values lie above t. */
    CHECK_EQ(count, 2080u * nb);
    return done((u64)nb * 4096, "compare_branch", count);
}
static u32 fpu_fcomi_branch(void) { return cmp_branch(1); }
static u32 fpu_fnstsw_sahf(void) { return cmp_branch(0); }

/* Branch-free clamp to [-4, 12] with FCOMI/FUCOMI and all eight FCMOVs. */
static u32 fpu_fcmov(void) {
    static const s32 lo = -4, hi = 12;
    const s32 n = 64 * HEAVY_SCALE;
    s32 passes = n, sum;
    init();
    cmp_fill();
    __asm__ volatile("fldz; fildl %[hi]; fildl %[lo]\n1: movl $56,%%ecx\n"
                     "2:\n.irp o,0,16,32,48\n"
                     " fldl \\o(%%esi,%%ecx,8); fcomi %%st(1),%%st; fcmovu %%st(3),%%st; fcmovb %%st(1),%%st;"
                     " fucomi %%st(2),%%st; fcmove %%st(3),%%st; fcmovnbe %%st(2),%%st; faddp %%st,%%st(3)\n"
                     " fldl \\o+8(%%esi,%%ecx,8); fucomi %%st(1),%%st; fcmovnu %%st(0),%%st; fcmovbe %%st(1),%%st;"
                     " fcomi %%st(2),%%st; fcmovne %%st(0),%%st; fcmovnb %%st(2),%%st; faddp %%st,%%st(3)\n"
                     ".endr\nsubl $8,%%ecx; jns 2b; decl %%eax; jnz 1b\n"
                     "fstp %%st(0); fstp %%st(0); fistpl %[s]"
                     : [s]"=m"(sum), "+a"(passes) : "S"(cmp_v), [lo]"m"(lo), [hi]"m"(hi) : "ecx", "cc", "memory");
    CHECK_EQ(sum, 192 * n);
    return done((u64)n * 64, "clamp", (u32)sum);
}

/* FRNDINT on k/4, k = -32..31: plain (round to nearest even), and the libc
 * floor()+ceil() pattern that swaps the rounding control around each one. */
static u32 fpu_frndint(void) {
    static const u32 quarter = 0x3e800000;
    static const s32 m8 = -8, k16 = 16;
    const s32 n = 64 * HEAVY_SCALE;
    s32 blocks = n, sum;
    init();
    __asm__ volatile("fldz; flds %[q]; fildl %[m8]\n1: movl $8,%%ecx\n"
                     "2:\n.rept 8\n fld %%st(0); frndint; faddp %%st,%%st(3); fadd %%st(1),%%st\n.endr\n"
                     "decl %%ecx; jnz 2b; fisubl %[k16]; decl %%eax; jnz 1b\n"
                     "fstp %%st(0); fstp %%st(0); fistpl %[s]"
                     : [s]"=m"(sum), "+a"(blocks) : [q]"m"(quarter), [m8]"m"(m8), [k16]"m"(k16) : "ecx", "cc", "memory");
    CHECK_EQ(sum, -8 * n);
    return done((u64)n * 64, "frndint", (u32)sum);
}
#define ROUND_WITH(or_mask, and_mask) \
    " fld %%st(0); fnstcw %[o]; movzwl %[o],%%edx; orl $" or_mask ",%%edx; andl $" and_mask ",%%edx;" \
    " movw %%dx,%[f]; fldcw %[f]; frndint; fldcw %[o]; faddp %%st,%%st(3)\n"
static u32 fpu_frndint_floor(void) {
    static const u32 quarter = 0x3e800000;
    static const s32 m8 = -8, k16 = 16;
    const s32 n = 32 * HEAVY_SCALE;
    s32 blocks = n, sum;
    u16 old, forced;
    init();
    __asm__ volatile("fldz; flds %[q]; fildl %[m8]\n1: movl $16,%%ecx\n"
                     "2:\n.rept 4\n" ROUND_WITH("0x400", "0xf7ff") ROUND_WITH("0x800", "0xfbff")
                     " fadd %%st(1),%%st\n.endr\n"
                     "decl %%ecx; jnz 2b; fisubl %[k16]; decl %%eax; jnz 1b\n"
                     "fstp %%st(0); fstp %%st(0); fistpl %[s]"
                     : [s]"=m"(sum), "+a"(blocks), [o]"=m"(old), [f]"=m"(forced)
                     : [q]"m"(quarter), [m8]"m"(m8), [k16]"m"(k16) : "ecx", "edx", "cc", "memory");
    CHECK_EQ(sum, -16 * n);
    return done((u64)n * 128, "frndint", (u32)sum);
}

/* Constant loads (D9 E8-EE) and FXAM on +normal, +0 and -normal. */
static u32 fpu_const(void) {
    static const u64 per = 0x401f9a009d1307e2ull; /* pi+log2(e)+log2(10)+log10(2)+ln(2)-1 */
    const s32 n = 512 * HEAVY_SCALE;
    s32 left = n;
    u32 classes = 0;
    u64 acc;
#define XAM "fxam; fnstsw %%ax; andl $0x4700,%%eax; addl %%eax,%%edx;"
    init();
    __asm__ volatile("fldz\n1: fldpi;" ADD "fldl2e;" ADD "fldl2t;" ADD "fldlg2;" ADD "fldln2;" ADD
                     "fldz;" ADD "fld1; fchs;" ADD XAM "fldz;" XAM "fstp %%st(0); fld1; fchs;" XAM "fstp %%st(0)\n"
                     "decl %%ecx; jnz 1b; fstpl %[acc]"
                     : [acc]"=m"(acc), "+c"(left), "+d"(classes) :: "eax", "cc", "memory");
    CHECK_EQ(classes, 0x4a00u * n);
    CHECK(fp_near(acc, per, n, -30));
    return done((u64)n * 10, "constant_or_fxam", (u32)(acc >> 32));
}

/* One transcendental per record: 64 evaluations at x0 + k*h per block,
 * summed and compared with the host high-precision block sum. */
static u32 trans_check(u64 acc, u64 per, s32 n) {
    if (!fp_near(acc, per, n, -30)) { t_note("sum=%llx per_block=%llx blocks=%d", acc, per, n); CHECK(0); }
    return done((u64)n * 64, "transcendental", (u32)(acc >> 32) ^ (u32)acc);
}
#define TRANS(fn, eval, X0, H, S) static u32 fn(void) { \
    static const u64 x0 = X0, h = H; \
    const s32 n = 4 * HEAVY_SCALE; s32 left = n; u64 acc; \
    init(); \
    __asm__ volatile("fldl %[h]; fldz; fldl %[x0]\n1: movl $8,%%ecx\n" \
                     "2:\n.rept 8\n " eval "; fadd %%st(2),%%st\n.endr\n" \
                     "decl %%ecx; jnz 2b; fstp %%st(0); fldl %[x0]; decl %%eax; jnz 1b\n" \
                     "fstp %%st(0); fstpl %[acc]; fstp %%st(0)" \
                     : [acc]"=m"(acc), "+a"(left) : [h]"m"(h), [x0]"m"(x0) : "ecx", "cc", "memory"); \
    return trans_check(acc, S, n); }
TRANS(fpu_trans_sin, "fld %%st(0); fsin; faddp %%st,%%st(2)",
      0x3fd0000000000000ull, 0x3fa0000000000000ull, 0x40496b2b530ec960ull)
TRANS(fpu_trans_sincos, "fld %%st(0); fsincos; faddp %%st,%%st(1); faddp %%st,%%st(2)",
      0x3fd0000000000000ull, 0x3fa0000000000000ull, 0x40512769dcb2c043ull)
TRANS(fpu_trans_ptan, "fld %%st(0); fptan; fstp %%st(0); faddp %%st,%%st(2)",
      0x3fb0000000000000ull, 0x3f90000000000000ull, 0x40468cf2e4e1cf0full)
TRANS(fpu_trans_patan, "fld %%st(0); fld1; fpatan; faddp %%st,%%st(2)",
      0x3fd0000000000000ull, 0x3fb0000000000000ull, 0x40509e299db336bfull)
TRANS(fpu_trans_yl2x, "fld1; fld %%st(1); fyl2x; faddp %%st,%%st(2)",
      0x3ff0000000000000ull, 0x3fb0000000000000ull, 0x4057104e7d834be6ull)
TRANS(fpu_trans_2xm1, "fld %%st(0); f2xm1; faddp %%st,%%st(2)",
      0x0000000000000000ull, 0x3f90000000000000ull, 0x403bd558baae753dull)

/* FSCALE of 1.5 by -8..7: the block sum 383.994140625 is exact. */
static u32 fpu_trans_scale(void) {
    static const u32 v = 0x3fc00000;
    static const s32 k0 = -8, k16 = 16, k512 = 512;
    const s32 n = 256 * HEAVY_SCALE;
    s32 left = n;
    s64 sum;
    init();
    __asm__ volatile("fld1; fldz; flds %[v]; fildl %[k0]\n"
                     "1:\n.rept 16\n fld %%st(1); fscale; faddp %%st,%%st(3); fadd %%st(3),%%st\n.endr\n"
                     "fisubl %[k16]; decl %%eax; jnz 1b\n"
                     "fstp %%st(0); fstp %%st(0); fimull %[k512]; fistpll %[s]; fstp %%st(0)"
                     : [s]"=m"(sum), "+a"(left) : [v]"m"(v), [k0]"m"(k0), [k16]"m"(k16), [k512]"m"(k512)
                     : "cc", "memory");
    CHECK(sum == (s64)196605 * n);
    return done((u64)n * 16, "fscale", (u32)sum);
}

/* FNINIT and FNCLEX (DB E3/E2) with a precision exception between them. */
static u32 fpu_init_clex(void) {
    const s32 n = 1024 * HEAVY_SCALE;
    s32 left = n, three = 0;
    u32 raised = 0, cleared = 0;
    __asm__ volatile("1: fninit; fldpi; fistl %[t]; fnstsw %%ax; orl %%eax,%%ebx;"
                     "fnclex; fnstsw %%ax; orl %%eax,%%edx; fstp %%st(0); decl %%ecx; jnz 1b"
                     : [t]"=m"(three), "+b"(raised), "+d"(cleared), "+c"(left) :: "eax", "cc", "memory");
    CHECK_EQ(three, 3);
    raised &= 0xffff;
    CHECK(raised & 0x20);
    CHECK_EQ(cleared & 0x80bf, 0);
    return done((u64)n * 2, "fninit_or_fnclex", raised);
}

/* FNSAVE/FRSTOR and FNSTENV/FLDENV round trips over a full register stack. */
static u32 fpu_ctxsw(void) {
    static u8 area[108] __attribute__((aligned(16)));
    static u32 env[7];
    const s32 n = 512 * HEAVY_SCALE;
    s32 left = n, out[8];
    u32 bad = 0;
    init();
    __asm__ volatile("fld1\n.rept 7\n fld1; fadd %%st(1),%%st\n.endr\n"
                     "1: fnsave %[a]; frstor %[a]; fnstenv %[e]; fldenv %[e]; fadd %%st(7),%%st; decl %%ecx; jnz 1b\n"
                     "fnstenv %[e]\n.irp i,0,4,8,12,16,20,24,28\n fistpl \\i(%[o])\n.endr"
                     : [a]"+m"(area), [e]"+m"(env), "+c"(left) : [o]"r"(out) : "cc", "memory");
    CHECK_EQ(env[0] & 0xffff, 0x037f);
    CHECK_EQ(env[2] & 0xffff, 0);
    for (unsigned i = 1; i < 8; i++) bad += out[i] != 8 - (s32)i;
    CHECK_EQ(bad, 0);
    CHECK_EQ(out[0], 8 + n);
    return done((u64)n * 4, "state_save_or_restore", (u32)out[0]);
}

/* The same exact FDIV loop under precision control 24, 53 and 64 bits. */
static u32 fdiv_loop(u16 cw) {
    static const s32 c3 = 3, c100 = 100, c16 = 16;
    const s32 n = 256 * HEAVY_SCALE;
    s32 left = n, block;
    u32 bad = 0;
    init();
    ctl(cw);
    __asm__ volatile("fld1; fildl %[c3]; fldz; fildl %[c100]\n"
                     "1:\n.rept 16\n fld %%st(0); fmul %%st(3),%%st; fdiv %%st(3),%%st; faddp %%st,%%st(2); fadd %%st(3),%%st\n.endr\n"
                     "fisubl %[c16]; fxch %%st(1); fistpl %[b]; cmpl $1720,%[b]; je 2f; incl %[bad]\n"
                     "2: fldz; fxch %%st(1); fld %%st(3); fadd %%st(0),%%st; faddp %%st,%%st(3); decl %[n]; jnz 1b"
                     : [b]"=m"(block), [bad]"+r"(bad), [n]"+r"(left)
                     : [c3]"m"(c3), [c100]"m"(c100), [c16]"m"(c16) : "cc", "memory");
    CHECK_EQ(bad, 0);
    return done((u64)n * 16, "fdiv", cw);
}
static u32 fpu_fdiv_pc24(void) { return fdiv_loop(0x007f); }
static u32 fpu_fdiv_pc53(void) { return fdiv_loop(0x027f); }
static u32 fpu_fdiv_pc64(void) { return fdiv_loop(0x037f); }

/* Quake-style perspective-correct span segments at 24-bit precision: one
 * FDIV (z = 65536/zi) overlapped with the integer texel fetches of the
 * previous segment, then s = sz*z and t = tz*z by FISTP. */
static u8 span_tex[4096];
static u32 fpu_fdiv_span(void) {
    static const u32 k65536 = 0x47800000, zi0 = 0x3f000000, sz0 = 0x40800000, tz0 = 0x3f800000,
                     dzi = 0x3a800000, dsz = 0x3c800000, dtz = 0x3d000000;
    static const u32 want = 0x1128ef1au; /* host exact-rounded sum of s and t over 256 segments */
    const s32 nb = 8 * HEAVY_SCALE;
    u32 first = 0, first_pix = 0, bad = 0;
    for (u32 i = 0; i < 4096; i++) span_tex[i] = (u8)(i * 0x9E37u >> 7 ^ i);
    init();
    ctl(0x007f);
    for (s32 b = 0; b < nb; b++) {
        s32 s = 0, t = 0, segs = 256;
        u32 sum = 0, pix = 0;
        __asm__ volatile("flds %[tz0]; flds %[sz0]; flds %[zi0]\n"
                         "1: flds %[k]; fdiv %%st(1),%%st\n"
                         "movl %[s],%%eax; movl %[t],%%edx\n"
                         "movl %%eax,%%ebx; shrl $16,%%ebx; andl $63,%%ebx; movl %%edx,%%ecx; shrl $10,%%ecx; andl $0xfc0,%%ecx\n"
                         "addl %%ecx,%%ebx; movzbl (%%esi,%%ebx),%%ecx; addl %%ecx,%[p]; addl $0x8000,%%eax; addl $0x4000,%%edx\n"
                         "movl %%eax,%%ebx; shrl $16,%%ebx; andl $63,%%ebx; movl %%edx,%%ecx; shrl $10,%%ecx; andl $0xfc0,%%ecx\n"
                         "addl %%ecx,%%ebx; movzbl (%%esi,%%ebx),%%ecx; addl %%ecx,%[p]\n"
                         "fld %%st(0); fmul %%st(3),%%st; fxch %%st(1); fmul %%st(4),%%st; fxch %%st(1); fistpl %[s]; fistpl %[t]\n"
                         "fadds %[dzi]; fxch %%st(1); fadds %[dsz]; fxch %%st(2); fadds %[dtz]; fxch %%st(2); fxch %%st(1)\n"
                         "movl %[s],%%eax; addl %[t],%%eax; addl %%eax,%[sum]; decl %[n]; jnz 1b\n"
                         "fstp %%st(0); fstp %%st(0); fstp %%st(0)"
                         : [s]"+m"(s), [t]"+m"(t), [n]"+m"(segs), [sum]"+m"(sum), [p]"+m"(pix)
                         : "S"(span_tex), [k]"m"(k65536), [zi0]"m"(zi0), [sz0]"m"(sz0), [tz0]"m"(tz0),
                           [dzi]"m"(dzi), [dsz]"m"(dsz), [dtz]"m"(dtz)
                         : "eax", "ebx", "ecx", "edx", "cc", "memory");
        if (!b) { first = sum; first_pix = pix; }
        bad += sum != first || pix != first_pix;
    }
    /* At most one unit of rounding difference per conversion. */
    if ((u32)(first - want + 512) > 1024) { t_note("span_sum=%08x want=%08x", first, want); CHECK(0); }
    CHECK_EQ(bad, 0);
    return done((u64)nb * 256, "span_segment", first_pix);
}

/* Float-to-int at 24-bit precision: Direct3D-era _ftol swapping the rounding
 * control around each FISTP, against the magic-number add with no FLDCW. */
static u32 ftol_loop(int magic) {
    static const u32 c75 = 0x3f400000, mg = 0x4b400000; /* 0.75, 1.5*2^23 */
    const s32 n = 2048 * HEAVY_SCALE;
    s32 start = -n, i = -n, left = n / 2, t;
    u32 bad = 0;
    u16 old, forced;
    init();
    ctl(0x007f);
#define FTOL_LOOP(body) "fld1; fildl %[i0]; fadds %[c]\n1:\n.rept 4\n fld %%st(0); " body \
    " je 2f; incl %[bad]\n 2: fadd %%st(1),%%st; incl %%edx\n.endr\ndecl %[n]; jnz 1b; fstp %%st(0); fstp %%st(0)"
    if (magic)
        __asm__ volatile(FTOL_LOOP("fadds %[m]; fstps %[t]; movl %[t],%%eax; subl $0x4b400000,%%eax; leal 1(%%edx),%%ecx; cmpl %%eax,%%ecx;")
                         : [t]"=m"(t), [bad]"+m"(bad), [n]"+m"(left), "+d"(i), [o]"=m"(old), [f]"=m"(forced)
                         : [i0]"m"(start), [c]"m"(c75), [m]"m"(mg) : "eax", "ecx", "cc", "memory");
    else
        __asm__ volatile(FTOL_LOOP("fnstcw %[o]; movzwl %[o],%%eax; orl $0xc00,%%eax; movw %%ax,%[f]; fldcw %[f]; fistpl %[t]; fldcw %[o];"
                                   " movl %%edx,%%ecx; shrl $31,%%ecx; addl %%edx,%%ecx; cmpl %[t],%%ecx;")
                         : [t]"=m"(t), [bad]"+m"(bad), [n]"+m"(left), "+d"(i), [o]"=m"(old), [f]"=m"(forced)
                         : [i0]"m"(start), [c]"m"(c75), [m]"m"(mg) : "eax", "ecx", "cc", "memory");
    CHECK_EQ(bad, 0);
    CHECK_EQ(i, n);
    return done((u64)n * 2, "float_to_int", (u32)t);
}
static u32 fpu_ftol_pc24(void) { return ftol_loop(0); }
static u32 fpu_ftol_magic(void) { return ftol_loop(1); }

/* 4x4 matrix times 64 vec4 vertices, m32 operands, Pentium FXCH scheduling. */
static u32 xf_m[16], xf_v[256], xf_o[256], xf_ref[256];
static u32 i2f(s32 v) {
    u32 a = v < 0 ? -(u32)v : (u32)v, e = 23;
    if (!v) return 0;
    while (!(a >> e)) e--;
    return (v < 0 ? 0x80000000u : 0) | (127 + e) << 23 | ((a << (23 - e)) & 0x7fffff);
}
static u32 fpu_xform4(void) {
    static int ready;
    const s32 n = 8 * HEAVY_SCALE;
    s32 left = n;
    if (!ready) {
        s32 m[16], v[256];
        u32 h = TEST_SEED;
        for (unsigned i = 0; i < 272; i++) {
            s32 x;
            h = h * 1103515245u + 12345u;
            x = i < 16 ? (s32)(h >> 16) % 6 - 3 : (s32)(h >> 16) % 16 - 8;
            x += x >= 0;
            if (i < 16) m[i] = x, xf_m[i] = i2f(x);
            else v[i - 16] = x, xf_v[i - 16] = i2f(x);
        }
        for (unsigned k = 0; k < 64; k++)
            for (unsigned r = 0; r < 4; r++)
                xf_ref[k * 4 + r] = i2f(m[r * 4] * v[k * 4] + m[r * 4 + 1] * v[k * 4 + 1] +
                                        m[r * 4 + 2] * v[k * 4 + 2] + m[r * 4 + 3] * v[k * 4 + 3]);
        ready = 1;
    }
    init();
    __asm__ volatile("1: movl $64,%%ecx\n2:\n.irp r,0,1,2,3\n"
                     " flds (%%esi); fmuls \\r*16(%%ebx); flds 4(%%esi); fmuls \\r*16+4(%%ebx);"
                     " flds 8(%%esi); fmuls \\r*16+8(%%ebx); fxch %%st(2); faddp %%st,%%st(1);"
                     " flds 12(%%esi); fmuls \\r*16+12(%%ebx); fxch %%st(2); faddp %%st,%%st(1); faddp %%st,%%st(1);"
                     " fstps \\r*4(%%edi)\n.endr\n"
                     "addl $16,%%esi; addl $16,%%edi; decl %%ecx; jnz 2b\n"
                     "subl $1024,%%esi; subl $1024,%%edi; decl %%eax; jnz 1b"
                     : "+a"(left) : "b"(xf_m), "S"(xf_v), "D"(xf_o) : "ecx", "cc", "memory");
    CHECK_EQ(kmemcmp(xf_o, xf_ref, sizeof xf_o), 0);
    return done((u64)n * 64 * 28, "flop", xf_o[255]);
}

/* FILD/FCHS/FISTP ping-pong over 64 integers of each width. */
static u32 iconv_val(unsigned i, unsigned bits) {
    u32 h = (TEST_SEED + i * 0x9E3779B1u) * 0xC2B2AE35u;
    return bits == 16 ? (u32)((s32)(h % 65535) - 32767) : h == 0x80000000u ? 0 : h;
}
#define ICONV(fn, T, BITS, LD, ST, SC) static u32 fn(void) { \
    static T a[64], b[64]; \
    const s32 n = 64 * HEAVY_SCALE; s32 left = n; u32 bad = 0; T *src = a, *dst = b; \
    for (unsigned i = 0; i < 64; i++) a[i] = BITS == 64 ? (T)(s32)iconv_val(i, 32) * 2097152 : (T)(s32)iconv_val(i, BITS); \
    init(); \
    __asm__ volatile("1: movl $60,%%ecx\n2:\n.irp o,0,1,2,3\n " LD " \\o*" #SC "(%%esi,%%ecx," #SC "); fchs; " \
                     ST " \\o*" #SC "(%%edi,%%ecx," #SC ")\n.endr\nsubl $4,%%ecx; jns 2b; xchgl %%esi,%%edi; decl %%eax; jnz 1b" \
                     : "+S"(src), "+D"(dst), "+a"(left) :: "ecx", "cc", "memory"); \
    for (unsigned i = 0; i < 64; i++) bad += b[i] != -a[i] || a[i] != (BITS == 64 ? (T)(s32)iconv_val(i, 32) * 2097152 : (T)(s32)iconv_val(i, BITS)); \
    CHECK_EQ(bad, 0); \
    return done((u64)n * 128, "int_load_or_store", (u32)b[63]); }
ICONV(fpu_int_conv_m16, s16, 16, "filds", "fistps", 2)
ICONV(fpu_int_conv_m32, s32, 32, "fildl", "fistpl", 4)
ICONV(fpu_int_conv_m64, s64, 64, "fildll", "fistpll", 8)

/* Single-precision one-pole filter: an impulse of 2^-120 decays through
 * the float denormal range to zero; #D and #U stay masked. */
static u32 da_x[32], da_y[32];
static u32 fpu_denormal_audio(void) {
    static const u32 half = 0x3f000000;
    const s32 n = 256 * HEAVY_SCALE;
    s32 left = n;
    u32 bad = 0;
    u16 sw;
    da_x[0] = 0x03800000;
    init();
    __asm__ volatile("flds %[h]; fldz\n1: xorl %%ecx,%%ecx\n2:\n.irp o,0,4,8,12\n"
                     " fmul %%st(1),%%st; fadds \\o(%%esi,%%ecx,4); fstps \\o(%%edi,%%ecx,4); flds \\o(%%edi,%%ecx,4)\n"
                     ".endr\naddl $4,%%ecx; cmpl $32,%%ecx; jne 2b; decl %%eax; jnz 1b\n"
                     "fnstsw %[sw]; fstp %%st(0); fstp %%st(0)"
                     : [sw]"=m"(sw), "+a"(left) : [h]"m"(half), "S"(da_x), "D"(da_y) : "ecx", "cc", "memory");
    for (unsigned i = 0; i < 32; i++)
        bad += da_y[i] != (i < 7 ? (7 - i) << 23 : i < 30 ? 1u << (29 - i) : 0);
    CHECK_EQ(bad, 0);
    t_note("status=%04x", sw);
    return done((u64)n * 32, "sample", sw);
}

/* Unmasked zero-divide delivered as #MF (CR0.NE=1) at the following FWAIT;
 * the handler clears the exception and FWAIT restarts. */
static volatile u32 mf_count;
static void mf_hook(regs_t *r) { (void)r; __asm__ volatile("fnclex"); mf_count++; }
static u32 fpu_mf_ne(void) {
    const s32 n = 256 * HEAVY_SCALE;
    s32 left = n;
    if (!(rd_cr0() & 0x20)) SKIP("CR0.NE is clear");
    init();
    mf_count = 0;
    int_hook[16] = mf_hook;
    ctl(0x037b);
    __asm__ volatile("fld1; fldz\n1: fld %%st(1); fdiv %%st(1),%%st; fwait; fstp %%st(0); decl %%ecx; jnz 1b\n"
                     "fstp %%st(0); fstp %%st(0)" : "+c"(left) :: "cc", "memory");
    int_hook[16] = 0;
    CHECK_EQ(mf_count, n);
    return done((u64)n, "fpu_exception", mf_count);
}

/* The same exception with CR0.NE=0: FERR# raises IRQ13. The handler writes
 * port F0h and clears the status word with FNSTENV/FLDENV, as DOS-era
 * handlers did; QEMU deasserts IGNNE# only when the status word is
 * reloaded, so FNCLEX alone would lose every IRQ after the first. */
static volatile u32 irq13_count;
static u32 irq13_n, irq13_lost;
static void ferr_ack(void) {
    u32 env[7];
    outb(0xf0, 0);
    __asm__ volatile("fnstenv %0" : "=m"(env));
    env[1] &= 0x7f00;
    __asm__ volatile("fldenv %0" :: "m"(env));
}
static void irq13_hook(regs_t *r) { (void)r; ferr_ack(); irq13_count++; }
static void irq13_body(void) {
    __asm__ volatile("fld1; fldz" ::: "memory");
    for (u32 i = 0; i < irq13_n; i++) {
        u32 before = irq13_count, spin = 0;
        __asm__ volatile("fld %%st(1); fdiv %%st(1),%%st; fwait; fstp %%st(0)" ::: "memory");
        while (irq13_count == before && ++spin < 1000000) __asm__ volatile("" ::: "memory");
        if (irq13_count == before) { irq13_lost++; return; }
    }
}
static u32 fpu_mf_irq13(void) {
    u32 cr0 = rd_cr0();
    u8 master = inb(0x21), slave = inb(0xa1);
    int rc;
    irq13_n = 64 * HEAVY_SCALE;
    irq13_count = irq13_lost = 0;
    init();
    ctl(0x037b);
    int_hook[0x2d] = irq13_hook;
    wr_cr0(cr0 & ~0x20u);
    outb(0xa1, slave & ~0x20);
    outb(0x21, master & ~0x04);
    rc = guard_call(irq13_body);
    outb(0x21, master);
    outb(0xa1, slave);
    wr_cr0(cr0);
    ferr_ack();
    int_hook[0x2d] = 0;
    init();
    CHECK_EQ(rc, GUARD_OK);
    CHECK_EQ(irq13_lost, 0);
    CHECK_EQ(irq13_count, irq13_n);
    return done(irq13_n, "fpu_exception", irq13_count);
}

/* 8087-style code: FWAIT before every x87 instruction (sum of squares). */
static u32 fpu_fwait(void) {
    static s32 x[64];
    const s32 n = 64 * HEAVY_SCALE;
    s32 left = n, sum, want = 0;
    for (s32 i = 0; i < 64; i++) x[i] = (i * 29) % 127 - 63, want += x[i] * x[i];
    init();
    __asm__ volatile("fwait; fldz\n1: movl $60,%%ecx\n2:\n.irp o,0,4,8,12\n"
                     " fwait; fildl \\o(%%esi,%%ecx,4); fwait; fmul %%st(0),%%st; fwait; faddp %%st,%%st(1)\n"
                     ".endr\nsubl $4,%%ecx; jns 2b; decl %%eax; jnz 1b; fwait; fistpl %[s]"
                     : [s]"=m"(sum), "+a"(left) : "S"(x) : "ecx", "cc", "memory");
    CHECK_EQ(sum, want * n);
    return done((u64)n * 64 * 3, "waited_x87_instruction", (u32)sum);
}

const test_t tests_fpu_ext[] = {
    {"fpu_m80_move", fpu_m80_move, REQ_FPU, 0}, {"fpu_fcomi_branch", fpu_fcomi_branch, REQ_FPU | REQ_CMOV, 0},
    {"fpu_fnstsw_sahf", fpu_fnstsw_sahf, REQ_FPU, 0}, {"fpu_fcmov", fpu_fcmov, REQ_FPU | REQ_CMOV, 0},
    {"fpu_frndint", fpu_frndint, REQ_FPU, 0}, {"fpu_frndint_floor", fpu_frndint_floor, REQ_FPU, 0},
    {"fpu_const", fpu_const, REQ_FPU, 0}, {"fpu_trans_sin", fpu_trans_sin, REQ_FPU, 0},
    {"fpu_trans_sincos", fpu_trans_sincos, REQ_FPU, 0}, {"fpu_trans_ptan", fpu_trans_ptan, REQ_FPU, 0},
    {"fpu_trans_patan", fpu_trans_patan, REQ_FPU, 0}, {"fpu_trans_yl2x", fpu_trans_yl2x, REQ_FPU, 0},
    {"fpu_trans_2xm1", fpu_trans_2xm1, REQ_FPU, 0}, {"fpu_trans_scale", fpu_trans_scale, REQ_FPU, 0},
    {"fpu_init_clex", fpu_init_clex, REQ_FPU, 0}, {"fpu_ctxsw", fpu_ctxsw, REQ_FPU, 0},
    {"fpu_fdiv_pc24", fpu_fdiv_pc24, REQ_FPU, 0}, {"fpu_fdiv_pc53", fpu_fdiv_pc53, REQ_FPU, 0},
    {"fpu_fdiv_pc64", fpu_fdiv_pc64, REQ_FPU, 0}, {"fpu_fdiv_span", fpu_fdiv_span, REQ_FPU, 0},
    {"fpu_ftol_pc24", fpu_ftol_pc24, REQ_FPU, 0}, {"fpu_ftol_magic", fpu_ftol_magic, REQ_FPU, 0},
    {"fpu_xform4", fpu_xform4, REQ_FPU, 0}, {"fpu_int_conv_m16", fpu_int_conv_m16, REQ_FPU, 0},
    {"fpu_int_conv_m32", fpu_int_conv_m32, REQ_FPU, 0}, {"fpu_int_conv_m64", fpu_int_conv_m64, REQ_FPU, 0},
    {"fpu_denormal_audio", fpu_denormal_audio, REQ_FPU, 0}, {"fpu_mf_ne", fpu_mf_ne, REQ_FPU | REQ_486, 0},
    {"fpu_mf_irq13", fpu_mf_irq13, REQ_FPU | REQ_486, 0}, {"fpu_fwait", fpu_fwait, REQ_FPU, 0},
    {0, 0, 0, 0}
};
