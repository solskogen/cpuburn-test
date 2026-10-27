#include "kernel.h"

#ifndef HEAVY_SCALE
#define HEAVY_SCALE 8
#endif
#ifndef TEST_SEED
#define TEST_SEED 0x86B04A21u
#endif

#define HP_BASE 0x50000000u
#define HP_PAGE 4096u
#define HP_DATA (HP_BASE + 4 * HP_PAGE)
#define HP_SRC (HP_BASE + 6 * HP_PAGE)
#define HP_STACK (HP_BASE + 8 * HP_PAGE)

static volatile u32 *hp_pd, *hp_pt;
static u8 *hp_code[2], *hp_data, *hp_src, *hp_stack;
static u32 hp_cr0, hp_cr3, hp_cr4, hp_flags, hp_sum;
static int hp_have4;
static isr_hook_t hp_old_pf, hp_old_exit;

static u32 hp_rng(u32 *s) {
    u32 x = *s ? *s : 0x86B04A21u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static void hp_probe4(void) { hp_cr4 = rd_cr4(); }

static void hp_serialize(void) {
    if (cpu.has_cpuid) {
        u32 a, b, c, d;
        cpuid(0, &a, &b, &c, &d);
    } else {
        /* A taken branch discards the 486 prefetch queue after a code write. */
        __asm__ volatile("jmp 1f\n1:" ::: "memory");
    }
}

static void hp_map(u32 n, u32 phys) {
    hp_pt[n] = phys ? phys | 7 : 0;
    invlpg((void *)(HP_BASE + n * HP_PAGE));
}

static void hp_bank(u32 bank, u32 reload) {
    for (u32 j = 0; j < 2; j++) {
        hp_pt[j] = (u32)hp_code[bank] + j * HP_PAGE + 7;
        hp_pt[j + 2] = (u32)hp_code[bank] + j * HP_PAGE + 7;
    }
    if (reload) wr_cr3((u32)hp_pd);
    else for (u32 j = 0; j < 4; j++) invlpg((void *)(HP_BASE + j * HP_PAGE));
}

static u32 hp_run(void (*body)(void)) {
    hp_cr0 = rd_cr0(); hp_cr3 = rd_cr3(); hp_flags = rd_eflags();
    hp_old_pf = int_hook[14]; hp_old_exit = int_hook[0x80];
    hp_have4 = guard_call(hp_probe4) == GUARD_OK;
    hp_pd = arena_alloc(HP_PAGE, HP_PAGE);
    hp_pt = arena_alloc(HP_PAGE, HP_PAGE);
    hp_code[0] = arena_alloc(2 * HP_PAGE, HP_PAGE);
    hp_code[1] = arena_alloc(2 * HP_PAGE, HP_PAGE);
    hp_data = arena_alloc(2 * HP_PAGE, HP_PAGE);
    hp_src = arena_alloc(2 * HP_PAGE, HP_PAGE);
    hp_stack = arena_alloc(2 * HP_PAGE, HP_PAGE);
    if (!hp_pd || !hp_pt || !hp_code[0] || !hp_code[1] ||
        !hp_data || !hp_src || !hp_stack) SKIP("arena too small");
    kmemcpy((void *)hp_pd, pg_dir, HP_PAGE);
    kmemset((void *)hp_pt, 0, HP_PAGE);
    hp_pd[HP_BASE >> 22] = (u32)hp_pt | 7;
    for (u32 j = 0; j < 2; j++) {
        hp_pt[j] = hp_pt[j + 2] = (u32)hp_code[0] + j * HP_PAGE + 7;
        hp_pt[j + 4] = (u32)hp_data + j * HP_PAGE + 7;
        hp_pt[j + 6] = (u32)hp_src + j * HP_PAGE + 7;
        hp_pt[j + 8] = (u32)hp_stack + j * HP_PAGE + 7;
    }
    hp_sum = 0;
    wr_cr3((u32)hp_pd);
    int rc = guard_call(body);
    int_hook[14] = hp_old_pf; int_hook[0x80] = hp_old_exit;
    /* Identity mappings and the harness stack survive every guarded escape. */
    cli();
    wr_cr3(hp_cr3);
    if (hp_have4) wr_cr4(hp_cr4);
    wr_cr0(hp_cr0);
    wr_eflags(hp_flags);
    CHECK_EQ(rc, GUARD_OK);
    return hp_sum;
}

static void hp_put32(u8 *p, u32 v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void hp_smc_body(void) {
    static const u32 offsets[] = { 0, 13, 29, 61, 125, 253, 509, 1021, 2045, 4093 };
    u32 rng = TEST_SEED, count = 131072u * HEAVY_SCALE, sum = 0;
    u32 imm = 0, arg = 0, want = 0, offset = 0, bank = 0, padding = 0;
    u32 instructions = 0, patches = 0, remaps = 0;
    u32 (*fn)(void) = (void *)HP_BASE;
    kmemset(hp_code[0], 0x90, 2 * HP_PAGE);
    kmemset(hp_code[1], 0x90, 2 * HP_PAGE);
    for (u32 i = 0; i < count; i++) {
        if (!(i & 31)) {
            if (!(i & 127)) {
                bank ^= 1; hp_bank(bank, i & 128); remaps++;
            }
            offset = offsets[hp_rng(&rng) % 10];
            padding = i & 32 ? 96 : 0;
            imm = hp_rng(&rng); arg = hp_rng(&rng);
            u8 *p = (u8 *)(HP_BASE + 2 * HP_PAGE + offset);
            p[0] = 0xB8; hp_put32(p + 1, imm);
            p[5] = i & 64 ? 0x35 : 0x05;
            hp_put32(p + 6, arg);
            kmemset(p + 10, 0x90, padding); p[10 + padding] = 0xC3;
            want = i & 64 ? imm ^ arg : imm + arg;
            fn = (void *)(HP_BASE + offset);
            hp_serialize(); patches++;
        }
        u32 got = fn();
        if (got != want) {
            t_note("seed=%08x dispatch=%u bank=%u offset=%u", TEST_SEED, i, bank, offset);
            CHECK_EQ(got, want); return;
        }
        sum += got; instructions += 3 + padding;
    }
    t_note("dispatches=%u patches=%u remaps=%u generated_instructions=%u sum=%08x (informational)",
           count, patches, remaps, instructions, sum);
    t_ops(instructions); hp_sum = sum;
}
static u32 hp_smc(void) { return hp_run(hp_smc_body); }

/* Each translated-looking node has both conditional outcomes and either a
 * direct block link or an indirect link. Rewrites overlap live cached blocks. */
static u32 hp_keys[2][32];
static void hp_node(u32 bank, u32 node, u32 key) {
    u8 *p = hp_code[bank] + node * 128;
    u32 next = HP_BASE + (node + 1) * 128;
    p[0] = 0xA8; p[1] = 1;             /* test al,1 */
    p[2] = 0x74; p[3] = 7;
    p[4] = 0x05; hp_put32(p + 5, key); /* add eax,key */
    p[9] = 0xEB; p[10] = 5;
    p[11] = 0x35; hp_put32(p + 12, key);
    p[16] = 0xD1; p[17] = 0xC0;       /* rol eax,1 */
    u32 pos = 18;
    if (node & 2) { kmemset(p + pos, 0x90, 64); pos += 64; }
    if (node == 31) p[pos] = 0xC3;
    else if (node & 1) {
        p[pos] = 0xBA; hp_put32(p + pos + 1, next);
        p[pos + 5] = 0xFF; p[pos + 6] = 0xE2;
    } else {
        p[pos] = 0xE9;
        hp_put32(p + pos + 1, next - (HP_BASE + node * 128 + pos + 5));
    }
    hp_keys[bank][node] = key;
}

static void hp_blocks_body(void) {
    u32 rng = TEST_SEED ^ 0x4B10C5u, sum = 0, bank = 0;
    u32 count = 4096u * HEAVY_SCALE;
    for (u32 b = 0; b < 2; b++)
        for (u32 n = 0; n < 32; n++) hp_node(b, n, hp_rng(&rng));
    hp_serialize();
    for (u32 i = 0; i < count; i++) {
        if (!(i & 63)) { bank ^= 1; hp_bank(bank, i & 64); }
        if (!(i & 7)) {
            hp_node(bank, (i >> 3) & 31, hp_rng(&rng));
            hp_serialize();
        }
        u32 input = hp_rng(&rng), want = input, got = input;
        for (u32 n = 0; n < 32; n++) {
            want = want & 1 ? want + hp_keys[bank][n] : want ^ hp_keys[bank][n];
            want = want << 1 | want >> 31;
        }
        __asm__ volatile("call *%1" : "+a"(got) : "r"(HP_BASE) : "edx", "ecx", "cc", "memory");
        if (got != want) {
            t_note("seed=%08x chain=%u bank=%u input=%08x", TEST_SEED, i, bank, input);
            CHECK_EQ(got, want); return;
        }
        sum += got;
    }
    t_note("chains=%u blocks=%u rewrites=%u direct/indirect/conditional long/short sum=%08x (informational)",
           count, count * 32, count / 8, sum);
    t_ops(count * 32); hp_sum = sum;
}
static u32 hp_blocks(void) { return hp_run(hp_blocks_body); }

static regs_t hp_before, hp_after, hp_fault, hp_exit;
static volatile u32 hp_faults, hp_exits;
static u32 hp_missing, hp_fault_eip, hp_fault_bits;

static void hp_pf(regs_t *r) {
    u32 va = rd_cr2();
    kmemcpy(&hp_fault, r, sizeof hp_fault);
    if (++hp_faults != 1 || (va & ~(HP_PAGE - 1)) != HP_BASE + hp_missing * HP_PAGE ||
        r->eip != hp_fault_eip || (r->err & 7) != hp_fault_bits)
        guard_recover(GUARD_EXC(14));
    u32 phys;
    if (hp_missing < 2) phys = (u32)hp_code[0] + hp_missing * HP_PAGE;
    else if (hp_missing < 6) phys = (u32)hp_data + (hp_missing - 4) * HP_PAGE;
    else if (hp_missing < 8) phys = (u32)hp_src + (hp_missing - 6) * HP_PAGE;
    else phys = (u32)hp_stack + (hp_missing - 8) * HP_PAGE;
    hp_map(hp_missing, phys);
}

static void hp_int80(regs_t *r) {
    hp_exits++;
    kmemcpy(&hp_exit, r, sizeof hp_exit);
    guard_recover(GUARD_R3_INT80);
}

static u32 hp_state_diff(const regs_t *a, const regs_t *b) {
    u32 m = 0;
    if (a->eax != b->eax) m |= 1;
    if (a->ebx != b->ebx) m |= 2;
    if (a->ecx != b->ecx) m |= 4;
    if (a->edx != b->edx) m |= 8;
    if (a->esi != b->esi) m |= 16;
    if (a->edi != b->edi) m |= 32;
    if (a->ebp != b->ebp) m |= 64;
    if (a->useresp != b->useresp) m |= 128;
    if (a->eip != b->eip) m |= 256;
    /* RF in a fault frame is not part of the application's arithmetic state. */
    if ((a->eflags ^ b->eflags) & 0x003E7FD7u) m |= 512;
    if ((a->cs & 0xFFFF) != b->cs || (a->ss & 0xFFFF) != b->ss ||
        (a->ds & 0xFFFF) != b->ds || (a->es & 0xFFFF) != b->es ||
        (a->fs & 0xFFFF) != b->fs || (a->gs & 0xFFFF) != b->gs) m |= 1024;
    return m;
}

static void hp_user_init(u32 value, u32 stack) {
    kmemset(&hp_before, 0, sizeof hp_before);
    hp_before.eax = value; hp_before.ebx = value ^ 0xB1B2B3B4;
    hp_before.ecx = value ^ 0xC1C2C3C4; hp_before.edx = value ^ 0xD1D2D3D4;
    hp_before.esi = value ^ 0x51525354; hp_before.edi = value ^ 0x61626364;
    hp_before.ebp = value ^ 0x71727374; hp_before.useresp = stack;
    hp_before.eflags = 2; hp_before.cs = SEL_UCODE; hp_before.ss = SEL_UDATA;
    hp_before.ds = hp_before.es = hp_before.fs = hp_before.gs = SEL_UDATA;
}

static u32 hp_emit_init(void) {
    const u32 values[] = { hp_before.eax, hp_before.ecx, hp_before.edx,
        hp_before.ebx, 0, hp_before.ebp, hp_before.esi, hp_before.edi };
    u32 pos = 0;
    for (u32 r = 0; r < 8; r++) if (r != 4) {
        hp_code[0][pos++] = 0xB8 + r;
        hp_put32(hp_code[0] + pos, values[r]); pos += 4;
    }
    if (hp_before.eflags & 0x400) hp_code[0][pos++] = 0xFD;
    return pos;
}

static int hp_user_run(u32 case_no) {
    hp_faults = hp_exits = 0;
    kmemset(&hp_fault, 0, sizeof hp_fault);
    kmemset(&hp_exit, 0, sizeof hp_exit);
    hp_before.eip = hp_fault_eip;
    int_hook[14] = hp_pf; int_hook[0x80] = hp_int80;
    hp_map(hp_missing, 0);
    hp_serialize();
    int rc = ring3_call((void *)HP_BASE, hp_before.useresp);
    u32 bad_before = hp_faults == 1 ? hp_state_diff(&hp_fault, &hp_before) : ~0u;
    u32 bad_after = hp_exits == 1 ? hp_state_diff(&hp_exit, &hp_after) : ~0u;
    if (rc != GUARD_R3_INT80 || hp_faults != 1 || hp_exits != 1 || bad_before || bad_after) {
        t_note("seed=%08x case=%u rc=%x faults=%u exits=%u state_mask_before=%x after=%x fault_eip=%08x cr2=%08x",
               TEST_SEED, case_no, rc, hp_faults, hp_exits, bad_before, bad_after, hp_fault.eip, g_exc.cr2);
        t_note("fault eax=%08x/%08x ecx=%08x/%08x esi=%08x/%08x edi=%08x/%08x ESP=%08x/%08x flags=%08x/%08x (actual/expected)",
               hp_fault.eax, hp_before.eax, hp_fault.ecx, hp_before.ecx,
               hp_fault.esi, hp_before.esi, hp_fault.edi, hp_before.edi,
               hp_fault.useresp, hp_before.useresp, hp_fault.eflags, hp_before.eflags);
        CHECK_EQ(rc, GUARD_R3_INT80); CHECK_EQ(hp_faults, 1); CHECK_EQ(hp_exits, 1);
        CHECK_EQ(bad_before, 0); CHECK_EQ(bad_after, 0);
        return 0;
    }
    return 1;
}

static void hp_fetch_body(void) {
    u32 rng = TEST_SEED, count = 2048u * HEAVY_SCALE;
    for (u32 i = 0; i < count; i++) {
        u32 initial = hp_rng(&rng), result = hp_rng(&rng);
        hp_user_init(initial, HP_STACK + HP_PAGE);
        u32 pos = hp_emit_init();
        hp_code[0][pos] = 0xE9;
        hp_put32(hp_code[0] + pos + 1, HP_PAGE - 2 - (pos + 5));
        pos = HP_PAGE - 2;
        hp_code[0][pos] = 0xB8; hp_put32(hp_code[0] + pos + 1, result);
        hp_code[0][pos + 5] = 0xCD; hp_code[0][pos + 6] = 0x80;
        hp_after = hp_before; hp_after.eax = result;
        hp_after.eip = HP_BASE + pos + 7;
        hp_fault_eip = HP_BASE + pos; hp_missing = 1; hp_fault_bits = 4;
        if (!hp_user_run(i)) return;
        hp_sum += hp_exit.eax;
    }
    t_note("split_fetch_restarts=%u whole_user_state_checked each fault and completion", count);
    t_ops(count);
}
static u32 hp_fetch(void) { return hp_run(hp_fetch_body); }

static void hp_split_body(void) {
    u32 rng = TEST_SEED, count = 4096u * HEAVY_SCALE;
    for (u32 i = 0; i < count; i++) {
        u32 value = hp_rng(&rng), initial = hp_rng(&rng), store = i & 1;
        hp_data[HP_PAGE - 3] = 0x5A; hp_data[HP_PAGE + 2] = 0xA5;
        hp_put32(hp_data + HP_PAGE - 2, value);
        hp_user_init(initial, HP_STACK + HP_PAGE);
        u32 pos = hp_emit_init();
        hp_code[0][pos] = store ? 0xA3 : 0xA1;
        hp_put32(hp_code[0] + pos + 1, HP_DATA + HP_PAGE - 2);
        hp_code[0][pos + 5] = 0xCD; hp_code[0][pos + 6] = 0x80;
        hp_after = hp_before; hp_after.eax = store ? initial : value;
        hp_after.eip = HP_BASE + pos + 7;
        hp_fault_eip = HP_BASE + pos; hp_missing = 5; hp_fault_bits = store ? 6 : 4;
        if (!hp_user_run(i)) return;
        u32 got;
        kmemcpy(&got, hp_data + HP_PAGE - 2, 4);
        /* Do not assume anything about partially committed split stores at #PF. */
        if (got != (store ? initial : value)) {
            t_note("seed=%08x split_data_case=%u", TEST_SEED, i);
            CHECK_EQ(got, store ? initial : value); return;
        }
        CHECK_EQ(hp_data[HP_PAGE - 3], 0x5A);
        CHECK_EQ(hp_data[HP_PAGE + 2], 0xA5);
        hp_sum += got;
    }
    t_note("split_data_restarts=%u read=%u write=%u final_bytes_and_whole_state_checked",
           count, count / 2, count / 2);
    t_ops(count);
}
static u32 hp_split(void) { return hp_run(hp_split_body); }

static void hp_rep_body(void) {
    u32 rng = TEST_SEED, count = 2048u * HEAVY_SCALE;
    for (u32 i = 0; i < count; i++) {
        u32 back = i & 1, source_fault = i & 2, value = hp_rng(&rng);
        u32 src = HP_SRC + HP_PAGE - 256, dst = HP_DATA + HP_PAGE - 256;
        for (u32 j = 0; j < 128; j++)
            hp_put32(hp_src + HP_PAGE - 256 + 4 * j, value ^ (j * 0x9E3779B9u));
        kmemset(hp_data + HP_PAGE - 260, 0xA5, 520);
        hp_user_init(value, HP_STACK + HP_PAGE);
        hp_before.ecx = 128;
        hp_before.esi = src + (back ? 508 : 0);
        hp_before.edi = dst + (back ? 508 : 0);
        if (back) hp_before.eflags |= 0x400;
        u32 pos = hp_emit_init();
        hp_code[0][pos] = 0xF3; hp_code[0][pos + 1] = 0xA5;
        hp_code[0][pos + 2] = 0xCD; hp_code[0][pos + 3] = 0x80;
        hp_after = hp_before;
        hp_after.ecx = 0;
        hp_after.esi += back ? -512u : 512u;
        hp_after.edi += back ? -512u : 512u;
        hp_after.eip = HP_BASE + pos + 4;
        hp_fault_eip = HP_BASE + pos;
        hp_missing = (source_fault ? 6 : 4) + (back ? 0 : 1);
        hp_fault_bits = source_fault ? 4 : 6;
        /* REP must report exactly the already completed 64 aligned elements. */
        hp_before.ecx = 64;
        hp_before.esi += back ? -256u : 256u;
        hp_before.edi += back ? -256u : 256u;
        if (!hp_user_run(i)) return;
        for (u32 j = 0; j < 128; j++) {
            u32 got;
            kmemcpy(&got, hp_data + HP_PAGE - 256 + 4 * j, 4);
            u32 want = value ^ (j * 0x9E3779B9u);
            if (got != want) {
                t_note("seed=%08x rep_case=%u element=%u", TEST_SEED, i, j);
                CHECK_EQ(got, want); return;
            }
        }
        CHECK_EQ(hp_data[HP_PAGE - 257], 0xA5);
        CHECK_EQ(hp_data[HP_PAGE + 256], 0xA5);
        hp_sum += value;
        /* Restore whichever half was missing before preparing the next case. */
        hp_map(4, (u32)hp_data); hp_map(5, (u32)hp_data + HP_PAGE);
        hp_map(6, (u32)hp_src); hp_map(7, (u32)hp_src + HP_PAGE);
    }
    t_note("partial_REP_restarts=%u movsd_elements=%u forward/backward source/destination whole_state", count, count * 128);
    t_ops(count * 128);
}
static u32 hp_rep(void) { return hp_run(hp_rep_body); }

static void hp_stack_body(void) {
    u32 rng = TEST_SEED, count = 2048u * HEAVY_SCALE;
    for (u32 i = 0; i < count; i++) {
        u32 pop = i & 1, value = hp_rng(&rng), initial = hp_rng(&rng);
        hp_stack[HP_PAGE - 3] = 0x5A; hp_stack[HP_PAGE + 2] = 0xA5;
        hp_put32(hp_stack + HP_PAGE - 2, value);
        hp_user_init(initial, HP_STACK + HP_PAGE + (pop ? -2u : 2u));
        u32 pos = hp_emit_init();
        hp_code[0][pos] = pop ? 0x58 : 0x50;
        hp_code[0][pos + 1] = 0xCD; hp_code[0][pos + 2] = 0x80;
        hp_after = hp_before;
        hp_after.useresp += pop ? 4 : -4u;
        hp_after.eax = pop ? value : initial;
        hp_after.eip = HP_BASE + pos + 3;
        hp_fault_eip = HP_BASE + pos; hp_missing = pop ? 9 : 8;
        hp_fault_bits = pop ? 4 : 6;
        if (!hp_user_run(i)) return;
        u32 got;
        kmemcpy(&got, hp_stack + HP_PAGE - 2, 4);
        if (got != (pop ? value : initial)) {
            t_note("seed=%08x stack_case=%u", TEST_SEED, i);
            CHECK_EQ(got, pop ? value : initial); return;
        }
        CHECK_EQ(hp_stack[HP_PAGE - 3], 0x5A);
        CHECK_EQ(hp_stack[HP_PAGE + 2], 0xA5);
        hp_sum += got;
        hp_map(8, (u32)hp_stack); hp_map(9, (u32)hp_stack + HP_PAGE);
    }
    t_note("split_stack_restarts=%u push=%u pop=%u ESP/registers/flags/segments/exact_EIP checked",
           count, count / 2, count / 2);
    t_ops(count);
}
static u32 hp_stack_test(void) { return hp_run(hp_stack_body); }

const test_t tests_heavy_paging[] = {
    { "heavy_pg_smc_alias", hp_smc, REQ_PAGING | REQ_486 },
    { "heavy_pg_block_links", hp_blocks, REQ_PAGING | REQ_486 },
    { "heavy_pg_split_fetch", hp_fetch, REQ_PAGING | REQ_486 },
    { "heavy_pg_split_data", hp_split, REQ_PAGING | REQ_486 },
    { "heavy_pg_rep_restart", hp_rep, REQ_PAGING | REQ_486 },
    { "heavy_pg_stack_restart", hp_stack_test, REQ_PAGING | REQ_486 },
    { NULL, NULL, 0 }
};
