#include "kernel.h"

#ifndef HEAVY_SCALE
#define HEAVY_SCALE 8
#endif
#ifndef TEST_SEED
#define TEST_SEED 0x86B04A21u
#endif

/*
 * All destructive work is inside a nested guard, so descriptor/LDTR/hook
 * restoration also runs after an unexpected CPU fault. TR is never changed.
 */
static u64 hp_saved_gdt[8];
static u16 hp_saved_ldt, hp_saved_tr, hp_saved_fs;
static u32 hp_saved_flags, hp_rng, hp_case;
static isr_hook_t hp_saved_db, hp_saved_irq;

static u32 hp_rand(void) {
    u32 x = hp_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return hp_rng = x;
}

static void hp_save(void) {
    hp_saved_flags = rd_eflags();
    __asm__ volatile("sldt %0; str %1; mov %%fs,%2"
                     : "=m"(hp_saved_ldt), "=m"(hp_saved_tr), "=m"(hp_saved_fs));
    for (u32 i = 0; i < 8; i++) hp_saved_gdt[i] = gdt_get(8 + i);
    hp_saved_db = int_hook[1];
    hp_saved_irq = int_hook[0x20];
    hp_rng = TEST_SEED;
    hp_case = 0;
}

static void hp_restore(void) {
    u16 tr;
    cli();
    exc_expect_clear();
    __asm__ volatile("mov %0,%%fs" :: "rm"((u16)SEL_KDATA) : "memory");
    for (u32 i = 0; i < 8; i++)
        gdt_set_raw(8 + i, (u32)hp_saved_gdt[i], (u32)(hp_saved_gdt[i] >> 32));
    __asm__ volatile("lldt %0" :: "m"(hp_saved_ldt) : "memory");
    __asm__ volatile("mov %1,%%fs; str %0" : "=m"(tr) : "m"(hp_saved_fs) : "memory");
    int_hook[1] = hp_saved_db;
    int_hook[0x20] = hp_saved_irq;
    wr_eflags(hp_saved_flags);
    CHECK_EQ(tr, hp_saved_tr);
}

static int hp_run(void (*body)(void)) {
    hp_save();
    int rc = guard_call(body);
    hp_restore();
    if (rc) {
        t_note("seed=%08x case=%u rc=%x vec=%u err=%x eip=%x",
               TEST_SEED, hp_case, rc, g_exc.vec, g_exc.err, g_exc.eip);
        CHECK_EQ(rc, GUARD_OK);
    }
    return rc;
}

static u32 hp_sum;
static u8 *hp_mem;
static u32 hp_far16[2] __attribute__((used)) = { 0, 0x40 };
extern const u8 hp_code16_begin[], hp_code16_end[];
extern u32 hp_enter16(void);

/*
 * Boot occupies 7c00..7dff, stage 2 starts at 10000. Save/restore 8000..ffff
 * and use a base-zero, B=0 stack there. Thus even a same-CPL exception has
 * a genuine linear ESP and remains recoverable by the ordinary 32-bit ISR.
 * No compiler code executes with the 16-bit DS/SS installed.
 */
__asm__(
    ".text\n"
    ".global hp_enter16\n"
    "hp_enter16:\n"
    "pushl %ebx\n pushl %esi\n pushl %edi\n"
    "leal -8(%esp),%edi\n"
    "lcall *hp_far16\n"
    "popl %edi\n popl %esi\n popl %ebx\n ret\n"
    ".global hp_code16_begin,hp_code16_end\n"
    "hp_code16_begin:\n"
    ".code16\n"
    "movw $0x48,%ax\n movw %ax,%ss\n movl $0xfff0,%esp\n"
    "movw $0x50,%ax\n movw %ax,%ds\n"
    "xorw %si,%si\n xorw %ax,%ax\n movw $128,%cx\n"
    "1: callw 3f\n lcallw $0x40,$(4f-hp_code16_begin)\n addw $2,%si\n loop 1b\n"
    "movzwl %ax,%edx\n"
    "cmpw $0xfff0,%sp\n je 2f\n orl $0x10000,%edx\n"
    "2: movw $0x10,%ax\n movw %ax,%ds\n movw %ax,%ss\n"
    "movl %edi,%esp\n movl %edx,%eax\n lretl\n"
    "3: addw (%si),%ax\n rolw $1,%ax\n pushw %ax\n popw %dx\n retw\n"
    "4: xorw $0x5a3c,%ax\n lretw\n"
    ".code32\n"
    "hp_code16_end:\n");

static void hp_16_body(void) {
    u32 bytes = (u32)(hp_code16_end - hp_code16_begin);
    kmemcpy(hp_mem, hp_code16_begin, bytes);
    gdt_set(8, (u32)hp_mem, bytes - 1, 0x9b, 0);
    gdt_set(9, 0, 0xffff, 0x93, 0);
    gdt_set(10, (u32)hp_mem + 256, 255, 0x93, 0);
    for (hp_case = 0; hp_case < 1024u * HEAVY_SCALE; hp_case++) {
        u16 expected = 0;
        for (u32 j = 0; j < 128; j++) {
            u16 v = (u16)hp_rand();
            ((u16 *)(hp_mem + 256))[j] = v;
            expected = (u16)(expected + v);
            expected = (u16)((expected << 1) | (expected >> 15));
            expected ^= 0x5a3c;
        }
        u32 flags = rd_eflags();
        cli();
        u32 got = hp_enter16();
        wr_eflags(flags);
        if (got != expected) {
            t_note("seed=%08x case=%u", TEST_SEED, hp_case);
            CHECK_EQ(got, expected);
            break;
        }
        hp_sum = csum_mix(hp_sum, got);
        t_ops(1 + 128 * 4); /* outer round trip, 128 near + 128 far 16-bit CALL/RETs */
    }
}

static u32 hp_16bit(void) {
    hp_mem = arena_alloc(512, 16);
    void *low_save = arena_alloc(32768, 16);
    CHECK(hp_mem && low_save);
    if (!hp_mem || !low_save) return 0;
    kmemcpy(low_save, (void *)0x8000, 32768);
    hp_sum = 0;
    hp_run(hp_16_body);
    kmemcpy((void *)0x8000, low_save, 32768);
    t_note("outer_roundtrips=%u near_RET16_per_trip=128 far_RETF16_per_trip=128 csD=0 ssB=0 address_bits=16",
           hp_case);
    return hp_sum;
}

static u64 *hp_ldt;

static void hp_ldt_oob(void) {
    __asm__ volatile("movw $0x24,%%ax; movw %%ax,%%fs" ::: "eax", "memory");
}
static void hp_ldt_wrong_type(void) {
    __asm__ volatile("movw $0x48,%%ax; lldt %%ax" ::: "eax", "memory");
}
static void hp_ldt_ti(void) {
    __asm__ volatile("movw $0x0c,%%ax; lldt %%ax" ::: "eax", "memory");
}
static void hp_ldt_absent(void) {
    __asm__ volatile("movw $0x50,%%ax; lldt %%ax" ::: "eax", "memory");
}

static u64 hp_descriptor(u32 base, u32 limit, u8 access, u8 flags) {
    u32 lo = (limit & 0xffff) | (base << 16);
    u32 hi = ((base >> 16) & 255) | ((u32)access << 8) |
             (limit & 0xf0000) | ((u32)flags << 20) | (base & 0xff000000);
    return ((u64)hi << 32) | lo;
}

static void hp_ldt_body(void) {
    gdt_set(8, (u32)hp_ldt, 31, 0x82, 0);
    gdt_set(9, 0, 0xffff, 0x93, 0);
    gdt_set(10, (u32)hp_ldt, 31, 0x02, 0);
    for (hp_case = 0; hp_case < 2048u * HEAVY_SCALE; hp_case++) {
        u32 v = hp_rand(), base = (u32)hp_mem + (v & 255);
        u32 limit = 0x1ff + ((v >> 8) & 0x1fff);
        u32 rights = 0, lim = 0, fl, fsbyte;
        u32 d = (v >> 23) & 4;
        hp_ldt[1] = hp_descriptor(base, limit, 0x93, d);
        hp_ldt[2] = hp_descriptor(base, limit, 0xf1, 0);
        __asm__ volatile("lldt %w0" :: "r"(0x40) : "memory");
        u32 sel = 0x0c;
        __asm__ volatile("lsl %w2,%0; pushfl; popl %1"
                         : "=&r"(lim), "=&r"(fl) : "r"(sel) : "cc");
        if (!(fl & 64) || lim != limit) {
            CHECK(fl & 64); CHECK_EQ(lim, limit); break;
        }
        __asm__ volatile("lar %w2,%0; pushfl; popl %1"
                         : "=&r"(rights), "=&r"(fl) : "r"(sel) : "cc");
        CHECK(fl & 64);
        CHECK_EQ(rights & 0x00f0ff00, (d << 20) | 0x9300);
        __asm__ volatile("mov %w1,%%fs; movzbl %%fs:256,%0"
                         : "=r"(fsbyte) : "r"(sel) : "memory");
        CHECK_EQ(fsbyte, hp_mem[(v & 255) + 256]);
        __asm__ volatile("verr %w1; pushfl; popl %0"
                         : "=r"(fl) : "r"(0x17) : "cc");
        CHECK(fl & 64); /* DPL3, RPL3, readable */
        __asm__ volatile("verw %w1; pushfl; popl %0"
                         : "=r"(fl) : "r"(0x17) : "cc");
        CHECK(!(fl & 64)); /* the same DPL3 segment is read-only */
        __asm__ volatile("lar %w2,%0; pushfl; popl %1"
                         : "=&r"(rights), "=&r"(fl) : "r"(0x0f) : "cc");
        CHECK(!(fl & 64)); /* RPL3 cannot inspect the DPL0 descriptor */
        hp_sum = csum_mix(hp_sum, lim ^ fsbyte ^ v);
        t_ops(7);
    }
    static void (*const bad[4])(void) = {
        hp_ldt_oob, hp_ldt_wrong_type, hp_ldt_ti, hp_ldt_absent
    };
    static const u16 errors[4] = { 0x24, 0x48, 0x0c, 0x50 };
    for (u32 round = 0; round < 128u * HEAVY_SCALE; round++) {
        for (u32 i = 0; i < 4; i++) {
            int rc = guard_call(bad[i]);
            u32 vec = i == 3 ? 11 : 13;
            if (rc != (int)GUARD_EXC(vec) || g_exc.err != errors[i]) {
                t_note("LDT bad_descriptor round=%u case=%u", round, i);
                CHECK_EQ(rc, GUARD_EXC(vec)); CHECK_EQ(g_exc.err, errors[i]);
                return;
            }
            t_ops(1);
        }
    }
    __asm__ volatile("mov %w0,%%fs; lldt %w1"
                     :: "r"(SEL_KDATA), "r"(0) : "memory");
    u32 fl, lim;
    __asm__ volatile("lsl %w2,%0; pushfl; popl %1"
                     : "=&r"(lim), "=&r"(fl) : "r"(0x0c) : "cc");
    CHECK(!(fl & 64));
}

static u32 hp_ldt_test(void) {
    hp_ldt = arena_alloc(32, 8);
    hp_mem = arena_alloc(16384, 16);
    CHECK(hp_ldt && hp_mem);
    if (!hp_ldt || !hp_mem) return 0;
    kmemset(hp_ldt, 0, 32);
    for (u32 i = 0; i < 16384; i++) hp_mem[i] = (u8)((i * 73) ^ (i >> 7));
    hp_sum = 0;
    hp_run(hp_ldt_body);
    t_note("seed=%08x lldt_rounds=%u TI=1 RPL=0,3", TEST_SEED, hp_case);
    return hp_sum;
}

static volatile u32 hp_fault_offset;
static void hp_fs_fault_byte(void) {
    __asm__ volatile("movl hp_fault_offset,%%eax; movb %%fs:(%%eax),%%al"
                     ::: "eax", "memory");
}
static void hp_fs_fault_word(void) {
    __asm__ volatile("movl hp_fault_offset,%%eax; movw %%fs:(%%eax),%%ax"
                     ::: "eax", "memory");
}

static int hp_expect_fs(u32 off, int word) {
    hp_fault_offset = off;
    u32 before = g_exc.count;
    exc_expect(13, word ? 4 : 3);
    if (word) hp_fs_fault_word(); else hp_fs_fault_byte();
    exc_expect_clear();
    if (g_exc.count != before + 1 || g_exc.vec != 13 || g_exc.err != 0) {
        t_note("seed=%08x case=%u offset=%08x word=%u faults=%u vec=%u err=%x",
               TEST_SEED, hp_case, off, word, g_exc.count - before, g_exc.vec, g_exc.err);
        CHECK_EQ(g_exc.count, before + 1);
        CHECK_EQ(g_exc.vec, 13); CHECK_EQ(g_exc.err, 0);
        return 0;
    }
    return 1;
}

extern void hp_stack16_underflow(void);
extern const u8 hp_stack16_bad_push[];
static u32 hp_ed_stack;
static u32 hp_ss_visits;
__asm__(
    ".text\n .global hp_stack16_underflow,hp_stack16_bad_push\n"
    "hp_stack16_underflow:\n"
    "movw $0x6b,%ax\n movw %ax,%ss\n movl $0x8004,%esp\n"
    "pushl $0x31b76a95\n"
    "hp_stack16_bad_push: pushl $0x2468ace0\n int $0x80\n");

static void hp_expand_body(void) {
    for (hp_case = 0; hp_case < 512u * HEAVY_SCALE; hp_case++) {
        u32 lim = 0x100 + (hp_rand() & 0x7fff);
        u32 got, sel = 0x40, off = lim + 1;
        gdt_set(8, (u32)hp_mem, lim, 0x97, 0); /* B=0: upper bound ffff */
        hp_mem[off] = (u8)hp_case;
        hp_mem[0xffff] = (u8)~hp_case;
        __asm__ volatile("mov %w1,%%fs; movzbl %%fs:(%2),%0"
                         : "=r"(got) : "r"(sel), "r"(off) : "memory");
        CHECK_EQ(got, (u8)hp_case);
        __asm__ volatile("movzbl %%fs:65535,%0" : "=r"(got));
        CHECK_EQ(got, (u8)~hp_case);
        if (!hp_expect_fs(lim, 0) || !hp_expect_fs(lim - 1, 0) ||
            !hp_expect_fs(0xffff, 1)) break;
        /* B=1 makes 10000 valid; the exact same B=0 access must fault. */
        if (!hp_expect_fs(0x10000, 0)) break;
        gdt_set(8, (u32)hp_mem, lim, 0x97, 4);
        hp_mem[0x10000] = 0xa7;
        __asm__ volatile("mov %w1,%%fs; movzbl %%fs:65536,%0"
                         : "=r"(got) : "r"(sel) : "memory");
        CHECK_EQ(got, 0xa7);
        if (!hp_expect_fs(lim, 0) || !hp_expect_fs(0xffffffffu, 1)) break;
        hp_sum = csum_mix(hp_sum, lim);
        t_ops(11); /* descriptor installs/reloads, legal reads, six boundary faults */
    }
    /*
     * A ring-0 stack-limit fault cannot safely push its own exception frame.
     * At CPL3, TSS.esp0 supplies the ordinary valid exception stack instead.
     */
    gdt_set(13, hp_ed_stack - 0x8000, 0x7fff, 0xf7, 0);
    hp_ss_visits = 0;
    for (u32 i = 0; i < 128u * HEAVY_SCALE; i++) {
        *(volatile u32 *)hp_ed_stack = 0;
        *(volatile u32 *)(hp_ed_stack - 4) = 0xdab3c579;
        int rc = ring3_call(hp_stack16_underflow, hp_ed_stack + 4096);
        if (rc != GUARD_EXC(12) || g_exc.err != 0 ||
            g_exc.eip != (u32)hp_stack16_bad_push) {
            t_note("expand-down SS16 round=%u rc=%x vec=%u err=%x eip=%x",
                   i, rc, g_exc.vec, g_exc.err, g_exc.eip);
            CHECK_EQ(rc, GUARD_EXC(12)); CHECK_EQ(g_exc.err, 0);
            CHECK_EQ(g_exc.eip, (u32)hp_stack16_bad_push);
            return;
        }
        CHECK_EQ(*(volatile u32 *)hp_ed_stack, 0x31b76a95);
        CHECK_EQ(*(volatile u32 *)(hp_ed_stack - 4), 0xdab3c579);
        hp_ss_visits++;
        t_ops(2);
    }
}

static u32 hp_expand(void) {
    hp_mem = arena_alloc(65538, 16);
    void *stack = arena_alloc(8192, 16);
    CHECK(hp_mem && stack);
    if (!hp_mem || !stack) return 0;
    hp_ed_stack = (u32)stack + 4096;
    hp_ss_visits = 0;
    hp_sum = 0;
    hp_run(hp_expand_body);
    t_note("seed=%08x cases=%u expand_down_B=0,1 faults_per_case=6", TEST_SEED, hp_case);
    t_note("ring3_SS16_low_boundary_rounds=%u normal_push_then_SS_fault=1",
           hp_ss_visits);
    return hp_sum;
}

static volatile u32 hp_gate_n, hp_gate_loops, hp_gate_next, hp_gate_visits;
static volatile u32 hp_gate_bad, hp_gate_total;
static volatile u32 hp_r3_stack, hp_r3_flags;
static void (*volatile hp_r3_code)(void);
extern void hp_gate_entry(void), hp_gate_user(void), hp_gate_bad_user(void);
extern void hp_r3_enter(void);

__asm__(
    ".text\n"
    ".global hp_r3_enter\n"
    "hp_r3_enter:\n"
    "movw $0x23,%ax\n movw %ax,%ds\n movw %ax,%es\n"
    "movw %ax,%fs\n movw %ax,%gs\n"
    "pushl $0x23\n pushl hp_r3_stack\n pushl hp_r3_flags\n"
    "pushl $0x1b\n pushl hp_r3_code\n iretl\n"
    ".global hp_gate_user,hp_gate_bad_user,hp_gate_entry\n"
    "hp_gate_user:\n"
    "movl %esp,%ebp\n movl hp_gate_next,%ebx\n movl hp_gate_loops,%ecx\n"
    "1: movl hp_gate_n,%esi\n xorl %edx,%edx\n"
    "2: decl %esi\n imull $0x10203,%esi,%eax\n addl %ebx,%eax\n"
    "pushl %eax\n addl %eax,%edx\n testl %esi,%esi\n jnz 2b\n"
    "lcall $0x5b,$0\n"
    "cmpl %edx,%eax\n je 3f\n orl $1,hp_gate_bad\n"
    "3: cmpl %ebp,%esp\n je 4f\n orl $2,hp_gate_bad\n movl %ebp,%esp\n"
    "4: addl %eax,hp_gate_total\n addl $0x9e3779b9,%ebx\n loop 1b\n"
    "int $0x80\n"
    "hp_gate_bad_user:\n lcall $0x5b,$0\n int $0x80\n"
    "hp_gate_entry:\n"
    "push %ds\n push %es\n pushal\n"
    "movw $0x10,%ax\n movw %ax,%ds\n movw %ax,%es\n"
    "leal 40(%esp),%esi\n"
    "cmpw $0x1b,4(%esi)\n je 5f\n orl $4,hp_gate_bad\n"
    "5: movw %ss,%ax\n cmpw $0x10,%ax\n je 6f\n orl $8,hp_gate_bad\n"
    "6: movl hp_gate_n,%ecx\n"
    "leal 8(%esi,%ecx,4),%edi\n"
    "cmpw $0x23,4(%edi)\n je 7f\n orl $16,hp_gate_bad\n"
    "7: movl (%edi),%edi\n xorl %ebx,%ebx\n"
    "movl hp_gate_next,%edx\n xorl %eax,%eax\n"
    "8: cmpl %edx,8(%esi,%ebx,4)\n je 9f\n orl $32,hp_gate_bad\n"
    "9: cmpl %edx,(%edi,%ebx,4)\n je 10f\n orl $64,hp_gate_bad\n"
    "10: addl 8(%esi,%ebx,4),%eax\n addl $0x10203,%edx\n"
    "incl %ebx\n cmpl %ecx,%ebx\n jb 8b\n"
    "movl %eax,28(%esp)\n"
    "addl $0x9e3779b9,hp_gate_next\n incl hp_gate_visits\n"
    "popal\n pop %es\n pop %ds\n"
    "cmpl $3,hp_gate_n\n jne 11f\n lret $12\n"
    "11: lret $124\n");

static void hp_gate_install(u8 access, u16 target) {
    u32 p = (u32)hp_gate_entry;
    gdt_set_raw(11, (p & 0xffff) | ((u32)target << 16),
                (p & 0xffff0000) | ((u32)access << 8) | hp_gate_n);
}

static void hp_gate_body(void) {
    hp_r3_code = hp_gate_user;
    hp_r3_flags = 2;
    for (u32 pass = 0; pass < 2; pass++) {
        hp_gate_n = pass ? 31 : 3;
        hp_gate_loops = 1024u * HEAVY_SCALE;
        hp_gate_next = TEST_SEED;
        hp_gate_total = hp_gate_visits = hp_gate_bad = 0;
        hp_gate_install(0xec, SEL_KCODE);
        int rc = guard_call(hp_r3_enter);
        CHECK_EQ(rc, GUARD_R3_INT80);
        CHECK_EQ(g_exc.cs & 0xffff, SEL_UCODE);
        CHECK_EQ(hp_gate_visits, hp_gate_loops);
        CHECK_EQ(hp_gate_bad, 0);
        u32 next = TEST_SEED, total = 0;
        for (u32 i = 0; i < hp_gate_loops; i++) {
            for (u32 j = 0; j < hp_gate_n; j++) total += next + j * 0x10203;
            next += 0x9e3779b9;
        }
        CHECK_EQ(hp_gate_total, total);
        CHECK_EQ(hp_gate_next, next);
        hp_sum = csum_mix(hp_sum, total);
        t_ops(hp_gate_visits * (hp_gate_n + 2));
        if (rc != GUARD_R3_INT80) return;
    }
}

static u32 hp_callgates(void) {
    void *stack = arena_alloc(8192, 16);
    CHECK(stack);
    if (!stack) return 0;
    hp_r3_stack = (u32)stack + 8192;
    hp_sum = 0;
    hp_run(hp_gate_body);
    t_note("gate32 DPL3->0->3 params=3,31 iterations_each=%u RETF_discards_both_copies=1",
           1024u * HEAVY_SCALE);
    return hp_sum;
}

__attribute__((naked)) static void hp_bad_ds(void) {
    __asm__ volatile("movw $0x10,%ax; movw %ax,%ds; int $0x80");
}
__attribute__((naked)) static void hp_bad_ss(void) {
    __asm__ volatile("movw $0x10,%ax; movw %ax,%ss; int $0x80");
}
__attribute__((naked)) static void hp_bad_lldt(void) {
    __asm__ volatile("xorl %eax,%eax; lldt %ax; int $0x80");
}
__attribute__((naked)) static void hp_bad_ltr(void) {
    __asm__ volatile("movw $0x28,%ax; ltr %ax; int $0x80");
}
__attribute__((naked)) static void hp_bad_far(void) {
    __asm__ volatile("lcall $8,$0; int $0x80");
}

static void hp_priv_body(void) {
    static const u8 access[5] = { 0x8c, 0x6c, 0xec, 0xec, 0xec };
    static const u16 target[5] = { 8, 8, 0, 0x10, 0x60 };
    static const u8 vec[5] = { 13, 11, 13, 13, 11 };
    static const u16 error[5] = { 0x58, 0x58, 0, 0x10, 0x60 };
    static void (*const entry[5])(void) = {
        hp_bad_ds, hp_bad_ss, hp_bad_lldt, hp_bad_ltr, hp_bad_far
    };
    static const u16 err2[5] = { 0x10, 0x10, 0, 0, 8 };
    hp_gate_n = 3;
    gdt_set(12, 0, 0xfffff, 0x1b, 0xc); /* non-present code target */
    for (hp_case = 0; hp_case < 128u * HEAVY_SCALE; hp_case++) {
        for (u32 i = 0; i < 5; i++) {
            hp_gate_install(access[i], target[i]);
            int rc = ring3_call(hp_gate_bad_user, hp_r3_stack);
            if (rc != (int)GUARD_EXC(vec[i]) || g_exc.err != error[i]) {
                t_note("seed=%08x case=%u gate_case=%u", TEST_SEED, hp_case, i);
                CHECK_EQ(rc, GUARD_EXC(vec[i])); CHECK_EQ(g_exc.err, error[i]);
                return;
            }
            rc = ring3_call(entry[i], hp_r3_stack);
            if (rc != GUARD_EXC(13) || g_exc.err != err2[i]) {
                t_note("seed=%08x case=%u privilege_case=%u", TEST_SEED, hp_case, i);
                CHECK_EQ(rc, GUARD_EXC(13)); CHECK_EQ(g_exc.err, err2[i]);
                return;
            }
            hp_sum = csum_mix(hp_sum, (i << 16) ^ error[i] ^ err2[i]);
            t_ops(2);
        }
    }
}

static u32 hp_privilege(void) {
    void *stack = arena_alloc(4096, 16);
    CHECK(stack);
    if (!stack) return 0;
    hp_r3_stack = (u32)stack + 4096;
    hp_sum = 0;
    hp_run(hp_priv_body);
    t_note("cases=%u checks_per_case=10 DPL/P/target/SS/DS/LLDT/LTR/direct_far", hp_case);
    return hp_sum;
}

static u32 hp_old_cr4, hp_probe_cr4;
static void hp_read_cr4(void) { hp_probe_cr4 = rd_cr4(); }
static void hp_write_cr4(void) { wr_cr4(hp_probe_cr4); }

static int hp_cr4_enable(u32 bits) {
    /* Called only from REQ_VME tests: never MOV CR4 on an early 486. */
    int rc = guard_call(hp_read_cr4);
    CHECK_EQ(rc, GUARD_OK);
    if (rc) return 0;
    hp_old_cr4 = hp_probe_cr4;
    hp_probe_cr4 |= bits;
    rc = guard_call(hp_write_cr4);
    CHECK_EQ(rc, GUARD_OK);
    if (rc) return 0;
    if ((rd_cr4() & bits) != bits) {
        CHECK_EQ(rd_cr4() & bits, bits);
        wr_cr4(hp_old_cr4);
        return 0;
    }
    return 1;
}

static volatile u32 hp_virtual_bad, hp_virtual_count;
extern void hp_pvi_user(void), hp_pvi_sti(void), hp_pvi_cli(void);
__asm__(
    ".text\n"
    ".global hp_pvi_user,hp_pvi_sti,hp_pvi_cli\n"
    "hp_pvi_user:\n"
    "movl hp_gate_loops,%ecx\n"
    "1: sti\n pushfl\n popl %eax\n andl $0x180200,%eax\n"
    "cmpl $0x80000,%eax\n je 2f\n orl $1,hp_virtual_bad\n"
    "2: cli\n pushfl\n popl %eax\n andl $0x180200,%eax\n"
    "jz 3f\n orl $2,hp_virtual_bad\n"
    /* POPFD at CPL3 cannot alter IF, VIF or VIP, even with PVI enabled. */
    "3: pushl $0x80202\n popfl\n pushfl\n popl %eax\n"
    "andl $0x180200,%eax\n jz 4f\n orl $4,hp_virtual_bad\n"
    "4: incl hp_virtual_count\n loop 1b\n int $0x80\n"
    "hp_pvi_sti:\n sti\n int $0x80\n"
    "hp_pvi_cli:\n cli\n int $0x80\n");

static void hp_pvi_body(void) {
    hp_gate_loops = 2048u * HEAVY_SCALE;
    hp_virtual_count = hp_virtual_bad = 0;
    hp_r3_flags = 2;
    hp_r3_code = hp_pvi_user;
    int rc = guard_call(hp_r3_enter);
    CHECK_EQ(rc, GUARD_R3_INT80);
    CHECK_EQ(hp_virtual_bad, 0);
    CHECK_EQ(hp_virtual_count, hp_gate_loops);
    t_ops(hp_virtual_count * 3);
    for (hp_case = 0; hp_case < 128u * HEAVY_SCALE; hp_case++) {
        hp_r3_flags = 0x100002; /* VIP=1, VIF=0, IF=0, IOPL=0 */
        hp_r3_code = hp_pvi_sti;
        rc = guard_call(hp_r3_enter);
        CHECK_EQ(rc, GUARD_EXC(13)); CHECK_EQ(g_exc.err, 0);
        CHECK_EQ(g_exc.eip, (u32)hp_pvi_sti);
        CHECK_EQ(g_exc.eflags & 0x180200, 0x100000);
        /*
         * Keep VIF clear on entry: VIF=VIP=1 requests a pending virtual-
         * interrupt #GP before even the first user instruction can execute.
         */
        hp_r3_flags = 0x100002; /* CLI is legal even with VIP=1 */
        hp_r3_code = hp_pvi_cli;
        rc = guard_call(hp_r3_enter);
        if (rc != GUARD_R3_INT80) {
            t_note("CLI with VIP case=%u rc=%x eip=%08x entry=%08x err=%x flags=%08x",
                   hp_case, rc, g_exc.eip, (u32)hp_pvi_cli, g_exc.err, g_exc.eflags);
        }
        CHECK_EQ(rc, GUARD_R3_INT80);
        CHECK_EQ(g_exc.eflags & 0x180200, 0x100000);
        t_ops(2);
        if (rc != GUARD_R3_INT80) return;
    }
}

static u32 hp_pvi(void) {
    void *stack = arena_alloc(4096, 16);
    CHECK(stack);
    if (!stack || !hp_cr4_enable(2)) return 0;
    hp_r3_stack = (u32)stack + 4096;
    hp_run(hp_pvi_body);
    wr_cr4(hp_old_cr4);
    t_note("PVI virtual_CLI_STI_POPFD=%u VIP_cases=%u", hp_virtual_count, hp_case * 2);
    return csum_mix(hp_virtual_count, hp_virtual_bad);
}

extern const u8 hp_vme_begin[], hp_vme_end[], hp_vme_hlt[];
static volatile u32 hp_vm_flags;
static void hp_vm_enter(void) {
    __asm__ volatile(
        "pushl $0x7000\n pushl $0x7000\n pushl $0x7000\n pushl $0x7000\n"
        "pushl $0x7000\n pushl $0xff00\n pushl hp_vm_flags\n"
        "pushl $0x7000\n pushl $0\n iretl\n" ::: "memory");
}

__asm__(
    ".text\n"
    ".global hp_vme_begin,hp_vme_end,hp_vme_hlt\n"
    "hp_vme_begin:\n .code16\n"
    "movw 0x202,%cx\n xorw %bx,%bx\n xorw %dx,%dx\n"
    "1: cli\n pushfw\n popw %ax\n testw $0x200,%ax\n jz 2f\n incw %bx\n"
    "2: sti\n pushfw\n popw %ax\n andw $0x3200,%ax\n"
    "cmpw $0x3200,%ax\n je 3f\n incw %bx\n"
    "3: pushw $2\n popfw\n pushfw\n popw %ax\n"
    "testw $0x200,%ax\n jz 4f\n incw %bx\n"
    "4: sti\n pushfw\n pushw %cs\n pushw $(5f-hp_vme_begin)\n cli\n iretw\n"
    "5: pushfw\n popw %ax\n testw $0x200,%ax\n jnz 6f\n incw %bx\n"
    "6: incw %dx\n loop 1b\n"
    "movw %bx,0x200\n movw %dx,0x204\n"
    "hp_vme_hlt: hlt\n"
    ".code32\n hp_vme_end:\n");

static void hp_vme_body(void) {
    u8 *p = (u8 *)0x70000;
    kmemcpy(p, hp_vme_begin, hp_vme_end - hp_vme_begin);
    *(u16 *)(p + 0x200) = 0xffff;
    *(u16 *)(p + 0x202) = (u16)(1024u * HEAVY_SCALE);
    *(u16 *)(p + 0x204) = 0;
    hp_vm_flags = 0x20002;
    int rc = guard_call(hp_vm_enter);
    CHECK_EQ(rc, GUARD_EXC(13)); CHECK_EQ(g_exc.err, 0);
    CHECK_EQ(g_exc.eip, hp_vme_hlt - hp_vme_begin);
    CHECK_EQ(g_exc.eflags & 0x1a0200, 0xa0000); /* VM+VIF, not real IF/VIP */
    CHECK_EQ(*(u16 *)(p + 0x200), 0);
    CHECK_EQ(*(u16 *)(p + 0x204), 1024u * HEAVY_SCALE);
    t_ops(1024u * HEAVY_SCALE * 7);
    static const u8 code[3][9] = {
        { 0xfb, 0xf4 },
        { 0x68, 0x02, 0x02, 0x9d, 0xf4 },
        { 0x68, 0x02, 0x02, 0x0e, 0x68, 0x08, 0x00, 0xcf, 0xf4 }
    };
    static const u8 fault[3] = { 0, 3, 7 };
    for (hp_case = 0; hp_case < 128u * HEAVY_SCALE; hp_case++) {
        for (u32 i = 0; i < 3; i++) {
            kmemcpy(p, code[i], 9);
            hp_vm_flags = 0x120002; /* VM+VIP, VIF=0 */
            rc = guard_call(hp_vm_enter);
            if (rc != GUARD_EXC(13) || g_exc.eip != fault[i] || g_exc.err) {
                t_note("VIP case=%u iteration=%u", i, hp_case);
                CHECK_EQ(rc, GUARD_EXC(13)); CHECK_EQ(g_exc.eip, fault[i]);
                CHECK_EQ(g_exc.err, 0);
                return;
            }
            CHECK_EQ(g_exc.eflags & 0x1a0200, 0x120000);
            t_ops(1);
        }
    }
}

static u32 hp_vme(void) {
    void *save = arena_alloc(8192, 16);
    CHECK(save);
    if (!save || !hp_cr4_enable(1)) return 0;
    kmemcpy(save, (void *)0x70000, 4096);
    kmemcpy((u8 *)save + 4096, (void *)0x7f000, 4096);
    hp_run(hp_vme_body);
    kmemcpy((void *)0x70000, save, 4096);
    kmemcpy((void *)0x7f000, (u8 *)save + 4096, 4096);
    wr_cr4(hp_old_cr4);
    t_note("VME16 iterations=%u virtual_IRET=1 VIP_STI_POPF_IRET_cases=%u",
           1024u * HEAVY_SCALE, hp_case * 3);
    return csum_mix(1024u * HEAVY_SCALE, hp_case);
}

static volatile u32 hp_shadow_step, hp_shadow_count, hp_shadow_eip, hp_shadow_bad;
static u32 hp_shadow_want;
extern void hp_shadow_sti(void), hp_shadow_iret(void), hp_shadow_movss(void), hp_shadow_popss(void);
extern const u8 hp_after_sti[], hp_after_iret[], hp_after_movss[], hp_after_popss[];

__asm__(
    ".text\n"
    ".global hp_shadow_sti,hp_shadow_iret,hp_shadow_movss,hp_shadow_popss\n"
    ".global hp_after_sti,hp_after_iret,hp_after_movss,hp_after_popss\n"
    "hp_shadow_sti:\n"
    "sti\n movl $1,hp_shadow_step\n hp_after_sti: cli\n ret\n"
    "hp_shadow_iret:\n"
    "pushl $0x202\n pushl $8\n pushl $hp_after_iret\n iretl\n"
    "hp_after_iret: movl $1,hp_shadow_step\n cli\n ret\n"
    "hp_shadow_movss:\n"
    "movw %ss,%ax\n pushfl\n orl $0x100,(%esp)\n popfl\n"
    "movw %ax,%ss\n movl $1,hp_shadow_step\n hp_after_movss: nop\n ret\n"
    "hp_shadow_popss:\n"
    "push %ss\n pushfl\n orl $0x100,(%esp)\n popfl\n"
    "pop %ss\n movl $1,hp_shadow_step\n hp_after_popss: nop\n ret\n");

static void hp_db_hook(regs_t *r) {
    hp_shadow_count++;
    hp_shadow_eip = r->eip;
    if (r->eip != hp_shadow_want || hp_shadow_step != 1) hp_shadow_bad++;
    r->eflags &= ~0x100u;
}

static void hp_irq_hook(regs_t *r) {
    hp_shadow_count++;
    hp_shadow_eip = r->eip;
    if (r->eip != hp_shadow_want ||
        hp_shadow_step != (hp_shadow_want == (u32)hp_after_sti ? 1u : 0u))
        hp_shadow_bad++;
    r->eflags &= ~0x200u;
}

static int hp_pending_irq0(void) {
    /*
     * OCW3=0a selects the PIC's read-only IRR; it never requests/pulses an IRQ
     * or changes masks/EOI state. Poll count, not stopped IF=0 PIT ticks, bounds
     * the wait. No PIT reprogramming, PIC masking or synthetic device IRQs.
     */
    outb(0x20, 0x0a);
    for (u32 i = 0; i < 10000000; i++)
        if (inb(0x20) & 1) {
            /* Let an asynchronous virtual PIC propagate INTR before STI/IRET. */
            for (u32 settle = 0; settle < 128; settle++) (void)inb(0x20);
            return 1;
        }
    return 0;
}

static void hp_shadow_body(void) {
    cli();
    int_hook[1] = hp_db_hook;
    hp_shadow_bad = hp_shadow_count = 0;
    for (hp_case = 0; hp_case < 1024u * HEAVY_SCALE; hp_case++) {
        hp_shadow_step = 0;
        hp_shadow_want = (u32)hp_after_movss;
        hp_shadow_movss();
        hp_shadow_step = 0;
        hp_shadow_want = (u32)hp_after_popss;
        hp_shadow_popss();
        if (hp_shadow_bad) break;
        t_ops(2);
    }
    CHECK_EQ(hp_shadow_bad, 0);
    CHECK_EQ(hp_shadow_count, 2048u * HEAVY_SCALE);
    t_note("MOV_SS_POP_SS_pending_TF_traps=%u first_bad_eip=%08x", hp_shadow_count,
           hp_shadow_bad ? hp_shadow_eip : 0);
    int_hook[1] = hp_saved_db;
    int_hook[0x20] = hp_irq_hook;
    hp_shadow_count = hp_shadow_bad = 0;
    for (hp_case = 0; hp_case < 4u * HEAVY_SCALE; hp_case++) {
        for (u32 kind = 0; kind < 2; kind++) {
            hp_shadow_step = 0;
            hp_shadow_want = kind ? (u32)hp_after_iret : (u32)hp_after_sti;
            u32 before = hp_shadow_count;
            if (!hp_pending_irq0()) {
                t_note("IRQ0 pending poll exhausted case=%u kind=%u", hp_case, kind);
                CHECK(0);
                return;
            }
            if (kind) hp_shadow_iret(); else hp_shadow_sti();
            if (hp_shadow_count != before + 1 || hp_shadow_bad) {
                t_note("pending IRQ case=%u kind=%u eip=%08x want=%08x step=%u count=%u",
                       hp_case, kind, hp_shadow_eip, hp_shadow_want,
                       hp_shadow_step, hp_shadow_count - before);
                CHECK_EQ(hp_shadow_count, before + 1);
                CHECK_EQ(hp_shadow_bad, 0);
                return;
            }
            t_ops(1);
        }
    }
    t_note("real_pending_IRQ0_samples=%u STI_delays_one=1 IRET_delays_zero=1",
           hp_shadow_count);
}

static u32 hp_shadows(void) {
    u32 dr6;
    __asm__ volatile("mov %%dr6,%0" : "=r"(dr6));
    hp_run(hp_shadow_body);
    __asm__ volatile("mov %0,%%dr6" :: "r"(dr6) : "memory");
    return csum_mix(hp_shadow_count, hp_shadow_bad);
}

static u32 hp_context_far[2] __attribute__((used)) = {0, 0x40};

static void hp_context_body(void) {
    /* Same bytes and selector: B8 consumes either imm16 or imm32.
     * Both paths use LRET32 to match the caller's 32-bit far-call frame. */
    static const u8 code[] = {0xb8, 0x34, 0x12, 0xeb, 0x02, 0xcb, 0x90, 0x66, 0xcb};
    hp_sum = 0;
    hp_mem = arena_alloc(64, 16);
    if (!hp_mem) { t_skip("arena too small"); return; }
    kmemcpy(hp_mem, code, sizeof code);
    hp_sum = 0;
    for (hp_case = 0; hp_case < 128u * HEAVY_SCALE; hp_case++) {
        u32 wide = hp_case & 1;
        gdt_set(8, (u32)hp_mem, 63, 0x9b, wide ? 4 : 0);
        for (u32 reuse = 0; reuse < 32; reuse++) {
            u32 got = 0xa5a50000u;
            __asm__ volatile("lcall *hp_context_far" : "+a"(got) :: "cc", "memory");
            u32 want = wide ? 0x02eb1234u : 0xa5a51234u;
            if (got != want) {
                t_note("case=%u reuse=%u cs_default=%u bytes=b83412eb02cb9066cb",
                       hp_case, reuse, wide ? 32 : 16);
                CHECK_EQ(got, want); return;
            }
            hp_sum = csum_mix(hp_sum, got);
        }
    }
    t_ops(128u * HEAVY_SCALE * 32u);
}

static u32 hp_context(void) {
    hp_run(hp_context_body);
    return hp_sum;
}

const test_t tests_heavy_pmode[] = {
    { "heavy_pm_code_context", hp_context, REQ_486 },
    { "heavy_pm_16bit", hp_16bit, REQ_486 },
    { "heavy_pm_ldt", hp_ldt_test, REQ_486 },
    { "heavy_pm_expand_down", hp_expand, REQ_486 },
    { "heavy_pm_call_gates", hp_callgates, REQ_486 },
    { "heavy_pm_privilege", hp_privilege, REQ_486 },
    { "heavy_pm_pvi", hp_pvi, REQ_VME },
    { "heavy_pm_vme", hp_vme, REQ_VME },
    { "heavy_pm_irq_shadows", hp_shadows, REQ_486 },
    { 0, 0, 0 }
};
