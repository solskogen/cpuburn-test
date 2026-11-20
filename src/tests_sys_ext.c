/* Emulator-path benchmarks, paging ON: TLB flush/capacity, CR3 and task
 * switches, port I/O, ring transitions, interrupt delivery, page-fault storms,
 * idle HLT, LAPIC timer, MMIO stores, locked RMW and DR7 writes. Each record
 * counts one fixed unit and checks its result once against a reference; the
 * 86Box path each one targets is listed in doc/workloads.md. */
#include "kernel.h"

#define PG 4096u
#define XW 0x60000000u /* private window; no other module maps it */

static volatile u32 xs_cnt __attribute__((used)), xs_n __attribute__((used));
static u32 xs_task_far[2] __attribute__((used));
static u32 xs_main_far[2] __attribute__((used)) = { 0, SEL_TSS_MAIN };
extern void xs_isr_lean(void), xs_isr_seg(void), xs_cg_stub(void), xs_sep_stub(void);
extern void xs_sc_stub(void), xs_apic_isr(void), xs_task_jmp(void), xs_task_iret(void);
extern void xs_r3_int(void), xs_r3_int_seg(void), xs_r3_cg(void), xs_r3_sep(void), xs_r3_sc(void);

/* Lean handlers: only a counter, no harness ISR path. The ring-3 loops run
 * xs_n round trips, then leave through the harness INT 0x80 gate. */
__asm__(
    ".text\n"
    ".globl xs_isr_lean,xs_isr_seg,xs_cg_stub,xs_sep_stub,xs_sc_stub,xs_apic_isr\n"
    ".globl xs_task_jmp,xs_task_iret,xs_r3_int,xs_r3_int_seg,xs_r3_cg,xs_r3_sep,xs_r3_sc\n"
    "xs_isr_lean: incl xs_cnt\n iretl\n"
    "xs_isr_seg: pushl %ds\n pushl %es\n pushl %eax\n movw $0x10,%ax\n movw %ax,%ds\n"
    " movw %ax,%es\n incl xs_cnt\n popl %eax\n popl %es\n popl %ds\n iretl\n"
    "xs_cg_stub: incl xs_cnt\n lret\n"
    "xs_sep_stub: incl xs_cnt\n sysexit\n"
    "xs_sc_stub: incl xs_cnt\n sysret\n"
    /* The xs_n-th interrupt masks the timer itself, so the count is exact. */
    "xs_apic_isr: pushl %eax\n movl xs_cnt,%eax\n incl %eax\n movl %eax,xs_cnt\n"
    " cmpl xs_n,%eax\n jb 1f\n movl $0x10091,0xfee00320\n movl $0,0xfee00380\n"
    "1: movl $0,0xfee000b0\n popl %eax\n iretl\n"
    "xs_task_jmp: incl xs_cnt\n ljmp *xs_main_far\n jmp xs_task_jmp\n"
    "xs_task_iret: incl xs_cnt\n iretl\n jmp xs_task_iret\n"
    "xs_r3_int: movl xs_n,%esi\n 1: int $0x88\n decl %esi\n jnz 1b\n int $0x80\n"
    "xs_r3_int_seg: movl xs_n,%esi\n 1: int $0x89\n decl %esi\n jnz 1b\n int $0x80\n"
    "xs_r3_cg: movl xs_n,%esi\n 1: lcall $0x43,$0\n decl %esi\n jnz 1b\n int $0x80\n"
    "xs_r3_sep: movl xs_n,%esi\n 1: movl %esp,%ecx\n movl $2f,%edx\n sysenter\n"
    "2: decl %esi\n jnz 1b\n int $0x80\n"
    "xs_r3_sc: movl xs_n,%esi\n 1: syscall\n decl %esi\n jnz 1b\n int $0x80\n");

/* Private page directory sharing the harness identity map. */
static u32 *xs_pd(void) {
    u32 *pd = arena_alloc(PG, PG);
    if (pd) kmemcpy(pd, pg_dir, PG);
    return pd;
}
static u32 *xs_pt(u32 *pd, u32 va) {
    u32 *pt = arena_alloc(PG, PG);
    if (pt) { kmemset(pt, 0, PG); pd[va >> 22] = (u32)pt | 7; }
    return pt;
}
static u32 xs_touch(u32 base, u32 pages, u32 passes) {
    u32 sum = 0;
    for (u32 p = 0; p < passes; p++)
        for (u32 i = 0; i < pages; i++) sum += *(volatile u32 *)(base + i * PG);
    return sum;
}
static void xs_done(const char *unit, u32 ops) { t_work_unit(unit); t_ops(ops); }

/* ---- INVLPG with a hot working set: one INVLPG of an unrelated page, then
 * every page of the set is loaded again. */
static u32 invlpg_ws(u32 n) {
    const u32 rounds = 512;
    u8 *buf = arena_alloc((n + 1) * PG, PG);
    if (!buf) SKIP("arena too small");
    u32 ref = 0;
    for (u32 i = 0; i < n; i++) ref += *(u32 *)(buf + i * PG) = i * 0x9E3779B9u + n;
    u32 sum = xs_touch((u32)buf, n, 1);
    for (u32 r = 0; r < rounds; r++) {
        invlpg(buf + n * PG);
        sum += xs_touch((u32)buf, n, 1);
    }
    CHECK_EQ(sum, ref * (rounds + 1));
    xs_done("invlpg", rounds);
    return sum;
}
static u32 pg_invlpg_ws_64(void) { return invlpg_ws(64); }
static u32 pg_invlpg_ws_256(void) { return invlpg_ws(256); }
static u32 pg_invlpg_ws_1024(void) { return invlpg_ws(1024); }

/* ---- n distinct virtual pages (aliasing 64 frames) loaded round-robin,
 * 262,144 loads per invocation whatever n is. */
static u32 tlb_capacity(u32 n) {
    const u32 passes = 262144 / n;
    u32 *pd = xs_pd(), *fr = arena_alloc(64 * PG, PG), ref = 0;
    if (!pd || !fr) SKIP("arena too small");
    for (u32 t = 0; t * 1024 < n; t++) {
        u32 *pt = xs_pt(pd, XW + t * 0x400000);
        if (!pt) SKIP("arena too small");
        for (u32 i = 0; i < 1024 && t * 1024 + i < n; i++)
            pt[i] = ((u32)fr + ((t * 1024 + i) & 63) * PG) | 3;
    }
    for (u32 j = 0; j < 64; j++) fr[j * 1024] = j * 0x9E3779B9u + 1;
    for (u32 i = 0; i < n; i++) ref += fr[(i & 63) * 1024];
    wr_cr3((u32)pd);
    u32 sum = xs_touch(XW, n, passes);
    wr_cr3((u32)pg_dir);
    CHECK_EQ(sum, ref * passes);
    xs_done("page_touch", n * passes);
    return sum;
}
static u32 pg_tlb_capacity_64(void) { return tlb_capacity(64); }
static u32 pg_tlb_capacity_256(void) { return tlb_capacity(256); }
static u32 pg_tlb_capacity_1024(void) { return tlb_capacity(1024); }
static u32 pg_tlb_capacity_4096(void) { return tlb_capacity(4096); }

/* ---- Eight 4 MiB PSE pages, all mapping physical 4..8 MiB, one load per
 * 4 KiB: eight large-page TLB entries on hardware, 8,192 4 KiB lookups in
 * an emulator that splits large pages. */
static u32 pg_pse_walk(void) {
    u32 *pd = xs_pd(), ref = 0, cr4 = rd_cr4();
    if (!pd || (u32)pd >= 0x400000u) SKIP("no page directory below 4 MiB");
    for (u32 s = 0; s < 8; s++) pd[(XW >> 22) + s] = 0x400000u | 0x83;
    for (u32 k = 0; k < 1024; k++)
        ref += *(u32 *)(0x400000u + k * PG) = k * 0x9E3779B9u ^ 0x5A5A5A5Au;
    wr_cr4(cr4 | 0x10);
    wr_cr3((u32)pd);
    u32 sum = xs_touch(XW, 8192, 8);
    wr_cr3((u32)pg_dir);
    wr_cr4(cr4);
    CHECK_EQ(sum, ref * 64);
    xs_done("page_touch", 65536);
    return sum;
}

/* ---- Four address spaces sharing a 32-page supervisor region (global with
 * PGE), each with 64 private pages: MOV CR3, load every shared and private
 * page, next space. */
static u32 ctx_switch(u32 g) {
    enum { D = 4, K = 32, P = 64, S = 1024 };
    u32 *pds[D], *kpt = arena_alloc(PG, PG);
    u32 *kfr = arena_alloc(K * PG, PG), *pfr = arena_alloc(D * P * PG, PG);
    u32 ref = 0, sum = 0, cr4 = g ? rd_cr4() : 0;
    if (!kpt || !kfr || !pfr) SKIP("arena too small");
    kmemset(kpt, 0, PG);
    for (u32 k = 0; k < K; k++) {
        kpt[k] = ((u32)kfr + k * PG) | 3 | g;
        ref += kfr[k * 1024] = k * 0x01000193u + 1;
    }
    ref *= S;
    for (u32 d = 0; d < D; d++) {
        u32 *ppt;
        if (!(pds[d] = xs_pd()) || !(ppt = xs_pt(pds[d], XW + 0x400000))) SKIP("arena too small");
        pds[d][XW >> 22] = (u32)kpt | 7;
        for (u32 p = 0; p < P; p++) {
            u32 *f = pfr + (d * P + p) * 1024;
            ppt[p] = (u32)f | 7;
            ref += (*f = (d * P + p) * 0x9E3779B9u + 7) * (S / D);
        }
    }
    if (g) wr_cr4(cr4 | 0x80);
    for (u32 s = 0; s < S; s++) {
        wr_cr3((u32)pds[s & (D - 1)]);
        sum += xs_touch(XW, K, 1) + xs_touch(XW + 0x400000, P, 1);
    }
    wr_cr3((u32)pg_dir);
    if (g) wr_cr4(cr4 & ~0x80u); /* drops the window's global entries */
    CHECK_EQ(sum, ref);
    xs_done("cr3_switch", S);
    return sum;
}
static u32 pg_ctx_switch(void) { return ctx_switch(0); }
static u32 pg_ctx_switch_pge(void) { return ctx_switch(0x100); }

/* ---- 32-bit TSS ping-pong between tss_main and tss_b, same CR3. */
static u32 task_loop(u32 call) {
    const u32 n = 8192;
    tss32_t saved = tss_b;
    u64 desc = gdt[6];
    u8 *stack = arena_alloc(PG, 16);
    if (!stack) SKIP("arena too small");
    kmemset(&tss_b, 0, sizeof tss_b);
    tss_b.cr3 = tss_main.cr3 = rd_cr3();
    tss_b.eip = call ? (u32)xs_task_iret : (u32)xs_task_jmp;
    tss_b.eflags = 2;
    tss_b.esp = (u32)stack + PG;
    tss_b.cs = SEL_KCODE;
    tss_b.ss = tss_b.ds = tss_b.es = tss_b.fs = tss_b.gs = SEL_KDATA;
    tss_b.iomap = sizeof tss_b;
    xs_task_far[0] = 0; xs_task_far[1] = SEL_TSS_B;
    xs_cnt = 0;
    u32 f = rd_eflags();
    cli();
    if (call) for (u32 i = 0; i < n; i++) __asm__ volatile("lcall *xs_task_far" ::: "memory");
    else for (u32 i = 0; i < n; i++) __asm__ volatile("ljmp *xs_task_far" ::: "memory");
    wr_eflags(f);
    CHECK_EQ(xs_cnt, n);
    CHECK_EQ((u32)(gdt[5] >> 40) & 15, 0xB);
    CHECK_EQ((u32)(gdt[6] >> 40) & 15, 9);
    tss_b = saved;
    gdt[6] = desc;
    xs_done("task_switch", 2 * n);
    return xs_cnt;
}
static u32 pm_task_switch_loop_jmp(void) { return task_loop(0); }
static u32 pm_task_switch_loop_call(void) { return task_loop(1); }

/* ---- Port polling, fixed counts. Results are device timing, so only the
 * PIT count range is checked; the configuration is left as found. */
static u32 io_poll(u32 kind) {
    const u32 n = 8192;
    u16 crt = bda_read16(0x63) == 0x3B4 ? 0x3BA : 0x3DA;
    u32 bad = 0;
    for (u32 i = 0; i < n; i++) {
        switch (kind) {
        case 0: (void)inb(crt); break;
        case 1: (void)inb(0x1F7); break;
        case 2: outb(0x80, 0); break;
        case 3: { /* counter latch: the mode/divisor stay programmed */
            u32 f = rd_eflags();
            cli();
            outb(0x43, 0);
            u32 c = inb(0x40);
            c |= (u32)inb(0x40) << 8;
            wr_eflags(f);
            bad += c > 11932;
            break;
        }
        default: outb(0x20, 0x0A); (void)inb(0x20); break; /* OCW3 read IRR */
        }
    }
    CHECK_EQ(bad, 0);
    xs_done(kind == 2 ? "port_write" : kind == 3 ? "pit_latch_read" : kind == 4 ? "pic_irr_read" : "port_read", n);
    return n;
}
static u32 io_poll_3da(void) { return io_poll(0); }
static u32 io_poll_1f7(void) { return io_poll(1); }
static u32 io_poll_out80(void) { return io_poll(2); }
static u32 io_poll_pit_latch(void) { return io_poll(3); }
static u32 io_poll_pic_irr(void) { return io_poll(4); }

/* ---- Interrupt delivery through lean gates, from ring 0 or ring 3. */
static u32 r3_loop(void (*entry)(void), u32 n) {
    u8 *stack = arena_alloc(PG, 16);
    if (!stack) SKIP("arena too small");
    xs_cnt = 0; xs_n = n;
    int rc = ring3_call(entry, (u32)stack + PG);
    CHECK_EQ(rc, GUARD_R3_INT80);
    CHECK_EQ(xs_cnt, n);
    xs_done("round_trip", n);
    return xs_cnt;
}
static u32 int_lean(u32 seg, u32 ring3) {
    const u32 n = 200000;
    idt_set_gate(0x88, xs_isr_lean, SEL_KCODE, 0xEE);
    idt_set_gate(0x89, xs_isr_seg, SEL_KCODE, 0xEE);
    if (ring3) return r3_loop(seg ? xs_r3_int_seg : xs_r3_int, n);
    u32 c = n;
    xs_cnt = 0;
    if (seg) __asm__ volatile("1: int $0x89\n decl %0\n jnz 1b" : "+r"(c) :: "memory", "cc");
    else __asm__ volatile("1: int $0x88\n decl %0\n jnz 1b" : "+r"(c) :: "memory", "cc");
    CHECK_EQ(xs_cnt, n);
    xs_done("interrupt", n);
    return xs_cnt;
}
static u32 pm_int_lean_r0(void) { return int_lean(0, 0); }
static u32 pm_int_lean_seg_r0(void) { return int_lean(1, 0); }
static u32 pm_int_lean_r3(void) { return int_lean(0, 1); }
static u32 pm_int_lean_seg_r3(void) { return int_lean(1, 1); }

static u32 pm_rt_callgate(void) {
    u32 p = (u32)xs_cg_stub;
    gdt_set_raw(8, (p & 0xFFFF) | ((u32)SEL_KCODE << 16), (p & 0xFFFF0000u) | 0xEC00); /* 32-bit, DPL3 */
    return r3_loop(xs_r3_cg, 65536);
}

static u32 pm_rt_sysenter(void) {
    u8 *kstack = arena_alloc(PG, 16);
    if (!kstack) SKIP("arena too small");
    u64 cs = rdmsr(0x174), sp = rdmsr(0x175), ip = rdmsr(0x176);
    wrmsr(0x174, SEL_KCODE);
    wrmsr(0x175, (u32)kstack + PG);
    wrmsr(0x176, (u32)xs_sep_stub);
    u32 r = r3_loop(xs_r3_sep, 65536);
    wrmsr(0x174, cs); wrmsr(0x175, sp); wrmsr(0x176, ip);
    return r;
}

/* AMD legacy SYSCALL keeps ESP, so the stub runs on the ring-3 stack. */
static u64 xs_efer, xs_star;
static int xs_sc_saved;
static void sc_on(void) {
    xs_efer = rdmsr(0xC0000080u);
    xs_star = rdmsr(0xC0000081u);
    xs_sc_saved = 1;
    wrmsr(0xC0000081u, ((u64)SEL_UCODE << 48) | ((u64)SEL_KCODE << 32) | (u32)xs_sc_stub);
    wrmsr(0xC0000080u, xs_efer | 1);
}
static void sc_off(void) { wrmsr(0xC0000081u, xs_star); wrmsr(0xC0000080u, xs_efer); }
static u32 pm_rt_syscall(void) {
    xs_sc_saved = 0;
    int rc = guard_call(sc_on);
    u32 r = rc ? 0 : r3_loop(xs_r3_sc, 65536);
    if (xs_sc_saved) CHECK_EQ(guard_call(sc_off), 0);
    CHECK_EQ(rc, 0);
    return r;
}

/* ---- 512 page faults per invocation through the harness #PF hook: demand
 * mapping of not-present pages, or copy-on-write of read-only pages with
 * CR0.WP=1 (copy 4 KiB to a fresh frame, map it writable, restart). */
static u32 *xs_ppt, *xs_src;
static u8 *xs_frames;
static volatile u32 xs_faults, xs_cow;
static void pf_hook(regs_t *r) {
    u32 va = rd_cr2(), i = (va - XW) >> 12;
    if (i >= 512 || (r->err & 7) != (xs_cow ? 3u : 0u)) guard_recover(GUARD_EXC(14));
    u8 *f = xs_frames + i * PG;
    if (xs_cow) kmemcpy(f, xs_src, PG);
    xs_ppt[i] = (u32)f | 3;
    invlpg((void *)va);
    xs_faults++;
}
static u32 pf_storm(u32 cow) {
    enum { N = 512 };
    u32 *pd = xs_pd(), sum = 0, ref = 0, bad = 0, cr0 = rd_cr0();
    xs_ppt = pd ? xs_pt(pd, XW) : NULL;
    xs_frames = arena_alloc(N * PG, PG);
    xs_src = arena_alloc(PG, PG);
    if (!xs_ppt || !xs_frames || !xs_src) SKIP("arena too small");
    for (u32 k = 0; k < 1024; k++) xs_src[k] = k * 0x01010101u ^ 0xC0DEu;
    for (u32 i = 0; i < N; i++) {
        u32 *w = (u32 *)(xs_frames + i * PG) + (i & 1023);
        if (cow) xs_ppt[i] = (u32)xs_src | 1;
        else ref += *w = i * 0x9E3779B9u ^ 0x1234567u;
    }
    xs_cow = cow; xs_faults = 0;
    int_hook[14] = pf_hook;
    if (cow) wr_cr0(cr0 | 0x10000);
    wr_cr3((u32)pd);
    for (u32 i = 0; i < N; i++) {
        volatile u32 *v = (volatile u32 *)(XW + i * PG) + (i & 1023);
        if (cow) *v = i ^ 0xA5A5A5A5u; else sum += *v;
    }
    wr_cr3((u32)pg_dir);
    wr_cr0(cr0);
    int_hook[14] = NULL;
    CHECK_EQ(xs_faults, N);
    if (cow) for (u32 i = 0; i < N; i++) {
        u32 *f = (u32 *)(xs_frames + i * PG), k = i & 1023, k2 = (k + 1) & 1023;
        bad += f[k] != (i ^ 0xA5A5A5A5u) || f[k2] != xs_src[k2] || xs_src[k] != (k * 0x01010101u ^ 0xC0DEu);
        sum = csum_mix(sum, f[k]);
    }
    else CHECK_EQ(sum, ref);
    CHECK_EQ(bad, 0);
    xs_done("page_fault", N);
    return sum;
}
static u32 pg_demand_storm(void) { return pf_storm(0); }
static u32 pg_cow_storm(void) { return pf_storm(1); }

/* ---- STI;HLT woken by IRQ0 at 7.6 kHz (PIT divisor 157, restored). */
static u32 sys_hlt_idle(void) {
    const u32 n = 512;
    sti();
    CHECK(pit_set_multiplier(76));
    u32 f0 = pit_fast_ticks, deadline = pit_ticks + 300, halts = 0;
    while (pit_fast_ticks - f0 < n && (s32)(pit_ticks - deadline) < 0) {
        __asm__ volatile("sti; hlt" ::: "memory");
        halts++;
    }
    u32 got = pit_fast_ticks - f0;
    pit_set_multiplier(1);
    CHECK(got >= n);
    t_note("irq0_rate_hz=7600 wakeups=%u halts=%u", n, halts);
    xs_done("hlt_wakeup", n);
    return n;
}

/* ---- LAPIC periodic timer at divide-by-1 and 20 kHz (count calibrated
 * once against the PIT): a fixed number of interrupts taken by a busy loop,
 * not HLT, so the delivery path rather than idle time is exercised. */
static u64 xs_apic_base;
static u32 xs_apic_init;
static void apic_rd(void) { xs_apic_base = rdmsr(0x1B); }
static u32 sys_apic_timer_rate(void) {
    volatile u32 *a = (volatile u32 *)0xFEE00000u;
    const u32 n = 8192;
    if (guard_call(apic_rd)) SKIP("APIC_BASE unreadable");
    if ((xs_apic_base & 0xFFFFFF000ull) != 0xFEE00000u || (xs_apic_base & 0xC00) != 0x800)
        SKIP("local APIC disabled, relocated or x2APIC");
    u32 svr = a[0xF0 / 4], tpr = a[0x80 / 4], lvt = a[0x320 / 4], div = a[0x3E0 / 4], init = a[0x380 / 4];
    if (!(lvt & 0x10000) || a[0x390 / 4]) SKIP("local APIC timer already in use");
    idt_set_gate(0x91, xs_apic_isr, SEL_KCODE, 0x8E);
    xs_cnt = 0; xs_n = n;
    a[0x320 / 4] = 0x10091;
    a[0x3E0 / 4] = 0xB;
    a[0x80 / 4] = 0;
    a[0xF0 / 4] = svr | 0x100;
    sti();
    if (!xs_apic_init) {
        a[0x380 / 4] = 0xFFFFFFFFu;
        for (u32 t = pit_ticks; pit_ticks == t;) hlt();
        u32 c0 = a[0x390 / 4], t0 = pit_ticks;
        while (pit_ticks - t0 < 2) hlt();
        u32 per = (c0 - a[0x390 / 4]) / 400; /* 20 ms / 400 = 50 us */
        a[0x380 / 4] = 0;
        xs_apic_init = per < 64 ? 64 : per;
    }
    u32 deadline = pit_ticks + 300;
    a[0x320 / 4] = 0x20091;
    a[0x380 / 4] = xs_apic_init;
    while (xs_cnt < n && (s32)(pit_ticks - deadline) < 0) { }
    a[0x320 / 4] = 0x10091;
    a[0x380 / 4] = 0;
    for (u32 i = 0; i < 1000 && (a[(0x200 + 0x40) / 4] & (1u << 0x11)); i++) { } /* drain IRR */
    a[0x3E0 / 4] = div; a[0x380 / 4] = init; a[0x320 / 4] = lvt; a[0xF0 / 4] = svr; a[0x80 / 4] = tpr;
    CHECK(xs_cnt >= n && xs_cnt <= n + 1);
    t_note("timer_count=%u target_hz=20000 irqs=%u", xs_apic_init, n);
    xs_done("apic_irq", n);
    return n;
}

/* ---- 64 passes of 1,024 dword stores to plain RAM, the hidden second text
 * page (B9000) or the A0000 window (unmapped in text mode); contents are
 * saved and put back. */
static u32 mmio_store(u32 kind) {
    enum { W = 1024, P = 64 };
    u32 *save = arena_alloc(PG, 16), *ram = arena_alloc(PG, 16), bad = 0;
    if (!save || !ram) SKIP("arena too small");
    if (kind == 1 && bda_read16(0x63) == 0x3B4) SKIP("monochrome adapter: no second text page");
    volatile u32 *dst = kind == 0 ? ram : (volatile u32 *)(kind == 1 ? 0xB9000u : 0xA0000u);
    kmemcpy(save, (void *)dst, PG);
    for (u32 p = 0; p < P; p++)
        for (u32 i = 0; i < W; i++) dst[i] = i * 0x9E3779B9u + p;
    if (kind != 2) for (u32 i = 0; i < W; i++) bad += dst[i] != i * 0x9E3779B9u + P - 1;
    kmemcpy((void *)dst, save, PG);
    CHECK_EQ(bad, 0);
    xs_done("store", W * P);
    return W * P;
}
static u32 mem_mmio_vs_ram_ram(void) { return mmio_store(0); }
static u32 mem_mmio_vs_ram_text(void) { return mmio_store(1); }
static u32 mem_mmio_vs_ram_a0000(void) { return mmio_store(2); }

/* ---- Locked read-modify-write loops on one dword. */
static volatile u32 xs_lockw;
static u32 lock_rate(u32 kind) {
    const u32 n = 65536;
    u32 c = n, s = 0, fail = 0;
    xs_lockw = 0;
    if (kind == 0) {
        __asm__ volatile("1: lock addl $1,%1\n decl %0\n jnz 1b" : "+r"(c), "+m"(xs_lockw) :: "cc");
        CHECK_EQ(xs_lockw, n);
    } else if (kind == 1) {
        /* XCHG stores the count and returns the previous one: 0, then c+1. */
        __asm__ volatile("1: movl %0,%%eax\n xchgl %%eax,%2\n addl %%eax,%1\n decl %0\n jnz 1b"
                         : "+r"(c), "+r"(s), "+m"(xs_lockw) :: "eax", "cc");
        CHECK_EQ(xs_lockw, 1);
        CHECK_EQ(s, n / 2 * (n + 1) - 1);
    } else {
        __asm__ volatile("1: movl %3,%%eax\n leal 1(%%eax),%%edx\n lock cmpxchgl %%edx,%3\n"
                         " jz 2f\n incl %1\n 2: decl %0\n jnz 1b"
                         : "+r"(c), "+r"(fail), "+r"(s), "+m"(xs_lockw) :: "eax", "edx", "cc");
        CHECK_EQ(fail, 0);
        CHECK_EQ(xs_lockw, n);
    }
    xs_done("locked_op", n);
    return xs_lockw;
}
static u32 mem_lock_rate_add(void) { return lock_rate(0); }
static u32 mem_lock_rate_xchg(void) { return lock_rate(1); }
static u32 mem_lock_rate_cmpxchg(void) { return lock_rate(2); }

/* ---- Arm and disarm a DR0 write breakpoint on a dword nothing touches,
 * with 16 loads/stores in each state. */
static volatile u32 xs_dr_target;
static u32 sys_dr7_write(void) {
    const u32 n = 8192, on = 0x000D0001u; /* L0, write, 4 bytes */
    u32 *buf = arena_alloc(PG, 16), dr0, dr7;
    if (!buf) SKIP("arena too small");
    kmemset(buf, 0, PG);
    __asm__ volatile("mov %%dr0,%0; mov %%dr7,%1" : "=r"(dr0), "=r"(dr7));
    __asm__ volatile("mov %0,%%dr0" :: "r"(&xs_dr_target));
    for (u32 i = 0; i < n; i++) {
        __asm__ volatile("mov %0,%%dr7" :: "r"(on) : "memory");
        for (u32 j = 0; j < 16; j++) buf[j * 64] += i;
        __asm__ volatile("mov %0,%%dr7" :: "r"(0) : "memory");
        for (u32 j = 0; j < 16; j++) buf[j * 64] += i;
    }
    __asm__ volatile("mov %0,%%dr0; mov %1,%%dr7" :: "r"(dr0), "r"(dr7) : "memory");
    u32 bad = 0;
    for (u32 j = 0; j < 16; j++) bad += buf[j * 64] != n * (n - 1);
    CHECK_EQ(bad, 0);
    xs_done("dr7_write", 2 * n);
    return buf[0];
}

const test_t tests_sys_ext[] = {
    { "pg_invlpg_ws_64", pg_invlpg_ws_64, REQ_PAGING | REQ_486, T_TAG_PAGING },
    { "pg_invlpg_ws_256", pg_invlpg_ws_256, REQ_PAGING | REQ_486, T_TAG_PAGING },
    { "pg_invlpg_ws_1024", pg_invlpg_ws_1024, REQ_PAGING | REQ_486, T_TAG_PAGING },
    { "pg_tlb_capacity_64", pg_tlb_capacity_64, REQ_PAGING, T_TAG_PAGING },
    { "pg_tlb_capacity_256", pg_tlb_capacity_256, REQ_PAGING, T_TAG_PAGING },
    { "pg_tlb_capacity_1024", pg_tlb_capacity_1024, REQ_PAGING, T_TAG_PAGING },
    { "pg_tlb_capacity_4096", pg_tlb_capacity_4096, REQ_PAGING, T_TAG_PAGING },
    { "pg_pse_walk", pg_pse_walk, REQ_PAGING | REQ_PSE | REQ_MEM8M, T_TAG_PAGING },
    { "pg_ctx_switch", pg_ctx_switch, REQ_PAGING, T_TAG_PAGING },
    { "pg_ctx_switch_pge", pg_ctx_switch_pge, REQ_PAGING | REQ_PGE, T_TAG_PAGING },
    { "pg_demand_storm", pg_demand_storm, REQ_PAGING | REQ_486, T_TAG_PAGING | T_TAG_EXC },
    { "pg_cow_storm", pg_cow_storm, REQ_PAGING | REQ_486, T_TAG_PAGING | T_TAG_EXC },
    { "pm_task_switch_loop_jmp", pm_task_switch_loop_jmp, REQ_486, T_TAG_MODESW | T_TAG_SEGLOAD },
    { "pm_task_switch_loop_call", pm_task_switch_loop_call, REQ_486, T_TAG_MODESW | T_TAG_SEGLOAD },
    { "pm_int_lean_r0", pm_int_lean_r0, REQ_486, T_TAG_EXC },
    { "pm_int_lean_seg_r0", pm_int_lean_seg_r0, REQ_486, T_TAG_EXC | T_TAG_SEGLOAD },
    { "pm_int_lean_r3", pm_int_lean_r3, REQ_486, T_TAG_EXC | T_TAG_MODESW },
    { "pm_int_lean_seg_r3", pm_int_lean_seg_r3, REQ_486, T_TAG_EXC | T_TAG_MODESW | T_TAG_SEGLOAD },
    { "pm_rt_callgate", pm_rt_callgate, REQ_486, T_TAG_MODESW | T_TAG_SEGLOAD },
    { "pm_rt_sysenter", pm_rt_sysenter, REQ_SEP | REQ_MSR, T_TAG_MODESW | T_TAG_SEGLOAD },
    { "pm_rt_syscall", pm_rt_syscall, REQ_SYSCALL | REQ_MSR, T_TAG_MODESW | T_TAG_SEGLOAD },
    { "io_poll_3da", io_poll_3da, 0, T_TAG_IO },
    { "io_poll_1f7", io_poll_1f7, 0, T_TAG_IO },
    { "io_poll_out80", io_poll_out80, 0, T_TAG_IO },
    { "io_poll_pit_latch", io_poll_pit_latch, 0, T_TAG_IO },
    { "io_poll_pic_irr", io_poll_pic_irr, 0, T_TAG_IO },
    { "sys_hlt_idle", sys_hlt_idle, 0, T_TAG_IRQ },
    { "sys_apic_timer_rate", sys_apic_timer_rate, REQ_PAGING | REQ_APIC | REQ_MSR, T_TAG_IRQ },
    { "mem_mmio_vs_ram_ram", mem_mmio_vs_ram_ram, REQ_PAGING, T_TAG_IO },
    { "mem_mmio_vs_ram_text", mem_mmio_vs_ram_text, REQ_PAGING, T_TAG_IO },
    { "mem_mmio_vs_ram_a0000", mem_mmio_vs_ram_a0000, REQ_PAGING, T_TAG_IO },
    { "mem_lock_rate_add", mem_lock_rate_add, 0 },
    { "mem_lock_rate_xchg", mem_lock_rate_xchg, 0 },
    { "mem_lock_rate_cmpxchg", mem_lock_rate_cmpxchg, REQ_486 },
    { "sys_dr7_write", sys_dr7_write, REQ_486 },
    { 0, 0, 0 }
};
