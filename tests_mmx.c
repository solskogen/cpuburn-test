/* Baseline Pentium-MMX; MMX extensions have their own requirement gate. */
#include "kernel.h"

#define BINARY_OPS(X) \
 X(packsswb) X(packssdw) X(packuswb) \
 X(punpcklbw) X(punpcklwd) X(punpckldq) X(punpckhbw) X(punpckhwd) X(punpckhdq) \
 X(paddb) X(paddw) X(paddd) X(psubb) X(psubw) X(psubd) \
 X(paddsb) X(paddsw) X(paddusb) X(paddusw) X(psubsb) X(psubsw) X(psubusb) X(psubusw) \
 X(pmullw) X(pmulhw) X(pmaddwd) X(pcmpeqb) X(pcmpeqw) X(pcmpeqd) \
 X(pcmpgtb) X(pcmpgtw) X(pcmpgtd) X(pand) X(pandn) X(por) X(pxor)
#define SHIFTS(X) X(psllw) X(pslld) X(psllq) X(psrlw) X(psrld) X(psrlq) X(psraw) X(psrad)
#define ENUM(n) op_##n,
enum { BINARY_OPS(ENUM) SHIFTS(ENUM) op_end };
#undef ENUM
#define NAME(n) #n,
static const char *const names[] = { BINARY_OPS(NAME) SHIFTS(NAME) };
#undef NAME

static u32 rng(u32 *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static u64 random64(u32 *s) { u32 hi=rng(s),lo=rng(s); return ((u64)hi<<32)|lo; }
static int tcg(void) { return cpu.hv && !kmemcmp(cpu.hv_vendor,"TCGTCGTCGTCG",12); }
static u64 lane(u64 a, unsigned i, unsigned w) {
    return (a >> (i * w)) & ((1ull << w) - 1);
}
static s64 signed_lane(u64 a, unsigned i, unsigned w) {
    u64 v = lane(a, i, w), sign = 1ull << (w - 1);
    return (s64)(v ^ sign) - (s64)sign;
}
static u64 sat(s64 v, unsigned w, int uns) {
    s64 lo = uns ? 0 : -(1ll << (w - 1));
    s64 hi = uns ? (1ll << w) - 1 : (1ll << (w - 1)) - 1;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (u64)v & ((1ull << w) - 1);
}
/* Avoid large per-opcode clones in the conventional-memory kernel. */
#if __has_attribute(noclone)
__attribute__((noclone))
#endif
static __attribute__((noinline)) u64 reference(unsigned op, u64 a, u64 b) {
    u64 out = 0, mask, v;
    unsigned w = 8, n, i;
    if (op <= op_packuswb) {
        w = op == op_packssdw ? 16 : 8; n = 64 / (2 * w);
        for (i = 0; i < n * 2; ++i) {
            v = sat(signed_lane(i < n ? a : b, i % n, w * 2), w, op == op_packuswb);
            out |= v << (i * w);
        }
        return out;
    }
    if (op >= op_punpcklbw && op <= op_punpckhdq) {
        unsigned k = op - op_punpcklbw;
        w = 8u << (k % 3); n = 64 / w;
        for (i = 0; i < n / 2; ++i) {
            unsigned j = i + (k >= 3 ? n / 2 : 0);
            out |= lane(a,j,w) << (2*i*w);
            out |= lane(b,j,w) << ((2*i+1)*w);
        }
        return out;
    }
    if (op == op_pand) return a & b;
    if (op == op_pandn) return ~a & b;
    if (op == op_por) return a | b;
    if (op == op_pxor) return a ^ b;
    if (op == op_pmaddwd) {
        for (i = 0; i < 2; ++i) {
            s64 r = signed_lane(a,2*i,16)*signed_lane(b,2*i,16)
                  + signed_lane(a,2*i+1,16)*signed_lane(b,2*i+1,16);
            out |= (u64)(u32)r << (32*i);
        }
        return out;
    }
    if (op >= op_psllw) {
        unsigned k = op - op_psllw;
        w = k < 6 ? (16u << (k % 3)) : (16u << (k-6));
        if (w == 64) return b >= 64 ? 0 : (k < 3 ? a << (unsigned)b : a >> (unsigned)b);
        mask = (1ull << w)-1;
        for (i = 0; i < 64/w; ++i) {
            v = lane(a,i,w);
            if (k >= 6) {
                unsigned count = b >= w ? w-1 : (unsigned)b;
                /* Explicit sign fill: no reliance on signed right shift. */
                if (count) v = (v >> count) | ((v & (1ull << (w-1))) ? (mask << (w-count)) : 0);
            } else v = b >= w ? 0 : (k < 3 ? v << (unsigned)b : v >> (unsigned)b);
            out |= (v & mask) << (i*w);
        }
        return out;
    }
    if (op <= op_psubd) w = 8u << ((op-op_paddb)%3);
    else if (op <= op_psubusw) w = 8u << ((op-op_paddsb)%2);
    else if (op <= op_pmulhw) w = 16;
    else if (op <= op_pcmpeqd) w = 8u << (op-op_pcmpeqb);
    else w = 8u << (op-op_pcmpgtb);
    mask = (1ull << w)-1;
    for (i = 0; i < 64/w; ++i) {
        u64 x = lane(a,i,w), y = lane(b,i,w);
        s64 sx = signed_lane(a,i,w), sy = signed_lane(b,i,w);
        if (op <= op_paddd) v = x+y;
        else if (op <= op_psubd) v = x-y;
        else if (op <= op_paddsw) v = sat(sx+sy,w,0);
        else if (op <= op_paddusw) v = sat((s64)x+(s64)y,w,1);
        else if (op <= op_psubsw) v = sat(sx-sy,w,0);
        else if (op <= op_psubusw) v = sat((s64)x-(s64)y,w,1);
        else if (op == op_pmullw) v = (u64)(sx*sy);
        else if (op == op_pmulhw) v = (u64)(sx*sy) >> 16;
        else if (op <= op_pcmpeqd) v = x == y ? mask : 0;
        else v = sx > sy ? mask : 0;
        out |= (v & mask) << (i*w);
    }
    return out;
}
static u64 execute(unsigned op, u64 a, u64 b) {
    u64 r = 0;
    switch (op) {
#define DO(n) case op_##n: __asm__ volatile("movq %1,%%mm0; movq %2,%%mm1; " #n " %%mm1,%%mm0; movq %%mm0,%0; emms" : "=m"(r) : "m"(a), "m"(b) : "memory"); break;
    BINARY_OPS(DO) SHIFTS(DO)
#undef DO
    }
    return r;
}
static u64 immediate(unsigned op, u64 a, unsigned count) {
    u64 r = 0;
#define IMM(c,n) case c: __asm__ volatile("movq %1,%%mm0; " #n " $" #c ",%%mm0; movq %%mm0,%0; emms" : "=m"(r) : "m"(a) : "memory"); break;
#define SHIFT(n) case op_##n: switch(count) { IMM(0,n) IMM(1,n) IMM(15,n) IMM(16,n) IMM(17,n) IMM(31,n) IMM(32,n) IMM(33,n) IMM(63,n) IMM(64,n) IMM(65,n) IMM(255,n) } break;
    switch(op) { SHIFTS(SHIFT) }
#undef SHIFT
#undef IMM
    return r;
}
static const u64 edges[] = {
    0, ~0ull, 0x8000800080008000ull, 0x7fff7fff7fff7fffull,
    0x8080808080808080ull, 0x7f7f7f7f7f7f7f7full,
    0x8000000080000000ull, 0x7fffffff7fffffffull,
    0x0001000100010001ull, 0x00ff00ff00ff00ffull,
    0x0123456789abcdefull, 0xfedcba9876543210ull
};
static u32 mmx_alu(void) {
    u32 seed=0x9e3779b9, sum=0;
    for (unsigned op=0; op<op_psllw; ++op) {
        unsigned bad=0;
        for (unsigned i=0; i<4096; ++i) {
            u64 a,b;
            if (i<144) { a=edges[i/12]; b=edges[i%12]; }
            else { a=(u64)rng(&seed)<<32; a|=rng(&seed); b=(u64)rng(&seed)<<32; b|=rng(&seed); }
            u64 got=execute(op,a,b), want=reference(op,a,b);
            if (got!=want && !bad++) {
                t_note("op=%s i=%u a=%llx b=%llx got=%llx want=%llx",names[op],i,a,b,got,want);
                CHECK(got==want);
            }
            sum=csum_mix(sum,(u32)got ^ (u32)(got>>32));
        }
    }
    t_ops(op_psllw*4096*8);
    return sum;
}
static u32 mmx_shifts(void) {
    static const unsigned counts[]={0,1,15,16,17,31,32,33,63,64,65,255};
    u32 seed=0x1234abcd, sum=0;
    for (unsigned op=op_psllw; op<op_end; ++op) {
        unsigned bad=0;
        for (unsigned i=0; i<4096; ++i) {
            u64 a=i<12?edges[i]:random64(&seed);
            unsigned c=counts[i%12];
            u64 want=reference(op,a,c), got=execute(op,a,c), imm=immediate(op,a,c);
            if ((want!=got || want!=imm) && !bad++) {
                t_note("op=%s count=%u a=%llx reg=%llx imm=%llx want=%llx",names[op],c,a,got,imm,want);
                CHECK(got==want); CHECK(imm==want);
            }
            if ((i&15)==0) {
                got=execute(op,a,~0ull); CHECK(got==reference(op,a,~0ull));
                got=execute(op,a,1ull<<32); CHECK(got==reference(op,a,1ull<<32));
            }
            sum=csum_mix(sum,(u32)got^(u32)(got>>32));
        }
    }
    t_ops(8*4096*8*2);
    return sum;
}
static u32 mmx_state(void) {
    u32 d=0x89abcdef, r=0; u64 q=0x0123456789abcdefull, out=0;
    __asm__ volatile("movd %2,%%mm0; movq %%mm0,%0; movd %%mm0,%1; emms" : "=m"(out),"=m"(r) : "m"(d) : "memory");
    CHECK(out==(u64)d); CHECK_EQ(r,d);
    __asm__ volatile("movq %1,%%mm0; movq %%mm0,%%mm1; movq %%mm1,%0; emms" : "=m"(out) : "m"(q) : "memory");
    CHECK(out==q);
    if (cpu_test_feature(REQ_FPU)) {
        u32 env[7];
        __asm__ volatile("fninit; movq %1,%%mm0; emms; fnstenv %0" : "=m"(env) : "m"(q) : "memory");
        CHECK_EQ(env[2]&0xffff,0xffff);
        __asm__ volatile("fld1; fistpl %0" : "=m"(r) :: "memory"); CHECK_EQ(r,1);
        __asm__ volatile("fninit; fld1; movq %2,%%mm0; movq %%mm0,%0; emms; fld1; fistpl %1" : "=m"(out),"=m"(r) : "m"(q) : "memory");
        CHECK(out==q); CHECK_EQ(r,1);
        __asm__ volatile("fninit");
    } else t_note("FPU EMMS tag checks omitted: no FPU");
    t_ops(40); return (u32)out;
}
static u32 mmx_alias(void) {
    if(!cpu_test_feature(REQ_FPU)) SKIP("FPU alias checks require an FPU");
    u8 save[108]; u32 env[7]; u16 sw; u64 q=0x0123456789abcdefull; unsigned bad=0;
    __asm__ volatile("fninit; fld1; fldz; movq %1,%%mm0; movq %%mm0,%%mm1; movq %%mm0,%%mm2; movq %%mm0,%%mm3; movq %%mm0,%%mm4; movq %%mm0,%%mm5; movq %%mm0,%%mm6; movq %%mm0,%%mm7; fnstenv %0" : "=m"(env) : "m"(q) : "memory");
    unsigned basic_ok=(env[1]&0x3800)==0;
    CHECK_EQ(env[1]&0x3800,0);
    __asm__ volatile("fnsave %0" : "=m"(save) :: "memory");
    if((*(u16 *)(save+4)&0x3800)!=0) basic_ok=0;
    CHECK_EQ(*(u16 *)(save+4)&0x3800,0);
    /* Some CPUs reconstruct special (10) tags from the MMX 0xffff exponents.
       The architectural occupancy invariant is that no slot is empty (11). */
    unsigned empty=0;
    for(unsigned i=0;i<8;++i) {
        if(((env[2]>>(i*2))&3)==3 || ((*(u16 *)(save+8)>>(i*2))&3)==3) ++empty;
    }
    bad+=empty;
    for(unsigned i=0;i<8;++i) {
        u64 mant; kmemcpy(&mant,save+28+i*10,8); CHECK(mant==q);
        if(mant!=q) basic_ok=0;
        if(*(u16 *)(save+28+i*10+8)!=0xffff) ++bad;
    }
    __asm__ volatile("fninit; movq %1,%%mm0; fld1; fnstsw %0; emms; fnclex" : "=m"(sw) : "m"(q) : "memory");
    if((sw&0x241)!=0x241) ++bad;
    t_note("mmx_alias env_tag=%04x save_tag=%04x first_exp=%04x no_emms_push_sw=%04x mismatches=%u",env[2]&0xffff,*(u16 *)(save+8),*(u16 *)(save+36),sw,bad);
    __asm__ volatile("fninit"); t_ops(40);
    if(tcg() && bad && basic_ok) SKIP("TCG omits MMX exponent/tag and x87 stack-fault semantics; raw state reported");
    CHECK_EQ(empty,0);
    for(unsigned i=0;i<8;++i) CHECK_EQ(*(u16 *)(save+28+i*10+8),0xffff);
    CHECK_EQ(sw&0x241,0x241); return (u32)q;
}

static void mmx_fault_instr(void) { __asm__ volatile(".byte 0x0f,0xef,0xc0" ::: "memory"); }
static u32 mmx_exc(void) {
    u32 cr0=rd_cr0(), n=g_exc.count;
    wr_cr0((cr0|4)&~8u);
    int rc=guard_call(mmx_fault_instr);
    wr_cr0(cr0);
    CHECK_EQ(rc,GUARD_EXC(6)); CHECK_EQ(g_exc.vec,6); CHECK_EQ(g_exc.count,n+1);
    n=g_exc.count; wr_cr0((cr0|8)&~4u);
    rc=guard_call(mmx_fault_instr);
    wr_cr0(cr0);
    CHECK_EQ(rc,GUARD_EXC(7)); CHECK_EQ(g_exc.vec,7); CHECK_EQ(g_exc.count,n+1);
    __asm__ volatile("emms");
    t_ops(2); return g_exc.count;
}
static u32 mmx_burn(void) {
    u32 seed=0xf00dcafe, sum=0;
    u64 zero=0, alpha=0x0040004000400040ull, beta=0x00c000c000c000c0ull;
    for (unsigned i=0;i<32768;++i) {
        u64 a=random64(&seed), b=random64(&seed), out, dot, sad;
        __asm__ volatile(
            "movq %3,%%mm0; movq %4,%%mm1; movq %%mm0,%%mm2; movq %%mm1,%%mm3;"
            "punpcklbw %5,%%mm0; punpckhbw %5,%%mm2; punpcklbw %5,%%mm1; punpckhbw %5,%%mm3;"
            "pmullw %6,%%mm0; pmullw %6,%%mm2; pmullw %7,%%mm1; pmullw %7,%%mm3;"
            "paddw %%mm1,%%mm0; paddw %%mm3,%%mm2; psrlw $8,%%mm0; psrlw $8,%%mm2;"
            "packuswb %%mm2,%%mm0; movq %%mm0,%0;"
            "movq %3,%%mm0; pmaddwd %4,%%mm0; movq %%mm0,%1;"
            "movq %3,%%mm0; movq %4,%%mm1; movq %%mm0,%%mm2; psubusb %%mm1,%%mm0;"
            "psubusb %%mm2,%%mm1; por %%mm1,%%mm0; movq %%mm0,%%mm2;"
            "punpcklbw %5,%%mm0; punpckhbw %5,%%mm2; paddw %%mm2,%%mm0; movq %%mm0,%2; emms"
            : "=m"(out),"=m"(dot),"=m"(sad) : "m"(a),"m"(b),"m"(zero),"m"(alpha),"m"(beta) : "memory");
        u64 want=0; unsigned total=0;
        for (unsigned j=0;j<8;++j) { unsigned x=lane(a,j,8), y=lane(b,j,8); want|=(u64)((x*64+y*192)>>8)<<(j*8); total+=x>y?x-y:y-x; }
        CHECK(out==want); CHECK(dot==reference(op_pmaddwd,a,b));
        unsigned actual=0; for(unsigned j=0;j<4;++j) actual+=(unsigned)lane(sad,j,16);
        CHECK_EQ(actual,total);
        sum=csum_mix(sum,(u32)out^(u32)(out>>32)^(u32)dot^total);
    }
    t_ops(32768*80); return sum;
}
static u32 mmx_x87_transitions(void) {
    u32 sum = 0, seed = TEST_SEED;
    for (u32 i = 0; i < 256u * HEAVY_SCALE; i++) {
        s32 a = (s32)(rng(&seed) & 0x7fff), b = (s32)(rng(&seed) & 0x7fff), result;
        u64 input = ((u64)(u32)a << 32) | (u32)b, output;
        u32 mm_env[7], empty_env[7], final_env[7];
        u8 saved[108] __attribute__((aligned(16)));
        __asm__ volatile(
            "fninit; fildl %7; fnsave %3;"
            "movq %6,%%mm0; paddd %6,%%mm0; movq %%mm0,%0;"
            "fnstenv %4; jmp 1f; 1: emms; fnstenv %5;"
            "frstor %3; fiaddl %8; fistpl %1; fnstenv %2; fninit"
            : "=m"(output), "=m"(result), "=m"(final_env), "=m"(saved),
              "=m"(mm_env), "=m"(empty_env)
            : "m"(input), "m"(a), "m"(b) : "memory");
        /* FNSTENV may reconstruct valid/zero/special tags from register data.
           MMX guarantees occupancy, not that every saved tag is 00. */
        u32 empty = 0;
        for (u32 j = 0; j < 8; j++)
            empty += ((mm_env[2] >> (j * 2)) & 3) == 3;
        if ((u32)output != (u32)b * 2 || (u32)(output >> 32) != (u32)a * 2 ||
            empty != 0 || (mm_env[1] & 0x3800) != 0 ||
            (empty_env[2] & 0xffff) != 0xffff || (empty_env[1] & 0x3800) != 0 ||
            result != a + b ||
            (final_env[2] & 0xffff) != 0xffff || (final_env[1] & 0x3841) != 0) {
            t_note("seed=%08x case=%u a=%d b=%d mm_tag=%04x empty_tag=%04x final_sw=%04x",
                   TEST_SEED, i, a, b, mm_env[2] & 0xffff, empty_env[2] & 0xffff,
                   final_env[1] & 0xffff);
            CHECK_EQ((u32)output, (u32)b * 2); CHECK_EQ(output >> 32, (u32)a * 2);
            CHECK_EQ(empty, 0); CHECK_EQ(mm_env[1] & 0x3800, 0);
            CHECK_EQ(empty_env[2] & 0xffff, 0xffff); CHECK_EQ(result, a + b);
            CHECK_EQ(empty_env[1] & 0x3800, 0);
            CHECK_EQ(final_env[2] & 0xffff, 0xffff); CHECK_EQ(final_env[1] & 0x3841, 0);
            return sum;
        }
        sum = csum_mix(sum, (u32)result ^ (u32)output);
    }
    t_ops(256u * HEAVY_SCALE); return sum;
}

#define EXT_BINARY(X) X(pavgb) X(pavgw) X(pmaxsw) X(pminsw) X(pmaxub) X(pminub) X(pmulhuw) X(psadbw)
#define EXT_ENUM(n) ext_##n,
enum { EXT_BINARY(EXT_ENUM) ext_end };
#undef EXT_ENUM

static u64 extended_reference(u32 op, u64 a, u64 b) {
    u64 result = 0;
    if (op == ext_psadbw) {
        for (u32 j = 0; j < 8; j++) {
            u32 x = lane(a, j, 8), y = lane(b, j, 8);
            result += x > y ? x - y : y - x;
        }
        return result;
    }
    u32 width = (op == ext_pavgb || op == ext_pmaxub || op == ext_pminub) ? 8 : 16;
    for (u32 j = 0; j < 64 / width; j++) {
        u32 x = lane(a, j, width), y = lane(b, j, width), value;
        if (op == ext_pavgb || op == ext_pavgw) value = (x + y + 1) >> 1;
        else if (op == ext_pmulhuw) value = (x * y) >> 16;
        else if (op == ext_pmaxsw) value = (s16)x > (s16)y ? x : y;
        else if (op == ext_pminsw) value = (s16)x < (s16)y ? x : y;
        else if (op == ext_pmaxub) value = x > y ? x : y;
        else value = x < y ? x : y;
        result |= (u64)value << (width * j);
    }
    return result;
}

static u64 extended_execute(u32 op, u64 a, u64 b, u32 memory) {
    u64 result = 0;
    switch (op) {
#define EXT_DO(n) case ext_##n: \
        if (memory) \
            __asm__ volatile("movq %1,%%mm0; " #n " %2,%%mm0; movq %%mm0,%0; emms" \
                             : "=m"(result) : "m"(a), "m"(b) : "memory"); \
        else \
            __asm__ volatile("movq %1,%%mm0; movq %2,%%mm1; " #n " %%mm1,%%mm0; movq %%mm0,%0; emms" \
                             : "=m"(result) : "m"(a), "m"(b) : "memory"); \
        break;
        EXT_BINARY(EXT_DO)
#undef EXT_DO
    }
    return result;
}

static u32 mmx_extended(void) {
    u32 seed = TEST_SEED, sum = 0;
    for (u32 i = 0; i < 2048; i++) {
        u64 a = random64(&seed), b = random64(&seed);
        if (i < 4) {
            const u64 edges[] = {0, ~0ull, 0x80007fff80007fffull, 0xff00ff0000ff00ffull};
            a = edges[i]; b = edges[3 - i];
        }
        for (u32 op = 0; op < ext_end; op++) {
            u64 want = extended_reference(op, a, b);
            for (u32 form = 0; form < 2; form++) {
                u64 got = extended_execute(op, a, b, form);
                CHECK_EQ((u32)got, (u32)want);
                CHECK_EQ(got >> 32, want >> 32);
                sum = csum_mix(sum, (u32)got);
            }
        }
        u64 shuffled, inserted, streamed __attribute__((aligned(8)));
        u32 mask, extracted, word = rng(&seed), expected_mask = 0;
        __asm__ volatile("movq %4,%%mm0; pshufw $0x1b,%%mm0,%%mm1; movq %%mm1,%0;"
                         "pmovmskb %%mm0,%1; pextrw $2,%%mm0,%2;"
                         "pinsrw $2,%5,%%mm0; movq %%mm0,%3; emms"
                         : "=m"(shuffled), "=&r"(mask), "=&r"(extracted), "=m"(inserted)
                         : "m"(a), "r"(word) : "memory");
        u64 want_shuffle = 0;
        for (u32 j = 0; j < 4; j++) want_shuffle |= lane(a, 3 - j, 16) << (16 * j);
        for (u32 j = 0; j < 8; j++) expected_mask |= ((a >> (8 * j + 7)) & 1) << j;
        CHECK(shuffled == want_shuffle);
        CHECK_EQ(mask, expected_mask);
        CHECK_EQ(extracted, lane(a, 2, 16));
        CHECK(inserted == ((a & ~0xffff00000000ull) | ((u64)(word & 65535) << 32)));
        __asm__ volatile("movq %1,%%mm0; movntq %%mm0,%0; sfence; emms"
                         : "=m"(streamed) : "m"(a) : "memory");
        CHECK(streamed == a);
        u8 masked[8];
        for (u32 j = 0; j < 8; j++) masked[j] = 0xa5;
        u8 *destination = masked;
        __asm__ volatile("movq %1,%%mm0; movq %2,%%mm1; maskmovq %%mm1,%%mm0; sfence; emms"
                         : "+D"(destination) : "m"(a), "m"(b) : "memory");
        for (u32 j = 0; j < 8; j++)
            CHECK_EQ(masked[j], (b & (0x80ull << (8 * j))) ? lane(a, j, 8) : 0xa5);
    }
    t_ops(2048 * (2 * ext_end + 6));
    return sum;
}

const test_t tests_mmx[] = {
    {"mmx_extended",mmx_extended,REQ_MMX|REQ_MMXEXT},
    {"mmx_x87_transitions",mmx_x87_transitions,REQ_MMX|REQ_FPU},
    {"mmx_alu",mmx_alu,REQ_MMX}, {"mmx_shifts",mmx_shifts,REQ_MMX},
    {"mmx_state",mmx_state,REQ_MMX}, {"mmx_alias",mmx_alias,REQ_MMX}, {"mmx_exc",mmx_exc,REQ_MMX},
    {"mmx_burn",mmx_burn,REQ_MMX}, {0,0,0}
};
