/* Timed MMX, MMX extension and 3DNow! records aimed at individual 86Box
 * paths (doc/workloads.md, "Targeted x87 and MMX records"). Each checks its
 * output against an integer C reference built once and cached. */
#include "kernel.h"

static u32 mix(u32 *h) { *h = *h * 1103515245u + 12345u; return *h ^ *h >> 15; }
static u32 done(u64 ops, const char *unit, u32 sum) { t_ops(ops); t_work_unit(unit); return sum; }

/* PSRL/PSRA/PSLL by MMX register count (0F D1-D3, E1-E2, F1-F3) and by
 * memory count, for counts 3, 9 and 17 (17 empties or sign-fills words). */
#define SHIFT_OPS(X) X(psrlw) X(psrld) X(psrlq) X(psraw) X(psrad) X(psllw) X(pslld) X(psllq)
#define SHIFT_ASM(op) "movq %%mm2,%%mm0; " #op " %%mm6,%%mm0; paddd %%mm0,%%mm1; movq %%mm2,%%mm0; " #op \
    " %%mm7,%%mm0; paddd %%mm0,%%mm1; movq %%mm2,%%mm0; " #op " %[c17],%%mm0; paddd %%mm0,%%mm1\n"
static u64 mshift(u64 v, unsigned k, unsigned c) {
    unsigned w = k == 2 || k == 7 ? 64 : k == 1 || k == 4 || k == 6 ? 32 : 16;
    u64 mask = w == 64 ? ~0ull : (1ull << w) - 1, r = 0;
    for (unsigned b = 0; b < 64; b += w) {
        u64 e = v >> b & mask, sign = (k == 3 || k == 4) && e >> (w - 1) ? mask : 0;
        if (k >= 5) e = c >= w ? 0 : e << c & mask;
        else e = c >= w ? sign : (e >> c | sign << (w - c)) & mask;
        r |= e << b;
    }
    return r;
}
static u32 mmx_shift_reg(void) {
    static const u64 c3 = 3, c9 = 9, c17 = 17;
    static u64 in[8], want;
    const s32 n = 32 * HEAVY_SCALE;
    s32 left = n;
    u64 acc;
    if (!want) {
        u32 h = TEST_SEED, lo = 0, hi = 0;
        for (unsigned i = 0; i < 8; i++) {
            in[i] = (u64)mix(&h) << 32 | mix(&h);
            for (unsigned k = 0; k < 8; k++)
                for (unsigned c = 3; c < 18; c += c == 3 ? 6 : 8) {
                    u64 r = mshift(in[i], k, c);
                    lo += (u32)r; hi += (u32)(r >> 32);
                }
        }
        want = (u64)(hi * n) << 32 | lo * n;
    }
    __asm__ volatile("movq %[c3],%%mm6; movq %[c9],%%mm7; pxor %%mm1,%%mm1\n"
                     "1: movl $7,%%ecx\n2: movq (%%esi,%%ecx,8),%%mm2\n" SHIFT_OPS(SHIFT_ASM)
                     "decl %%ecx; jns 2b; decl %%eax; jnz 1b; movq %%mm1,%[acc]; emms"
                     : [acc]"=m"(acc), "+a"(left) : "S"(in), [c3]"m"(c3), [c9]"m"(c9), [c17]"m"(c17)
                     : "ecx", "cc", "memory");
    CHECK_EQ((u32)acc, (u32)want);
    CHECK_EQ((u32)(acc >> 32), (u32)(want >> 32));
    return done((u64)n * 8 * 24, "mmx_shift", (u32)acc);
}

/* Motion estimation: 8x8 SAD at 64 full-pel and 64 PAVGB half-pel offsets
 * in a 16x16 window, best match by PSHUFW broadcast, PCMPGTW and PMOVMSKB. */
static u8 me_cur[64], me_ref[16 * 17];
static u32 mmx_ext_kernel(void) {
    static const u64 init = 0x7fff7fff7fff7fffull;
    static u32 best_want, idx_want, total_want;
    const s32 n = 4 * HEAVY_SCALE;
    s32 left = n;
    u32 best, idx = 0, total;
    if (!total_want) {
        u32 h = TEST_SEED;
        best_want = 0x7fff;
        for (unsigned i = 0; i < sizeof me_ref; i++) me_ref[i] = (u8)mix(&h);
        for (unsigned i = 0; i < 64; i++) me_cur[i] = (u8)(me_ref[(3 + i / 8) * 16 + 5 + i % 8] + (mix(&h) & 3));
        /* Candidate order and strict improvement match the guest loop. */
        for (unsigned c = 0; c < 128; c++) {
            const u8 *r = me_ref + (c >> 1 & 56) * 2 + (c >> 1 & 7);
            u32 sad = 0;
            for (unsigned i = 0; i < 64; i++) {
                const u8 *q = r + i / 8 * 16 + i % 8;
                s32 p = c & 1 ? (q[0] + q[1] + 1) >> 1 : q[0];
                sad += p > me_cur[i] ? p - me_cur[i] : me_cur[i] - p;
            }
            total_want += sad;
            if (sad < best_want) best_want = sad, idx_want = (c >> 1) + 64 * (c & 1);
        }
        total_want *= n;
    }
    __asm__ volatile("pxor %%mm6,%%mm6\n1: xorl %%ecx,%%ecx; movq %[init],%%mm7\n"
                     "2: movl %%ecx,%%edi; andl $7,%%edi; movl %%ecx,%%eax; andl $56,%%eax; leal (%%edi,%%eax,2),%%edi; addl %[ref],%%edi\n"
                     "pxor %%mm0,%%mm0; pxor %%mm1,%%mm1\n.irp r,0,1,2,3,4,5,6,7\n"
                     " movq \\r*8(%%esi),%%mm2; movq \\r*16(%%edi),%%mm3; movq \\r*16+1(%%edi),%%mm4; pavgb %%mm3,%%mm4;"
                     " psadbw %%mm2,%%mm3; psadbw %%mm2,%%mm4; paddw %%mm3,%%mm0; paddw %%mm4,%%mm1\n.endr\n"
                     "paddd %%mm0,%%mm6; paddd %%mm1,%%mm6\n"
                     "pshufw $0,%%mm0,%%mm0; movq %%mm7,%%mm5; pcmpgtw %%mm0,%%mm5; pmovmskb %%mm5,%%eax; testl %%eax,%%eax; jz 3f\n"
                     "movq %%mm0,%%mm7; movl %%ecx,%%edx\n"
                     "3: pshufw $0,%%mm1,%%mm1; movq %%mm7,%%mm5; pcmpgtw %%mm1,%%mm5; pmovmskb %%mm5,%%eax; testl %%eax,%%eax; jz 4f\n"
                     "movq %%mm1,%%mm7; leal 64(%%ecx),%%edx\n"
                     "4: incl %%ecx; cmpl $64,%%ecx; jne 2b; decl %[n]; jnz 1b\n"
                     "movd %%mm7,%%eax; movd %%mm6,%%ecx; emms"
                     : "=a"(best), "=c"(total), "+d"(idx), [n]"+m"(left)
                     : "S"(me_cur), [ref]"g"(me_ref), [init]"m"(init) : "edi", "cc", "memory");
    CHECK_EQ(best & 0xffff, best_want);
    CHECK_EQ(idx, idx_want);
    CHECK_EQ(total, total_want);
    return done((u64)n * 128, "sad_candidate", total);
}

/* 3DNow!: vec4 dot products by PFMUL/PFADD/PFACC/PF2ID, plus PMULHRW and
 * PAVGUSB on packed integers. Small integer values keep every float exact. */
static u32 f32_of(s32 v) {
    u32 a = v < 0 ? -(u32)v : (u32)v, e = 23;
    if (!v) return 0;
    while (!(a >> e)) e--;
    return (v < 0 ? 0x80000000u : 0) | (127 + e) << 23 | ((a << (23 - e)) & 0x7fffff);
}
static u32 now_v[64][4], now_b[4];
static u64 now_i[256]; /* words, word multipliers, bytes, byte partners */
static u32 now_dot(void) {
    static u64 want[3];
    static int ready;
    const s32 n = 32 * HEAVY_SCALE;
    s32 left = n;
    u64 got[3];
    if (!ready) {
        u32 h = TEST_SEED, dot = 0;
        s32 b[4];
        u16 w[4] = {0};
        u8 by[8] = {0};
        for (unsigned j = 0; j < 4; j++) b[j] = (s32)(mix(&h) % 31) - 15, now_b[j] = f32_of(b[j]);
        for (unsigned k = 0; k < 64; k++)
            for (unsigned j = 0; j < 4; j++) {
                s32 v = (s32)(mix(&h) % 127) - 63;
                now_v[k][j] = f32_of(v);
                dot += (u32)(v * b[j]);
            }
        for (unsigned k = 0; k < 256; k++) now_i[k] = (u64)mix(&h) << 32 | mix(&h);
        for (unsigned k = 0; k < 64; k++)
            for (unsigned j = 0; j < 8; j++) {
                if (j < 4) w[j] += (u16)(((s32)(s16)(now_i[k] >> 16 * j) * (s16)(now_i[k + 64] >> 16 * j) + 0x8000) >> 16);
                by[j] += (u8)(((u8)(now_i[k + 128] >> 8 * j) + (u8)(now_i[k + 192] >> 8 * j) + 1) >> 1);
            }
        want[0] = (u64)(dot * n) << 32 | dot * n;
        for (unsigned j = 0; j < 4; j++) want[1] |= (u64)(u16)(w[j] * n) << 16 * j;
        for (unsigned j = 0; j < 8; j++) want[2] |= (u64)(u8)(by[j] * n) << 8 * j;
        ready = 1;
    }
    __asm__ volatile("movq %[b],%%mm6; movq %[b]+8,%%mm7; pxor %%mm5,%%mm5; pxor %%mm4,%%mm4; pxor %%mm3,%%mm3\n"
                     "1: xorl %%ecx,%%ecx\n2:\n.irp k,0,1,2,3\n"
                     " movq \\k*16(%%esi,%%ecx,8),%%mm0; pfmul %%mm6,%%mm0; movq \\k*16+8(%%esi,%%ecx,8),%%mm1; pfmul %%mm7,%%mm1;"
                     " pfadd %%mm1,%%mm0; pfacc %%mm0,%%mm0; pf2id %%mm0,%%mm0; paddd %%mm0,%%mm5\n"
                     " movq \\k*8(%%edi,%%ecx,4),%%mm1; pmulhrw \\k*8+512(%%edi,%%ecx,4),%%mm1; paddw %%mm1,%%mm4;"
                     " movq \\k*8+1024(%%edi,%%ecx,4),%%mm2; pavgusb \\k*8+1536(%%edi,%%ecx,4),%%mm2; paddb %%mm2,%%mm3\n"
                     ".endr\naddl $8,%%ecx; cmpl $128,%%ecx; jne 2b; decl %%eax; jnz 1b\n"
                     "movq %%mm5,%[g0]; movq %%mm4,%[g1]; movq %%mm3,%[g2]; femms"
                     : [g0]"=m"(got[0]), [g1]"=m"(got[1]), [g2]"=m"(got[2]), "+a"(left) : "S"(now_v), "D"(now_i), [b]"m"(now_b) : "ecx", "cc", "memory");
    CHECK_EQ((u32)got[0], (u32)want[0]); CHECK_EQ((u32)(got[0] >> 32), (u32)(want[0] >> 32));
    CHECK_EQ((u32)got[1], (u32)want[1]); CHECK_EQ((u32)(got[1] >> 32), (u32)(want[1] >> 32));
    CHECK_EQ((u32)got[2], (u32)want[2]); CHECK_EQ((u32)(got[2] >> 32), (u32)(want[2] >> 32));
    return done((u64)n * 64, "vertex", (u32)got[0]);
}

/* Extended 3DNow!: complex multiply by PI2FW, PSWAPD, PFMUL and PFPNACC,
 * PFNACC differences, and PF2IW back to integers. */
static u32 now_c[128][2]; /* low words are the signed inputs; high words are noise PI2FW ignores */
static u32 now_ext(void) {
    static u64 want[2];
    static int ready;
    const s32 n = 64 * HEAVY_SCALE;
    s32 left = n;
    u64 got[2];
    if (!ready) {
        u32 h = TEST_SEED ^ 0x5a5a5a5au, re = 0, im = 0, diff = 0;
        for (unsigned k = 0; k < 128; k++)
            for (unsigned j = 0; j < 2; j++)
                now_c[k][j] = (mix(&h) & 0xffff0000u) | (u16)((s32)(mix(&h) % 201) - 100);
        for (unsigned k = 0; k < 64; k++) {
            s32 ar = (s16)now_c[2 * k][0], ai = (s16)now_c[2 * k][1], br = (s16)now_c[2 * k + 1][0], bi = (s16)now_c[2 * k + 1][1];
            re += (u32)(ar * br - ai * bi); im += (u32)(ar * bi + ai * br); diff += (u32)(br - bi);
        }
        want[0] = (u64)(im * n) << 32 | re * n;
        want[1] = (u64)(diff * n) << 32 | diff * n;
        ready = 1;
    }
    __asm__ volatile("pxor %%mm5,%%mm5; pxor %%mm4,%%mm4\n1: xorl %%ecx,%%ecx\n2:\n.irp k,0,16,32,48\n"
                     " pi2fw \\k(%%esi,%%ecx,8),%%mm0; pi2fw \\k+8(%%esi,%%ecx,8),%%mm1; pswapd %%mm1,%%mm2; movq %%mm0,%%mm3;"
                     " pfmul %%mm1,%%mm3; pfmul %%mm2,%%mm0; pfpnacc %%mm0,%%mm3; pf2iw %%mm3,%%mm3; paddd %%mm3,%%mm5;"
                     " pfnacc %%mm1,%%mm1; pf2iw %%mm1,%%mm1; paddd %%mm1,%%mm4\n"
                     ".endr\naddl $8,%%ecx; cmpl $128,%%ecx; jne 2b; decl %%eax; jnz 1b\n"
                     "movq %%mm5,%[g0]; movq %%mm4,%[g1]; femms"
                     : [g0]"=m"(got[0]), [g1]"=m"(got[1]), "+a"(left) : "S"(now_c) : "ecx", "cc", "memory");
    CHECK_EQ((u32)got[0], (u32)want[0]); CHECK_EQ((u32)(got[0] >> 32), (u32)(want[0] >> 32));
    CHECK_EQ((u32)got[1], (u32)want[1]); CHECK_EQ((u32)(got[1] >> 32), (u32)(want[1] >> 32));
    return done((u64)n * 64, "complex_multiply", (u32)got[0]);
}

/* IDCT-style row transform: duplicated input pairs, 16 PMADDWD per row,
 * rounding, PSRAD 11 and PACKSSDW. */
static s16 idct_tab[64], idct_in[512], idct_out[512], idct_ref[512];
static u32 mmx_idct_row(void) {
    static const u32 round[2] = {1024, 1024};
    static int ready;
    const s32 n = 16 * HEAVY_SCALE;
    s32 left = n;
    if (!ready) {
        static const s16 cosine[9] = {2048, 2009, 1892, 1703, 1448, 1138, 784, 400, 0};
        u32 h = TEST_SEED;
        s16 c[8][8];
        for (unsigned j = 0; j < 8; j++)
            for (unsigned i = 0; i < 8; i++) {
                unsigned m = (2 * i + 1) * j % 32;
                s32 sign = 1;
                if (m > 16) m = 32 - m;
                if (m > 8) m = 16 - m, sign = -1;
                c[j][i] = (s16)(j ? sign * cosine[m] : 1448);
            }
        for (unsigned p = 0; p < 4; p++)
            for (unsigned q = 0; q < 4; q++) {
                s16 *t = idct_tab + p * 16 + q * 4;
                t[0] = c[2 * p][2 * q]; t[1] = c[2 * p][2 * q + 1]; t[2] = c[2 * p + 1][2 * q]; t[3] = c[2 * p + 1][2 * q + 1];
            }
        for (unsigned i = 0; i < 512; i++) idct_in[i] = (s16)((s32)(mix(&h) % 4095) - 2047);
        for (unsigned r = 0; r < 64; r++)
            for (unsigned j = 0; j < 8; j++) {
                s32 v = 1024;
                for (unsigned i = 0; i < 8; i++) v += c[j][i] * idct_in[r * 8 + i];
                v >>= 11;
                idct_ref[r * 8 + j] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        ready = 1;
    }
#define IDCT_PAIR(p, R) " movq %%mm1," R "; pmaddwd " #p "*32(%%ebx)," R "; movq %%mm0,%%mm5; pmaddwd " #p "*32+8(%%ebx),%%mm5;" \
    " paddd %%mm5," R "; movq %%mm3,%%mm5; pmaddwd " #p "*32+16(%%ebx),%%mm5; paddd %%mm5," R ";" \
    " movq %%mm2,%%mm5; pmaddwd " #p "*32+24(%%ebx),%%mm5; paddd %%mm5," R "; paddd %%mm7," R "; psrad $11," R "\n"
    __asm__ volatile("movq %[rd],%%mm7\n1: movl $64,%%ecx\n"
                     "2: movq (%%esi),%%mm0; movq %%mm0,%%mm1; punpckldq %%mm1,%%mm1; punpckhdq %%mm0,%%mm0\n"
                     "movq 8(%%esi),%%mm2; movq %%mm2,%%mm3; punpckldq %%mm3,%%mm3; punpckhdq %%mm2,%%mm2\n"
                     IDCT_PAIR(0, "%%mm6") IDCT_PAIR(1, "%%mm4") "packssdw %%mm4,%%mm6; movq %%mm6,(%%edi)\n"
                     IDCT_PAIR(2, "%%mm6") IDCT_PAIR(3, "%%mm4") "packssdw %%mm4,%%mm6; movq %%mm6,8(%%edi)\n"
                     "addl $16,%%esi; addl $16,%%edi; decl %%ecx; jnz 2b\n"
                     "subl $1024,%%esi; subl $1024,%%edi; decl %%eax; jnz 1b; emms"
                     : "+a"(left) : "b"(idct_tab), "S"(idct_in), "D"(idct_out), [rd]"m"(round) : "ecx", "cc", "memory");
    CHECK_EQ(kmemcmp(idct_out, idct_ref, sizeof idct_out), 0);
    return done((u64)n * 64, "row", (u32)idct_out[511]);
}

/* YUV 4:2:2 to planar RGB: 4 pixels per step from Y0-Y3 U0 U1 V0 V1. */
static u8 yuv_in[512], yuv_out[1024], yuv_ref[1024];
static s32 hi16(s32 a, s32 b) { return (a * b) >> 16; }
static u8 sat8(s32 v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }
static u32 mmx_yuv_rgb(void) {
    static const u64 k16 = 0x0010001000100010ull, k128 = 0x0080008000800080ull, ky = 0x4a7f4a7f4a7f4a7full,
        kvr = 0x6625662566256625ull, kug = 0x1906190619061906ull, kvg = 0x3408340834083408ull, kub = 0x4093409340934093ull;
    static int ready;
    const s32 n = 32 * HEAVY_SCALE;
    s32 left = n;
    if (!ready) {
        u32 h = TEST_SEED;
        for (unsigned i = 0; i < 512; i++) yuv_in[i] = (u8)mix(&h);
        for (unsigned g = 0; g < 64; g++)
            for (unsigned p = 0; p < 4; p++) {
                const u8 *s = yuv_in + g * 8;
                s32 y = hi16((s[p] - 16) * 64, 19071), u = s[4 + p / 2] - 128, v = s[6 + p / 2] - 128;
                yuv_ref[g * 16 + p] = sat8((y + hi16(v * 64, 26149)) >> 4);
                yuv_ref[g * 16 + 4 + p] = sat8((y - (hi16(u * 64, 6406) + hi16(v * 64, 13320))) >> 4);
                yuv_ref[g * 16 + 8 + p] = sat8((y + hi16(u * 128, 16531)) >> 4);
            }
        ready = 1;
    }
    __asm__ volatile("pxor %%mm7,%%mm7\n1: movl $64,%%ecx\n2:\n"
                     "movd (%%esi),%%mm0; punpcklbw %%mm7,%%mm0; psubw %[k16],%%mm0; psllw $6,%%mm0; pmulhw %[ky],%%mm0\n"
                     "movd 4(%%esi),%%mm1; punpcklbw %%mm7,%%mm1; psubw %[k128],%%mm1; movq %%mm1,%%mm2; punpcklwd %%mm1,%%mm1; punpckhwd %%mm2,%%mm2\n"
                     "movq %%mm1,%%mm3; psllw $7,%%mm3; pmulhw %[kub],%%mm3; paddw %%mm0,%%mm3; psraw $4,%%mm3\n"
                     "psllw $6,%%mm1; pmulhw %[kug],%%mm1; psllw $6,%%mm2; movq %%mm2,%%mm4; pmulhw %[kvr],%%mm4; paddw %%mm0,%%mm4; psraw $4,%%mm4\n"
                     "pmulhw %[kvg],%%mm2; paddw %%mm2,%%mm1; psubw %%mm1,%%mm0; psraw $4,%%mm0\n"
                     "packuswb %%mm4,%%mm4; movd %%mm4,(%%edi); packuswb %%mm0,%%mm0; movd %%mm0,4(%%edi); packuswb %%mm3,%%mm3; movd %%mm3,8(%%edi)\n"
                     "addl $8,%%esi; addl $16,%%edi; decl %%ecx; jnz 2b\n"
                     "subl $512,%%esi; subl $1024,%%edi; decl %%eax; jnz 1b; emms"
                     : "+a"(left) : "S"(yuv_in), "D"(yuv_out), [k16]"m"(k16), [k128]"m"(k128), [ky]"m"(ky),
                       [kvr]"m"(kvr), [kug]"m"(kug), [kvg]"m"(kvg), [kub]"m"(kub) : "ecx", "cc", "memory");
    CHECK_EQ(kmemcmp(yuv_out, yuv_ref, sizeof yuv_out), 0);
    return done((u64)n * 256, "pixel", *(u32 *)yuv_out);
}

/* MOVD r32 <-> mm round trips in a dependent integer/MMX chain. */
static u32 mmx_movd_pingpong(void) {
    static const u32 k1 = 0x9e3779b9u, k2 = 0x7f4a7c15u;
    static u32 want;
    const s32 n = 256 * HEAVY_SCALE;
    s32 left = n;
    u32 x = TEST_SEED;
    if (!want) {
        u32 v = TEST_SEED;
        for (s32 i = 0; i < n * 16; i++) {
            u32 d = v + k1;
            d = d << 5 | d >> 27;
            v = (d ^ k2) + d;
        }
        want = v;
    }
    __asm__ volatile("movd %[k1],%%mm6; movd %[k2],%%mm7\n1:\n.rept 16\n"
                     " movd %%eax,%%mm0; paddd %%mm6,%%mm0; movd %%mm0,%%edx; roll $5,%%edx; movd %%edx,%%mm1; pxor %%mm7,%%mm1;"
                     " movd %%mm1,%%eax; addl %%edx,%%eax\n.endr\ndecl %%ecx; jnz 1b; emms"
                     : "+a"(x), "+c"(left) : [k1]"m"(k1), [k2]"m"(k2) : "edx", "cc", "memory");
    CHECK_EQ(x, want);
    return done((u64)n * 64, "movd", x);
}

/* MMX block, EMMS, x87 block, repeated: two state switches per step. The
 * x87 pushes land in mm7/mm6, so the MMX accumulator lives in mm1. */
static u32 mmx_x87_switch(void) {
    static const u64 a = 0x0003fffe00070001ull, b = 0x0002000300fb0004ull;
    static const s32 three = 3;
    const s32 n = 256 * HEAVY_SCALE;
    s32 left = n, acc = 0;
    u64 got, want = 0;
    for (unsigned j = 0; j < 4; j++) {
        u16 s = (u16)((u16)(a >> 16 * j) + (u16)(b >> 16 * j));
        want |= (u64)(u16)((u32)(u16)(s * s) * 4 * (u32)n) << 16 * j;
    }
    __asm__ volatile("fninit; pxor %%mm1,%%mm1\n1:\n.rept 4\n"
                     " movq %[a],%%mm0; paddw %[b],%%mm0; pmullw %%mm0,%%mm0; paddw %%mm0,%%mm1; emms;"
                     " fildl %[acc]; fildl %[k]; fmul %%st(0),%%st; faddp %%st,%%st(1); fistpl %[acc]\n.endr\n"
                     "decl %%ecx; jnz 1b; movq %%mm1,%[g]; emms"
                     : [acc]"+m"(acc), [g]"=m"(got), "+c"(left) : [a]"m"(a), [b]"m"(b), [k]"m"(three) : "cc", "memory");
    CHECK_EQ(acc, 36 * n);
    CHECK_EQ((u32)got, (u32)want);
    CHECK_EQ((u32)(got >> 32), (u32)(want >> 32));
    return done((u64)n * 8, "switch", (u32)got ^ (u32)acc);
}

const test_t tests_mmx_ext[] = {
    {"mmx_shift_reg", mmx_shift_reg, REQ_MMX, 0},
    {"mmx_ext_kernel", mmx_ext_kernel, REQ_MMX | REQ_MMXEXT, 0},
    {"3dnow_dot", now_dot, REQ_MMX | REQ_3DNOW, 0},
    {"3dnow_ext_dot", now_ext, REQ_MMX | REQ_3DNOWEXT, 0},
    {"mmx_idct_row", mmx_idct_row, REQ_MMX, 0},
    {"mmx_yuv_rgb", mmx_yuv_rgb, REQ_MMX, 0},
    {"mmx_movd_pingpong", mmx_movd_pingpong, REQ_MMX, 0},
    {"mmx_x87_switch", mmx_x87_switch, REQ_MMX | REQ_FPU, T_TAG_X87},
    {0, 0, 0, 0}
};
