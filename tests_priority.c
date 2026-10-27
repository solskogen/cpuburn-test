#include "kernel.h"

/*
 * Architectural oracles, not emulator-derived results:
 * Intel SDM Vol. 3A, protection/address translation, Page-Fault Exceptions,
 * Alignment Check, and Prioritization of Concurrent Events; Vol. 2, MOV,
 * LGDT/LIDT and LOCK. Section numbers differ between SDM editions.
 *
 * Segment validation precedes translating a data address. A fully fetched
 * invalid LOCK MOV is #UD, not a data access. LGDT/LIDT require CPL=0 before
 * reading their operand. Instruction-fetch #PF precedes executing a data
 * access that would #AC. P=0 in either paging level
 * means a nonpresent fault regardless of that entry's ignored permission
 * bits; permission faults below use an entirely present walk.
 *
 * No split stores, multiple memory operands, reserved PTE bits, speculative
 * prefetch assumptions, or overlapping contributory exceptions are tested.
 * In particular, do NOT rank #PF against #AC on the SAME data access:
 * the SDM's concurrent-event table explicitly does not order execution
 * faults. Use aligned #PF and mapped misaligned #AC controls instead, and
 * compete fetch #PF against data #AC in pr_fetch().
 * The only split instruction is a legal MOV with an unfetched disp32: those
 * instruction bytes are required before its operand address can be formed.
 */
#define PR_CODE  0x58000000u
#define PR_DATA  0x58400000u
#define PR_STACK (PR_CODE + 8192u)
#define PR_PAGE 4096u
#define PR_SEL 0x43u
#define PR_CASES (2048u * HEAVY_SCALE)
#define PR_AC 0x40000u

static u32 *pr_pd, *pr_code_pt, *pr_data_pt;
static u8 *pr_code, *pr_data, *pr_stack;
static u32 pr_old_cr0, pr_old_cr3, pr_old_flags;
static u16 pr_old_seg[4];
static u64 pr_old_desc;
static const u32 pr_vectors[] = { 6, 10, 11, 12, 13, 14, 17, 0x80 };
static isr_hook_t pr_old_hooks[8];
static struct { u16 limit; u32 base; } __attribute__((packed)) pr_gdtr, pr_idtr;
static regs_t pr_want, pr_got;
static volatile u32 pr_seen, pr_cr2;
static u32 pr_rng, pr_case, pr_variant, pr_kind, pr_sum, pr_bad;
static u32 pr_vec, pr_err, pr_addr, pr_cr2_mask, pr_off;
static u8 pr_bytes[32], pr_stack_bytes[64];

static u32 pr_rand(void) {
    u32 x = pr_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return pr_rng = x;
}

static void pr_put32(u8 *p, u32 v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void pr_capture(regs_t *r) {
    pr_seen++;
    pr_cr2 = rd_cr2();
    kmemcpy(&pr_got, r, sizeof pr_got);
    /* Never IRET to a failed instruction or reload its bounded user DS. */
    guard_recover(GUARD_EXC(r->vec));
}

static u32 pr_state_diff(void) {
    u32 m = 0;
    if (pr_got.eax != pr_want.eax) m |= 1;
    if (pr_got.ebx != pr_want.ebx) m |= 2;
    if (pr_got.ecx != pr_want.ecx) m |= 4;
    if (pr_got.edx != pr_want.edx) m |= 8;
    if (pr_got.esi != pr_want.esi) m |= 16;
    if (pr_got.edi != pr_want.edi) m |= 32;
    if (pr_got.ebp != pr_want.ebp) m |= 64;
    if (pr_got.useresp != pr_want.useresp) m |= 128;
    if (pr_got.eip != pr_want.eip) m |= 256;
    /* RF in a fault frame is not pre-instruction application state. */
    if ((pr_got.eflags ^ pr_want.eflags) & 0x00047FD7u) m |= 512;
    if ((pr_got.cs & 0xffff) != pr_want.cs ||
        (pr_got.ss & 0xffff) != pr_want.ss ||
        (pr_got.ds & 0xffff) != pr_want.ds ||
        (pr_got.es & 0xffff) != pr_want.es ||
        (pr_got.fs & 0xffff) != pr_want.fs ||
        (pr_got.gs & 0xffff) != pr_want.gs) m |= 1024;
    return m;
}

static void pr_init(void) {
    kmemset(&pr_want, 0, sizeof pr_want);
    pr_want.eax = pr_rand(); pr_want.ebx = pr_rand();
    pr_want.ecx = pr_rand(); pr_want.edx = pr_rand();
    pr_want.esi = pr_rand(); pr_want.edi = pr_rand();
    pr_want.ebp = pr_rand();
    pr_want.cs = SEL_UCODE; pr_want.ss = SEL_UDATA;
    pr_want.ds = pr_want.es = pr_want.fs = pr_want.gs = SEL_UDATA;
    pr_want.useresp = PR_STACK + PR_PAGE;
    pr_want.eflags = 2 | (pr_rand() & 0xcd5u);
    pr_off = 32 + (pr_rand() % 240) * 16;
    for (u32 j = 0; j < sizeof pr_bytes; j++)
        pr_data[pr_off - 8 + j] = (u8)pr_rand();
    for (u32 j = 0; j < sizeof pr_stack_bytes; j++)
        pr_stack[PR_PAGE - sizeof pr_stack_bytes + j] = (u8)pr_rand();
    pr_vec = 14; pr_err = 4; pr_addr = PR_DATA + pr_off;
    pr_cr2_mask = ~0u;
    pr_want.ebx = pr_addr;
    pr_pd[PR_DATA >> 22] = (u32)pr_data_pt | 7;
    pr_data_pt[0] = (u32)pr_data; /* owned RAM frame, P=0 */
    pr_code_pt[1] = (u32)pr_code + PR_PAGE + 7;
}

static u32 pr_emit_init(void) {
    u32 p = 0;
    /* These two instructions precede register/flag initialization. */
    if (pr_want.ds == PR_SEL || pr_want.fs == PR_SEL) {
        pr_code[p++] = 0x66; pr_code[p++] = 0xb8;
        pr_code[p++] = PR_SEL; pr_code[p++] = 0;
        pr_code[p++] = 0x8e;
        pr_code[p++] = pr_want.ds == PR_SEL ? 0xd8 : 0xe0;
    }
    pr_code[p++] = 0x68;
    pr_put32(pr_code + p, pr_want.eflags); p += 4;
    pr_code[p++] = 0x9d; /* PUSH imm32; POPFD, on an aligned present stack */
    u32 values[8] = { pr_want.eax, pr_want.ecx, pr_want.edx, pr_want.ebx,
                     0, pr_want.ebp, pr_want.esi, pr_want.edi };
    for (u32 r = 0; r < 8; r++) if (r != 4) {
        pr_code[p++] = 0xb8 + r;
        pr_put32(pr_code + p, values[r]); p += 4;
    }
    return p;
}

static void pr_check(u32 p) {
    kmemcpy(pr_bytes, pr_data + pr_off - 8, sizeof pr_bytes);
    /* PUSH imm32 is the sole intentional user stack write during setup. */
    pr_put32(pr_stack + PR_PAGE - 4, pr_want.eflags);
    kmemcpy(pr_stack_bytes, pr_stack + PR_PAGE - sizeof pr_stack_bytes,
            sizeof pr_stack_bytes);
    pr_seen = 0; pr_cr2 = 0;
    kmemset(&pr_got, 0, sizeof pr_got);
    pr_code[p++] = 0xcd; pr_code[p] = 0x80; /* finite no-fault escape */
    /* MOV CR3 serializes code writes and flushes both changed alias PDEs. */
    wr_cr3((u32)pr_pd);
    int rc = ring3_call((void *)PR_CODE, pr_want.useresp);
    u32 state = pr_seen == 1 ? pr_state_diff() : ~0u;
    u32 mem = kmemcmp(pr_bytes, pr_data + pr_off - 8, sizeof pr_bytes) != 0;
    mem |= (kmemcmp(pr_stack_bytes, pr_stack + PR_PAGE - sizeof pr_stack_bytes,
                    sizeof pr_stack_bytes) != 0) << 1;
    u32 bad = rc != (int)GUARD_EXC(pr_vec) || pr_seen != 1 ||
              pr_got.vec != pr_vec || pr_got.err != pr_err || state || mem ||
              (pr_vec == 14 && ((pr_cr2 ^ pr_addr) & pr_cr2_mask));
    if (bad && !pr_bad) {
        t_note("seed=%08x first_bad=%u kind=%u variant=%u rc=%x seen=%u vector=%u/%u error=%x/%x CR2_defined=%u CR2=%08x/%08x mask=%08x state=%x memory=%x",
               TEST_SEED, pr_case, pr_kind, pr_variant, rc, pr_seen,
               pr_got.vec, pr_vec, pr_got.err, pr_err, pr_vec == 14,
               pr_cr2, pr_addr, pr_cr2_mask, state, mem);
        t_note("fault_EIP=%08x/%08x flags=%08x/%08x EBX=%08x DS=%x FS=%x opcode=%02x,%02x,%02x,%02x",
               pr_got.eip, pr_want.eip, pr_got.eflags, pr_want.eflags,
               pr_want.ebx, pr_want.ds, pr_want.fs,
               pr_code[pr_want.eip - PR_CODE], pr_code[pr_want.eip - PR_CODE + 1],
               pr_code[pr_want.eip - PR_CODE + 2], pr_code[pr_want.eip - PR_CODE + 3]);
        CHECK_EQ(rc, GUARD_EXC(pr_vec)); CHECK_EQ(pr_seen, 1);
        CHECK_EQ(pr_got.vec, pr_vec); CHECK_EQ(pr_got.err, pr_err);
        if (pr_vec == 14) CHECK_EQ(pr_cr2 & pr_cr2_mask, pr_addr & pr_cr2_mask);
        CHECK_EQ(state, 0); CHECK_EQ(mem, 0);
    }
    pr_bad += bad;
    pr_sum = csum_mix(pr_sum, pr_got.vec ^ pr_got.err ^ pr_want.eax);
    t_ops(1);
}

static void pr_segment(void) {
    pr_variant = pr_case & 7;
    u32 fs = pr_variant & 2, store = pr_variant & 1;
    u32 lim = 0x3ff + (pr_rand() & 0x3ff);
    gdt_set(8, PR_DATA, lim, 0xf3, 4); /* byte-granular DPL3 data */
    if (fs) pr_want.fs = PR_SEL; else pr_want.ds = PR_SEL;
    /* Either the first byte, or only the last bytes, exceed the limit.
     * All four bytes remain inside one nonpresent owned data page. */
    pr_want.ebx = pr_variant & 4 ? lim - 1 : lim + 1;
    pr_vec = 13; pr_err = 0;
    u32 p = pr_emit_init();
    pr_want.eip = PR_CODE + p;
    if (fs) pr_code[p++] = 0x64;
    pr_code[p++] = store ? 0x89 : 0x8b; pr_code[p++] = 0x03;
    /* Check the actual operand bytes, not the initially chosen offset. */
    pr_off = pr_want.ebx;
    pr_check(p);
}

static void pr_alignment(void) {
    pr_variant = pr_case & 7;
    u32 store = pr_variant & 1, mode = pr_variant >> 1;
    pr_want.eflags |= PR_AC;
    if (mode < 2) { /* #AC controls, no competing data paging violation */
        pr_off += 1 + (pr_rand() % 3); /* unaligned, no page split */
        pr_data_pt[0] = (u32)pr_data | (mode == 1 && !store ? 5 : 7);
        pr_vec = 17; pr_err = 0;
    } else if (mode == 2) { /* aligned nonpresent access with AC enabled */
        pr_err = store ? 6 : 4;
    } else { /* aligned supervisor-only access with AC enabled */
        pr_data_pt[0] = (u32)pr_data | 3;
        pr_err = store ? 7 : 5;
    }
    pr_want.ebx = pr_addr = PR_DATA + pr_off;
    u32 p = pr_emit_init();
    pr_want.eip = PR_CODE + p;
    pr_code[p++] = store ? 0x89 : 0x8b; pr_code[p++] = 0x03;
    pr_check(p);
}

static void pr_privilege(void) {
    pr_variant = pr_case & 7;
    u32 lidt = pr_variant & 1;
    /* Even an emulator ignoring both protections would load a valid table. */
    kmemcpy(pr_data + pr_off, lidt ? (void *)&pr_idtr : (void *)&pr_gdtr, 6);
    if (pr_variant & 2) pr_data_pt[0] = (u32)pr_data | 3;
    pr_vec = 13; pr_err = 0;
    u32 p = pr_emit_init();
    pr_want.eip = PR_CODE + p;
    if (pr_variant & 4) pr_code[p++] = 0x66;
    /* Exact ModR/M: 00 010 011 = LGDT [EBX], 00 011 011 = LIDT [EBX]. */
    pr_code[p++] = 0x0f; pr_code[p++] = 0x01;
    pr_code[p++] = lidt ? 0x1b : 0x13;
    pr_check(p);
}

static void pr_invalid_lock(void) {
    pr_variant = pr_case & 7;
    if (pr_variant & 2) pr_data_pt[0] = (u32)pr_data | 3;
    pr_vec = 6; pr_err = 0;
    u32 p = pr_emit_init();
    pr_want.eip = PR_CODE + p;
    if (pr_variant & 4) pr_code[p++] = 0x66;
    /* LOCK MOV r/m16,32,AX/EAX or MOV AX/EAX,r/m16,32.
     * 03 = mod00 reg000 r/m011: a real [EBX] memory operand, not a
     * register-only LOCK test or an inadvertently different instruction. */
    pr_code[p++] = 0xf0;
    pr_code[p++] = pr_variant & 1 ? 0x89 : 0x8b;
    pr_code[p++] = 0x03;
    pr_check(p);
}

static void pr_paging(void) {
    /* First six walks contain a P=0 entry; permission bits cannot make
     * these protection faults. Last six walks are wholly present, denied
     * at PDE, PTE, or both. No reserved bits or large pages are involved. */
    static const u8 directory[12] = { 7, 3, 5, 1, 0, 2, 7, 3, 1, 5, 7, 1 };
    static const u8 table[12]     = { 0, 0, 0, 0, 1, 3, 1, 7, 7, 7, 5, 1 };
    pr_variant = pr_case % 24;
    u32 m = pr_variant >> 1, store = pr_variant & 1;
    /* The last three combinations use writes to exercise RW independently
     * of U/S. Supervisor-only combinations also include reads. */
    if (m >= 9) store = 1;
    pr_pd[PR_DATA >> 22] = (u32)pr_data_pt | directory[m];
    pr_data_pt[0] = (u32)pr_data | table[m];
    pr_err = 4 | (store ? 2 : 0) | (m >= 6 ? 1 : 0);
    u32 p = pr_emit_init();
    pr_want.eip = PR_CODE + p;
    pr_code[p++] = store ? 0x89 : 0x8b; pr_code[p++] = 0x03;
    pr_check(p);
}

static void pr_fetch(void) {
    pr_variant = pr_case % 6;
    u32 store = pr_variant & 1;
    if ((pr_variant >> 1) == 1) pr_data_pt[0] = (u32)pr_data | 3;
    if ((pr_variant >> 1) == 2) {
        /* The data access would #AC, but cannot execute until its disp32
         * has been fetched. No simultaneous data #AC/#PF is assumed. */
        pr_want.eflags |= PR_AC;
        pr_off += 1 + (pr_rand() % 3);
        pr_want.ebx = PR_DATA + pr_off;
        pr_data_pt[0] = (u32)pr_data | 7;
    }
    u32 p = pr_emit_init();
    /* JMP rel32 to MOV EAX,[EBX+disp32] (or its store inverse).
     * Both opcode and ModR/M are present; all four mandatory displacement
     * bytes are on the next nonpresent instruction page. */
    pr_code[p++] = 0xe9;
    pr_put32(pr_code + p, PR_PAGE - 2 - (p + 4));
    p = PR_PAGE - 2;
    pr_want.eip = PR_CODE + p;
    pr_code[p++] = store ? 0x89 : 0x8b; pr_code[p++] = 0x83;
    pr_put32(pr_code + p, 0); p += 4;
    pr_code_pt[1] = (u32)pr_code + PR_PAGE; /* P=0 */
    pr_addr = PR_CODE + PR_PAGE; pr_err = 4;
    /* Check the missing instruction page, not an implementation's fetch
     * granule within it. Data operands above check every CR2 bit. */
    pr_cr2_mask = ~(PR_PAGE - 1);
    pr_check(p);
}

static void pr_body(void) {
    static void (*const cases[])(void) = {
        pr_segment, pr_alignment, pr_privilege, pr_invalid_lock, pr_paging, pr_fetch
    };
    wr_cr0(pr_old_cr0 | (pr_kind == 1 || pr_kind == 5 ? PR_AC : 0));
    wr_cr3((u32)pr_pd);
    for (pr_case = 0; pr_case < PR_CASES; pr_case++) {
        pr_init();
        cases[pr_kind]();
        /* Bound the IF-off interval to one case. This also services the
         * harness watchdog; no execution path retries a faulting opcode. */
        sti();
        __asm__ volatile("nop" ::: "memory");
        cli();
    }
}

static u32 pr_run(u32 kind) {
    pr_kind = kind; pr_rng = TEST_SEED ^ (kind * 0x9e3779b9u);
    if (!pr_rng) pr_rng = TEST_SEED;
    pr_sum = pr_bad = pr_case = 0;
    pr_pd = arena_alloc(PR_PAGE, PR_PAGE);
    pr_code_pt = arena_alloc(PR_PAGE, PR_PAGE);
    pr_data_pt = arena_alloc(PR_PAGE, PR_PAGE);
    pr_code = arena_alloc(2 * PR_PAGE, PR_PAGE);
    pr_data = arena_alloc(PR_PAGE, PR_PAGE);
    pr_stack = arena_alloc(PR_PAGE, PR_PAGE);
    CHECK(pr_pd && pr_code_pt && pr_data_pt && pr_code && pr_data && pr_stack);
    if (!pr_pd || !pr_code_pt || !pr_data_pt || !pr_code || !pr_data || !pr_stack)
        return 0;
    pr_old_cr0 = rd_cr0(); pr_old_cr3 = rd_cr3(); pr_old_flags = rd_eflags();
    __asm__ volatile("mov %%ds,%0; mov %%es,%1; mov %%fs,%2; mov %%gs,%3"
                     : "=m"(pr_old_seg[0]), "=m"(pr_old_seg[1]),
                       "=m"(pr_old_seg[2]), "=m"(pr_old_seg[3]));
    __asm__ volatile("sgdt %0; sidt %1" : "=m"(pr_gdtr), "=m"(pr_idtr));
    pr_old_desc = gdt_get(8);
    for (u32 j = 0; j < 8; j++) pr_old_hooks[j] = int_hook[pr_vectors[j]];
    kmemcpy(pr_pd, pg_dir, PR_PAGE);
    kmemset(pr_code_pt, 0, PR_PAGE); kmemset(pr_data_pt, 0, PR_PAGE);
    pr_pd[PR_CODE >> 22] = (u32)pr_code_pt | 7;
    pr_code_pt[0] = (u32)pr_code | 7;
    pr_code_pt[2] = (u32)pr_stack | 7;
    cli();
    for (u32 j = 0; j < 8; j++) int_hook[pr_vectors[j]] = pr_capture;
    int rc = guard_call(pr_body);
    cli();
    /* Identity mappings never change, including ISR/TSS/GDT and both stacks.
     * Cleanup also runs for an unexpected guarded fault or watchdog escape.
     * CR4 is deliberately never accessed: early 486s need not implement it. */
    wr_cr3(pr_old_cr3);
    wr_cr0(pr_old_cr0);
    __asm__ volatile("lgdt %0; lidt %1" :: "m"(pr_gdtr), "m"(pr_idtr) : "memory");
    gdt_set_raw(8, (u32)pr_old_desc, (u32)(pr_old_desc >> 32));
    for (u32 j = 0; j < 8; j++) int_hook[pr_vectors[j]] = pr_old_hooks[j];
    exc_expect_clear();
    __asm__ volatile("mov %0,%%ds; mov %1,%%es; mov %2,%%fs; mov %3,%%gs"
                     :: "m"(pr_old_seg[0]), "m"(pr_old_seg[1]),
                        "m"(pr_old_seg[2]), "m"(pr_old_seg[3]) : "memory");
    wr_eflags(pr_old_flags);
    if (rc) {
        t_note("seed=%08x guard_case=%u kind=%u rc=%x vector=%u error=%x EIP=%08x",
               TEST_SEED, pr_case, kind, rc, g_exc.vec, g_exc.err, g_exc.eip);
        CHECK_EQ(rc, GUARD_OK);
    }
    t_note("seed=%08x cases=%u planned=%u mismatches=%u GPR/flags/segments/ESP/EIP target_bytes=32 stack_bytes=64 CR2=data_exact/fetch_page/PF_only",
           TEST_SEED, pr_case, PR_CASES, pr_bad);
    return pr_sum;
}

static u32 pr_seg_test(void) { return pr_run(0); }
static u32 pr_ac_test(void) { return pr_run(1); }
static u32 pr_priv_test(void) { return pr_run(2); }
static u32 pr_lock_test(void) { return pr_run(3); }
static u32 pr_pg_test(void) { return pr_run(4); }
static u32 pr_fetch_test(void) { return pr_run(5); }

const test_t tests_priority[] = {
    { "priority_segment_page", pr_seg_test, REQ_PAGING | REQ_486 },
    { "priority_alignment_page", pr_ac_test, REQ_PAGING | REQ_486 },
    { "priority_privileged_page", pr_priv_test, REQ_PAGING | REQ_486 },
    { "priority_lock_decode_page", pr_lock_test, REQ_PAGING | REQ_486 },
    { "priority_page_permissions", pr_pg_test, REQ_PAGING | REQ_486 },
    { "priority_fetch_operand", pr_fetch_test, REQ_PAGING | REQ_486 },
    { NULL, NULL, 0 }
};
