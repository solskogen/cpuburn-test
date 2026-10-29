#include "kernel.h"

typedef struct {
    const char *name, *vendor;
    u32 family;
    u32 model_mask;
    u32 required;
} cpu_model_t;

#define CPU_MODEL(name, vendor, family, model_mask, required) { name, vendor, family, model_mask, required },
static const cpu_model_t models[] = {
#include "cpu_models.h"
};
#undef CPU_MODEL

static const cpu_model_t *expected;
static char configured_model[48], configured_fpu[16];
static int configured, accuracy;
static u32 detected_required;
static u32 configured_signature;
u32 cpu_workload_features;
static u64 apic_base;
static void read_apic_base(void) { apic_base = rdmsr(0x1b); }

static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int cpu_accuracy_mode(void) { return !BENCHMARKS || accuracy; }
int cpu_contract_known(void) { return expected != NULL; }
const char *cpu_model_name(void) { return expected ? expected->name : "unknown"; }
u32 cpu_extended_standard_edx(void) {
    /* Cyrix extended leaf bit 16 is FCMOV, bit 24 is EMMI, not PAT/FXSR. */
    return !kmemcmp(cpu.vendor, "CyrixInstead", 12)
               ? cpu.ext_edx & ~((1u << 16) | (1u << 24))
               : cpu.ext_edx;
}
u32 cpu_raw_required(u32 word) {
    if (word == 2 && expected && equal(expected->name, "mediagxm"))
        return 0x01808131u;
    if (!word && expected && expected->family == 6 &&
        equal(expected->vendor, "GenuineIntel")) return 1u << 14;
    return 0;
}
u32 cpu_required_features(void) {
    u32 required = configured ? (expected ? expected->required : 0) : detected_required;
    if (expected && expected->family >= 5) required |= REQ_P5;
    if (expected && expected->family >= 6) required |= REQ_P6;
    if (configured && !equal(configured_fpu, "none") &&
        !equal(configured_fpu, "auto")) required |= REQ_FPU;
    if (expected && equal(expected->name, "k6_2")) {
        u32 stepping = configured_signature ? configured_signature & 15 : cpu.stepping;
        if (stepping >= 8) required |= REQ_PGE;
    }
    return required;
}

/* A bounded, optional startup exchange. Absence of a controller preserves
 * standalone boot; malformed input must not silently become autodetection. */
void cpu_control(int serial_available) {
    char frame[96];
    u32 length = 0, deadline = ms_now() + 2000;
    int overflow = 0;
    if (!serial_available) return;
    kputs("CONTROL protocol=cpu1 ready=1\n");
    while ((s32)(ms_now() - deadline) < 0) {
        /* RX interrupts are disabled; HLT would allow a frame to overrun the
         * UART FIFO between the 100 Hz timer interrupts. */
        if (!(inb(0x3FD) & 1)) continue;
        char c = inb(0x3F8);
        if (c == '\r') continue;
        if (c != '\n') {
            if (length + 1 < sizeof frame) frame[length++] = c;
            else overflow = 1;
            continue;
        }
        frame[length] = 0;
        char *model = frame, *fpu, *mode;
        if (length < 6 || kmemcmp(model, "@cpu1 ", 6)) goto invalid;
        model += 6;
        fpu = model;
        while (*fpu && *fpu != ' ') fpu++;
        if (!*fpu) goto invalid;
        *fpu++ = 0;
        mode = fpu;
        while (*mode && *mode != ' ') mode++;
        if (!*mode) goto invalid;
        *mode++ = 0;
        char *signature = mode;
        while (*signature && *signature != ' ') signature++;
        if (*signature) {
            *signature++ = 0;
            u32 value = 0, n = 0;
            while (signature[n]) {
                u32 digit = (u8)signature[n++];
                if (digit >= '0' && digit <= '9') digit -= '0';
                else if (digit >= 'a' && digit <= 'f') digit -= 'a' - 10;
                else goto invalid;
                if (n > 8) goto invalid;
                value = (value << 4) | digit;
            }
            if (n != 8) goto invalid;
            configured_signature = value;
        }
        u32 nm = 0, nf = 0;
        while (model[nm]) nm++;
        while (fpu[nf]) nf++;
        if (overflow || !nm || nm >= sizeof configured_model ||
            !nf || nf >= sizeof configured_fpu ||
            (!equal(mode, "accuracy") && !equal(mode, "benchmark"))) goto invalid;
        kmemcpy(configured_model, model, nm + 1);
        kmemcpy(configured_fpu, fpu, nf + 1);
        configured = 1;
        accuracy = equal(mode, "accuracy");
        kprintf("CONTROL protocol=cpu1 accepted=1 model=%s fpu=%s mode=%s signature=%08x\n",
                configured_model, configured_fpu,
                cpu_accuracy_mode() ? "accuracy" : "benchmark", configured_signature);
        return;
invalid:
        kputs("FATAL invalid CPU control frame\nDONE\n");
        for (;;) { cli(); hlt(); }
    }
    if (length || overflow) {
        kputs("FATAL incomplete CPU control frame\nDONE\n");
        for (;;) { cli(); hlt(); }
    }
}

void cpu_contract_init(void) {
    int matches = 0;
    for (u32 i = 0; i < sizeof models / sizeof models[0]; i++) {
        const cpu_model_t *m = &models[i];
        if (configured) {
            if (equal(configured_model, m->name)) { expected = m; break; }
        } else if (cpu.has_cpuid && cpu.family == m->family && cpu.model < 32 &&
                   (m->model_mask & (1u << cpu.model)) &&
                   !kmemcmp(cpu.vendor, m->vendor, 12)) {
            if (!matches++) { expected = m; detected_required = m->required; }
            else detected_required &= m->required;
        }
    }
    kprintf("CONFORMANCE model=%s source=%s catalogue=%s coverage=partial required=%08x mode=%s\n",
            configured ? configured_model : matches > 1 ? "ambiguous" : expected ? expected->name : "unknown",
            configured ? "configured" : "detected",
            expected ? "known" : "unknown", cpu_required_features(),
            cpu_accuracy_mode() ? "accuracy" : "benchmark");
}

static u32 cpu_identity(void) {
    if (!expected) SKIP("model_contract_unavailable");
    if (expected->required & REQ_AC) CHECK_EQ(cpu.is486, 1);
    if (expected->required & REQ_CPUID) CHECK_EQ(cpu.has_cpuid, 1);
    if (cpu.has_cpuid) {
        if (*expected->vendor) CHECK_EQ(kmemcmp(cpu.vendor, expected->vendor, 12), 0);
        CHECK_EQ(cpu.family, expected->family);
        if (expected->model_mask)
            CHECK(cpu.model < 32 && (expected->model_mask & (1u << cpu.model)));
        if (configured_signature) {
            u32 a, b, c, d;
            cpuid(1, &a, &b, &c, &d);
            CHECK_EQ(a, configured_signature);
        }
    }
    if (configured && !equal(configured_fpu, "auto"))
        CHECK_EQ(cpu.has_fpu, !equal(configured_fpu, "none"));
    if (expected->required & REQ_FPU) CHECK_EQ(cpu.has_fpu, 1);
    if ((expected->required & REQ_FPU) && cpu.has_cpuid)
        CHECK_EQ(cpu.feat_edx & 1, 1);
    u32 raw_required = cpu_raw_required(0);
    CHECK_EQ(cpu.feat_edx & raw_required, raw_required);
    raw_required = cpu_raw_required(2);
    CHECK_EQ(cpu.ext_edx & raw_required, raw_required);
    u32 required = cpu_required_features();
    if (!cpu.has_cpuid && !(required & REQ_CPUID))
        required &= REQ_FPU | REQ_486 | REQ_AC;
    /* WinChip always executes CMPXCHG8B, but masks its CPUID bit by default
     * for Windows NT compatibility (WinChip 2 datasheet, chapter 3 note 6). */
    if (!kmemcmp(cpu.vendor, "CentaurHauls", 12) && cpu.family == 5)
        required &= ~REQ_CX8;
    /* Clearing IA32_APIC_BASE.EN also clears Intel's CPUID APIC flag.
     * Disabled APIC workloads remain SKIP/UNTESTED, not verified support. */
    if ((required & REQ_APIC) && !cpu.has_apic &&
        !kmemcmp(cpu.vendor, "GenuineIntel", 12) &&
        !guard_call(read_apic_base) && !(apic_base & 0x800))
        required &= ~REQ_APIC;
    const struct { u32 bit; int available; const char *check; } features[] = {
        {REQ_TSC, cpu.has_tsc, "required_tsc"}, {REQ_MSR, cpu.has_msr, "required_msr"},
        {REQ_CX8, cpu.has_cx8, "required_cx8"}, {REQ_MMX, cpu.has_mmx, "required_mmx"},
        {REQ_CMOV, cpu.has_cmov, "required_cmov"}, {REQ_PAE, cpu.has_pae, "required_pae"},
        {REQ_PSE, cpu.has_pse, "required_pse"}, {REQ_PGE, cpu.has_pge, "required_pge"},
        {REQ_VME, cpu.has_vme, "required_vme"}, {REQ_DE, cpu.has_de, "required_de"},
        {REQ_MCE, cpu.has_mce, "required_mce"}, {REQ_APIC, cpu.has_apic, "required_apic"},
        {REQ_MTRR, cpu.has_mtrr, "required_mtrr"}, {REQ_SEP, cpu.has_sep, "required_sep"},
        {REQ_PAT, cpu.has_pat, "required_pat"}, {REQ_PSE36, cpu.has_pse36, "required_pse36"},
        {REQ_FXSR, cpu.has_fxsr, "required_fxsr"}, {REQ_3DNOW, cpu.has_3dnow, "required_3dnow"},
        {REQ_3DNOWEXT, cpu.has_3dnowext, "required_3dnowext"},
        {REQ_EMMI, cpu.has_emmi, "required_emmi"},
        {REQ_SYSCALL, cpu.has_syscall, "required_syscall"},
        {REQ_MMXEXT, cpu.has_mmxext, "required_mmxext"},
    };
    for (u32 i = 0; i < sizeof features / sizeof features[0]; i++) {
        if (!(required & features[i].bit)) continue;
        if (!features[i].available) {
            t_note("missing_required=%08x", features[i].bit);
            t_failv(features[i].check, __LINE__, 0, 1);
        }
    }
    t_ops(1);
    return required;
}

const test_t tests_cpu_contract[] = {
    { "cpu_model_contract", cpu_identity, 0 },
    { NULL, NULL, 0 }
};

u32 cpu_detected_features(void) {
    u32 mask = 0;
#define FEATURE(bit, value) if (value) mask |= bit
    FEATURE(REQ_CPUID, cpu.has_cpuid); FEATURE(REQ_486, cpu.is486);
    FEATURE(REQ_AC, cpu.is486);
    FEATURE(REQ_P5, cpu.family >= 5); FEATURE(REQ_P6, cpu.family >= 6);
    FEATURE(REQ_FPU, cpu.has_fpu); FEATURE(REQ_TSC, cpu.has_tsc);
    FEATURE(REQ_MSR, cpu.has_msr); FEATURE(REQ_CX8, cpu.has_cx8);
    FEATURE(REQ_MMX, cpu.has_mmx); FEATURE(REQ_CMOV, cpu.has_cmov);
    FEATURE(REQ_FXSR, cpu.has_fxsr); FEATURE(REQ_PSE, cpu.has_pse);
    FEATURE(REQ_PAE, cpu.has_pae); FEATURE(REQ_PGE, cpu.has_pge);
    FEATURE(REQ_SEP, cpu.has_sep); FEATURE(REQ_VME, cpu.has_vme);
    FEATURE(REQ_DE, cpu.has_de); FEATURE(REQ_MCE, cpu.has_mce);
    FEATURE(REQ_APIC, cpu.has_apic); FEATURE(REQ_MTRR, cpu.has_mtrr);
    FEATURE(REQ_PAT, cpu.has_pat); FEATURE(REQ_PSE36, cpu.has_pse36);
    FEATURE(REQ_3DNOW, cpu.has_3dnow); FEATURE(REQ_3DNOWEXT, cpu.has_3dnowext);
    FEATURE(REQ_EMMI, cpu.has_emmi);
    FEATURE(REQ_SYSCALL, cpu.has_syscall); FEATURE(REQ_MMXEXT, cpu.has_mmxext);
#undef FEATURE
    const struct { u32 bit, requirement; } extended[] = {
        {0, REQ_FPU}, {1, REQ_VME}, {2, REQ_DE}, {3, REQ_PSE},
        {4, REQ_TSC}, {5, REQ_MSR}, {6, REQ_PAE}, {7, REQ_MCE},
        {8, REQ_CX8}, {9, REQ_APIC}, {12, REQ_MTRR}, {13, REQ_PGE},
        {15, REQ_CMOV}, {16, REQ_PAT}, {17, REQ_PSE36},
        {23, REQ_MMX}, {24, REQ_FXSR},
    };
    for (u32 i = 0; i < sizeof extended / sizeof extended[0]; i++)
        if (cpu_extended_standard_edx() & (1u << extended[i].bit)) mask |= extended[i].requirement;
    return mask;
}

#define CPU_FEATURE(bit, name, witness) { bit, name, witness, -1, 0, 0, 0, 0, 0, 0, 0 },
#define CPU_CLAIM(word, bit, name, witness) { 0, name, witness, word, 1u << bit, 0, 0, 0, 0, 0, 0 },
static struct {
    u32 bit;
    const char *name, *witness;
    int word;
    u32 raw_bit;
    u32 pass, fail, fault, skip, verified, filtered;
} features[] = {
#include "cpu_features.h"
};
#undef CPU_FEATURE
#undef CPU_CLAIM

void cpu_feature_result(const test_t *test, const char *status) {
    if (equal(status, "SKIP") || equal(status, "FILTERED")) {
        u32 prerequisites = test->req & ~(REQ_NOHV | REQ_PAGING | REQ_MEM8M | REQ_PCI);
        if (prerequisites & ~cpu_workload_features) return;
        const u32 raw[] = { cpu.feat_edx | cpu_extended_standard_edx(), cpu.feat_ecx, cpu.ext_edx };
        for (u32 i = 0; i < sizeof features / sizeof features[0]; i++)
            if (features[i].word >= 0 && equal(test->name, features[i].witness) &&
                !((raw[features[i].word] | cpu_raw_required(features[i].word)) & features[i].raw_bit))
                return;
    }
    for (u32 i = 0; i < sizeof features / sizeof features[0]; i++) {
        int witness = equal(test->name, features[i].witness);
        if (!witness && !(test->req & features[i].bit)) continue;
        if (equal(status, "PASS")) {
            features[i].pass++;
            if (witness) features[i].verified = 1;
        }
        else if (equal(status, "FAIL")) features[i].fail++;
        else if (equal(status, "FAULT")) features[i].fault++;
        else if (equal(status, "FILTERED")) features[i].filtered++;
        else features[i].skip++;
    }
}

void cpu_feature_report(void) {
    u32 required = cpu_required_features(), detected = cpu.claimed_features;
    u32 raw[] = { cpu.feat_edx, cpu.feat_ecx, cpu.ext_edx };
    for (u32 i = 0; i < sizeof features / sizeof features[0]; i++) {
        const char *status;
        u32 r = !!(required & features[i].bit), d = !!(detected & features[i].bit);
        u32 p = !!(cpu.probed_features & features[i].bit);
        if (features[i].word >= 0) {
            r = !!(cpu_raw_required(features[i].word) & features[i].raw_bit);
            d = !!((raw[features[i].word] |
                    (features[i].word == 0 ? cpu_extended_standard_edx() : 0)) & features[i].raw_bit);
        }
        if (!(r | d | p)) status = "NA";
        else if (features[i].fail || features[i].fault) status = "FAIL";
        else if (!features[i].verified || features[i].skip || features[i].filtered) status = "UNTESTED";
        else status = "PASS";
        kprintf("FEATURE name=%s required=%u detected=%u probed=%u status=%s witness=%s pass=%u fail=%u fault=%u skip=%u filtered=%u scope=sampled\n",
                features[i].name, r, d, p,
                status, *features[i].witness ? features[i].witness : "none",
                features[i].pass, features[i].fail, features[i].fault, features[i].skip,
                features[i].filtered);
    }
    /* Unmapped CPUID claims are visible coverage gaps, never silently passed. */
    const u32 mapped_edx = 0x0183fbffu;
    u32 mapped_ext = (mapped_edx & ~(1u << 11)) | 0xc0400000u |
                    (cpu.has_syscall ? 0xc00u : 0);
    if (!kmemcmp(cpu.vendor, "CyrixInstead", 12)) {
        mapped_ext &= ~((1u << 16) | (1u << 24));
        if (cpu.has_emmi) mapped_ext |= 1u << 24;
    }
    u32 unknown[] = { cpu.feat_edx & ~mapped_edx, cpu.feat_ecx,
                     cpu.ext_edx & ~mapped_ext };
    for (u32 word = 0; word < 3; word++)
        for (u32 bit = 0; bit < 32; bit++)
            if (unknown[word] & (1u << bit))
                kprintf("FEATURE name=cpuid_%u_bit%u required=0 detected=1 status=UNTESTED witness=none scope=unmapped\n",
                        word, bit);
}
