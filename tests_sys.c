#include "kernel.h"

static u32 r3_stack(void) { u8 *p = arena_alloc(4096, 4096); return p ? (u32)p + 4096 : 0; }
static volatile u64 sampled;
static volatile u32 arg;
static void probe_cr4(void) { arg = rd_cr4(); }
static void write_cr4(void) { wr_cr4(arg); }
static void user_tsc(void) { sampled = rdtsc(); __asm__ volatile("int $0x80"); }
static void user_pmc(void) { __asm__ volatile("xor %%ecx,%%ecx; rdpmc; int $0x80" ::: "eax", "ecx", "edx", "memory"); }
static u32 sys_tsc(void) {
    u64 last = rdtsc(), sum = 0;
    for (u32 i = 0; i < 4096; i++) { u64 now = rdtsc(); CHECK(now >= last); sum += now - last; last = now; }
    t_note("RDTSC average=%llu cycles", sum / 4096);
    u32 t = pit_ticks; while (pit_ticks == t) hlt();
    t = pit_ticks; u64 start = rdtsc();
    while (pit_ticks - t < 20) hlt();
    u64 delta = rdtsc() - start;
    u32 elapsed = pit_ticks - t, mhz = (u32)(delta / (elapsed * 10000u));
    t_note("TSC MHz=%u calibrated=%u PIT_ticks=%u", mhz, cpu.mhz, elapsed);
    if (cpu.mhz) { CHECK(mhz * 4 >= cpu.mhz * 3); CHECK(mhz * 4 <= cpu.mhz * 5); }
    if (cpu.family >= 5 && guard_call(probe_cr4) == 0) {
        u32 saved = arg, stack = r3_stack();
        if (stack) {
            arg = saved | 4; int rc = guard_call(write_cr4);
            if (!rc) {
                CHECK(rdtsc() >= last);
                rc = ring3_call(user_tsc, stack);
            }
            arg = saved; CHECK_EQ(guard_call(write_cr4), 0);
            CHECK_EQ(rc, GUARD_EXC(13));
        }
    }
    t_ops(4096); return (u32)delta;
}

static volatile u32 msr_index;
static volatile u64 msr_value;
static void msr_read(void) { msr_value = rdmsr(msr_index); }
static void msr_write(void) { wrmsr(msr_index, msr_value); }
static int get_msr(u32 index, u64 *value, int report) {
    msr_index = index; msr_value = 0;
    int rc = guard_call(msr_read);
    if (report) {
        if (!rc) t_note("MSR 0x%03x = %016llx", index, msr_value);
        else t_note("MSR 0x%03x #GP rc=%x", index, rc);
    }
    if (!rc && value) *value = msr_value;
    CHECK(rc == 0 || rc == GUARD_EXC(13) || rc == GUARD_EXC(6));
    return rc;
}
static u32 sys_msr_probe(void) {
    u64 value, caps = 0; u32 sum = 0;
    if (cpu.family == 5) {
        const u32 p5[] = {0, 1, 0x11, 0x12, 0x13};
        for (u32 i = 0; i < sizeof(p5) / sizeof(p5[0]); i++) if (!get_msr(p5[i], &value, 1)) sum = csum_mix(sum, value);
    }
    if (cpu_test_feature(REQ_TSC)) {
        u64 before = stamp(); int rc = get_msr(0x10, &value, 0); u64 after = stamp();
        if (!rc) {
            CHECK(value >= before && value <= after);
            t_note("MSR 0x010 = %016llx RDTSC before=%llx after=%llx", value, before, after);
        }
        else t_note("MSR 0x010 #GP");
    }
    if (cpu_test_feature(REQ_APIC)) get_msr(0x1B, &value, 1);
    if (cpu.family >= 6) {
        const u32 p6[] = {0x2A, 0x3A, 0x8B, 0xC1, 0xC2, 0x186, 0x187, 0x1D9};
        for (u32 i = 0; i < sizeof(p6) / sizeof(p6[0]); i++) if (!get_msr(p6[i], &value, 1)) sum = csum_mix(sum, value);
    }
    if (cpu_test_feature(REQ_SEP)) for (u32 i = 0x174; i <= 0x176; i++) get_msr(i, &value, 1);
    if (cpu_test_feature(REQ_MCE) && cpu.family >= 6) for (u32 i = 0x179; i <= 0x17B; i++) get_msr(i, &value, 1);
    if (cpu_test_feature(REQ_MTRR) && !get_msr(0xFE, &caps, 1)) {
        u32 n = (u32)caps & 255;
        t_note("MTRR variable_count=%u (first eight pairs maximum)", n);
        for (u32 i = 0; i < n && i < 8; i++) {
            CHECK_EQ(get_msr(0x200 + i * 2, &value, 1), 0);
            CHECK_EQ(get_msr(0x201 + i * 2, &value, 1), 0);
        }
        if (caps & 0x100) {
            get_msr(0x250, &value, 1); get_msr(0x258, &value, 1); get_msr(0x259, &value, 1);
            for (u32 i = 0x268; i <= 0x26F; i++) get_msr(i, &value, 1);
        }
        get_msr(0x2FF, &value, 1);
    }
    if (cpu_test_feature(REQ_PAT)) get_msr(0x277, &value, 1);
    t_ops(1); return sum;
}

static int put_msr(u32 index, u64 value) {
    msr_index = index; msr_value = value; return guard_call(msr_write);
}
static u32 sys_msr_tsc(void) {
    u64 before = rdtsc(), value = 0;
    int rc = get_msr(0x10, &value, 0);
    u64 after = rdtsc();
    CHECK_EQ(rc, 0);
    if (!rc) CHECK(value >= before && value <= after);
    t_ops(1);
    return (u32)value;
}

static int memory_type(u32 type) {
    return type == 0 || type == 1 || type == 4 || type == 5 || type == 6;
}

static u32 sys_mtrr_state(void) {
    u64 caps = 0, value = 0;
    int rc = get_msr(0xfe, &caps, 0);
    CHECK_EQ(rc, 0);
    if (rc) return 0;
    CHECK_EQ(get_msr(0x2ff, &value, 0), 0);
    CHECK(memory_type(value & 255));
    u32 count = (u32)caps & 255;
    for (u32 i = 0; i < count; i++) {
        rc = get_msr(0x200 + 2 * i, &value, 0);
        CHECK_EQ(rc, 0);
        if (!rc) CHECK(memory_type(value & 255));
        CHECK_EQ(get_msr(0x201 + 2 * i, &value, 0), 0);
    }
    if (caps & 0x100) {
        const u32 fixed[] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26a,
                             0x26b, 0x26c, 0x26d, 0x26e, 0x26f};
        for (u32 i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
            rc = get_msr(fixed[i], &value, 0);
            CHECK_EQ(rc, 0);
            if (!rc) for (u32 j = 0; j < 8; j++) CHECK(memory_type((value >> (8 * j)) & 255));
        }
    }
    t_note("MTRR register inventory verified; cache-type effects not measured");
    t_ops(count + 1);
    return (u32)caps;
}

static u32 sys_pat_state(void) {
    u64 value = 0, got = 0;
    int rc = get_msr(0x277, &value, 0);
    CHECK_EQ(rc, 0);
    if (rc) return 0;
    for (u32 i = 0; i < 8; i++) {
        u32 type = (value >> (8 * i)) & 255;
        CHECK(memory_type(type) || type == 7);
    }
    CHECK_EQ(put_msr(0x277, value), 0);
    CHECK_EQ(get_msr(0x277, &got, 0), 0);
    CHECK(got == value);
    t_note("PAT read/unchanged-write verified; cache-type effects not measured");
    t_ops(8);
    return (u32)value;
}

static u32 sys_mce_control(void) {
    u32 saved = rd_cr4();
    arg = saved ^ 0x40;
    int rc = guard_call(write_cr4);
    CHECK_EQ(rc, 0);
    if (!rc) CHECK_EQ(rd_cr4() & 0x40, arg & 0x40);
    arg = saved;
    CHECK_EQ(guard_call(write_cr4), 0);
    t_note("CR4.MCE control verified; external machine-check injection not covered");
    t_ops(1);
    return saved;
}

static u32 sys_mca_state(void) {
    if (!((cpu.feat_edx | cpu_extended_standard_edx() | cpu_raw_required(0)) & (1u << 14)))
        SKIP("missing_mca");
    u64 caps = 0, status = 0, value = 0;
    int rc = get_msr(0x179, &caps, 0);
    CHECK_EQ(rc, 0);
    if (rc) return 0;
    u32 count = (u32)caps & 255;
    if (cpu_raw_required(0) & (1u << 14)) CHECK_EQ(count, 5);
    CHECK_EQ(get_msr(0x17a, &value, 0), 0);
    if (caps & 0x100) CHECK_EQ(get_msr(0x17b, &value, 0), 0);
    for (u32 bank = 0; bank < count; bank++) {
        CHECK_EQ(get_msr(0x400 + 4 * bank, &value, 0), 0);
        rc = get_msr(0x401 + 4 * bank, &status, 0);
        CHECK_EQ(rc, 0);
        if (!rc && (status & (1ull << 58)))
            CHECK_EQ(get_msr(0x402 + 4 * bank, &value, 0), 0);
        if (!rc && (status & (1ull << 59)))
            CHECK_EQ(get_msr(0x403 + 4 * bank, &value, 0), 0);
    }
    t_note("MCA bank/register inventory; no external error injection or status clearing");
    t_ops(count + 1);
    return (u32)caps;
}

static u32 sys_msr_rw(void) {
    u64 saved[3], got; int saved_n = 0;
    if (cpu_test_feature(REQ_SEP)) {
        for (u32 i = 0; i < 3; i++) {
            if (get_msr(0x174 + i, &saved[i], 0)) break;
            saved_n++;
        }
        if (saved_n == 3) {
            const u32 values[] = {SEL_KCODE, 0x90000, 0x11000};
            for (u32 i = 0; i < 3; i++) {
                int rc = put_msr(0x174 + i, values[i]);
                if (!rc) {
                    int read_rc = get_msr(0x174 + i, &got, 0);
                    CHECK_EQ(read_rc, 0);
                    if (!read_rc) CHECK_EQ(got, values[i]);
                }
                CHECK_EQ(put_msr(0x174 + i, saved[i]), 0);
                CHECK_EQ(rc, 0);
            }
        } else t_note("SYSENTER MSRs unavailable; no writes attempted");
    }
    if (cpu_test_feature(REQ_PAT) && !get_msr(0x277, &got, 0) && got == 0x0007040600070406ull) {
        /* Only rewrite the default PAT when it already is the live value. */
        CHECK_EQ(put_msr(0x277, got), 0);
        u64 readback;
        int rc = get_msr(0x277, &readback, 0);
        CHECK_EQ(rc, 0);
        if (!rc) CHECK(readback == got);
        t_note("PAT unchanged=%016llx", got);
    }
    if (cpu_test_feature(REQ_TSC)) {
        /* Intel P6+ (SDM vol. 3, "Time-Stamp Counter") and VIA C3 write only
           TSC[31:0] and clear TSC[63:32]; Pentium, AMD, Cyrix and WinChip
           write all 64 bits. A low-only write with high bits set would rewind
           the counter that stamp() uses, so only write in the same epoch. */
        int low_only = cpu.family >= 6 && (!kmemcmp(cpu.vendor, "GenuineIntel", 12) ||
                                           !kmemcmp(cpu.vendor, "CentaurHauls", 12));
        u64 before = rdtsc();
        if (low_only && ((before >> 32) || before > 0xf0000000ull)) {
            t_note("TSC write skipped: P6 clears TSC[63:32] and tsc=%016llx", before);
        } else {
            int rc = put_msr(0x10, before);
            u64 after = rdtsc();
            if (!rc) CHECK(after >= before);
            if (!rc && low_only) CHECK_EQ(after >> 32, 0);
            /* Never rewind the TSC: restore its epoch with a fresh, forward value. */
            if (!rc) CHECK_EQ(put_msr(0x10, after + (rdtsc() - after)), 0);
            t_note("TSC same-epoch write rc=%x; elapsed write overhead intentionally not compensated", rc);
        }
    }
    return saved_n;
}

static void write_cr2(u32 v) { __asm__ volatile("mov %0,%%cr2" :: "r"(v) : "memory"); }
static volatile u32 debug_saved[6];
static void debug_roundtrip(void) {
    __asm__ volatile("mov %%dr0,%%eax; mov %%eax,%0" : "=m"(debug_saved[0]) :: "eax");
    __asm__ volatile("mov %%dr1,%%eax; mov %%eax,%0" : "=m"(debug_saved[1]) :: "eax");
    __asm__ volatile("mov %%dr2,%%eax; mov %%eax,%0" : "=m"(debug_saved[2]) :: "eax");
    __asm__ volatile("mov %%dr3,%%eax; mov %%eax,%0" : "=m"(debug_saved[3]) :: "eax");
    __asm__ volatile("mov %%dr6,%%eax; mov %%eax,%0" : "=m"(debug_saved[4]) :: "eax");
    __asm__ volatile("mov %%dr7,%%eax; mov %%eax,%0" : "=m"(debug_saved[5]) :: "eax");
    u32 v = 0x400; __asm__ volatile("mov %0,%%dr7" :: "r"(v));
    v = 0x12345000; __asm__ volatile("mov %0,%%dr0; mov %%dr0,%0" : "+r"(v)); CHECK_EQ(v, 0x12345000);
    v = 0x23456000; __asm__ volatile("mov %0,%%dr1; mov %%dr1,%0" : "+r"(v)); CHECK_EQ(v, 0x23456000);
    v = 0x34567000; __asm__ volatile("mov %0,%%dr2; mov %%dr2,%0" : "+r"(v)); CHECK_EQ(v, 0x34567000);
    v = 0x45678000; __asm__ volatile("mov %0,%%dr3; mov %%dr3,%0" : "+r"(v)); CHECK_EQ(v, 0x45678000);
    v = debug_saved[4] ^ 15; __asm__ volatile("mov %0,%%dr6; mov %%dr6,%0" : "+r"(v)); CHECK_EQ(v & 15, (debug_saved[4] ^ 15) & 15);
    v = 0x400; __asm__ volatile("mov %0,%%dr7; mov %%dr7,%0" : "+r"(v)); CHECK_EQ(v & 0xFF, 0);
    __asm__ volatile("mov %0,%%dr0" :: "r"(debug_saved[0]));
    __asm__ volatile("mov %0,%%dr1" :: "r"(debug_saved[1]));
    __asm__ volatile("mov %0,%%dr2" :: "r"(debug_saved[2]));
    __asm__ volatile("mov %0,%%dr3" :: "r"(debug_saved[3]));
    __asm__ volatile("mov %0,%%dr6" :: "r"(debug_saved[4]));
    /* Tests leave every hardware breakpoint disabled. */
    v = debug_saved[5] & ~0xFFu; __asm__ volatile("mov %0,%%dr7" :: "r"(v));
}
static void dr4_read(void) { u32 v; __asm__ volatile("mov %%dr4,%0" : "=r"(v)); arg = v; }
static void dr5_read(void) { u32 v; __asm__ volatile("mov %%dr5,%0" : "=r"(v)); arg = v; }
static u32 sys_debug_extensions(void) {
    u32 saved = rd_cr4();
    arg = saved | 8;
    int rc = guard_call(write_cr4), rc4 = rc, rc5 = rc;
    if (!rc) {
        rc4 = guard_call(dr4_read);
        rc5 = guard_call(dr5_read);
    }
    arg = saved;
    CHECK_EQ(guard_call(write_cr4), 0);
    CHECK_EQ(rc, 0);
    CHECK_EQ(rc4, GUARD_EXC(6));
    CHECK_EQ(rc5, GUARD_EXC(6));
    t_ops(2);
    return saved;
}
static u32 sys_cr_regs(void) {
    u32 save0 = rd_cr0(), save2 = rd_cr2(), save3 = rd_cr3();
    const u32 bits[] = {0x10000, 0x40000, 0x20, 2};
    for (u32 i = 0; i < 4; i++) {
        wr_cr0(save0 ^ bits[i]); CHECK_EQ(rd_cr0() & bits[i], (save0 ^ bits[i]) & bits[i]); wr_cr0(save0);
    }
    wr_cr3(save3 | 0x18); CHECK_EQ(rd_cr3() & 0x18, 0x18); wr_cr3(save3);
    write_cr2(0x76543210); CHECK_EQ(rd_cr2(), 0x76543210); write_cr2(save2);
    int drc = guard_call(debug_roundtrip); t_note("debug-register round trips rc=%x", drc);
    CHECK(drc == 0 || drc == GUARD_EXC(13) || drc == GUARD_EXC(6) || drc == GUARD_EXC(1));
    if (guard_call(probe_cr4)) { t_note("CR4 unavailable"); return save0; }
    u32 save4 = arg, supported = 0;
    if (cpu_test_feature(REQ_VME)) supported |= 3;
    if (cpu_test_feature(REQ_TSC)) supported |= 4;
    if (cpu_test_feature(REQ_DE)) supported |= 8;
    if (cpu_test_feature(REQ_PSE)) supported |= 0x10;
    if (cpu_test_feature(REQ_MCE)) supported |= 0x40;
    if (cpu_test_feature(REQ_PGE)) supported |= 0x80;
    if (cpu.family >= 6) supported |= 0x100;
    if (cpu_test_feature(REQ_FXSR) && (cpu.feat_edx & (1u << 25))) supported |= 0x200;
    for (u32 bit = 1; bit <= 0x200; bit <<= 1) if (supported & bit) {
        arg = save4 ^ bit; int rc = guard_call(write_cr4);
        if (!rc) {
            CHECK_EQ(rd_cr4() & bit, (save4 ^ bit) & bit);
        }
        else t_note("CR4 bit=%x rejected rc=%x", bit, rc);
        arg = save4; CHECK_EQ(guard_call(write_cr4), 0);
    }
    int rc;
    if (!cpu.hv) {
        arg = save4 | 0x80000000u; rc = guard_call(write_cr4);
        arg = save4; CHECK_EQ(guard_call(write_cr4), 0); CHECK_EQ(rc, GUARD_EXC(13));
    } else t_note("reserved CR4 write omitted under hypervisor: some TCG versions spin instead of #GP");
    if (cpu_test_feature(REQ_DE)) {
        arg = save4 | 8; rc = guard_call(write_cr4);
        if (!rc) {
            int rc4 = guard_call(dr4_read), rc5 = guard_call(dr5_read);
            arg = save4; CHECK_EQ(guard_call(write_cr4), 0);
            CHECK_EQ(rc4, GUARD_EXC(6)); CHECK_EQ(rc5, GUARD_EXC(6));
        }
    }
    t_note("CR4 exercised=%x; PAE excluded while paging enabled", supported); return save0 ^ save4;
}

static void read_pmc(void) {
    u32 lo, hi; __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(arg)); sampled = ((u64)hi << 32) | lo;
}
static u32 sys_rdpmc(void) {
    if (guard_call(probe_cr4)) SKIP("CR4 unavailable");
    u32 saved = arg; arg = saved | 0x100;
    int enable = guard_call(write_cr4), good = 0;
    if (!enable) for (u32 i = 0; i < 2; i++) {
        arg = i; int rc = guard_call(read_pmc);
        if (!rc) { t_note("RDPMC counter=%u value=%llx", i, sampled); good++; }
        else t_note("RDPMC counter=%u trapped rc=%x", i, rc);
    }
    arg = saved & ~0x100u; int rc = guard_call(write_cr4);
    u32 stack = r3_stack();
    if (!rc && good && stack) {
        int urc = ring3_call(user_pmc, stack);
        CHECK_EQ(urc, GUARD_EXC(13));
    }
    arg = saved; CHECK_EQ(guard_call(write_cr4), 0);
    if (!good) SKIP("RDPMC unavailable or trapped");
    return good;
}

static void cpuid_sub(u32 leaf, u32 sub, u32 out[4]) {
    __asm__ volatile("cpuid" : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3]) : "0"(leaf), "2"(sub));
}
static u32 sys_cpuid_leaves(void) {
    u32 v[4], ref[4], sum = 0; char vendor[13];
    cpuid_sub(0, 0, v); kmemcpy(vendor, &v[1], 4); kmemcpy(vendor + 4, &v[3], 4); kmemcpy(vendor + 8, &v[2], 4); vendor[12] = 0;
    CHECK_EQ(kmemcmp(vendor, cpu.vendor, 12), 0);
    for (u32 i = 0; i <= cpu.max_leaf && i < 16; i++) {
        cpuid_sub(i, 0, v); t_note("CPUID %08x: %08x %08x %08x %08x", i, v[0], v[1], v[2], v[3]);
        for (u32 j = 0; j < 4; j++) sum = csum_mix(sum, v[j]);
    }
    if (cpu.ext_max >= 0x80000000u) for (u32 i = 0; i < 16 && i <= cpu.ext_max - 0x80000000u; i++) {
        cpuid_sub(0x80000000u + i, 0, v); t_note("CPUID %08x: %08x %08x %08x %08x", 0x80000000u + i, v[0], v[1], v[2], v[3]);
    }
    if (cpu.max_leaf >= 1) {
        cpuid_sub(1, 0, ref);
        for (u32 i = 0; i < 32; i++) { cpuid_sub(1, 0, v); CHECK_EQ(kmemcmp(v, ref, sizeof(v)), 0); }
    }
    if (cpu.max_leaf >= 2) {
        cpuid_sub(2, 0, ref);
        if ((ref[0] & 255) == 1) for (u32 i = 1; i < 4; i++) {
            cpuid_sub(2, i, v); CHECK_EQ(kmemcmp(v, ref, sizeof(v)), 0);
        } else t_note("leaf 2 is multi-iteration: subleaf equality not required");
    }
    t_ops(32); return sum;
}

static void pit2_delay_500(void) {
    u8 saved = inb(0x61);
    for (u32 k = 0; k < 10; k++) {
        outb(0x61, saved & ~3u); outb(0x43, 0xB0);
        outb(0x42, 59659 & 255); outb(0x42, 59659 >> 8); outb(0x61, (saved & ~2u) | 1);
        u32 spins = 20000000;
        while (!(inb(0x61) & 0x20) && --spins) { }
        CHECK(spins != 0);
    }
    outb(0x61, saved);
}
static u32 sys_irq_timer(void) {
    sti(); u32 start_tick = pit_ticks; u64 start = stamp();
    if (cpu_test_feature(REQ_TSC) && cpu.tsc_per_ms) {
        while (rdtsc() - start < cpu.tsc_per_ms * 500) hlt();
    } else pit2_delay_500();
    u32 ticks = pit_ticks - start_tick; CHECK(ticks >= 40 && ticks <= 60);
    t_note("500ms IRQ0 ticks=%u expected=50", ticks);
    if (cpu_test_feature(REQ_TSC)) {
        u32 t = pit_ticks; u64 begin = rdtsc();
        while (pit_ticks - t < 4) { __asm__ volatile("sti; hlt" ::: "memory"); }
        t_note("STI/HLT cycles/tick=%llu", (rdtsc() - begin) / (pit_ticks - t));
    }
    t_ops(ticks); return ticks;
}

const test_t tests_sys[] = {
    {"sys_tsc", sys_tsc, REQ_TSC},
    {"sys_msr_probe", sys_msr_probe, REQ_MSR},
    {"sys_msr_rw", sys_msr_rw, REQ_MSR | REQ_P6 | REQ_NOHV},
    {"sys_cr_regs", sys_cr_regs, REQ_486},
    {"sys_rdpmc", sys_rdpmc, REQ_P5},
    {"sys_msr_tsc", sys_msr_tsc, REQ_MSR | REQ_TSC},
    {"sys_mtrr_state", sys_mtrr_state, REQ_MTRR | REQ_MSR},
    {"sys_pat_state", sys_pat_state, REQ_PAT | REQ_MSR},
    {"sys_mce_control", sys_mce_control, REQ_MCE},
    {"sys_mca_state", sys_mca_state, REQ_MSR},
    {"sys_debug_extensions", sys_debug_extensions, REQ_DE},
    {"sys_cpuid_leaves", sys_cpuid_leaves, REQ_CPUID},
    {"sys_irq_timer", sys_irq_timer, 0},
    {0, 0, 0}
};
