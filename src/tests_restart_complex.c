#include "kernel.h"

#ifndef HEAVY_SCALE
#define HEAVY_SCALE 8
#endif
#ifndef TEST_SEED
#define TEST_SEED 0x86B04A21u
#endif

#define RCX_BASE 0x50000000u
#define RCX_PAGE 4096u
#define RCX_CODE (RCX_BASE)
#define RCX_DATA (RCX_BASE + 4 * RCX_PAGE)
#define RCX_STACK (RCX_BASE + 8 * RCX_PAGE)
#define RCX_WINDOW_PAGES 10
#define RCX_FAULT_MASK 0x00000FD5u
#define RCX_CASES (4096u * HEAVY_SCALE)

static volatile u32 *rcx_pd, *rcx_pt;
static u8 *rcx_code, *rcx_data, *rcx_stack;
static u32 rcx_phys[RCX_WINDOW_PAGES];
static u32 rcx_missing, rcx_fault_eip, rcx_fault_access;
static u32 rcx_fault_count, rcx_exit_count;
static u32 rcx_fault_addr, rcx_fault_vec, rcx_fault_err, rcx_fault_ip;
static volatile u32 rcx_fault_cs;
static regs_t rcx_exit;
static isr_hook_t rcx_old_pf, rcx_old_exit;
static u32 rcx_sum;
static u32 rcx_case_no;
static const char *rcx_case_op;

typedef struct rcx_state {
    u32 eax, ebx, ecx, edx, esi, edi, ebp, esp;
    u32 eflags;
} rcx_state_t;

static u32 rcx_rand(u32 *s) {
    u32 x = *s ? *s : 0x86B04A21u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void rcx_put16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void rcx_put32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 rcx_get16(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 rcx_get32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void rcx_map(u32 page, int present) {
    rcx_pt[page] = present ? rcx_phys[page] | 7u : 0;
    invlpg((void *)(RCX_BASE + page * RCX_PAGE));
}

/* Fault-time checks are limited to restart EIP, CPL, CR2, vector, and PF bits.
   Multi-store instructions may have partially written memory before restarting. */
static void rcx_pf(regs_t *r) {
    u32 addr = rd_cr2();
    rcx_fault_count++;
    rcx_fault_addr = addr;
    rcx_fault_vec = r->vec;
    rcx_fault_err = r->err;
    rcx_fault_cs = r->cs;
    rcx_fault_ip = r->eip;
    if (rcx_fault_count != 1 ||
        (addr & ~(RCX_PAGE - 1)) != RCX_BASE + rcx_missing * RCX_PAGE ||
        r->eip != rcx_fault_eip || (r->cs & 3u) != 3u ||
        (r->err & 7u) != rcx_fault_access)
        guard_recover(GUARD_EXC(14));
    rcx_map(rcx_missing, 1);
}

static void rcx_int80(regs_t *r) {
    if (++rcx_exit_count != 1)
        guard_recover(GUARD_R3_INT80);
    kmemcpy(&rcx_exit, r, sizeof rcx_exit);
    guard_recover(GUARD_R3_INT80);
}

static void rcx_case_begin(u32 missing_page, u32 access) {
    rcx_missing = missing_page;
    rcx_fault_access = access;
    rcx_fault_eip = 0;
    rcx_fault_count = rcx_exit_count = 0;
    rcx_fault_addr = rcx_fault_vec = rcx_fault_err = 0;
    rcx_fault_cs = rcx_fault_ip = 0;
    kmemset(&rcx_exit, 0, sizeof rcx_exit);
    for (u32 i = 0; i < RCX_WINDOW_PAGES; i++)
        rcx_map(i, rcx_phys[i] != 0);
    rcx_map(missing_page, 0);
}

static void rcx_user_init(const rcx_state_t *s) {
    u32 pos = 0;
    const u32 regs[7] = { s->eax, s->ecx, s->edx, s->ebx, s->ebp, s->esi, s->edi };
    const u8 op[7] = { 0xB8, 0xB9, 0xBA, 0xBB, 0xBD, 0xBE, 0xBF };
    for (u32 i = 0; i < 7; i++) {
        rcx_code[pos++] = op[i];
        rcx_put32(rcx_code + pos, regs[i]);
        pos += 4;
    }
    rcx_code[pos++] = 0x6A;
    rcx_code[pos++] = 0x02;
    rcx_code[pos++] = 0x9D;
    rcx_code[pos++] = 0xFC;
    rcx_code[pos++] = 0xBC;
    rcx_put32(rcx_code + pos, s->esp);
    pos += 4;
    rcx_fault_eip = RCX_CODE + pos;
}

static u32 rcx_finish(u32 pos) {
    rcx_code[pos++] = 0xCD;
    rcx_code[pos++] = 0x80;
    return pos;
}

static u32 rcx_state_diff(const rcx_state_t *want) {
    u32 m = 0;
    if (rcx_exit.eax != want->eax) m |= 1u;
    if (rcx_exit.ebx != want->ebx) m |= 2u;
    if (rcx_exit.ecx != want->ecx) m |= 4u;
    if (rcx_exit.edx != want->edx) m |= 8u;
    if (rcx_exit.esi != want->esi) m |= 16u;
    if (rcx_exit.edi != want->edi) m |= 32u;
    if (rcx_exit.ebp != want->ebp) m |= 64u;
    if (rcx_exit.useresp != want->esp) m |= 128u;
    if (((rcx_exit.eflags ^ want->eflags) & RCX_FAULT_MASK) != 0) m |= 256u;
    if ((rcx_exit.cs & 0xFFFFu) != SEL_UCODE ||
        (rcx_exit.ss & 0xFFFFu) != SEL_UDATA ||
        (rcx_exit.ds & 0xFFFFu) != SEL_UDATA ||
        (rcx_exit.es & 0xFFFFu) != SEL_UDATA ||
        (rcx_exit.fs & 0xFFFFu) != SEL_UDATA ||
        (rcx_exit.gs & 0xFFFFu) != SEL_UDATA) m |= 512u;
    return m;
}

static int rcx_run_user(u32 entry, u32 stack_top, u32 case_no, const char *op) {
    rcx_case_no = case_no;
    rcx_case_op = op;
    (void)stack_top;
    int rc = ring3_call((void (*)(void))entry, RCX_DATA + 2 * RCX_PAGE - 64);
    if (rc != GUARD_R3_INT80 || rcx_fault_count != 1 || rcx_exit_count != 1 ||
        rcx_fault_vec != 14 || rcx_fault_ip != rcx_fault_eip ||
        (rcx_fault_err & 7u) != rcx_fault_access ||
        (rcx_fault_addr & ~(RCX_PAGE - 1)) != RCX_BASE + rcx_missing * RCX_PAGE) {
        t_note("seed=%08x round=%u case=%u op=%s rc=%x faults=%u exits=%u fault=%u err=%x cr2=%08x eip=%08x want_eip=%08x",
               TEST_SEED, g_round, case_no, op, (u32)rc, rcx_fault_count, rcx_exit_count,
               rcx_fault_vec, rcx_fault_err, rcx_fault_addr, rcx_fault_ip, rcx_fault_eip);
        if (rcx_exit_count)
            t_note("exit eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x eip=%08x pt8=%08x pt9=%08x",
                   rcx_exit.eax, rcx_exit.ebx, rcx_exit.ecx, rcx_exit.edx,
                   rcx_exit.esi, rcx_exit.edi, rcx_exit.ebp, rcx_exit.useresp,
                   rcx_exit.eip, rcx_pt[8], rcx_pt[9]);
        CHECK_EQ(rc, GUARD_R3_INT80);
        CHECK_EQ(rcx_fault_count, 1);
        CHECK_EQ(rcx_exit_count, 1);
        CHECK_EQ(rcx_fault_vec, 14);
        CHECK_EQ(rcx_fault_ip, rcx_fault_eip);
        CHECK_EQ(rcx_fault_err & 7u, rcx_fault_access);
        CHECK_EQ(rcx_fault_addr & ~(RCX_PAGE - 1), RCX_BASE + rcx_missing * RCX_PAGE);
        return 0;
    }
    return 1;
}

static void rcx_check_state(const rcx_state_t *want, u32 case_no, const char *op) {
    u32 bad = rcx_state_diff(want);
    if (bad) {
        t_note("seed=%08x round=%u case=%u op=%s state_mask=%x eax=%08x/%08x ebx=%08x/%08x ecx=%08x/%08x edx=%08x/%08x esi=%08x/%08x edi=%08x/%08x ebp=%08x/%08x esp=%08x/%08x flags=%08x/%08x",
               TEST_SEED, g_round, case_no, op, bad,
               rcx_exit.eax, want->eax, rcx_exit.ebx, want->ebx,
               rcx_exit.ecx, want->ecx, rcx_exit.edx, want->edx,
               rcx_exit.esi, want->esi, rcx_exit.edi, want->edi,
               rcx_exit.ebp, want->ebp, rcx_exit.useresp, want->esp,
               rcx_exit.eflags, want->eflags);
        CHECK_EQ(bad, 0);
    }
}

static void rcx_seed_state(rcx_state_t *s, u32 seed, u32 esp) {
    s->eax = rcx_rand(&seed);
    s->ebx = rcx_rand(&seed);
    s->ecx = rcx_rand(&seed);
    s->edx = rcx_rand(&seed);
    s->esi = rcx_rand(&seed);
    s->edi = rcx_rand(&seed);
    s->ebp = rcx_rand(&seed);
    s->esp = esp;
    s->eflags = 2;
}

static u32 rcx_prepare(void (*body)(void)) {
    u32 old_cr3 = rd_cr3(), old_flags = rd_eflags();
    rcx_old_pf = int_hook[14];
    rcx_old_exit = int_hook[0x80];
    rcx_pd = arena_alloc(RCX_PAGE, RCX_PAGE);
    rcx_pt = arena_alloc(RCX_PAGE, RCX_PAGE);
    rcx_code = arena_alloc(2 * RCX_PAGE, RCX_PAGE);
    rcx_data = arena_alloc(2 * RCX_PAGE, RCX_PAGE);
    rcx_stack = arena_alloc(2 * RCX_PAGE, RCX_PAGE);
    if (!rcx_pd || !rcx_pt || !rcx_code || !rcx_data || !rcx_stack)
        SKIP("arena too small");
    kmemcpy((void *)rcx_pd, pg_dir, RCX_PAGE);
    kmemset((void *)rcx_pt, 0, RCX_PAGE);
    kmemset(rcx_code, 0x90, 2 * RCX_PAGE);
    kmemset(rcx_data, 0, 2 * RCX_PAGE);
    kmemset(rcx_stack, 0xA5, 2 * RCX_PAGE);
    rcx_phys[0] = (u32)rcx_code;
    rcx_phys[1] = (u32)rcx_code + RCX_PAGE;
    rcx_phys[4] = (u32)rcx_data;
    rcx_phys[5] = (u32)rcx_data + RCX_PAGE;
    rcx_phys[8] = (u32)rcx_stack;
    rcx_phys[9] = (u32)rcx_stack + RCX_PAGE;
    rcx_pd[RCX_BASE >> 22] = (u32)rcx_pt | 7u;
    for (u32 i = 0; i < RCX_WINDOW_PAGES; i++)
        if (rcx_phys[i]) rcx_pt[i] = rcx_phys[i] | 7u;
    rcx_sum = 0;
    int_hook[14] = rcx_pf;
    int_hook[0x80] = rcx_int80;
    wr_cr3((u32)rcx_pd);
    int guard = guard_call(body);
    int_hook[14] = rcx_old_pf;
    int_hook[0x80] = rcx_old_exit;
    cli();
    wr_cr3(old_cr3);
    wr_eflags(old_flags);
    if (guard != GUARD_OK)
        t_note("seed=%08x round=%u case=%u op=%s guard=%x faults=%u exits=%u vec=%u err=%x cr2=%08x eip=%08x expected_eip=%08x missing=%u",
               TEST_SEED, g_round, rcx_case_no, rcx_case_op, (u32)guard, rcx_fault_count, rcx_exit_count,
               rcx_fault_vec, rcx_fault_err, rcx_fault_addr, rcx_fault_ip,
               rcx_fault_eip, rcx_missing);
    CHECK_EQ(guard, GUARD_OK);
    return rcx_sum;
}

static void rcx_pushpop_body(void) {
    u32 seed = TEST_SEED, boundary = RCX_STACK + RCX_PAGE;
    for (u32 i = 0; i < RCX_CASES; i++) {
        u32 word16 = i & 1u;
        rcx_state_t in, want;
        rcx_seed_state(&in, rcx_rand(&seed), boundary + (word16 ? 14u : 30u));
        u32 width = word16 ? 2u : 4u;
        u32 frame = boundary - 2u;
        rcx_case_begin(8, 6);
        rcx_user_init(&in);
        u32 pos = rcx_fault_eip - RCX_CODE;
        rcx_fault_eip = RCX_CODE + pos;
        if (word16) rcx_code[pos++] = 0x66;
        rcx_code[pos++] = 0x60;
        pos = rcx_finish(pos);
        want = in;
        want.esp = in.esp - (word16 ? 16u : 32u);
        rcx_stack[RCX_PAGE - 3] = 0x5A;
        rcx_stack[RCX_PAGE + (word16 ? 14u : 30u)] = 0xC3;
        if (!rcx_run_user(RCX_CODE, in.esp, i * 2, word16 ? "pusha16" : "pushad")) return;
        rcx_check_state(&want, i * 2, word16 ? "pusha16" : "pushad");
        const u32 values[8] = { in.edi, in.esi, in.ebp, in.esp, in.ebx, in.edx, in.ecx, in.eax };
        for (u32 j = 0; j < 8; j++) {
            u32 got = word16 ? rcx_get16(rcx_stack + RCX_PAGE - 2 + j * 2)
                             : rcx_get32(rcx_stack + RCX_PAGE - 2 + j * 4);
            u32 expected = word16 && j == 3 ? (in.esp & 0xFFFFu) : values[j];
            if (word16) expected &= 0xFFFFu;
            if (got != expected) {
                t_note("seed=%08x round=%u case=%u op=%s slot=%u got=%08x want=%08x",
                       TEST_SEED, g_round, i, word16 ? "pusha16" : "pushad", j, got, expected);
                CHECK_EQ(got, expected);
                return;
            }
        }
        CHECK_EQ(rcx_stack[RCX_PAGE - 3], 0x5A);
        CHECK_EQ(rcx_stack[RCX_PAGE + (word16 ? 14u : 30u)], 0xC3);
        rcx_sum = csum_mix(rcx_sum, in.eax ^ in.esp);

        rcx_state_t popin, popwant;
        rcx_seed_state(&popin, rcx_rand(&seed), frame);
        const u32 popped[8] = {
            rcx_rand(&seed), rcx_rand(&seed), rcx_rand(&seed), rcx_rand(&seed),
            rcx_rand(&seed), rcx_rand(&seed), rcx_rand(&seed), rcx_rand(&seed)
        };
        rcx_stack[RCX_PAGE - 3] = 0x5A;
        rcx_stack[RCX_PAGE + (word16 ? 14u : 30u)] = 0xC3;
        for (u32 j = 0; j < 8; j++) {
            if (word16) rcx_put16(rcx_stack + RCX_PAGE - 2 + j * 2, (u16)popped[j]);
            else rcx_put32(rcx_stack + RCX_PAGE - 2 + j * 4, popped[j]);
        }
        rcx_case_begin(9, 4);
        rcx_user_init(&popin);
        pos = rcx_fault_eip - RCX_CODE;
        rcx_fault_eip = RCX_CODE + pos;
        if (word16) rcx_code[pos++] = 0x66;
        rcx_code[pos++] = 0x61;
        pos = rcx_finish(pos);
        popwant = popin;
        if (word16) {
            popwant.edi = (popin.edi & 0xFFFF0000u) | (popped[0] & 0xFFFFu);
            popwant.esi = (popin.esi & 0xFFFF0000u) | (popped[1] & 0xFFFFu);
            popwant.ebp = (popin.ebp & 0xFFFF0000u) | (popped[2] & 0xFFFFu);
            popwant.ebx = (popin.ebx & 0xFFFF0000u) | (popped[4] & 0xFFFFu);
            popwant.edx = (popin.edx & 0xFFFF0000u) | (popped[5] & 0xFFFFu);
            popwant.ecx = (popin.ecx & 0xFFFF0000u) | (popped[6] & 0xFFFFu);
            popwant.eax = (popin.eax & 0xFFFF0000u) | (popped[7] & 0xFFFFu);
        } else {
            popwant.edi = popped[0]; popwant.esi = popped[1]; popwant.ebp = popped[2];
            popwant.ebx = popped[4]; popwant.edx = popped[5]; popwant.ecx = popped[6];
            popwant.eax = popped[7];
        }
        popwant.esp = frame + (word16 ? 16u : 32u);
        if (!rcx_run_user(RCX_CODE, popin.esp, i * 2 + 1, word16 ? "popa16" : "popad")) return;
        rcx_check_state(&popwant, i * 2 + 1, word16 ? "popa16" : "popad");
        u32 got_ecx = word16 ? rcx_get16(rcx_stack + RCX_PAGE - 2 + 6u * width)
                             : rcx_get32(rcx_stack + RCX_PAGE - 2 + 6u * width);
        CHECK_EQ(got_ecx, popped[6] & (word16 ? 0xFFFFu : ~0u));
        CHECK_EQ(rcx_stack[RCX_PAGE - 3], 0x5A);
        CHECK_EQ(rcx_stack[RCX_PAGE + (word16 ? 14u : 30u)], 0xC3);
        rcx_sum = csum_mix(rcx_sum, popwant.eax ^ popwant.esp);
    }
    t_note("seed=%08x pairs=%u; PUSHA+POPA total fault/restarts=%u; each form=%u; final state/frame/guards checked",
           TEST_SEED, RCX_CASES, RCX_CASES * 2, RCX_CASES / 2);
    t_ops(RCX_CASES * 2);
}

static u32 rcx_pushpop(void) { return rcx_prepare(rcx_pushpop_body); }

static void rcx_enterleave_body(void) {
    u32 seed = TEST_SEED ^ 0xE17E2u, boundary = RCX_STACK + RCX_PAGE;
    for (u32 i = 0; i < RCX_CASES; i++) {
        rcx_state_t in, want;
        rcx_seed_state(&in, rcx_rand(&seed), boundary + 2u);
        rcx_case_begin(8, 6);
        rcx_user_init(&in);
        u32 pos = rcx_fault_eip - RCX_CODE;
        rcx_code[pos++] = 0xC8; rcx_code[pos++] = 24; rcx_code[pos++] = 0; rcx_code[pos++] = 0;
        rcx_code[pos++] = 0xC9;
        rcx_finish(pos);
        want = in;
        if (!rcx_run_user(RCX_CODE, in.esp, i * 2, "enter_leave")) return;
        rcx_check_state(&want, i * 2, "enter_leave");
        CHECK_EQ(rcx_get32(rcx_stack + RCX_PAGE - 2), in.ebp);
        CHECK_EQ(rcx_stack[RCX_PAGE - 3], 0xA5);
        CHECK_EQ(rcx_stack[RCX_PAGE + 2], 0xA5);
        rcx_sum = csum_mix(rcx_sum, in.ebp ^ in.esp);

        rcx_state_t lin, lwant;
        rcx_seed_state(&lin, rcx_rand(&seed), boundary + 0x80);
        lin.ebp = boundary - 2u;
        u32 saved = rcx_rand(&seed);
        rcx_put32(rcx_stack + RCX_PAGE - 2, saved);
        rcx_case_begin(8, 4);
        rcx_user_init(&lin);
        pos = rcx_fault_eip - RCX_CODE;
        rcx_code[pos++] = 0xC9;
        pos = rcx_finish(pos);
        rcx_fault_eip = RCX_CODE + (rcx_fault_eip - RCX_CODE);
        lwant = lin;
        lwant.ebp = saved;
        lwant.esp = boundary + 2u;
        if (!rcx_run_user(RCX_CODE, lin.esp, i * 2 + 1, "leave")) return;
        rcx_check_state(&lwant, i * 2 + 1, "leave");
        rcx_sum = csum_mix(rcx_sum, saved ^ lwant.esp);
    }
    t_note("seed=%08x pairs=%u; ENTER+LEAVE total fault/restarts=%u; final state checked",
           TEST_SEED, RCX_CASES, RCX_CASES * 2);
    t_ops(RCX_CASES * 2);
}

static u32 rcx_enterleave(void) { return rcx_prepare(rcx_enterleave_body); }

static void rcx_far_body(void) {
    u32 seed = TEST_SEED ^ 0xFA12E7u, boundary = RCX_STACK + RCX_PAGE;
    gdt_set(8, 0, 0xFFFFF, 0xFA, 0xC);
    for (u32 i = 0; i < RCX_CASES; i++) {
        rcx_state_t in, want;
        rcx_seed_state(&in, rcx_rand(&seed), boundary + 2u);
        rcx_case_begin(8, 6);
        rcx_user_init(&in);
        u32 call_pos = rcx_fault_eip - RCX_CODE;
        rcx_code[call_pos] = 0x9A;
        rcx_put32(rcx_code + call_pos + 1, RCX_CODE + call_pos + 32);
        rcx_put16(rcx_code + call_pos + 5, 0x43);
        rcx_code[call_pos + 7] = 0xCD;
        rcx_code[call_pos + 8] = 0x80;
        rcx_code[call_pos + 32] = 0xCB;
        u32 exit_pos = call_pos + 9;
        rcx_fault_eip = RCX_CODE + call_pos;
        rcx_stack[RCX_PAGE - 7] = 0x5A;
        rcx_stack[RCX_PAGE + 2] = 0xC3;
        want = in;
        if (!rcx_run_user(RCX_CODE, in.esp, i * 2, "far_call_retf")) return;
        rcx_check_state(&want, i * 2, "far_call_retf");
        CHECK_EQ(rcx_get32(rcx_stack + RCX_PAGE - 6), RCX_CODE + call_pos + 7);
        CHECK_EQ(rcx_get16(rcx_stack + RCX_PAGE - 2), SEL_UCODE);
        CHECK_EQ(rcx_stack[RCX_PAGE - 7], 0x5A);
        CHECK_EQ(rcx_stack[RCX_PAGE + 2], 0xC3);
        CHECK_EQ(rcx_exit.eip, RCX_CODE + exit_pos);
        rcx_sum = csum_mix(rcx_sum, want.eax ^ want.esp);

        rcx_state_t rin, rwant;
        rcx_seed_state(&rin, rcx_rand(&seed), boundary + 0x80);
        u32 frame = boundary - 4;
        u32 ret_pos = 128;
        rcx_put32(rcx_stack + RCX_PAGE - 4, RCX_CODE + ret_pos);
        rcx_put32(rcx_stack + RCX_PAGE, SEL_UCODE);
        rcx_stack[RCX_PAGE - 5] = 0x5A;
        rcx_stack[RCX_PAGE + 4] = 0xC3;
        rcx_case_begin(8, 4);
        rcx_user_init(&rin);
        u32 p = rcx_fault_eip - RCX_CODE;
        rcx_code[p++] = 0xBC; rcx_put32(rcx_code + p, frame); p += 4;
        rcx_code[p++] = 0xCB;
        rcx_code[ret_pos] = 0xCD; rcx_code[ret_pos + 1] = 0x80;
        rcx_fault_eip = RCX_CODE + (p - 1);
        rwant = rin;
        rwant.esp = frame + 8;
        if (!rcx_run_user(RCX_CODE, rin.esp, i * 2 + 1, "retf")) return;
        rcx_check_state(&rwant, i * 2 + 1, "retf");
        CHECK_EQ(rcx_exit.eip, RCX_CODE + ret_pos + 2);
        CHECK_EQ(rcx_stack[RCX_PAGE - 5], 0x5A);
        CHECK_EQ(rcx_stack[RCX_PAGE + 4], 0xC3);
        rcx_sum = csum_mix(rcx_sum, rwant.eax ^ rwant.esp);
    }
    t_note("seed=%08x pairs=%u; far CALL+its RETF+standalone RETF total fault/restarts=%u; final frames/state checked",
           TEST_SEED, RCX_CASES, RCX_CASES * 3);
    t_ops(RCX_CASES * 3);
}

static u32 rcx_far(void) { return rcx_prepare(rcx_far_body); }

static void rcx_iret_body(void) {
    u32 seed = TEST_SEED ^ 0x1AE7u, boundary = RCX_STACK + RCX_PAGE;
    for (u32 i = 0; i < RCX_CASES; i++) {
        rcx_state_t in, want;
        rcx_seed_state(&in, rcx_rand(&seed), boundary + 0x80);
        u32 frame = boundary - 4;
        u32 target = 256;
        const u32 frame_flags = 0x243u;
        rcx_put32(rcx_stack + RCX_PAGE - 4, RCX_CODE + target);
        rcx_put32(rcx_stack + RCX_PAGE, SEL_UCODE);
        rcx_put32(rcx_stack + RCX_PAGE + 4, frame_flags);
        rcx_stack[RCX_PAGE - 5] = 0x5A;
        rcx_stack[RCX_PAGE + 8] = 0xC3;
        rcx_case_begin(8, 4);
        rcx_user_init(&in);
        u32 p = rcx_fault_eip - RCX_CODE;
        rcx_code[p++] = 0xBC; rcx_put32(rcx_code + p, frame); p += 4;
        rcx_code[p++] = 0xCF;
        rcx_code[target] = 0xCD; rcx_code[target + 1] = 0x80;
        rcx_fault_eip = RCX_CODE + p - 1;
        want = in;
        want.esp = frame + 12;
        want.eflags = frame_flags & ~0x200u;
        if (!rcx_run_user(RCX_CODE, in.esp, i, "iret")) return;
        rcx_check_state(&want, i, "iret");
        if (rcx_exit.eip != RCX_CODE + target + 2) {
            t_note("seed=%08x round=%u case=%u op=iret final_eip=%08x want=%08x",
                   TEST_SEED, g_round, i, rcx_exit.eip, RCX_CODE + target + 2);
            CHECK_EQ(rcx_exit.eip, RCX_CODE + target + 2);
            return;
        }
        CHECK_EQ(rcx_get32(rcx_stack + RCX_PAGE - 4), RCX_CODE + target);
        CHECK_EQ(rcx_get32(rcx_stack + RCX_PAGE + 4), frame_flags);
        CHECK_EQ(rcx_stack[RCX_PAGE - 5], 0x5A);
        CHECK_EQ(rcx_stack[RCX_PAGE + 8], 0xC3);
        rcx_sum = csum_mix(rcx_sum, want.eax ^ want.esp ^ want.eflags);
    }
    t_note("seed=%08x IRET fault/restarts=%u; same-ring frame crosses page, IF remains clear; final registers/flags/stack checked",
           TEST_SEED, RCX_CASES);
    t_ops(RCX_CASES);
}

static u32 rcx_iret(void) { return rcx_prepare(rcx_iret_body); }

const test_t tests_restart_complex[] = {
    { "restart_pusha_popa", rcx_pushpop, REQ_PAGING | REQ_486 },
    { "restart_enter_leave", rcx_enterleave, REQ_PAGING | REQ_486 },
    { "restart_far_call_retf", rcx_far, REQ_PAGING | REQ_486 },
    { "restart_iret", rcx_iret, REQ_PAGING | REQ_486 },
    { NULL, NULL, 0 }
};
