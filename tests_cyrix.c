#include "kernel.h"

enum { CYRIX_SMM = 1, CYRIX_CPUID = 2, CYRIX_SHR = 4, CYRIX_GXM = 8 };
static __attribute__((noinline)) u32 cyrix_capabilities(void) {
    if (cpu.has_cpuid && kmemcmp(cpu.vendor, "CyrixInstead", 12)) return 0;
    static const struct { const char *name; u32 caps; } models[] = {
        {"cx486dx2", CYRIX_SMM}, {"cx486dx4", CYRIX_SMM}, {"cx5x86", CYRIX_SMM},
        {"cx6x86", CYRIX_SMM}, {"cx6x86l", CYRIX_SMM},
        {"cx6x86mx", CYRIX_SMM | CYRIX_CPUID | CYRIX_SHR},
        {"mii", CYRIX_SMM | CYRIX_CPUID | CYRIX_SHR},
        {"mediagx_mmx", CYRIX_SMM | CYRIX_CPUID | CYRIX_GXM}
    };
    for (u32 i = 0; i < sizeof models / sizeof models[0]; i++) {
        const char *model = cpu_model_name(), *name = models[i].name;
        while (*name && *name == *model) { name++; model++; }
        if (*name == *model) return models[i].caps;
    }
    return 0;
}

static u8 ccr_read(u8 index) { outb(0x22, index); return inb(0x23); }
static void ccr_write(u8 index, u8 value) { outb(0x22, index); outb(0x23, value); }

static u32 cpuid_result[4];
static void cyrix_cpuid_probe(void) {
    cpuid(0, &cpuid_result[0], &cpuid_result[1], &cpuid_result[2], &cpuid_result[3]);
}

static int id_writable(void) {
    u32 before = rd_eflags();
    wr_eflags(before ^ (1u << 21));
    u32 after = rd_eflags();
    wr_eflags(before);
    return !!((after ^ before) & (1u << 21));
}

static u32 cyrix_cpuid_control(void) {
    if (!(cyrix_capabilities() & CYRIX_CPUID))
        SKIP("no documented Cyrix CCR4 CPUID contract for this model");
    t_work_unit("cpuid_control_state");
    u32 flags = rd_eflags(), sum = 0;
    cli();
    u8 ccr3 = ccr_read(0xc3);
    ccr_write(0xc3, (ccr3 & 15) | 0x10);
    u8 ccr4 = ccr_read(0xe8);
    for (u32 enabled = 0; enabled < 2; enabled++) {
        ccr_write(0xe8, (ccr4 & 0x7f) | (enabled << 7));
        u8 readback = ccr_read(0xe8);
        int writable = id_writable();
        int rc = guard_call(cyrix_cpuid_probe);
        CHECK_EQ(readback & 0x80, enabled << 7);
        CHECK_EQ(writable, enabled);
        CHECK_EQ(rc, enabled ? 0 : GUARD_EXC(6));
        if (!rc && enabled) {
            CHECK_EQ(cpuid_result[1], 0x69727943u);
            CHECK_EQ(cpuid_result[3], 0x736e4978u);
            CHECK_EQ(cpuid_result[2], 0x64616574u);
            sum = csum_mix(sum, cpuid_result[0]);
        }
    }
    ccr_write(0xe8, ccr4);
    u8 restored4 = ccr_read(0xe8);
    ccr_write(0xc3, ccr3);
    u8 restored3 = ccr_read(0xc3);
    wr_eflags(flags);
    CHECK_EQ(restored4, ccr4);
    CHECK_EQ(restored3, ccr3);
    t_note("CCR4.CPUID disabled/enabled: readback, EFLAGS.ID, #UD and vendor; CCR3/4 restored");
    t_ops(2);
    return sum;
}

static u32 cyrix_smm_disabled(void) {
    u32 caps = cyrix_capabilities();
    if (!(caps & CYRIX_SMM)) SKIP("no documented Cyrix SMM configuration contract for this model");
    t_work_unit("smm_disabled_opcode_case");
    u8 *code = arena_alloc(16, 16), *operand = arena_alloc(32, 16);
    if (!code || !operand) SKIP("no owned Cyrix SMM probe storage");
    u32 flags = rd_eflags();
    cli();
    u8 ccr3 = ccr_read(0xc3), ccr1 = ccr_read(0xc1);
    if ((ccr3 & 1) && (ccr1 & 4)) {
        wr_eflags(flags);
        SKIP("SMI_LOCK prevents disabling SMAC safely outside SMM");
    }
    /* No SMAR/ARR changes and no SMM entry: SMAC=0 forbids these instructions. */
    ccr_write(0xc1, ccr1 & ~4u);
    u8 disabled = ccr_read(0xc1);
    CHECK_EQ(disabled & 4, 0);
    u32 sum = 0, count = 0;
    if (!(disabled & 4)) {
        const u8 opcodes[] = {0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0xaa};
        for (u32 i = 0; i < sizeof opcodes + 3; i++) {
            u8 op;
            if (i < sizeof opcodes) op = opcodes[i];
            else if (i == sizeof opcodes)
                op = caps & CYRIX_CPUID ? 0x38 : 0x7e;
            else {
                if (!(caps & CYRIX_SHR)) continue;
                op = i == sizeof opcodes + 1 ? 0x36 : 0x37;
            }
            kmemset(operand, 0xa5, 32);
            code[0] = 0xb8; /* MOV EAX, owned memory operand */
            u32 address = (u32)operand;
            kmemcpy(code + 1, &address, 4);
            code[5] = 0x0f; code[6] = op;
            u32 length = op == 0xaa || op == 0x38 || op == 0x7e ? 2 : 3;
            code[7] = 0; /* memory ModR/M: [EAX], ES for SVDC/RSDC */
            code[5 + length] = 0xc3;
            int rc = guard_call((void (*)(void))code);
            CHECK_EQ(rc, GUARD_EXC(6));
            if (rc == GUARD_EXC(6)) {
                CHECK_EQ(g_exc.eip, (u32)code + 5);
                CHECK_EQ(g_exc.vec, 6);
            }
            for (u32 j = 0; j < 32; j++) CHECK_EQ(operand[j], 0xa5);
            sum = csum_mix(sum, (u32)rc ^ op);
            count++;
        }
    }
    ccr_write(0xc1, ccr1);
    u8 restored = ccr_read(0xc1);
    wr_eflags(flags);
    CHECK_EQ(restored, ccr1);
    t_note("SMAC=0: %u SMM opcodes require precise #UD and unchanged operand; CCR1 restored; SMM entry untested",
           count);
    t_ops(count);
    return sum;
}

static u32 mediagx_cpuid(void) {
    if (!(cyrix_capabilities() & CYRIX_GXM)) SKIP("requires MediaGX MMX (GXm) contract");
    t_work_unit("cpuid_leaf_case");
    u32 a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    CHECK_EQ(a, 2);
    cpuid(1, &a, &b, &c, &d);
    CHECK_EQ(a & 0xff0, 0x540);
    CHECK_EQ(d & 0x0183fbffu, 0x00808131u);
    cpuid(2, &a, &b, &c, &d);
    CHECK_EQ(a & 0xff, 1);
    CHECK_EQ((a >> 8) & 0xff, 0x70);
    CHECK_EQ(d & 0xff, 0x80);
    cpuid(0x80000000u, &a, &b, &c, &d);
    CHECK_EQ(a, 0x80000005u);
    cpuid(0x80000001u, &a, &b, &c, &d);
    CHECK_EQ(a & 0xff0, 0x540);
    CHECK_EQ(d & 0x01808131u, 0x01808131u);
    cpuid(0x80000005u, &a, &b, &c, &d);
    CHECK_EQ(b & 0xffff, 0x7001);
    CHECK_EQ(c & 0xff, 0x80);
    t_note("GXm rev2.0 tables 9-17..25; reserved fields unasserted; extended bit24=EMMI, not FXSAVE");
    t_ops(6);
    return d;
}

const test_t tests_cyrix[] = {
    {"cyrix_cpuid_control", cyrix_cpuid_control, 0},
    {"cyrix_smm_disabled", cyrix_smm_disabled, 0},
    {"mediagx_cpuid", mediagx_cpuid, REQ_CPUID},
    {0, 0, 0}
};
