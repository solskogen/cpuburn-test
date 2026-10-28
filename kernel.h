/* cpuburn-test: shared kernel header. Freestanding 32-bit protected-mode, flat, ring 0. */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdarg.h>

#ifndef BENCHMARKS
#define BENCHMARKS 1
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
#define NULL ((void *)0)

/* ---- GDT selectors (fixed by core) ---- */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x1B /* RPL3 */
#define SEL_UDATA 0x23 /* RPL3 */
#define SEL_TSS_MAIN 0x28
#define SEL_TSS_B 0x30
#define SEL_TSS_DF 0x38
/* GDT index 8..23 (selectors 0x40..0xB8) are free scratch for tests: use gdt_set() */
#define GDT_SCRATCH_FIRST 8
#define GDT_ENTRIES 24

/* ---- Test registry ---- */
/* Unavailable requirements auto-SKIP unless required by the CPU contract. */
#define REQ_CPUID (1u << 0)
#define REQ_TSC (1u << 1)
#define REQ_FPU (1u << 2)
#define REQ_MMX (1u << 3)
#define REQ_CMOV (1u << 4)
#define REQ_FXSR (1u << 5)
#define REQ_PSE (1u << 6)
#define REQ_PAE (1u << 7)
#define REQ_PGE (1u << 8)
#define REQ_SEP (1u << 9)
#define REQ_MSR (1u << 10)
#define REQ_CX8 (1u << 11)
#define REQ_486 (1u << 12) /* 486 instruction set, including 386-bus hybrids */
#define REQ_P5 (1u << 13)  /* family >= 5 */
#define REQ_P6 (1u << 14)  /* family >= 6 */
#define REQ_NOHV (1u << 15) /* skip under hypervisors (trapped/unreliable) */
#define REQ_PAGING (1u << 16) /* needs paging_enable() to have happened */
#define REQ_PSE36 (1u << 17)
#define REQ_PAT (1u << 18)
#define REQ_MTRR (1u << 19)
#define REQ_APIC (1u << 20)
#define REQ_MEM8M (1u << 21) /* owned arena covers physical 0x400000..0x7FFFFF */
#define REQ_PCI (1u << 22)   /* PCI config mechanism 1 present */
#define REQ_MCE (1u << 23)
#define REQ_VME (1u << 24)
#define REQ_DE (1u << 25)
#define REQ_3DNOW (1u << 26)
#define REQ_3DNOWEXT (1u << 27)
#define REQ_EMMI (1u << 28)
#define REQ_SYSCALL (1u << 29)
#define REQ_MMXEXT (1u << 30)
#define REQ_AC (1u << 31)

typedef struct test {
    const char *name;     /* short snake_case id, e.g. "int_alu" */
    u32 (*fn)(void);      /* returns informational checksum; use CHECK()/t_fail()/t_skip() */
    u32 req;              /* REQ_* flags */
} test_t;

/* Each module exports a NULL-terminated array; kmain owns the execution order.
 * Integer/corpus/FPU/protected-mode/chipset modules run with paging OFF.
 * Paging/memory/sweeps/system/state/burn run with paging ON (identity 0..16MiB, U/S=1). */
extern const test_t tests_int[], tests_fpu[], tests_mmx[], tests_pmode[], tests_chipset[];
extern const test_t tests_paging[], tests_mem[], tests_sys[], tests_burn[];
extern const test_t tests_heavy_paging[], tests_heavy_pmode[], tests_heavy_state[];
extern const test_t tests_corpus[];
extern const test_t tests_fp_edges[], tests_sweeps[];
extern const test_t tests_priority[];
extern const test_t tests_restart_complex[];
extern const test_t tests_vendor[];
extern const test_t tests_cpu_contract[];
extern const test_t tests_cyrix[];
void cpu_control(int serial_available);
void cpu_contract_init(void);
u32 cpu_required_features(void);
u32 cpu_raw_required(u32 word);
int cpu_contract_known(void);
const char *cpu_model_name(void);
u32 cpu_extended_standard_edx(void);
u32 cpu_detected_features(void);
void cpu_feature_result(const test_t *test, const char *status);
void cpu_feature_report(void);
int cpu_accuracy_mode(void);
extern u32 cpu_workload_features;
static inline int cpu_test_feature(u32 bit) {
    return (cpu_workload_features & bit) == bit;
}

/* ---- Test result helpers (valid only inside a running test) ---- */
void t_fail(const char *what, int line);
void t_failv(const char *what, int line, u32 got, u32 want);
void t_skip(const char *reason);
void t_note(const char *fmt, ...); /* prints "NOTE <testname> ..." line (data for the AI reader) */
void t_ops(u64 n);                 /* add n "operations" for ops/sec reporting */
void t_work_unit(const char *unit); /* static token naming the counted work, not the timer */
#define CHECK(c) do { if (!(c)) t_fail(#c, __LINE__); } while (0)
#define CHECK_EQ(a, b) do { u32 _a = (u32)(a), _b = (u32)(b); if (_a != _b) t_failv(#a " == " #b, __LINE__, _a, _b); } while (0)
#define SKIP(msg) do { t_skip(msg); return 0; } while (0)
/* Mix a value into a running checksum */
static inline u32 csum_mix(u32 s, u32 v) { s ^= v; s = (s << 5 | s >> 27) * 0x9E3779B1u; return s; }

/* ---- Output ---- */
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...); /* %s %c %d %u %x %X %p %02x %08x %llu %lld %llx, width/0-pad */
void post(u8 code);                 /* port 0x80 checkpoint */

/* ---- CPU info (filled by cpu_detect() in core.c) ---- */
typedef struct cpuinfo {
    int has_cpuid, is386, is486;
    u32 family, model, stepping, max_leaf, ext_max;
    char vendor[13];
    u32 feat_edx, feat_ecx, ext_edx;
    int hv;            /* hypervisor present bit (CPUID.1:ECX[31]) */
    char hv_vendor[13];
    int has_fpu;       /* probed with FNINIT/FNSTSW */
    int has_tsc, has_msr, has_cx8, has_cmov, has_mmx, has_fxsr, has_pse, has_pae, has_pge, has_sep,
        has_pat, has_mtrr, has_apic, has_pse36, has_mce, has_vme, has_de;
    int has_3dnow, has_3dnowext, has_emmi, has_syscall, has_mmxext;
    u32 probed_features, claimed_features;
    u32 mhz;           /* calibrated via PIT, 0 if unknown */
    u64 tsc_per_ms;
} cpuinfo_t;
extern cpuinfo_t cpu;
extern int paging_on;

/* ---- Memory ---- */
extern u32 arena_base, arena_size; /* usable, test-owned RAM above 1 MiB (physical == virtual) */
extern u32 arena_end;               /* end of the arena; the 64 KiB above it is usable RAM nothing else touches */
extern u32 ram_top;                /* highest usable address from E820/INT15 (bytes, capped 4 GiB-1) */
void *arena_alloc(u32 size, u32 align); /* bump allocator; reset automatically before every test */
extern u32 *pg_dir;                /* persistent page directory (identity 0..16 MiB, 4K pages, P|RW|US) */
void paging_enable(void);
void paging_disable(void);
void kmemset(void *d, int c, u32 n);
void kmemcpy(void *d, const void *s, u32 n);
int kmemcmp(const void *a, const void *b, u32 n);

/* ---- Time ---- */
extern volatile u32 pit_ticks;     /* 100 Hz, driven by IRQ0 (needs IF=1) */
u64 stamp(void);                   /* TSC if available else PIT-derived 1.193 MHz count */
extern int stamp_is_tsc;
void delay_ms(u32 ms);             /* needs IF=1 */
static inline u32 ms_now(void) { return pit_ticks * 10; }

/* ---- Exceptions / guard ---- */
typedef struct regs { /* pushed by isr stubs; iret frame follows */
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 vec, err;
    u32 eip, cs, eflags; /* + useresp, ss when coming from ring3; + es,ds,fs,gs also from V86 */
    u32 useresp, ss;
} regs_t;

typedef struct exc_rec {
    volatile u32 count; /* total exceptions seen since reset */
    volatile u32 vec, err, eip, cs, eflags, cr2;
} exc_rec_t;
extern exc_rec_t g_exc;

/* guard_call result codes */
#define GUARD_OK 0
#define GUARD_EXC(v) (0x100u | (u32)(v)) /* exception/INT vector v unexpectedly raised */
#define GUARD_WATCHDOG 0x200u
#define GUARD_R3_INT80 0x180u /* ring3_call: code exited via INT 0x80 (== GUARD_EXC(0x80)) */

int guard_call(void (*fn)(void));  /* run fn; any unexpected exception or watchdog timeout returns an error code
                                      (stack/eflags restored, nesting supported). Handlers run on same stack. */
void guard_recover(u32 code) __attribute__((noreturn)); /* abort innermost guard_call returning 'code' */
/* Expect a specific exception (fault) and resume at eip+skip_len instead of aborting.
   Cleared by exc_expect_clear(). g_exc.count increments each time it triggers. */
void exc_expect(int vec, u32 skip_len);
void exc_expect_clear(void);
/* Generic hook: if non-NULL, called for the vector (exceptions, IRQs 0x21-0x2F, soft INTs); then iret resumes.
   For IRQs the core sends EOI afterwards. Hook may modify *r. */
typedef void (*isr_hook_t)(regs_t *r);
extern isr_hook_t int_hook[256];
void idt_set_gate(int vec, void (*handler)(void), u16 sel, u8 type_attr); /* type_attr e.g. 0x8E int gate DPL0, 0xEE DPL3 */
extern void (*const isr_stub[256])(void); /* core default stub per vector */
void gdt_set(int idx, u32 base, u32 limit, u8 access, u8 flags); /* flags high nibble: G=0x8,D=0x4 ; idx >= GDT_SCRATCH_FIRST */
void gdt_set_raw(int idx, u32 lo, u32 hi);
u64 gdt_get(int idx);
extern u64 gdt[GDT_ENTRIES];
extern u8 idt_ptr_dummy;

/* Ring-3 helper: iret to flat ring3 (CS=SEL_UCODE, SS/DS/ES/FS/GS=SEL_UDATA, EFLAGS=0x2 i.e. IF=0, IOPL=0)
   at 'entry' with 'stack_top'. Ring-3 code returns by INT 0x80 (DPL3 gate; returns GUARD_R3_INT80)
   or SYSENTER/exception. Wrapped in guard_call. Needs user-accessible pages if paging is on. */
int ring3_call(void (*entry)(void), u32 stack_top);

/* TSS (32-bit) */
typedef struct tss32 {
    u32 link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs, ldt;
    u16 trap, iomap;
} tss32_t;
extern tss32_t tss_main, tss_b;
extern u8 exc_stack[16384]; /* ring0 stack used by TSS esp0 (top = exc_stack+16384) */

/* ---- Raw CPU access ---- */
/* Firmware-owned memory is defined at its physical address by link.ld. */
extern volatile u8 bios_data_area[256];
static inline u16 bda_read16(u32 offset) {
    u16 lo = bios_data_area[offset];
    return lo | ((u16)bios_data_area[offset + 1] << 8);
}
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p)); }
static inline void io_delay(void) { outb(0x80, 0); }
static inline u32 rd_cr0(void) { u32 v; __asm__ volatile("mov %%cr0,%0" : "=r"(v)); return v; }
static inline void wr_cr0(u32 v) { __asm__ volatile("mov %0,%%cr0" ::"r"(v) : "memory"); }
static inline u32 rd_cr2(void) { u32 v; __asm__ volatile("mov %%cr2,%0" : "=r"(v)); return v; }
static inline u32 rd_cr3(void) { u32 v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static inline void wr_cr3(u32 v) { __asm__ volatile("mov %0,%%cr3" ::"r"(v) : "memory"); }
static inline u32 rd_cr4(void) { u32 v; __asm__ volatile("mov %%cr4,%0" : "=r"(v)); return v; }
static inline void wr_cr4(u32 v) { __asm__ volatile("mov %0,%%cr4" ::"r"(v) : "memory"); }
static inline void invlpg(void *a) { __asm__ volatile("invlpg (%0)" ::"r"(a) : "memory"); }
static inline u32 rd_eflags(void) { u32 v; __asm__ volatile("pushfl; popl %0" : "=r"(v)); return v; }
static inline void wr_eflags(u32 v) { __asm__ volatile("pushl %0; popfl" ::"r"(v) : "memory", "cc"); }
static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "0"(leaf), "2"(0));
}
static inline u64 rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
static inline u64 rdmsr(u32 m) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((u64)hi << 32) | lo; }
static inline void wrmsr(u32 m, u64 v) { __asm__ volatile("wrmsr" ::"c"(m), "a"((u32)v), "d"((u32)(v >> 32))); }

/* ---- PCI config mechanism 1 (read-only helpers) ---- */
extern int pci_present;
u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 reg);

/* ---- Burn configuration (see Makefile: -DROUNDS=, -DBURN_SECONDS=) ---- */
#ifndef ROUNDS
#define ROUNDS 2
#endif
#ifndef BURN_SECONDS
#define BURN_SECONDS 20
#endif
#ifndef WATCHDOG_SECONDS
#define WATCHDOG_SECONDS 90 /* per-invocation guest-clock (PIT) watchdog */
#endif
#ifndef HEAVY_SCALE
#define HEAVY_SCALE 8
#endif
#ifndef TEST_SEED
#define TEST_SEED 0x86B04A21u
#endif
#ifndef TEST_SECONDS
#define TEST_SECONDS 30
#endif
#if TEST_SECONDS < 1 || TEST_SECONDS > 3600
#error TEST_SECONDS must be between 1 and 3600
#endif
#if HEAVY_SCALE < 1 || HEAVY_SCALE > 32
#error HEAVY_SCALE must be between 1 and 32
#endif
#if TEST_SEED == 0 || TEST_SEED > 0xffffffffu
#error TEST_SEED must be a nonzero 32-bit integer
#endif
#if ROUNDS < 1 || ROUNDS > 32
#error ROUNDS must be between 1 and 32
#endif
extern u32 g_round; /* current round, 1-based */

#endif
