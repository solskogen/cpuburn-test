#include "kernel.h"

#define SEL_SCRATCH0 0x40

static volatile u32 pm_int_count;
static volatile u32 pm_tf_count;
static volatile u32 pm_db_count, pm_db_status;
static volatile u32 pm_watch_word;
static volatile u32 pm_sep_ss, pm_sep_cs, pm_sep_ds;
extern void pm_sysenter_landing(void);
static volatile u32 pm_ac_addr;
static void pm_r3_unaligned(void);

static void pm_count_int(regs_t *r) { (void)r; pm_int_count++; }

static void pm_tf_hook(regs_t *r) {
    pm_tf_count++;
    r->eflags &= ~(1u << 8);
}

static void pm_db_hook(regs_t *r) {
    u32 dr6;
    __asm__ volatile("mov %%dr6,%0" : "=r"(dr6));
    pm_db_status = dr6;
    pm_db_count++;
    if (dr6 & 1u) r->eflags |= 1u << 16;
}

static void pm_check_exc(u32 vec, u32 err) {
    CHECK_EQ(g_exc.vec, vec);
    CHECK_EQ(g_exc.err, err);
    CHECK(g_exc.eip != 0);
}

static u32 pm_exceptions(void) {
    u32 sum = 0;
    u32 before;

    before = g_exc.count;
    exc_expect(0, 2);
    __asm__ volatile("xorl %%edx,%%edx; xorl %%ecx,%%ecx; divl %%ecx" ::: "eax", "ecx", "edx", "cc");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(0, 0);
    sum = csum_mix(sum, g_exc.eip);

    before = g_exc.count;
    exc_expect(3, 0);
    __asm__ volatile("int3" ::: "memory");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(3, 0);
    sum = csum_mix(sum, g_exc.eip);

    before = g_exc.count;
    exc_expect(4, 0);
    __asm__ volatile("movl $0x7fffffff,%%eax; addl $1,%%eax; into" ::: "eax", "cc");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(4, 0);
    sum = csum_mix(sum, g_exc.eip);

    s32 bounds[2] = { 0, 10 };
    u32 bounds_addr = (u32)bounds;
    before = g_exc.count;
    exc_expect(5, 2);
    __asm__ volatile("movl $11,%%eax; movl %0,%%edx; .byte 0x62,0x02"
                     :: "r"(bounds_addr) : "eax", "edx", "cc", "memory");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(5, 0);
    sum = csum_mix(sum, g_exc.eip);

    before = g_exc.count;
    exc_expect(6, 2);
    __asm__ volatile("ud2" ::: "memory");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(6, 0);

    before = g_exc.count;
    exc_expect(6, 3);
    __asm__ volatile(".byte 0xf0,0x01,0xd8" ::: "eax", "ebx", "cc", "memory");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(6, 0);

    before = g_exc.count;
    exc_expect(6, 2);
    __asm__ volatile(".byte 0x0f,0xff" ::: "memory");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(6, 0);

    u32 cr0 = rd_cr0();
    wr_cr0(cr0 | 8u);
    before = g_exc.count;
    exc_expect(7, 2);
    __asm__ volatile("fninit" ::: "memory");
    exc_expect_clear();
    wr_cr0(cr0);
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(7, 0);

    gdt_set(8, 0, 0xFFFFF, 0x12, 0xC); /* not-present writable data */
    before = g_exc.count;
    exc_expect(11, 2);
    __asm__ volatile("movw $0x40,%%ax; movw %%ax,%%ds" ::: "eax", "memory");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(11, SEL_SCRATCH0);

    before = g_exc.count;
    exc_expect(12, 2);
    __asm__ volatile("movw $0x40,%%ax; movw %%ax,%%ss" ::: "eax", "memory");
    exc_expect_clear();
    CHECK_EQ(g_exc.count, before + 1);
    pm_check_exc(12, SEL_SCRATCH0);

    return sum;
}

static u32 pm_alignment_check(void) {
    void *mem = arena_alloc(8, 4);
    void *stack = arena_alloc(4096, 16);
    CHECK(mem && stack);
    if (!mem || !stack) return 0;
    pm_ac_addr = (u32)mem;
    u32 old_cr0 = rd_cr0();
    wr_cr0(old_cr0 | (1u << 18));
    int rc = ring3_call(pm_r3_unaligned, (u32)stack + 4096);
    wr_cr0(old_cr0);
    if (rc == GUARD_R3_INT80) {
        t_note("required_trap_missing=AC CPL=3 CR0.AM=1 EFLAGS.AC=1 unaligned_access_completed=1");
        t_fail("i486+ must deliver #AC for the enabled ring-3 alignment check", __LINE__);
        return 0;
    }
    if (rc == GUARD_EXC(6)) {
        t_note("required_trap_missing=AC unexpected_UD eip=%08x", g_exc.eip);
        t_fail("i486+ alignment-check probe unexpectedly raised #UD", __LINE__);
        return g_exc.eip;
    }
    CHECK_EQ(rc, GUARD_EXC(17));
    CHECK_EQ(g_exc.vec, 17);
    CHECK_EQ(g_exc.err, 0);
    CHECK(g_exc.eflags & (1u << 18));
    return csum_mix(g_exc.eip, g_exc.eflags);
}

static u32 pm_segments(void) {
    u32 sum = 0, rights = 0, lim = 0, flags, sel;

    gdt_set(8, 0, 0xFFFFF, 0x92, 0xC);
    sel = SEL_SCRATCH0;
    __asm__ volatile("movw %w0,%%ax; movw %%ax,%%ds" :: "r"(sel) : "eax", "memory");
    CHECK((u8)(gdt_get(8) >> 40) & 1); /* descriptor accessed bit */

    __asm__ volatile("lar %w2,%0; pushfl; popl %1"
                     : "=r"(rights), "=r"(flags) : "r"(sel) : "cc");
    CHECK(flags & (1u << 6));
    sum = csum_mix(sum, rights);

    __asm__ volatile("lsl %w2,%0; pushfl; popl %1"
                     : "=r"(lim), "=r"(flags) : "r"(sel) : "cc");
    CHECK(flags & (1u << 6));
    CHECK_EQ(lim, 0xFFFFFFFFu);
    sum = csum_mix(sum, lim);

    __asm__ volatile("verr %w1; pushfl; popl %0" : "=r"(flags) : "r"(sel) : "cc");
    CHECK(flags & (1u << 6));
    __asm__ volatile("verw %w1; pushfl; popl %0" : "=r"(flags) : "r"(sel) : "cc");
    CHECK(flags & (1u << 6));

    sel = 0;
    __asm__ volatile("lar %w2,%0; pushfl; popl %1"
                     : "=r"(rights), "=r"(flags) : "r"(sel) : "cc");
    CHECK(!(flags & (1u << 6)));
    __asm__ volatile("lsl %w2,%0; pushfl; popl %1"
                     : "=r"(lim), "=r"(flags) : "r"(sel) : "cc");
    CHECK(!(flags & (1u << 6)));

    /* A CPL0 software interrupt is handled by the ordinary ISR hook and IRET path. */
    pm_int_count = 0;
    int_hook[0x81] = pm_count_int;
    idt_set_gate(0x81, isr_stub[0x81], SEL_KCODE, 0x8E);
    for (u32 i = 0; i < 1024; i++) __asm__ volatile("int $0x81" ::: "memory");
    CHECK_EQ(pm_int_count, 1024);
    t_ops(1024);
    sum = csum_mix(sum, pm_int_count);

    return sum;
}

static void pm_r3_bad_cli(void) { __asm__ volatile("cli"); }
static void pm_r3_bad_int(void) { __asm__ volatile("int $0x81"); }
static void pm_r3_unaligned(void) {
    __asm__ volatile("pushfl; orl $0x40000,(%%esp); popfl" ::: "memory", "cc");
    (void)*(volatile u16 *)(pm_ac_addr + 1);
    __asm__ volatile("int $0x80" ::: "memory");
}

static u32 pm_gates_rings(void) {
    u32 sum = 0;
    void *stack = arena_alloc(4096, 16);
    CHECK(stack != NULL);
    if (!stack) return 0;

    idt_set_gate(0x82, isr_stub[0x82], SEL_KCODE, 0x0E); /* present bit clear */
    exc_expect(11, 2);
    __asm__ volatile("int $0x82" ::: "memory");
    exc_expect_clear();
    CHECK_EQ(g_exc.vec, 11);
    CHECK_EQ(g_exc.err, (0x82u << 3) | 2u);
    sum = csum_mix(sum, g_exc.err);

    int rc = ring3_call(pm_r3_bad_cli, (u32)stack + 4096);
    CHECK_EQ(rc, GUARD_EXC(13));
    CHECK_EQ(g_exc.vec, 13);
    CHECK_EQ(g_exc.err, 0);
    sum = csum_mix(sum, g_exc.eip);

    idt_set_gate(0x81, isr_stub[0x81], SEL_KCODE, 0x8E);
    rc = ring3_call(pm_r3_bad_int, (u32)stack + 4096);
    CHECK_EQ(rc, GUARD_EXC(13));
    CHECK_EQ(g_exc.vec, 13);
    CHECK_EQ(g_exc.err, (0x81u << 3) | 2u);
    sum = csum_mix(sum, g_exc.err);
    return sum;
}

static u64 pm_rdmsr(u32 index) { return rdmsr(index); }

__attribute__((naked)) static void pm_r3_sysenter(void) {
    __asm__ volatile(
        "movl %%esp,%%ecx\n\t"
        "leal 1f,%%edx\n\t"
        "sysenter\n"
        "1: int $0x80\n"
        ::: "memory");
}

__asm__(
    ".text\n"
    ".global pm_sysenter_landing\n"
    "pm_sysenter_landing:\n"
    "movw %ss,%ax\n\t"
    "movzwl %ax,%eax\n\t"
    "movl %eax,pm_sep_ss\n\t"
    "movw %cs,%ax\n\t"
    "movzwl %ax,%eax\n\t"
    "movl %eax,pm_sep_cs\n\t"
    "movw %ds,%ax\n\t"
    "movzwl %ax,%eax\n\t"
    "movl %eax,pm_sep_ds\n\t"
    "sysexit\n");

static u32 pm_sysenter(void) {
    u32 sum = 0;
    void *stack = arena_alloc(4096, 16);
    void *kstack = arena_alloc(4096, 16);
    CHECK(stack && kstack);
    if (!stack || !kstack) return 0;

    u64 old_cs = pm_rdmsr(0x174), old_esp = pm_rdmsr(0x175), old_eip = pm_rdmsr(0x176);
    pm_sep_ss = pm_sep_cs = pm_sep_ds = 0;
    wrmsr(0x174, SEL_KCODE);
    wrmsr(0x175, (u32)kstack + 4096);
    wrmsr(0x176, (u32)pm_sysenter_landing);
    int rc = ring3_call(pm_r3_sysenter, (u32)stack + 4096);
    wrmsr(0x174, old_cs);
    wrmsr(0x175, old_esp);
    wrmsr(0x176, old_eip);

    CHECK_EQ(rc, GUARD_R3_INT80);
    CHECK_EQ(pm_sep_ss, SEL_KDATA);
    CHECK_EQ(pm_sep_cs, SEL_KCODE);
    CHECK_EQ(pm_sep_ds, SEL_UDATA);
    sum = csum_mix(sum, pm_sep_ss | (pm_sep_cs << 16));
    return sum;
}

static u32 pm_irq_nesting(void) {
    pm_int_count = 0;
    int_hook[0x81] = pm_count_int;
    idt_set_gate(0x81, isr_stub[0x81], SEL_KCODE, 0x8E);
    u32 ticks = pit_ticks;
    for (u32 i = 0; i < 200000; i++) __asm__ volatile("int $0x81" ::: "memory");
    CHECK_EQ(pm_int_count, 200000);
    t_ops(200000);
    CHECK(pit_ticks > ticks);
    t_note("software_ints=%u pit_ticks=%u", pm_int_count, pit_ticks - ticks);
    return csum_mix(pm_int_count, pit_ticks - ticks);
}

extern void pm_bp_target(void);
__asm__(
    ".text\n"
    ".global pm_bp_target\n"
    "pm_bp_target:\n"
    "nop\n"
    "ret\n");

static void pm_debug_probe(void) {
    pm_db_count = pm_db_status = 0;
    int_hook[1] = pm_db_hook;
    u32 reset_dr6 = 0xffff0ff0u, enable_exec = 1, disable = 0;
    u32 target = (u32)pm_bp_target;
    __asm__ volatile("mov %0,%%dr0; mov %1,%%dr6; mov %2,%%dr7"
                     :: "r"(target), "r"(reset_dr6), "r"(enable_exec) : "memory");
    pm_bp_target();
    __asm__ volatile("mov %0,%%dr7" :: "r"(disable) : "memory");

    pm_db_status = 0;
    u32 watch = (u32)&pm_watch_word, enable_write = 0x00010001;
    __asm__ volatile("mov %0,%%dr0; mov %1,%%dr6; mov %2,%%dr7"
                     :: "r"(watch), "r"(reset_dr6), "r"(enable_write) : "memory");
    __asm__ volatile("movl $0x13579bdf,(%0)" :: "r"(watch) : "memory");
    __asm__ volatile("mov %0,%%dr7" :: "r"(disable) : "memory");
    int_hook[1] = NULL;
}

static u32 pm_debug_regs(void) {
    u32 dr0, dr1, dr2, dr3, dr6, dr7;
    __asm__ volatile("mov %%dr0,%0; mov %%dr1,%1; mov %%dr2,%2; mov %%dr3,%3; mov %%dr6,%4; mov %%dr7,%5"
                     : "=r"(dr0), "=r"(dr1), "=r"(dr2), "=r"(dr3), "=r"(dr6), "=r"(dr7));
    pm_tf_count = 0;
    int_hook[1] = pm_tf_hook;
    __asm__ volatile("pushfl; orl $0x100,(%%esp); popfl; nop" ::: "memory", "cc");
    int_hook[1] = NULL;
    CHECK(pm_tf_count >= 1);
    t_ops(pm_tf_count);

    int rc = guard_call(pm_debug_probe);
    u32 disable = 0;
    __asm__ volatile("mov %6,%%dr7; mov %0,%%dr0; mov %1,%%dr1; mov %2,%%dr2; mov %3,%%dr3; mov %4,%%dr6; mov %5,%%dr7"
                     :: "r"(dr0), "r"(dr1), "r"(dr2), "r"(dr3), "r"(dr6), "r"(dr7), "r"(disable) : "memory");
    int_hook[1] = NULL;
    if (rc) {
        CHECK_EQ(rc, 0);
        return pm_tf_count;
    }
    if (pm_db_count < 2 || !(pm_db_status & 1u)) {
        t_note("required_trap_missing=DB breakpoint_count=%u expected=2 dr6=%08x", pm_db_count, pm_db_status);
        t_fail("i486+ execution/data breakpoints must deliver #DB", __LINE__);
    } else {
        t_ops(pm_db_count);
    }
    return csum_mix(pm_tf_count, pm_db_status);
}

static volatile u32 pm_task_visits;
static volatile u32 pm_task_mode;
static u32 pm_task_stack;
static u32 pm_task_far[2] __attribute__((aligned(4)));
u32 pm_task_main_far[2] __attribute__((aligned(4))) = { 0, SEL_TSS_MAIN };
extern void pm_task_entry(void);

__asm__(
    ".text\n"
    ".global pm_task_entry\n"
    "pm_task_entry:\n"
    "incl pm_task_visits\n"
    "cmpl $0,pm_task_mode\n"
    "jne 1f\n"
    "ljmp *pm_task_main_far\n"
    "1: iret\n");

static void pm_prepare_task(u32 nested) {
    kmemset(&tss_b, 0, sizeof tss_b);
    tss_b.cr3 = rd_cr3();
    tss_b.eip = (u32)pm_task_entry;
    tss_b.eflags = 2;
    tss_b.esp = pm_task_stack;
    tss_b.ebp = pm_task_stack;
    tss_b.cs = SEL_KCODE;
    tss_b.ss = tss_b.ds = tss_b.es = tss_b.fs = tss_b.gs = SEL_KDATA;
    tss_b.iomap = sizeof tss_b;
    pm_task_mode = nested;
    pm_task_far[0] = 0;
    pm_task_far[1] = SEL_TSS_B;
}

static u32 pm_task_switch(void) {
    u32 sum = 0;
    tss32_t saved_tss_b = tss_b;
    u64 saved_desc_b = gdt[6];
    void *stack = arena_alloc(4096, 16);
    CHECK(stack != NULL);
    if (!stack) return 0;
    pm_task_stack = (u32)stack + 4096;
    pm_task_visits = 0;

    pm_prepare_task(0);
    __asm__ volatile("ljmp *%0" :: "m"(pm_task_far) : "memory");
    CHECK_EQ(pm_task_visits, 1);
    CHECK_EQ((u32)((gdt_get(5) >> 40) & 0x0F), 0x0B);
    CHECK_EQ((u32)((gdt_get(6) >> 40) & 0x0F), 0x09);
    CHECK(rd_cr0() & 8u);
    sum = csum_mix(sum, pm_task_visits);

    pm_prepare_task(1);
    __asm__ volatile("lcall *%0" :: "m"(pm_task_far) : "memory");
    CHECK_EQ(pm_task_visits, 2);
    CHECK_EQ((u32)((gdt_get(5) >> 40) & 0x0F), 0x0B);
    CHECK_EQ((u32)((gdt_get(6) >> 40) & 0x0F), 0x09);
    CHECK(rd_cr0() & 8u);

    pm_prepare_task(1);
    idt_set_gate(0x83, (void (*)(void))0, SEL_TSS_B, 0x85);
    __asm__ volatile("int $0x83" ::: "memory");
    CHECK_EQ(pm_task_visits, 3);
    CHECK_EQ((u32)((gdt_get(5) >> 40) & 0x0F), 0x0B);
    CHECK_EQ((u32)((gdt_get(6) >> 40) & 0x0F), 0x09);
    CHECK(rd_cr0() & 8u);
    t_ops(3);
    sum = csum_mix(sum, pm_task_visits);
    tss_b = saved_tss_b;
    gdt[6] = saved_desc_b;
    return sum;
}

static volatile u32 pm_v86_mode;

static void pm_v86_enter(void) {
    __asm__ volatile(
        "pushl $0x7000\n\t" /* GS */
        "pushl $0x7000\n\t" /* FS */
        "pushl $0x7000\n\t" /* DS */
        "pushl $0x7000\n\t" /* ES */
        "pushl $0x7000\n\t" /* SS */
        "pushl $0x800\n\t"
        "cmpl $0,pm_v86_mode\n\t"
        "jne 1f\n\t"
        "pushl $0x00020002\n\t" /* VM, IOPL=0, IF=0 */
        "jmp 2f\n"
        "1: pushl $0x00023002\n" /* VM, IOPL=3, IF=0 */
        "2: pushl $0x7000\n\t"
        "pushl $0\n\t"
        "iretl\n"
        ::: "memory", "cc");
}

static u32 pm_v86(void) {
    volatile u8 *code = (volatile u8 *)0x70000;
    u32 sum = 0;
    int rc;

    /* MOV AX,imm16; INT 80h. At IOPL=0 the INT must #GP before IVT access. */
    code[0] = 0xB8; code[1] = 0x34; code[2] = 0x12;
    code[3] = 0xCD; code[4] = 0x80;
    pm_v86_mode = 0;
    rc = guard_call(pm_v86_enter);
    CHECK_EQ(rc, GUARD_EXC(13));
    CHECK_EQ(g_exc.vec, 13);
    CHECK_EQ(g_exc.err, 0);
    CHECK(g_exc.eflags & (1u << 17));
    sum = csum_mix(sum, g_exc.eip);

    /* IOPL=3 allows flag operations, but HLT remains privileged in V86. */
    code[0] = 0xF4;
    pm_v86_mode = 1;
    rc = guard_call(pm_v86_enter);
    CHECK_EQ(rc, GUARD_EXC(13));
    CHECK_EQ(g_exc.vec, 13);
    CHECK_EQ(g_exc.err, 0);
    CHECK(g_exc.eflags & (1u << 17));
    return csum_mix(sum, g_exc.eflags);
}

const test_t tests_pmode[] = {
    { "pm_exceptions", pm_exceptions, REQ_486 },
    { "pm_alignment_check", pm_alignment_check, REQ_486 },
    { "pm_segments", pm_segments, REQ_486 },
    { "pm_gates_rings", pm_gates_rings, REQ_486 },
    { "pm_task_switch", pm_task_switch, REQ_486 },
    { "pm_v86", pm_v86, REQ_486 },
    { "pm_sysenter", pm_sysenter, REQ_SEP | REQ_MSR },
    { "pm_irq_nesting", pm_irq_nesting, REQ_486 },
    { "pm_debug_regs", pm_debug_regs, REQ_486 },
    { 0, 0, 0 }
};
