#include "kernel.h"

#define APIC_BASE_MSR 0x1Bu
#define APIC_MMIO_BASE 0xFEE00000u
#define APIC_ID 0x020u
#define APIC_TPR 0x080u
#define APIC_EOI 0x0B0u
#define APIC_SVR 0x0F0u
#define APIC_LVT_TIMER 0x320u
#define APIC_TIMER_DIV 0x3E0u
#define APIC_TIMER_INIT 0x380u
#define APIC_TIMER_CURRENT 0x390u
#define APIC_TIMER_VECTOR 0x90u

static volatile u32 mixed_soft_count;
static volatile u32 mixed_bad_vector;
static volatile u32 mixed_fpu_count;
static u8 fpu_irq_image[108] __attribute__((aligned(16)));
static u32 mixed_result, *mixed_data, *mixed_pt, *mixed_frames, mixed_iterations;
static void mixed_body_trampoline(void);

static u32 lcg_next(u32 x) { return x * 1664525u + 1013904223u; }

static u32 lcg_advance(u32 x, u32 n) {
    u32 acc_mul = 1, acc_add = 0;
    u32 cur_mul = 1664525u, cur_add = 1013904223u;
    while (n) {
        if (n & 1) {
            acc_add = acc_add * cur_mul + cur_add;
            acc_mul = acc_mul * cur_mul;
        }
        cur_add = cur_add * (cur_mul + 1u);
        cur_mul *= cur_mul;
        n >>= 1;
    }
    return acc_mul * x + acc_add;
}

static void mixed_irq_hook(regs_t *r) {
    if (r->vec != APIC_TIMER_VECTOR) {
        mixed_bad_vector++;
        return;
    }
    mixed_soft_count++;
    if (cpu.has_fpu) {
        __asm__ volatile("fnsave %0\n\tfninit\n\tfld1\n\tfld1\n\tfaddp\n\tfrstor %0"
                         : "+m"(fpu_irq_image) :: "memory");
        mixed_fpu_count++;
    }
}

static void fill_seeded(u32 *p, u32 n, u32 seed) {
    u32 x = seed;
    for (u32 i = 0; i < n; i++) {
        x = lcg_next(x);
        p[i] = x ^ (i * 0x9E3779B9u);
    }
}

static u32 mixed_body(u32 *data, u32 *pt, u32 *frames, u32 iterations) {
    static const u64 fp_a = 0x3FF4000000000000ull;
    static const u64 fp_b = 0x4004000000000000ull;
    static const u64 fp_sum = 0x400E000000000000ull;
    static const u64 fp_one = 0x3FF0000000000000ull;
    static const u64 mmx_step = 0x0000000300000007ull;
    volatile u32 *window = (volatile u32 *)0x50000000u;
    u32 x = (u32)TEST_SEED, sum = 0;
    u32 old_cw = 0, custom_cw = 0x077Fu;
    if (cpu.has_fpu) {
        __asm__ volatile("fnstcw %0" : "=m"(old_cw));
        __asm__ volatile("fninit; fldcw %0" :: "m"(custom_cw) : "memory");
    }
    for (u32 i = 0; i < iterations; i++) {
        x = lcg_next(x);
        u32 ix = (x >> 24) & 255u;
        volatile u32 *slot = &data[ix];
        u32 v = *slot ^ x ^ i;
        *slot = v;
        CHECK_EQ(*slot, v);
        sum = csum_mix(sum, v ^ x);

        if (cpu.has_fpu) {
            u64 got;
            u16 cw;
            __asm__ volatile("fldl %2; faddl %3; fstpl %0; fnstcw %1"
                             : "=m"(got), "=m"(cw) : "m"(fp_a), "m"(fp_b) : "memory");
            CHECK(got == fp_sum);
            CHECK_EQ(cw, custom_cw);
        }

        if (cpu.has_mmx && cpu.has_fpu) {
            u64 got;
            __asm__ volatile("movq %1,%%mm0; paddd %2,%%mm0; movq %%mm0,%0; emms"
                             : "=m"(got) : "m"(mmx_step), "m"(mmx_step) : "memory");
            CHECK_EQ((u32)got, 14u);
            CHECK_EQ((u32)(got >> 32), 6u);
        }

        if (!(i & 255u)) {
            if (cpu.has_fpu) __asm__ volatile("fldl %0" :: "m"(fp_one) : "memory");
            __asm__ volatile("int $0x90" ::: "memory", "cc");
            if (cpu.has_fpu) {
                u64 restored;
                __asm__ volatile("fstpl %0" : "=m"(restored) :: "memory");
                CHECK(restored == fp_one);
            }
        }

        u32 frame = x & 3u;
        pt[0] = (u32)&frames[frame * 1024] | 3u;
        invlpg((void *)0x50000000u);
        CHECK_EQ(window[0], 0x6A530000u + frame);
        if (!(i & 7u)) wr_cr3(rd_cr3());
    }
    if (cpu.has_fpu) __asm__ volatile("fldcw %0" :: "m"(old_cw) : "memory");
    CHECK_EQ(x, lcg_advance((u32)TEST_SEED, iterations));
    return sum ^ x;
}

static u32 heavy_mix_transitions(void) {
    enum { WORDS = 256, FRAME_COUNT = 4, PAGE_WORDS = 1024 };
    u32 *data = arena_alloc(WORDS * sizeof(u32), 16);
    u32 *reference = arena_alloc(WORDS * sizeof(u32), 16);
    u32 *pt = arena_alloc(4096, 4096);
    u32 *frames = arena_alloc(FRAME_COUNT * 4096, 4096);
    if (!data || !reference || !pt || !frames) SKIP("arena too small for mixed-state pages");
    const u32 pde_index = 0x50000000u >> 22;
    u32 old_pde = pg_dir[pde_index];
    if (old_pde) SKIP("scratch page-directory slot is occupied");
    fill_seeded(data, WORDS, (u32)TEST_SEED);
    fill_seeded(reference, WORDS, (u32)TEST_SEED);
    kmemset(pt, 0, 4096);
    for (u32 i = 0; i < FRAME_COUNT; i++) frames[i * PAGE_WORDS] = 0x6A530000u + i;
    u32 saved_cr3 = rd_cr3();
    u32 iterations = HEAVY_SCALE * 16384u;
    mixed_soft_count = mixed_bad_vector = mixed_fpu_count = 0;
    pg_dir[pde_index] = (u32)pt | 3u;
    wr_cr3(saved_cr3);
    isr_hook_t old_hook = int_hook[APIC_TIMER_VECTOR];
    idt_set_gate(APIC_TIMER_VECTOR, isr_stub[APIC_TIMER_VECTOR], SEL_KCODE, 0x8E);
    int_hook[APIC_TIMER_VECTOR] = mixed_irq_hook;
    mixed_data = data;
    mixed_pt = pt;
    mixed_frames = frames;
    mixed_iterations = iterations;
    int rc = guard_call(mixed_body_trampoline);
    pg_dir[pde_index] = old_pde;
    wr_cr3(saved_cr3);
    invlpg((void *)0x50000000u);
    int_hook[APIC_TIMER_VECTOR] = old_hook;
    if (cpu.has_mmx && cpu.has_fpu) __asm__ volatile("emms");
    if (cpu.has_fpu) __asm__ volatile("fninit");
    CHECK_EQ(rc, GUARD_OK);
    CHECK_EQ(mixed_bad_vector, 0);
    CHECK_EQ(mixed_soft_count, (iterations + 255u) / 256u);
    if (cpu.has_fpu) CHECK_EQ(mixed_fpu_count, (iterations + 255u) / 256u);
    u32 ref_x = (u32)TEST_SEED;
    for (u32 i = 0; i < iterations; i++) {
        ref_x = lcg_next(ref_x);
        u32 ix = (ref_x >> 24) & 255u;
        reference[ix] ^= ref_x ^ i;
    }
    for (u32 i = 0; i < WORDS; i++) CHECK_EQ(data[i], reference[i]);
    u32 ops = iterations * (cpu.has_fpu ? (cpu.has_mmx ? 18u : 14u) : 8u);
    t_ops(ops);
    t_note("iterations=%u integer=%u x87=%u mmx=%u page_remaps=%u cr3_reloads=%u checksum=%08x",
           iterations, iterations, cpu.has_fpu ? iterations : 0,
           cpu.has_mmx && cpu.has_fpu ? iterations : 0, iterations, (iterations + 7u) / 8u, mixed_result);
    return mixed_result;
}

static void mixed_body_trampoline(void) {
    mixed_result = mixed_body(mixed_data, mixed_pt, mixed_frames, mixed_iterations);
}

static u32 heavy_state_interrupt(void) {
    static const u64 one = 0x3FF0000000000000ull;
    static const u64 x87_want = 0x3FF0000000000000ull;
    static const u64 mmx_want = 0x1122334455667788ull;
    u32 count = HEAVY_SCALE * 4096u;
    u32 old_cw = 0, custom_cw = 0x077Fu;
    u32 a, b, c, d, s, di, before;
    u64 fp_result = 0, mmx_result = 0;
    u16 after_cw = 0;
    isr_hook_t old_hook = int_hook[APIC_TIMER_VECTOR];
    mixed_soft_count = mixed_bad_vector = mixed_fpu_count = 0;
    idt_set_gate(APIC_TIMER_VECTOR, isr_stub[APIC_TIMER_VECTOR], SEL_KCODE, 0x8E);
    int_hook[APIC_TIMER_VECTOR] = mixed_irq_hook;
    if (cpu.has_fpu) {
        __asm__ volatile("fnstcw %0" : "=m"(old_cw));
        __asm__ volatile("fninit; fldcw %0" :: "m"(custom_cw) : "memory");
    }
    for (u32 i = 0; i < count; i++) {
        int do_x87 = cpu.has_fpu && (!cpu.has_mmx || !(i & 1));
        if (do_x87) __asm__ volatile("fldl %0" :: "m"(one) : "memory");
        else if (cpu.has_mmx && cpu.has_fpu)
            __asm__ volatile("movq %0,%%mm0" :: "m"(mmx_want) : "memory");
        before = mixed_soft_count;
        __asm__ volatile(
            "movl $0x13579bdf,%%eax\n\t"
            "movl $0x2468ace0,%%ebx\n\t"
            "movl $0xdeadbeef,%%ecx\n\t"
            "movl $0x76543210,%%edx\n\t"
            "movl $0x89abcdef,%%esi\n\t"
            "movl $0xfedcba98,%%edi\n\t"
            "int $0x90"
            : "=a"(a), "=b"(b), "=c"(c), "=d"(d), "=S"(s), "=D"(di)
            :
            : "memory", "cc");
        CHECK_EQ(mixed_soft_count, before + 1);
        CHECK_EQ(a, 0x13579BDFu);
        CHECK_EQ(b, 0x2468ACE0u);
        CHECK_EQ(c, 0xDEADBEEFu);
        CHECK_EQ(d, 0x76543210u);
        CHECK_EQ(s, 0x89ABCDEFu);
        CHECK_EQ(di, 0xFEDCBA98u);
        if (do_x87) {
            __asm__ volatile("fnstcw %0; fstpl %1" : "=m"(after_cw), "=m"(fp_result) :: "memory");
            CHECK_EQ(after_cw, custom_cw);
            CHECK(fp_result == x87_want);
        } else if (cpu.has_mmx && cpu.has_fpu) {
            __asm__ volatile("movq %%mm0,%0; emms" : "=m"(mmx_result) :: "memory");
            CHECK(mmx_result == mmx_want);
        }
    }
    if (cpu.has_fpu) __asm__ volatile("fldcw %0" :: "m"(old_cw) : "memory");
    int_hook[APIC_TIMER_VECTOR] = old_hook;
    if (cpu.has_mmx && cpu.has_fpu) __asm__ volatile("emms");
    CHECK_EQ(mixed_bad_vector, 0);
    CHECK_EQ(mixed_soft_count, count);
    t_ops(count * (cpu.has_fpu ? (cpu.has_mmx ? 12u : 9u) : 8u));
    t_note("software_interrupts=%u integer_register_checks=%u x87_checks=%u mmx_checks=%u handler_fnsave=%u",
           count, count * 6u, cpu.has_fpu ? (cpu.has_mmx ? count / 2u : count) : 0,
           cpu.has_mmx && cpu.has_fpu ? count / 2u : 0, mixed_fpu_count);
    return mixed_soft_count;
}

static u32 heavy_x87_save_restore(void) {
    static const u64 patterns[8] = {
        0x3FF0000000000000ull, 0x4000000000000000ull,
        0x4008000000000000ull, 0x4010000000000000ull,
        0x4014000000000000ull, 0x4018000000000000ull,
        0x401C000000000000ull, 0x4020000000000000ull
    };
    u8 image[108] __attribute__((aligned(16)));
    u64 got[8];
    u16 original_cw = 0, custom_cw = 0x077Fu, got_cw = 0;
    u32 iterations = HEAVY_SCALE * 8192u;
    __asm__ volatile("fnstcw %0" : "=m"(original_cw));
    for (u32 i = 0; i < iterations; i++) {
        u32 rotation = i & 7u;
        __asm__ volatile("fninit; fldcw %0" :: "m"(custom_cw) : "memory");
        for (u32 j = 0; j < 8; j++)
            __asm__ volatile("fldl %0" :: "m"(patterns[(rotation + j) & 7u]) : "memory");
        __asm__ volatile("fnsave %0" : "=m"(image) :: "memory");
        __asm__ volatile("fninit; fldz; fld1; frstor %1; fnstcw %0"
                         : "=m"(got_cw) : "m"(image) : "memory");
        __asm__ volatile("fstpl %0; fstpl %1; fstpl %2; fstpl %3; "
                         "fstpl %4; fstpl %5; fstpl %6; fstpl %7"
                         : "=m"(got[0]), "=m"(got[1]), "=m"(got[2]), "=m"(got[3]),
                           "=m"(got[4]), "=m"(got[5]), "=m"(got[6]), "=m"(got[7])
                         :: "memory");
        CHECK_EQ(got_cw, custom_cw);
        for (u32 j = 0; j < 8; j++)
            CHECK(got[j] == patterns[(rotation + 7u - j) & 7u]);
    }
    __asm__ volatile("fldcw %0" :: "m"(original_cw) : "memory");
    t_ops(iterations * 18u);
    t_note("FNSAVE/FRSTOR iterations=%u rotated_x87_patterns=8 checked_per_restore control_word=%04x top=%016llx bottom=%016llx",
           iterations, got_cw, got[0], got[7]);
    return got_cw ^ (u32)got[0] ^ (u32)got[7] ^ iterations;
}

static u8 fx_original[512] __attribute__((aligned(16)));
static u8 fx_test[512] __attribute__((aligned(16)));
static u8 fx_verify[512] __attribute__((aligned(16)));
static u32 fx_valid;
static u32 fx_result;
static u32 fx_progress;

static void fx_restore_original(void) {
    __asm__ volatile("fxrstor %0" :: "m"(fx_original) : "memory");
    fx_valid = 0;
}

static void fx_body(void) {
    static const u64 patterns[8] = {
        0x3FF0000000000000ull, 0x4000000000000000ull,
        0x4008000000000000ull, 0x4010000000000000ull,
        0x4014000000000000ull, 0x4018000000000000ull,
        0x401C000000000000ull, 0x4020000000000000ull
    };
    u64 got_x87[4], got_mmx[8];
    u32 expected_mxcsr, got_mxcsr;
    u16 cw = 0, custom_cw = 0x077Fu;
    __asm__ volatile("fxsave %0" : "=m"(fx_original) :: "memory");
    fx_valid = 1;
    u32 iterations = HEAVY_SCALE * 8192u;
    for (u32 i = 0; i < iterations; i++) {
        fx_progress = i * 3u + 1u;
        u32 rotation = i & 7u;
        __asm__ volatile("fninit; fldcw %0" :: "m"(custom_cw) : "memory");
        for (u32 j = 0; j < 4; j++)
            __asm__ volatile("fldl %0" :: "m"(patterns[(rotation + j) & 7u]) : "memory");
        __asm__ volatile("fxsave %0" : "=m"(fx_test) :: "memory");
        fx_progress = i * 3u + 2u;
        __asm__ volatile("fninit; fldz; fxrstor %6; fnstcw %0; "
                         "fstpl %1; fstpl %2; fstpl %3; fstpl %4; fxsave %5"
                         : "=m"(cw), "=m"(got_x87[0]), "=m"(got_x87[1]),
                           "=m"(got_x87[2]), "=m"(got_x87[3]), "=m"(fx_verify)
                         : "m"(fx_test) : "memory");
        CHECK_EQ(cw, custom_cw);
        for (u32 j = 0; j < 4; j++)
            CHECK(got_x87[j] == patterns[(rotation + 3u - j) & 7u]);
        expected_mxcsr = *(u32 *)(fx_test + 24);
        got_mxcsr = *(u32 *)(fx_verify + 24);
        CHECK_EQ(got_mxcsr, expected_mxcsr);

        __asm__ volatile("emms" ::: "memory");
        for (u32 j = 0; j < 8; j++) {
            switch (j) {
            case 0: __asm__ volatile("movq %0,%%mm0" :: "m"(patterns[(rotation + j) & 7u]) : "memory"); break;
            case 1: __asm__ volatile("movq %0,%%mm1" :: "m"(patterns[(rotation + j) & 7u]) : "memory"); break;
            case 2: __asm__ volatile("movq %0,%%mm2" :: "m"(patterns[(rotation + j) & 7u]) : "memory"); break;
            case 3: __asm__ volatile("movq %0,%%mm3" :: "m"(patterns[(rotation + j) & 7u]) : "memory"); break;
            case 4: __asm__ volatile("movq %0,%%mm4" :: "m"(patterns[(rotation + j) & 7u]) : "memory"); break;
            case 5: __asm__ volatile("movq %0,%%mm5" :: "m"(patterns[(rotation + j) & 7u]) : "memory"); break;
            case 6: __asm__ volatile("movq %0,%%mm6" :: "m"(patterns[(rotation + j) & 7u]) : "memory"); break;
            default: __asm__ volatile("movq %0,%%mm7" :: "m"(patterns[(rotation + j) & 7u]) : "memory"); break;
            }
        }
        fx_progress = i * 3u + 3u;
        __asm__ volatile("fxsave %0; emms; fxrstor %0" : "+m"(fx_test) :: "memory");
        __asm__ volatile("movq %%mm0,%0; movq %%mm1,%1; movq %%mm2,%2; movq %%mm3,%3; "
                         "movq %%mm4,%4; movq %%mm5,%5; movq %%mm6,%6; movq %%mm7,%7; emms"
                         : "=m"(got_mmx[0]), "=m"(got_mmx[1]), "=m"(got_mmx[2]), "=m"(got_mmx[3]),
                           "=m"(got_mmx[4]), "=m"(got_mmx[5]), "=m"(got_mmx[6]), "=m"(got_mmx[7])
                         :: "memory");
        for (u32 j = 0; j < 8; j++)
            CHECK(got_mmx[j] == patterns[(rotation + j) & 7u]);
    }
    fx_restore_original();
    fx_result = cw ^ got_mxcsr ^ (u32)got_x87[0] ^ (u32)got_mmx[0];
}

static u32 heavy_fx_save_restore(void) {
    fx_valid = 0;
    fx_progress = 0;
    int rc = guard_call(fx_body);
    if (fx_valid) {
        int restore_rc = guard_call(fx_restore_original);
        if (restore_rc) t_failv("FXSAVE original-state restoration", __LINE__, restore_rc, GUARD_OK);
    }
    if (rc) t_note("FXSAVE/FXRSTOR fault rc=%x vector=%u eip=%08x progress=%u",
                   rc, g_exc.vec, g_exc.eip, fx_progress);
    CHECK_EQ(rc, GUARD_OK);
    t_ops(HEAVY_SCALE * 8192u * 40u);
    t_note("FXSAVE/FXRSTOR iterations=%u rotated_x87_patterns=4 and rotated_MMX_patterns=8 checked per restore",
           HEAVY_SCALE * 8192u);
    return fx_result;
}

static u64 apic_base_value;
static u32 apic_saved_svr, apic_saved_tpr, apic_saved_lvt;
static u32 apic_saved_div, apic_saved_init, apic_saved_current, apic_id, apic_version;
static volatile u32 apic_irq_count, apic_bad_vector;
static volatile u32 *apic;
static int apic_config_started;
static u32 apic_periodic_mode;
static u32 apic_test_elapsed_ms;
static void apic_timer_trampoline(void);

static void read_apic_base(void) {
    apic_base_value = rdmsr(APIC_BASE_MSR);
}

static void apic_snapshot(void) {
    apic = (volatile u32 *)APIC_MMIO_BASE;
    apic_id = apic[APIC_ID / 4];
    apic_version = apic[0x30 / 4];
    apic_saved_svr = apic[APIC_SVR / 4];
    apic_saved_tpr = apic[APIC_TPR / 4];
    apic_saved_lvt = apic[APIC_LVT_TIMER / 4];
    apic_saved_div = apic[APIC_TIMER_DIV / 4];
    apic_saved_init = apic[APIC_TIMER_INIT / 4];
    apic_saved_current = apic[APIC_TIMER_CURRENT / 4];
}

static void apic_timer_hook(regs_t *r) {
    if (r->vec != APIC_TIMER_VECTOR) apic_bad_vector++;
    apic[APIC_EOI / 4] = 0;
    apic_irq_count++;
}

static void apic_restore_body(void) {
    apic[APIC_LVT_TIMER / 4] = apic_saved_lvt | (1u << 16);
    apic[APIC_TIMER_INIT / 4] = 0;
    apic[APIC_TIMER_DIV / 4] = apic_saved_div;
    apic[APIC_TIMER_INIT / 4] = apic_saved_init;
    apic[APIC_LVT_TIMER / 4] = apic_saved_lvt;
    apic[APIC_SVR / 4] = apic_saved_svr;
    apic[APIC_TPR / 4] = apic_saved_tpr;
}

static void wait_for_ticks(u32 target, u32 deadline) {
    while (apic_irq_count < target && (s32)(pit_ticks - deadline) < 0) hlt();
}

static u32 apic_ticks_per_ms;
static u32 apic_elapsed_ms(u32 start_tick) { return (pit_ticks - start_tick) * 10u; }

static void apic_timer_body(u32 periodic) {
    u32 lvt = APIC_TIMER_VECTOR | (1u << 16);
    u32 start_count, before, after, start_tick, calibration_start;
    if (periodic) lvt |= 1u << 17;
    apic_config_started = 1;
    apic[APIC_LVT_TIMER / 4] = lvt;
    apic[APIC_TIMER_DIV / 4] = 3u;
    apic[APIC_TPR / 4] = 0;
    apic[APIC_SVR / 4] = apic_saved_svr | (1u << 8);
    apic[APIC_LVT_TIMER / 4] = lvt | (1u << 16);
    apic[APIC_TIMER_INIT / 4] = 0x70000000u;
    before = apic[APIC_TIMER_CURRENT / 4];
    calibration_start = pit_ticks;
    while ((s32)(pit_ticks - (calibration_start + 3u)) < 0) hlt();
    after = apic[APIC_TIMER_CURRENT / 4];
    u32 elapsed_ms = apic_elapsed_ms(calibration_start);
    CHECK(before > after);
    CHECK(elapsed_ms >= 20u);
    apic_ticks_per_ms = (before - after) / elapsed_ms;
    CHECK(apic_ticks_per_ms != 0);
    start_count = apic_ticks_per_ms;
    apic[APIC_TIMER_INIT / 4] = 0;
    apic[APIC_LVT_TIMER / 4] = lvt & ~(1u << 16);
    start_tick = pit_ticks;
    u32 deadline_ms, deadline;
    if (periodic) {
        u32 target_irqs = 256u * HEAVY_SCALE;
        u32 min_elapsed_ms = 250u * HEAVY_SCALE;
        deadline_ms = min_elapsed_ms * 2u + 5000u;
        if (deadline_ms > 30000u) deadline_ms = 30000u;
        deadline = start_tick + (deadline_ms + 9u) / 10u;
        apic[APIC_TIMER_INIT / 4] = start_count;
        while ((apic_irq_count < target_irqs || apic_elapsed_ms(start_tick) < min_elapsed_ms) &&
               (s32)(pit_ticks - deadline) < 0) hlt();
        CHECK(apic_irq_count >= target_irqs);
        CHECK(apic_elapsed_ms(start_tick) >= min_elapsed_ms);
        CHECK(apic_irq_count < target_irqs * 4u);
        CHECK(apic[APIC_TIMER_CURRENT / 4] <= start_count);
    } else {
        u32 target_irqs = 128u * HEAVY_SCALE;
        deadline_ms = target_irqs * 3u + 5000u;
        if (deadline_ms > 30000u) deadline_ms = 30000u;
        deadline = start_tick + (deadline_ms + 9u) / 10u;
        for (u32 n = 0; n < target_irqs; n++) {
            apic[APIC_TIMER_INIT / 4] = start_count;
            wait_for_ticks(n + 1u, deadline);
            if ((s32)(pit_ticks - deadline) >= 0) break;
            CHECK_EQ(apic[APIC_TIMER_CURRENT / 4], 0u);
        }
        CHECK(apic_irq_count >= target_irqs);
    }
    apic_test_elapsed_ms = apic_elapsed_ms(start_tick);
}

static u32 apic_timer_test(u32 periodic) {
    if (guard_call(read_apic_base)) {
        if (cpu.hv) t_note("hypervisor=%s APIC feature advertised but APIC_BASE RDMSR trapped (metadata gap)", cpu.hv_vendor);
        SKIP("APIC_BASE RDMSR unavailable");
    }
    u64 base = apic_base_value;
    if ((base & 0xFFFFFF000ull) != APIC_MMIO_BASE || (base & (1u << 10))) {
        t_note("APIC_BASE=%llx; active xAPIC test requires enabled fixed xAPIC base", base);
        SKIP("APIC base relocated or x2APIC mode");
    }
    if (!(base & (1u << 11))) {
        t_note("APIC_BASE=%llx; APIC CPUID bit set but local APIC disabled", base);
        SKIP(cpu.hv ? "hypervisor advertises APIC but leaves LAPIC disabled" : "local APIC disabled");
    }
    if (guard_call(apic_snapshot)) {
        if (cpu.hv) t_note("hypervisor=%s APIC_BASE enabled but MMIO snapshot faulted (metadata gap)", cpu.hv_vendor);
        SKIP("local APIC MMIO is not accessible");
    }
    if (!(apic_version & 0xFFu) || ((apic_version >> 16) & 0xFFu) < 2u)
        SKIP("local APIC version register does not expose a timer LVT");
    if (!(apic_saved_lvt & (1u << 16)) || apic_saved_current) {
        t_note("LVT_TIMER=%08x current=%u; prior timer is active/unmasked and will not be overwritten",
               apic_saved_lvt, apic_saved_current);
        SKIP("existing local APIC timer is active or unmasked");
    }

    t_note("mode=%s APIC_ID=%08x VERSION=%08x SVR=%08x TPR=%08x LVT=%08x current=%u",
           periodic ? "periodic" : "one_shot", apic_id, apic_version, apic_saved_svr,
           apic_saved_tpr, apic_saved_lvt, apic_saved_current);
    apic_irq_count = apic_bad_vector = 0;
    apic_config_started = 0;
    apic_periodic_mode = periodic;
    isr_hook_t old_hook = int_hook[APIC_TIMER_VECTOR];
    idt_set_gate(APIC_TIMER_VECTOR, isr_stub[APIC_TIMER_VECTOR], SEL_KCODE, 0x8E);
    int_hook[APIC_TIMER_VECTOR] = apic_timer_hook;
    int rc = guard_call(apic_timer_trampoline);
    if (apic_config_started) {
        int cleanup_rc = guard_call(apic_restore_body);
        if (cleanup_rc) t_failv("APIC state restoration", __LINE__, cleanup_rc, GUARD_OK);
    }
    int_hook[APIC_TIMER_VECTOR] = old_hook;
    CHECK_EQ(rc, GUARD_OK);
    CHECK_EQ(apic_bad_vector, 0);
    u32 expected = periodic ? 256u * HEAVY_SCALE : 128u * HEAVY_SCALE;
    CHECK(apic_irq_count >= expected);
    t_ops(apic_irq_count + 5u);
    t_note("delivered=%u expected_min=%u mode=%s timer_ticks_per_ms=%u elapsed_ms=%u TPR/SVR/LVT/divide/init restored=%u",
           apic_irq_count, expected, periodic ? "periodic" : "one_shot",
           apic_ticks_per_ms, apic_test_elapsed_ms, apic_config_started);
    return apic_irq_count;
}

static void apic_timer_trampoline(void) { apic_timer_body(apic_periodic_mode); }

static u32 heavy_apic_periodic(void) { return apic_timer_test(1); }
static u32 heavy_apic_oneshot(void) { return apic_timer_test(0); }

const test_t tests_heavy_state[] = {
    {"heavy_mix_transitions", heavy_mix_transitions, REQ_PAGING | REQ_486},
    {"heavy_state_interrupt", heavy_state_interrupt, REQ_486},
    {"heavy_x87_save_restore", heavy_x87_save_restore, REQ_486 | REQ_FPU},
    {"heavy_fx_save_restore", heavy_fx_save_restore, REQ_486 | REQ_FXSR | REQ_FPU},
    {"heavy_apic_periodic", heavy_apic_periodic, REQ_PAGING | REQ_486 | REQ_APIC},
    {"heavy_apic_oneshot", heavy_apic_oneshot, REQ_PAGING | REQ_486 | REQ_APIC},
    {0, 0, 0}
};
