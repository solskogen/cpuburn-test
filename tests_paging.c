#include "kernel.h"

#define WIN 0x40000000u
#define PAGE 4096u
static volatile u32 *pd, *pt;
static u32 *page, *other;
static u8 *ustack;
static volatile u32 target, sink;
static u32 old0, old3, old4;
static int have4;
static void probe4(void) { old4 = rd_cr4(); }
static void load_target(void) { sink = *(volatile u32 *)target; }
static void load_target_cr3(void) { wr_cr3(target); }
static void store_target(void) { *(volatile u32 *)target = 0x12345678; }
static void user_load(void) { sink = *(volatile u32 *)target; __asm__ volatile("int $0x80"); }
static void user_store(void) { *(volatile u32 *)target = 1; __asm__ volatile("int $0x80"); }
static void user_push(void) {
    __asm__ volatile("mov %0,%%esp; pushl $1; int $0x80" :: "r"(target) : "memory");
}

/* Every test uses a disposable directory; even guard recovery cannot corrupt the harness tables. */
static u32 run_page(void (*body)(void)) {
    old0 = rd_cr0(); old3 = rd_cr3();
    have4 = guard_call(probe4) == GUARD_OK;
    pd = arena_alloc(PAGE, PAGE); pt = arena_alloc(PAGE, PAGE);
    page = arena_alloc(PAGE, PAGE); other = arena_alloc(PAGE, PAGE);
    ustack = arena_alloc(PAGE, PAGE);
    if (!pd || !pt || !page || !other || !ustack) SKIP("arena too small");
    kmemcpy((void *)pd, pg_dir, PAGE); kmemset((void *)pt, 0, PAGE);
    kmemset(page, 0, PAGE); kmemset(other, 0, PAGE);
    pd[WIN >> 22] = (u32)pt | 7;
    pt[0] = (u32)page | 7; pt[1] = (u32)other | 7;
    wr_cr3((u32)pd);
    int rc = guard_call(body);
    int_hook[14] = NULL;
    /* This path also handles faults while the PAE directory is active. */
    cli(); wr_cr0(rd_cr0() & ~0x80000000u);
    if (have4) wr_cr4(old4);
    wr_cr3(old3); wr_cr0(old0); sti();
    CHECK_EQ(rc, GUARD_OK);
    return sink;
}

static void alias_body(void) {
    pt[1] = (u32)page | 7; invlpg((void *)(WIN + PAGE));
    *(volatile u32 *)WIN = 0x87654321;
    CHECK_EQ(*(volatile u32 *)(WIN + PAGE), 0x87654321);
    CHECK_EQ(page[0], 0x87654321);
    for (u32 i = 0; i < 2048; i++) {
        u32 *p = i & 1 ? page : other;
        p[0] = i ^ 0xFA19B37Du; pt[0] = (u32)p | 7;
        if (i & 2) invlpg((void *)WIN); else wr_cr3((u32)pd);
        CHECK_EQ(*(volatile u32 *)WIN, i ^ 0xFA19B37Du);
    }
    t_ops(2048);
}
static u32 pg_map_alias(void) { return run_page(alias_body); }

static void fault_check(void (*fn)(void), u32 addr, u32 bits, u32 mask) {
    target = addr;
    int rc = guard_call(fn);
    CHECK_EQ(rc, GUARD_EXC(14));
    if (rc == GUARD_EXC(14)) {
        CHECK_EQ(g_exc.err & mask, bits);
        CHECK_EQ(g_exc.cr2 & ~4095u, addr & ~4095u);
    }
}
static volatile u32 demands, demand_phys, progress_c, progress_s, progress_d;
static void demand_handler(regs_t *r) {
    u32 va = rd_cr2();
    if ((va & ~4095u) != WIN && (va & ~4095u) != WIN + PAGE)
        guard_recover(GUARD_EXC(14));
    progress_c = r->ecx; progress_s = r->esi; progress_d = r->edi;
    pt[(va - WIN) >> 12] = demand_phys | 7;
    invlpg((void *)va); demands++;
}
static void faults_body(void) {
    pt[0] = 0; invlpg((void *)WIN);
    fault_check(load_target, WIN, 0, 7);
    fault_check(store_target, WIN, 2, 7);
    fault_check((void (*)(void))WIN, WIN, 0, 7);
    pt[0] = (u32)page | 5; invlpg((void *)WIN);
    wr_cr0(old0 | 0x10000); fault_check(store_target, WIN, 3, 7);
    wr_cr0(old0 & ~0x10000u); target = WIN; CHECK_EQ(guard_call(store_target), 0);
    wr_cr0(old0);
    pt[0] = (u32)page | 3; invlpg((void *)WIN); target = WIN;
    CHECK_EQ(ring3_call(user_load, (u32)ustack + PAGE), GUARD_EXC(14));
    CHECK_EQ(g_exc.err & 7, 5); CHECK_EQ(g_exc.cr2, WIN);
    pt[0] = (u32)page | 5; invlpg((void *)WIN);
    CHECK_EQ(ring3_call(user_store, (u32)ustack + PAGE), GUARD_EXC(14));
    CHECK_EQ(g_exc.err & 7, 7);
    pd[WIN >> 22] = 0; wr_cr3((u32)pd);
    fault_check(load_target, WIN, 0, 7);
    pd[WIN >> 22] = (u32)pt | 7; pt[0] = (u32)page | 7; pt[1] = 0;
    wr_cr3((u32)pd); target = WIN + PAGE - 2;
    CHECK_EQ(guard_call(load_target), GUARD_EXC(14)); CHECK_EQ(g_exc.err & 7, 0);
    /* A split access may report any byte in the missing second page. */
    CHECK_EQ(g_exc.cr2 & ~4095u, WIN + PAGE);
    target = WIN + 2 * PAGE;
    CHECK_EQ(ring3_call(user_push, (u32)ustack + PAGE), GUARD_EXC(14));
    CHECK_EQ(g_exc.err & 7, 6); CHECK_EQ(g_exc.cr2 & ~4095u, WIN + PAGE);
    demands = 0; demand_phys = (u32)other; int_hook[14] = demand_handler;
    u32 count = 32, src = (u32)page, dst = WIN + PAGE - 64;
    __asm__ volatile("cld; rep movsl" : "+c"(count), "+S"(src), "+D"(dst) :: "memory");
    CHECK_EQ(demands, 1); CHECK_EQ(progress_c, 16);
    CHECK_EQ(progress_s, (u32)page + 64); CHECK_EQ(progress_d, WIN + PAGE);
    CHECK_EQ(count, 0);
    u32 rep_c = progress_c, rep_s = progress_s, rep_d = progress_d;
    for (u32 i = 0; i < 2048; i++) {
        pt[0] = 0; invlpg((void *)WIN); other[0] = i;
        CHECK_EQ(*(volatile u32 *)WIN, i);
    }
    CHECK_EQ(demands, 2049); int_hook[14] = NULL;
    t_note("demand_faults=%u REP remaining=%u esi=%08x edi=%08x", demands, rep_c, rep_s, rep_d);
    t_ops(demands);
}
static u32 pg_faults(void) { return run_page(faults_body); }

static void ad_body(void) {
    for (u32 i = 0; i < 128; i++) {
        pt[0] = (u32)page | 7; pd[WIN >> 22] = (u32)pt | 7; wr_cr3((u32)pd);
        sink = *(volatile u32 *)WIN;
        invlpg((void *)WIN); wr_cr3((u32)pd);
        CHECK(pt[0] & 0x20); CHECK(pd[WIN >> 22] & 0x20); CHECK(!(pt[0] & 0x40));
        *(volatile u32 *)WIN = i; invlpg((void *)WIN); wr_cr3((u32)pd);
        CHECK(pt[0] & 0x40);
    }
    pt[0] = (u32)page | 5; wr_cr3((u32)pd); wr_cr0(old0 | 0x10000);
    fault_check(store_target, WIN, 3, 7);
    CHECK(!(pt[0] & 0x40));
    t_note("RO fault A=%u D=%u (A on fault is not required)", !!(pt[0] & 32), !!(pt[0] & 64));
    wr_cr0(old0);
}
static u32 pg_accessed_dirty(void) { return run_page(ad_body); }

static void tlb_body(void) {
    u32 *tables = arena_alloc(4 * PAGE, PAGE);
    u32 n = 512; u32 *data = arena_alloc(n * PAGE, PAGE);
    if (!tables || !data) { t_skip("arena too small for TLB walk"); return; }
    for (u32 i = 0; i < n; i++) data[i * 1024] = i ^ 0x759318D3;
    for (u32 i = 0; i < 4; i++) pd[(WIN >> 22) + i] = (u32)(tables + i * 1024) | 7;
    u32 sum = 0, seed = 0x1234567;
    u64 start = stamp();
    for (u32 round = 0; round < 8; round++) {
        for (u32 i = 0; i < 4096; i++) tables[i] = (u32)(data + (((i * 73 + round * 17) & (n - 1)) * 1024)) | 7;
        wr_cr3((u32)pd);
        for (u32 j = 0; j < 8192; j++) {
            seed = seed * 1664525u + 1013904223u;
            u32 i = (seed >> 12) & 4095, want = ((i * 73 + round * 17) & (n - 1)) ^ 0x759318D3;
            u32 got = *(volatile u32 *)(WIN + i * PAGE);
            CHECK_EQ(got, want); sum = csum_mix(sum, got);
            if (!(j & 255)) invlpg((void *)(WIN + i * PAGE));
        }
    }
    t_note("16MiB virtual / %u physical pages cycles/access=%llu", n, (stamp() - start) / 65536);
    t_ops(65536); sink = sum;
}
static u32 pg_tlb_stress(void) { return run_page(tlb_body); }

static void global_body(void) {
    wr_cr4(old4 | 0x80); page[0] = 1; other[0] = 2;
    pt[0] = (u32)page | 0x107; invlpg((void *)WIN); CHECK_EQ(*(volatile u32 *)WIN, 1);
    pt[0] = (u32)other | 0x107; wr_cr3((u32)pd);
    t_note("after CR3 global value=%u (retention optional)", *(volatile u32 *)WIN);
    invlpg((void *)WIN); CHECK_EQ(*(volatile u32 *)WIN, 2);
    pt[0] = (u32)page | 0x107; wr_cr4(old4 & ~0x80u); wr_cr4(old4 | 0x80);
    CHECK_EQ(*(volatile u32 *)WIN, 1);
    wr_cr4(old4 & ~0x80u); wr_cr3((u32)pd);
}
static u32 pg_global(void) { return run_page(global_body); }

static void pse_body(void) {
    u32 *large = arena_alloc(0x400000, 0x400000);
    if (!large) { t_skip("no owned aligned 4MiB block"); return; }
    wr_cr4(old4 | 0x10);
    pd[512] = (u32)large | 0x87; wr_cr3((u32)pd);
    for (u32 i = 0; i < 1024; i++) {
        *(volatile u32 *)(0x80000000u + i * PAGE) = i ^ 0x1F46ABCD;
        CHECK_EQ(large[i * 1024], i ^ 0x1F46ABCD);
    }
    CHECK((pd[512] & 0x60) == 0x60);
    pt[0] = (u32)large | 7; invlpg((void *)WIN);
    CHECK_EQ(*(volatile u32 *)WIN, large[0]);
    pd[512] = (u32)large | 0x85; wr_cr3((u32)pd); wr_cr0(old0 | 0x10000);
    fault_check(store_target, 0x80001000u, 3, 7); wr_cr0(old0);
    pd[512] = (u32)large | 0x87; invlpg((void *)0x80001000u);
    *(volatile u32 *)0x80001000u = 0xABCDEF01; CHECK_EQ(large[1024], 0xABCDEF01);
    /* Bit 21 cannot encode a 4MiB page address, even with PSE36. */
    pd[512] = (u32)large | 0x200087; wr_cr3((u32)pd); target = 0x80000000u;
    int rc = guard_call(load_target);
    if (rc == GUARD_EXC(14) && (g_exc.err & 8)) t_note("PSE reserved-bit #PF err=%x", g_exc.err);
    else t_note("PSE reserved-bit checking not exposed rc=%x err=%x", rc, g_exc.err);
    pd[512] = 0; wr_cr3((u32)pd);
    if (cpu.has_pse36) t_note("PSE36 high physical addresses skipped: no owned RAM above 4GiB");
}
static u32 pg_pse4m(void) { return run_page(pse_body); }

static void pae_body(void) {
    volatile u64 *pdpt = arena_alloc(PAGE, PAGE), *dirs = arena_alloc(4 * PAGE, PAGE);
    volatile u64 *table = arena_alloc(PAGE, PAGE);
    if (!pdpt || !dirs || !table) { t_skip("arena too small"); return; }
    kmemset((void *)pdpt, 0, PAGE); kmemset((void *)dirs, 0, 4 * PAGE); kmemset((void *)table, 0, PAGE);
    for (u32 i = 0; i < 4; i++) pdpt[i] = (u32)(dirs + i * 512) | 1;
    for (u32 i = 0; i < 8; i++) dirs[i] = ((u64)i << 21) | 0x87;
    dirs[512] = (u32)table | 7; table[0] = (u32)page | 7;
    cli(); wr_cr0(old0 & ~0x80000000u); wr_cr4((old4 & ~0x80u) | 0x20);
    wr_cr3((u32)pdpt); wr_cr0(old0);
    page[0] = 0x619A32BC; CHECK_EQ(*(volatile u32 *)WIN, 0x619A32BC);
    invlpg((void *)WIN); wr_cr3((u32)pdpt);
    CHECK((table[0] & 0x20) != 0);
    *(volatile u32 *)WIN = 0x1234ABCD; CHECK_EQ(page[0], 0x1234ABCD);
    invlpg((void *)WIN); wr_cr3((u32)pdpt);
    CHECK((table[0] & 0x40) != 0); CHECK((dirs[0] & 0x60) == 0x60);
    table[0] = 0; invlpg((void *)WIN); fault_check(load_target, WIN, 0, 7);
    table[0] = (u32)page | 5; invlpg((void *)WIN); wr_cr0(old0 | 0x10000);
    fault_check(store_target, WIN, 3, 7); wr_cr0(old0);
    /* Validate reserved PDPTE bits through a guarded CR3 reload, not an unsafe paging enable. */
    if (!cpu.hv) {
        pdpt[3] |= 2; target = (u32)pdpt;
        int rc = guard_call(load_target_cr3);
        pdpt[3] &= ~2ull; wr_cr3((u32)pdpt);
        if (rc == GUARD_EXC(13)) CHECK_EQ(g_exc.err, 0);
        else t_note("reserved PDPTE reload rc=%x: reserved-bit validation not exposed", rc);
    } else t_note("invalid PDPTE reload skipped under hypervisor");
    t_note("PAE identity 0..16MiB, 2MiB pages, alias/A/D/#PF checked");
    wr_cr0(old0 & ~0x80000000u); wr_cr4(old4); wr_cr3((u32)pd); wr_cr0(old0); sti();
}
static u32 pg_pae(void) { return run_page(pae_body); }

static volatile u64 apic_base;
static void get_apic(void) { apic_base = rdmsr(0x1B); }
static void read_apic(void) {
    u32 *map = arena_alloc(PAGE, PAGE);
    if (!map) { t_skip("arena too small"); return; }
    kmemset(map, 0, PAGE);
    map[(0xFEE00000u >> 12) & 1023] = 0xFEE00000u | 0x1B;
    pd[0xFEE00000u >> 22] = (u32)map | 3;
    invlpg((void *)0xFEE00000u);
    volatile u32 *a = (volatile u32 *)0xFEE00000u;
    u32 id = a[0x20 / 4], ver = a[0x30 / 4], svr = a[0xF0 / 4];
    t_note("APIC_BASE=%llx ID=%08x VERSION=%08x SVR=%08x", apic_base, id, ver, svr);
}
static u32 pg_apic_mmio(void) {
    if (guard_call(get_apic)) SKIP("APIC_BASE RDMSR fault");
    if (!(apic_base & 0x800)) SKIP("APIC disabled");
    if ((apic_base & 0xFFFFFF000ull) != 0xFEE00000ull || (apic_base & 0x400)) SKIP("relocated or x2APIC");
    return run_page(read_apic);
}

const test_t tests_paging[] = {
    {"pg_map_alias", pg_map_alias, REQ_PAGING | REQ_486},
    {"pg_faults", pg_faults, REQ_PAGING | REQ_486},
    {"pg_accessed_dirty", pg_accessed_dirty, REQ_PAGING | REQ_486},
    {"pg_tlb_stress", pg_tlb_stress, REQ_PAGING | REQ_486},
    {"pg_global", pg_global, REQ_PAGING | REQ_PGE | REQ_P5},
    {"pg_pse4m", pg_pse4m, REQ_PAGING | REQ_486 | REQ_PSE | REQ_MEM8M},
    {"pg_pae", pg_pae, REQ_PAGING | REQ_486 | REQ_PAE | REQ_MEM8M},
    {"pg_apic_mmio", pg_apic_mmio, REQ_PAGING | REQ_APIC | REQ_MSR},
    {0, 0, 0}
};
