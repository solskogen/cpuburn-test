/* Core runtime: output, descriptors, interrupts, CPU detection, memory/paging, test harness, kmain. */
#include "kernel.h"

/* ============================ small libc ============================ */
void kmemset(void *d, int c, u32 n) { __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); }
void kmemcpy(void *d, const void *s, u32 n) { __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); }
int kmemcmp(const void *a, const void *b, u32 n) {
    const u8 *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
static u32 kstrlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static int kstreq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* ============================ output ============================ */
static int ser_ok, ser_fail;
static char note_buf[32768];
static u32 note_len;
static int note_capture, note_overflow;
static u16 *vga = (u16 *)0xB8000;
static int vga_x, vga_y;

void post(u8 code) { outb(0x80, code); }

static void ser_init(void) {
    outb(0x3F8 + 7, 0x5A);
    if (inb(0x3F8 + 7) != 0x5A) return;
    outb(0x3F8 + 7, 0xA5);
    if (inb(0x3F8 + 7) != 0xA5) return;
    if (inb(0x3F8 + 5) == 0xFF) return;
    outb(0x3F8 + 1, 0x00); /* no IRQs */
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01); /* 115200 */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x03);
    ser_ok = 1;
}

static void ser_putc(char c) {
    if (!ser_ok) return;
    int n = 20000;
    while (!(inb(0x3F8 + 5) & 0x20)) {
        if (--n == 0) { if (++ser_fail >= 3) ser_ok = 0; return; }
    }
    ser_fail = 0;
    outb(0x3F8, (u8)c);
}

static void vga_init(void) {
    u16 crtc = *(volatile u16 *)0x463;
    if (crtc == 0x3B4) vga = (u16 *)0xB0000;
    else vga = (u16 *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x0720;
}

static void vga_putc(char c) {
    if (c == '\r') { vga_x = 0; return; }
    if (c == '\n') { vga_x = 0; vga_y++; }
    else if (c == '\t') { vga_x = (vga_x + 8) & ~7; }
    else { vga[vga_y * 80 + vga_x] = 0x0700 | (u8)c; vga_x++; }
    if (vga_x >= 80) { vga_x = 0; vga_y++; }
    if (vga_y >= 25) {
        for (int i = 0; i < 80 * 24; i++) vga[i] = vga[i + 80];
        for (int i = 80 * 24; i < 80 * 25; i++) vga[i] = 0x0720;
        vga_y = 24;
    }
}

void kputc(char c) {
    if (note_capture) {
        if (note_len < sizeof note_buf - 1) note_buf[note_len++] = c;
        else note_overflow = 1;
        return;
    }
    outb(0xE9, (u8)c);
    if (c == '\n') ser_putc('\r');
    ser_putc(c);
    vga_putc(c);
}
void kputs(const char *s) { while (*s) kputc(*s++); }

static void put_num(u64 v, u32 base, int width, int zero, int neg, int upper) {
    char buf[24];
    int n = 0;
    const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) buf[n++] = '0';
    while (v) { buf[n++] = dg[v % base]; v /= base; }
    if (neg) { if (zero) { kputc('-'); width--; } else buf[n++] = '-'; }
    while (width > n) { kputc(zero ? '0' : ' '); width--; }
    while (n) kputc(buf[--n]);
}

static void kvprintf(const char *f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') { kputc(*f); continue; }
        f++;
        int zero = 0, width = 0, lng = 0;
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') { lng++; f++; }
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; int l = (int)kstrlen(s); while (width > l) { kputc(' '); width--; } kputs(s); break; }
        case 'c': kputc((char)va_arg(ap, int)); break;
        case 'd': { s64 v = lng >= 2 ? va_arg(ap, s64) : va_arg(ap, s32); int neg = v < 0; put_num(neg ? (u64)-v : (u64)v, 10, width, zero, neg, 0); break; }
        case 'u': put_num(lng >= 2 ? va_arg(ap, u64) : va_arg(ap, u32), 10, width, zero, 0, 0); break;
        case 'x': put_num(lng >= 2 ? va_arg(ap, u64) : va_arg(ap, u32), 16, width, zero, 0, 0); break;
        case 'X': put_num(lng >= 2 ? va_arg(ap, u64) : va_arg(ap, u32), 16, width, zero, 0, 1); break;
        case 'p': kputs("0x"); put_num(va_arg(ap, u32), 16, 8, 1, 0, 0); break;
        case '%': kputc('%'); break;
        default: kputc('%'); kputc(*f); break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

/* ============================ descriptors ============================ */
u64 gdt[GDT_ENTRIES] __attribute__((aligned(8)));
static u64 idt[256] __attribute__((aligned(8)));
tss32_t tss_main, tss_b, tss_df;
u8 exc_stack[16384] __attribute__((aligned(16)));
static u8 df_stack[4096] __attribute__((aligned(16)));
isr_hook_t int_hook[256];
exc_rec_t g_exc;
extern u32 g_guard;
extern void df_task_entry(void);
extern void ring3_tramp(void);
u32 g_r3_entry, g_r3_stack;
static volatile int g_expect_vec = -1;
static volatile u32 g_skip;
static volatile u32 g_wd_deadline;
static u32 g_cr0_base, g_cr4_base;
static int cr4_ok;
u32 g_round;

void gdt_set_raw(int idx, u32 lo, u32 hi) { gdt[idx] = ((u64)hi << 32) | lo; }
u64 gdt_get(int idx) { return gdt[idx]; }
void gdt_set(int idx, u32 base, u32 limit, u8 access, u8 flags) {
    u32 lo = (limit & 0xFFFF) | (base << 16);
    u32 hi = ((base >> 16) & 0xFF) | ((u32)access << 8) | (limit & 0xF0000) | ((u32)(flags & 0xF) << 20) | (base & 0xFF000000);
    gdt_set_raw(idx, lo, hi);
}

void idt_set_gate(int vec, void (*h)(void), u16 sel, u8 type_attr) {
    u32 o = (u32)h;
    idt[vec] = (o & 0xFFFF) | ((u64)sel << 16) | ((u64)type_attr << 40) | ((u64)(o >> 16) << 48);
}

static void idt_init(void) {
    for (int i = 0; i < 256; i++) idt_set_gate(i, isr_stub[i], SEL_KCODE, 0x8E);
    idt_set_gate(0x80, isr_stub[0x80], SEL_KCODE, 0xEE);
    idt[8] = ((u64)SEL_TSS_DF << 16) | ((u64)0x85 << 40); /* DF: task gate */
}

static void gdt_init(void) {
    kmemset(gdt, 0, sizeof gdt);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xC);
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xC);
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xC);
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xC);
    kmemset(&tss_main, 0, sizeof tss_main);
    tss_main.ss0 = SEL_KDATA;
    tss_main.esp0 = (u32)exc_stack + sizeof exc_stack;
    tss_main.iomap = sizeof tss_main;
    gdt_set(5, (u32)&tss_main, sizeof(tss32_t) - 1, 0x89, 0x0);
    gdt_set(6, (u32)&tss_b, sizeof(tss32_t) - 1, 0x89, 0x0);
    kmemset(&tss_df, 0, sizeof tss_df);
    tss_df.eip = (u32)df_task_entry;
    tss_df.esp = (u32)df_stack + sizeof df_stack;
    tss_df.cs = SEL_KCODE;
    tss_df.ss = tss_df.ds = tss_df.es = tss_df.fs = tss_df.gs = SEL_KDATA;
    tss_df.eflags = 2;
    tss_df.iomap = sizeof tss_df;
    gdt_set(7, (u32)&tss_df, sizeof(tss32_t) - 1, 0x89, 0x0);
}

static void load_tables(void) {
    struct { u16 lim; u32 base; } __attribute__((packed)) g = { sizeof gdt - 1, (u32)gdt }, i = { sizeof idt - 1, (u32)idt };
    __asm__ volatile("lgdt %0" ::"m"(g));
    __asm__ volatile("lidt %0" ::"m"(i));
    __asm__ volatile("ljmp %0, $1f\n1:\n"
                     "mov %1,%%ax\n mov %%ax,%%ds\n mov %%ax,%%es\n mov %%ax,%%fs\n mov %%ax,%%gs\n mov %%ax,%%ss\n" ::"i"(SEL_KCODE),
                     "i"(SEL_KDATA) : "eax", "memory");
}

static void ltr_main(void) {
    gdt[5] &= ~(1ull << 41); /* clear busy */
    __asm__ volatile("ltr %%ax" ::"a"((u16)SEL_TSS_MAIN));
}

/* ============================ PIC / PIT / time ============================ */
volatile u32 pit_ticks;
int stamp_is_tsc;
static volatile u32 spurious; /* diagnostic counter, inspectable from a debugger */
static volatile u32 lease_active_id;
static void poll_lease(u32 id);

static void pic_init(void) {
    outb(0x20, 0x11); io_delay(); outb(0xA0, 0x11); io_delay();
    outb(0x21, 0x20); io_delay(); outb(0xA1, 0x28); io_delay();
    outb(0x21, 0x04); io_delay(); outb(0xA1, 0x02); io_delay();
    outb(0x21, 0x01); io_delay(); outb(0xA1, 0x01); io_delay();
    outb(0x21, 0xFE); outb(0xA1, 0xFF); /* only IRQ0 */
    outb(0x43, 0x34);
    outb(0x40, 11932 & 0xFF);
    outb(0x40, 11932 >> 8);
}

static int timer_ready(void) {
    u8 saved = inb(0x61);
    if (saved == 0xFF) return 0;
    outb(0x61, saved & 0xFC);
    outb(0x43, 0xB0);
    outb(0x42, 0xFF);
    outb(0x42, 0xFF); /* 54.9 ms: independent of CPU / hypervisor spin speed */
    outb(0x61, (saved & 0xFC) | 1);
    for (u32 n = 0; n < 1000000 && !pit_ticks; n++)
        if (inb(0x61) & 0x20) break;
    outb(0x61, saved & ~2u);
    return pit_ticks != 0;
}

u64 stamp(void) {
    if (stamp_is_tsc) {
        u32 a, b, c, d;
        cpuid(0, &a, &b, &c, &d);
        u64 v = rdtsc();
        cpuid(0, &a, &b, &c, &d);
        __asm__ volatile("" ::: "memory");
        return v;
    }
    u32 t1, t2, c;
    do {
        t1 = pit_ticks;
        outb(0x43, 0);
        c = inb(0x40);
        c |= (u32)inb(0x40) << 8;
        t2 = pit_ticks;
    } while (t1 != t2);
    return (u64)t1 * 11932 + (11932 - c);
}

void delay_ms(u32 ms) {
    u32 target = ms_now() + ms + 10;
    while ((s32)(ms_now() - target) < 0) hlt();
}

/* ============================ exceptions ============================ */
void exc_expect(int vec, u32 skip) { g_expect_vec = vec; g_skip = skip; }
void exc_expect_clear(void) { g_expect_vec = -1; g_skip = 0; }

static void dump_regs(regs_t *r) {
    kprintf("  vec=%u err=%08x eip=%08x cs=%04x eflags=%08x cr2=%08x cr0=%08x\n", r->vec, r->err, r->eip, r->cs, r->eflags, rd_cr2(), rd_cr0());
    kprintf("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n", r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp);
}

static void fatal(regs_t *r) {
    kprintf("FATAL unexpected exception outside guard\n");
    dump_regs(r);
    for (;;) { cli(); hlt(); }
}

void df_handler_c(void) {
    kprintf("FATAL double fault (last exc vec=%u err=%08x eip=%08x cr2=%08x)\nDONE\n", g_exc.vec, g_exc.err, g_exc.eip, g_exc.cr2);
    for (;;) { cli(); hlt(); }
}

static void irq_dispatch(regs_t *r) {
    u32 irq = r->vec - 0x20;
    if (irq == 7 || irq == 15) {
        u16 p = irq == 7 ? 0x20 : 0xA0;
        outb(p, 0x0B);
        if (!(inb(p) & 0x80)) { spurious++; if (irq == 15) outb(0x20, 0x20); return; }
    }
    if (irq == 0) {
        pit_ticks++;
        if (lease_active_id) poll_lease(lease_active_id);
    }
    if (int_hook[r->vec]) int_hook[r->vec](r);
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    if (irq == 0 && g_wd_deadline && (s32)(pit_ticks - g_wd_deadline) >= 0 && g_guard) {
        g_wd_deadline = 0;
        guard_recover(GUARD_WATCHDOG);
    }
}

void c_isr(regs_t *r) {
    u32 v = r->vec;
    if (v >= 0x20 && v < 0x30) { irq_dispatch(r); return; }
    g_exc.count++;
    g_exc.vec = v; g_exc.err = r->err; g_exc.eip = r->eip; g_exc.cs = r->cs; g_exc.eflags = r->eflags;
    g_exc.cr2 = v == 14 ? rd_cr2() : g_exc.cr2;
    if (int_hook[v]) { int_hook[v](r); return; }
    if (g_expect_vec == (int)v) { r->eip += g_skip; return; }
    if (g_guard) guard_recover(GUARD_EXC(v));
    fatal(r);
}

int ring3_call(void (*entry)(void), u32 stack_top) {
    g_r3_entry = (u32)entry;
    g_r3_stack = stack_top;
    return guard_call(ring3_tramp);
}

/* ============================ memory / paging ============================ */
u32 arena_base, arena_size, ram_top;
static u32 arena_ptr;
u32 arena_end;
u32 *pg_dir;
int paging_on;
static int pg_built;
static u32 *bootinfo = (u32 *)0x600;

void *arena_alloc(u32 size, u32 align) {
    u32 p = (arena_ptr + align - 1) & ~(align - 1);
    if (p + size > arena_end || p + size < p) return NULL;
    arena_ptr = p + size;
    return (void *)p;
}

static void mem_init(void) {
    u32 top = 0;
    u32 best_base = 0, best_end = 0;
    u32 n = bootinfo[5];
    if (n > 32) n = 32;
    for (u32 i = 0; i < n; i++) {
        u32 *e = &bootinfo[8 + i * 6];
        if (e[4] != 1 || e[1] != 0 || !(e[5] & 1)) continue;
        u64 base = ((u64)e[1] << 32) | e[0], len = ((u64)e[3] << 32) | e[2];
        if (base + len < base) continue;
        u64 t = base + len;
        if (t > top) top = t > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)t;
        if (base < 0x110000) base = 0x110000;
        base = (base + 4095) & ~4095ull;
        if (t > 0x1000000) t = 0x1000000;
        t &= ~4095ull;
        /* Reserved records take precedence even if firmware overlaps ranges. */
        for (u32 j = 0; j < n; j++) {
            u32 *r = &bootinfo[8 + j * 6];
            if (r[4] == 1 || r[1]) continue;
            u64 rb = r[0], re = rb + (((u64)r[3] << 32) | r[2]);
            if (rb <= base && re > base) t = base;
            else if (rb > base && rb < t) t = rb & ~4095ull;
        }
        if (t > base && t - base > best_end - best_base) {
            best_base = (u32)base;
            best_end = (u32)t;
        }
    }
    if (!n) {
        u32 kb88 = bootinfo[2], e801 = bootinfo[3], blk = bootinfo[4];
        if (e801) top = 0x100000 + e801 * 1024 + blk * 65536;
        else top = 0x100000 + kb88 * 1024;
        best_base = 0x110000;
        best_end = (top > 0x1000000 ? 0x1000000 : top) & ~4095u;
    }
    ram_top = top;
    arena_base = best_base ? best_base : 0x110000;
    if (best_end > arena_base + 0x20000) {
        arena_end = best_end - 0x10000;
        arena_size = arena_end - arena_base - 0x10000;
    } else {
        arena_end = arena_base;
        arena_size = 0;
    }
    arena_ptr = arena_base + 0x10000;
}

static void arena_reset(void) { arena_ptr = arena_base + 0x10000; }

static void build_pagetables(void) {
    if (pg_built || !arena_size) return;
    pg_dir = (u32 *)arena_base;
    kmemset(pg_dir, 0, 4096);
    for (int t = 0; t < 4; t++) {
        u32 *pt = (u32 *)(arena_base + 0x1000 + t * 0x1000);
        pg_dir[t] = (u32)pt | 7;
        for (int i = 0; i < 1024; i++) pt[i] = ((u32)(t * 1024 + i) << 12) | 7;
    }
    if (cpu.has_apic) {
        u32 *pt = (u32 *)(arena_base + 0x5000);
        kmemset(pt, 0, 4096);
        pt[(0xFEE00000u >> 12) & 1023] = 0xFEE00000u | 0x1F;
        pg_dir[0xFEE00000u >> 22] = (u32)pt | 7;
    }
    tss_df.cr3 = (u32)pg_dir;
    pg_built = 1;
}

void paging_enable(void) {
    if (paging_on) return;
    build_pagetables();
    if (!pg_built) return;
    wr_cr3((u32)pg_dir);
    wr_cr0(rd_cr0() | 0x80000000u);
    __asm__ volatile("jmp 1f\n1:");
    paging_on = 1;
    g_cr0_base |= 0x80000000u;
}

void paging_disable(void) {
    if (!paging_on) return;
    wr_cr0(rd_cr0() & ~0x80000000u);
    __asm__ volatile("jmp 1f\n1:");
    paging_on = 0;
    g_cr0_base &= ~0x80000000u;
}

/* ============================ PCI ============================ */
int pci_present;
u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 reg) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (reg & 0xFC));
    u32 v = inl(0xCFC);
    outl(0xCF8, 0);
    return v;
}
static void pci_probe(void) {
    outl(0xCF8, 0x80000000u);
    if (inl(0xCF8) == 0x80000000u) {
        u32 id = pci_read32(0, 0, 0, 0);
        pci_present = id != 0xFFFFFFFFu && id != 0;
    }
    outl(0xCF8, 0);
}

/* ============================ CPU detection ============================ */
cpuinfo_t cpu;

static int flag_toggle(u32 mask) {
    u32 o = rd_eflags();
    wr_eflags(o ^ mask);
    u32 n = rd_eflags();
    wr_eflags(o);
    return ((o ^ n) & mask) != 0;
}

static u16 fpu_sw, fpu_cw;
static void fpu_probe_fn(void) {
    fpu_sw = 0xFFFF;
    __asm__ volatile("fninit; fnstsw %0; fnstcw %1" : "=m"(fpu_sw), "=m"(fpu_cw));
}
static void cr4_probe_fn(void) { g_cr4_base = rd_cr4(); cr4_ok = 1; }

static void cpu_detect(void) {
    kmemset(&cpu, 0, sizeof cpu);
    cpu.is486 = flag_toggle(1u << 18);
    cpu.is386 = !cpu.is486;
    cpu.has_cpuid = flag_toggle(1u << 21);
    cpu.family = cpu.is486 ? 4 : 3;
    if (cpu.has_cpuid) {
        u32 a, b, c, d;
        cpuid(0, &a, &b, &c, &d);
        cpu.max_leaf = a;
        *(u32 *)&cpu.vendor[0] = b; *(u32 *)&cpu.vendor[4] = d; *(u32 *)&cpu.vendor[8] = c;
        if (a >= 1) {
            cpuid(1, &a, &b, &c, &d);
            cpu.stepping = a & 15; cpu.model = (a >> 4) & 15; cpu.family = (a >> 8) & 15;
            if (cpu.family == 6) cpu.model |= ((a >> 16) & 15) << 4;
            if (cpu.family == 15) { cpu.family += (a >> 20) & 0xFF; cpu.model |= ((a >> 16) & 15) << 4; }
            cpu.feat_edx = d; cpu.feat_ecx = c;
            cpu.hv = (c >> 31) & 1;
            if (cpu.hv) {
                u32 e;
                cpuid(0x40000000, &e, &b, &c, &d);
                *(u32 *)&cpu.hv_vendor[0] = b; *(u32 *)&cpu.hv_vendor[4] = c; *(u32 *)&cpu.hv_vendor[8] = d;
            }
        }
        cpuid(0x80000000, &a, &b, &c, &d);
        cpu.ext_max = (a & 0x80000000u) ? a : 0;
        if (cpu.ext_max >= 0x80000001u) {
            cpuid(0x80000001, &a, &b, &c, &d);
            cpu.ext_edx = d;
        }
    }
    u32 f = cpu.feat_edx;
    cpu.has_vme = f >> 1 & 1; cpu.has_de = f >> 2 & 1; cpu.has_pse = f >> 3 & 1; cpu.has_tsc = f >> 4 & 1;
    cpu.has_msr = f >> 5 & 1; cpu.has_pae = f >> 6 & 1; cpu.has_mce = f >> 7 & 1; cpu.has_cx8 = f >> 8 & 1;
    cpu.has_apic = f >> 9 & 1;
    cpu.has_sep = (f >> 11 & 1) && !(cpu.family == 6 && cpu.model < 3 && cpu.stepping < 3);
    cpu.has_mtrr = f >> 12 & 1; cpu.has_pge = f >> 13 & 1; cpu.has_cmov = f >> 15 & 1; cpu.has_pat = f >> 16 & 1;
    cpu.has_pse36 = f >> 17 & 1; cpu.has_mmx = f >> 23 & 1; cpu.has_fxsr = f >> 24 & 1;
    cpu.has_3dnow = cpu.ext_edx >> 31 & 1;
    cpu.has_3dnowext = cpu.ext_edx >> 30 & 1;
    /* EMMI has no CPUID feature bit; restrict config-port access to 6x86MX/MII. */
    cpu.has_emmi = cpu.has_cpuid && !kmemcmp(cpu.vendor, "CyrixInstead", 12) &&
                   cpu.family == 6 && cpu.model == 0 && cpu.has_mmx;

    /* CR4 availability / baseline CR0 */
    cr4_ok = 0;
    if (cpu.has_cpuid && guard_call(cr4_probe_fn) != 0) cr4_ok = 0;
    u32 cr0 = rd_cr0();
    cr0 &= ~(0x4u | 0x8u); /* EM TS: preserve firmware cache policy */
    cr0 |= 0x2u;                                        /* MP */
    if (cpu.is486) cr0 |= 0x20u;                        /* NE */
    wr_cr0(cr0);
    g_cr0_base = cr0;
    if (cr4_ok && cpu.has_fxsr) { wr_cr4(rd_cr4() | 0x200); g_cr4_base = rd_cr4(); }

    /* FPU probe */
    fpu_sw = 0xFFFF; fpu_cw = 0;
    if ((!cpu.has_cpuid || (cpu.feat_edx & 1)) &&
        guard_call(fpu_probe_fn) == 0 && (fpu_sw & 0xFF) == 0 &&
        (fpu_cw & 0x103F) == 0x003F) cpu.has_fpu = 1;
}

static void tsc_probe_fn(void) { (void)rdtsc(); }

static void calibrate(void) {
    if (!cpu.has_tsc) return;
    if (guard_call(tsc_probe_fn) != 0) { cpu.has_tsc = 0; return; }
    stamp_is_tsc = 1;
    u8 p61 = inb(0x61);
    outb(0x61, p61 & 0xFC);
    outb(0x43, 0xB0);
    outb(0x42, 59659 & 0xFF);
    outb(0x42, 59659 >> 8);
    u64 t0 = rdtsc();
    outb(0x61, (p61 & 0xFC) | 1);
    u32 n = 20000000;
    while (!(inb(0x61) & 0x20) && --n) {}
    u64 t1 = rdtsc();
    outb(0x61, p61);
    if (!n || t1 <= t0) return;
    cpu.tsc_per_ms = (t1 - t0) * 1000 / 50000; /* 59659 counts = 50.0 ms */
    cpu.mhz = (u32)((t1 - t0) / 50000);
}

/* ============================ harness ============================ */
static const test_t *g_cur;
static u32 g_sum, g_nfail;
static u64 g_ops;
static const char *g_skip_reason;
static const char *g_fail_what;
static int g_fail_line;
static u32 g_fail_got, g_fail_want;
static int g_fail_has_values;
static u32 tot_pass, tot_fail, tot_skip, tot_fault, tot_tests;
static int test_quiet;

void t_fail(const char *what, int line) {
    if (!g_nfail) { g_fail_what = what; g_fail_line = line; g_fail_has_values = 0; }
    if (g_nfail < 8 && !test_quiet) kprintf("FAILDETAIL test=%s line=%d check=%s\n", g_cur->name, line, what);
    if (g_nfail != ~0u) g_nfail++;
}
void t_failv(const char *what, int line, u32 got, u32 want) {
    if (!g_nfail) {
        g_fail_what = what; g_fail_line = line;
        g_fail_got = got; g_fail_want = want; g_fail_has_values = 1;
    }
    if (g_nfail < 8 && !test_quiet) kprintf("FAILDETAIL test=%s line=%d check=%s got=%08x want=%08x\n", g_cur->name, line, what, got, want);
    if (g_nfail != ~0u) g_nfail++;
}
void t_skip(const char *reason) { g_skip_reason = reason; }
void t_ops(u64 n) { g_ops += n; }
void t_note(const char *fmt, ...) {
    if (test_quiet) return;
    va_list ap;
    note_capture = 1;
    kprintf("NOTE test=%s ", g_cur ? g_cur->name : "-");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kputc('\n');
    note_capture = 0;
}

static void test_tramp(void) { g_sum = g_cur->fn(); }

static const char *missing_req(u32 req) {
    static const struct { u32 bit; const char *name; } m[] = {
        { REQ_CPUID, "cpuid" }, { REQ_TSC, "tsc" }, { REQ_FPU, "fpu" }, { REQ_MMX, "mmx" }, { REQ_CMOV, "cmov" },
        { REQ_FXSR, "fxsr" }, { REQ_PSE, "pse" }, { REQ_PAE, "pae" }, { REQ_PGE, "pge" }, { REQ_SEP, "sep" },
        { REQ_MSR, "msr" }, { REQ_CX8, "cx8" }, { REQ_486, "486" }, { REQ_P5, "family5" }, { REQ_P6, "family6" },
        { REQ_NOHV, "hypervisor" }, { REQ_PAGING, "paging" }, { REQ_PSE36, "pse36" }, { REQ_PAT, "pat" },
        { REQ_MTRR, "mtrr" }, { REQ_APIC, "apic" }, { REQ_MEM8M, "ram8m" }, { REQ_PCI, "pci" }, { REQ_MCE, "mce" },
        { REQ_VME, "vme" }, { REQ_DE, "de" }, { REQ_3DNOW, "3dnow" },
        { REQ_3DNOWEXT, "3dnowext" }, { REQ_EMMI, "cyrix_emmi" },
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
        if (!(req & m[i].bit)) continue;
        int ok = 1;
        switch (m[i].bit) {
        case REQ_CPUID: ok = cpu.has_cpuid; break;
        case REQ_TSC: ok = cpu.has_tsc; break;
        case REQ_FPU: ok = cpu.has_fpu; break;
        case REQ_MMX: ok = cpu.has_mmx; break;
        case REQ_CMOV: ok = cpu.has_cmov; break;
        case REQ_FXSR: ok = cpu.has_fxsr && cr4_ok; break;
        case REQ_PSE: ok = cpu.has_pse && cr4_ok; break;
        case REQ_PAE: ok = cpu.has_pae && cr4_ok; break;
        case REQ_PGE: ok = cpu.has_pge && cr4_ok; break;
        case REQ_SEP: ok = cpu.has_sep; break;
        case REQ_MSR: ok = cpu.has_msr; break;
        case REQ_CX8: ok = cpu.has_cx8; break;
        case REQ_486: ok = cpu.is486; break;
        case REQ_P5: ok = cpu.family >= 5; break;
        case REQ_P6: ok = cpu.family >= 6; break;
        case REQ_NOHV: ok = !cpu.hv; break;
        case REQ_PAGING: ok = paging_on; break;
        case REQ_PSE36: ok = cpu.has_pse36; break;
        case REQ_PAT: ok = cpu.has_pat && cpu.has_msr; break;
        case REQ_MTRR: ok = cpu.has_mtrr && cpu.has_msr; break;
        case REQ_APIC: ok = cpu.has_apic; break;
        case REQ_MEM8M: ok = arena_base + 0x10000 <= 0x400000 &&
                            arena_end >= 0x800000; break;
        case REQ_PCI: ok = pci_present; break;
        case REQ_MCE: ok = cpu.has_mce; break;
        case REQ_VME: ok = cpu.has_vme; break;
        case REQ_DE: ok = cpu.has_de; break;
        case REQ_3DNOW: ok = cpu.has_3dnow; break;
        case REQ_3DNOWEXT: ok = cpu.has_3dnowext; break;
        case REQ_EMMI: ok = cpu.has_emmi; break;
        }
        if (!ok) return m[i].name;
    }
    return NULL;
}

static void restore_state(void) {
    /* An IRQ return reloads segment selectors, so retire scratch selectors
     * before invalidating their descriptors, with interrupts masked. */
    cli();
    __asm__ volatile("mov %0,%%ax; mov %%ax,%%ds; mov %%ax,%%es; mov %%ax,%%fs; mov %%ax,%%gs; mov %%ax,%%ss" ::"i"(SEL_KDATA) : "eax", "memory");
    for (int i = 0; i < 256; i++) int_hook[i] = 0;
    exc_expect_clear();
    g_wd_deadline = 0;
    idt_init();
    for (int i = GDT_SCRATCH_FIRST; i < GDT_ENTRIES; i++) gdt[i] = 0;
    wr_cr0(g_cr0_base);
    if (cr4_ok) wr_cr4(g_cr4_base);
    if (paging_on && pg_dir) wr_cr3((u32)pg_dir);
    if (cpu.has_fpu) __asm__ volatile("fninit");
    sti();
}

static u32 test_index;
static u64 runtime_warmup_ms, runtime_sample_ms, runtime_overhead_ms;

static void report_runtime(const char *name, u32 start, u32 warmup, u32 sample) {
    u32 total = ms_now() - start;
    u32 overhead = total - warmup - sample;
    runtime_warmup_ms += warmup;
    runtime_sample_ms += sample;
    runtime_overhead_ms += overhead;
    kprintf("RUNTIME scope=test r=%u id=%u name=%s total_ms=%u warmup_ms=%u sample_ms=%u overhead_ms=%u resolution_ms=10\n",
            g_round, test_index, name, total, warmup, sample, overhead);
}

/* Host input extends the same checked run; an absent/dead controller never
 * leaves the floppy waiting forever. Frames are keyed to a unique workload. */
static volatile u32 lease_deadline;
static char lease_frame[9];
static u32 lease_length;
static void poll_lease(u32 id) {
    if (!ser_ok) return;
    for (u32 n = 0; n < 256 && (inb(0x3FD) & 1); n++) {
        char c = inb(0x3F8);
        if (c == '+' || c == '-') {
            lease_length = 1;
            lease_frame[0] = c;
        } else if (c == '\n') {
            if (lease_length == 9) {
                u32 value = 0, valid = 1;
                for (u32 i = 1; i < 9; i++) {
                    char x = lease_frame[i];
                    u32 digit = x >= '0' && x <= '9' ? (u32)(x - '0') :
                                x >= 'a' && x <= 'f' ? (u32)(x - 'a' + 10) :
                                x >= 'A' && x <= 'F' ? (u32)(x - 'A' + 10) : 16;
                    if (digit > 15) valid = 0;
                    value = (value << 4) | (digit & 15);
                }
                if (valid && value == id)
                    lease_deadline = lease_frame[0] == '+' ? pit_ticks + 3000 : 0;
            }
            lease_length = 0;
        } else if (lease_length && lease_length < 9) lease_frame[lease_length++] = c;
        else lease_length = 0;
    }
}

#ifndef ONLY
#define ONLY ""
#endif
/* ONLY is a comma-separated list of exact test names; empty runs every test. */
static int selected(const char *name) {
    const char *p = ONLY;
    if (!*p) return 1;
    while (*p) {
        const char *n = name;
        while (*n && *n == *p) { n++; p++; }
        if (!*n && (!*p || *p == ',')) return 1;
        while (*p && *p != ',') p++;
        if (*p) p++;
    }
    return 0;
}

static void run_tests(const test_t *t) {
    for (; t->name; t++) {
        ++test_index;
        if (!selected(t->name)) continue;
        u32 runtime_start = ms_now();
        g_cur = t;
        tot_tests++;
        post((u8)test_index);
        g_nfail = 0; g_ops = 0; g_sum = 0; g_skip_reason = NULL; g_fail_what = NULL;
        note_len = 0; note_capture = 0; note_overflow = 0;
        test_quiet = 0;
        const char *miss = missing_req(t->req);
        const char *status;
        u64 c0 = 0, c1 = 0, cycles = 0, operations = 0;
        u32 m0 = ms_now(), m1;
        u32 rc = 0;
        u32 iterations = 0, sample_ms = 0, sum = 0, total_failures = 0;
        u32 sample_start = 0, sample_window_ms = 0;
        u32 failed_iterations = 0, warmup_ms = 0, warmup_failed = 0;
        u64 iteration_ops = 0;
        int variable_ops = 0;
        const char *first_failure = NULL;
        int first_line = 0, timing_invalid = 0;
        u32 first_iteration = 0, first_got = 0, first_want = 0;
        int first_has_values = 0;
        u32 e_vec = 0, e_err = 0, e_eip = 0, e_cr2 = 0;
        if (miss) {
            status = "SKIP";
            g_skip_reason = miss;
            tot_skip++;
            kprintf("TEST r=%u name=%s status=SKIP reason=missing_%s id=%u req=%08x\n",
                    g_round, t->name, miss, test_index, t->req);
            report_runtime(t->name, runtime_start, 0, 0);
            continue;
        }
        kprintf("BEGIN r=%u name=%s phase=%s\n", g_round, t->name, g_round == 1 ? "first" : "repeat");
        kprintf("WORK phase=warmup r=%u id=%u name=%s\n", g_round, test_index, t->name);
        lease_deadline = 0; lease_length = 0;
        for (;;) {
            arena_reset();
            g_ops = 0; g_nfail = 0; g_sum = 0;
            g_fail_what = NULL; g_skip_reason = NULL;
            sti();
            g_wd_deadline = pit_ticks + WATCHDOG_SECONDS * 100;
            m0 = ms_now();
            c0 = stamp();
            rc = (u32)guard_call(test_tramp);
            c1 = stamp();
            m1 = ms_now();
            g_wd_deadline = 0;
            e_vec = g_exc.vec; e_err = g_exc.err; e_eip = g_exc.eip; e_cr2 = g_exc.cr2;
            note_capture = 0;
            restore_state();
            if (g_nfail) {
                failed_iterations++;
                if (!first_failure) {
                    first_failure = g_fail_what; first_line = g_fail_line;
                    first_iteration = iterations;
                    first_has_values = g_fail_has_values;
                    first_got = g_fail_got; first_want = g_fail_want;
                }
                if (~0u - total_failures < g_nfail) total_failures = ~0u;
                else total_failures += g_nfail;
            }
            if (!iterations) {
                warmup_ms = m1 - m0;
                warmup_failed = g_nfail != 0;
                if (rc || g_skip_reason) break;
                lease_active_id = test_index;
                kprintf("WORK phase=sample r=%u id=%u name=%s\n", g_round, test_index, t->name);
                sample_start = ms_now();
                iterations = 1;
                sample_ms = 0;
                test_quiet = 1;
                continue;
            }
            iterations++;
            sample_ms += m1 - m0;
            if (iterations == 2) iteration_ops = g_ops;
            else if (iteration_ops != g_ops) variable_ops = 1;
            operations += g_ops;
            sum = csum_mix(sum, g_sum);
            if (c1 < c0) timing_invalid = 1;
            else cycles += c1 - c0;
            sample_window_ms = ms_now() - sample_start;
            if (rc || g_skip_reason) break;
            if (sample_window_ms >= TEST_SECONDS * 1000u &&
                (!lease_deadline || (s32)(pit_ticks - lease_deadline) >= 0)) break;
        }
        lease_active_id = 0;
        test_quiet = 0;
        kprintf("WORK phase=end r=%u id=%u name=%s\n", g_round, test_index, t->name);
        note_buf[note_len] = 0;
        kputs(note_buf);
        if (note_overflow) kprintf("WARN test=%s notes_truncated=1\n", t->name);
        if (rc) { status = "FAULT"; tot_fault++; }
        else if (total_failures) { status = "FAIL"; tot_fail++; }
        else if (g_skip_reason) { status = "SKIP"; tot_skip++; }
        else { status = "PASS"; tot_pass++; }
        kprintf("TEST r=%u name=%s status=%s ms=%u cyc=%llu unit=%s ops=%llu sum=%08x",
                g_round, t->name, status, sample_ms, cycles,
                stamp_is_tsc ? "tsc" : "pit", operations, sum);
        kprintf(" iterations=%u warmup_ms=%u warmup_failed=%u failed_iterations=%u id=%u",
                iterations ? iterations - 1 : 0, warmup_ms, warmup_failed, failed_iterations, test_index);
        kprintf(" iteration_ops=%llu sample_window_ms=%u req=%08x",
                variable_ops ? 0 : iteration_ops, sample_window_ms, t->req);
        if (rc) kprintf(" rc=%03x exc_vec=%u exc_err=%08x exc_eip=%08x cr2=%08x", rc, e_vec, e_err, e_eip, e_cr2);
        if (timing_invalid) kputs(" timing_invalid=counter_regressed");
        kprintf(" ops_constant=%u ms_resolution=10", !variable_ops && iteration_ops != 0);
        if (!sample_ms) kputs(" ms_unresolved=1");
        if (!cycles) kputs(" counter_unresolved=1");
        if (g_skip_reason) kprintf(" reason=%s", g_skip_reason);
        if (total_failures) {
            kprintf(" nfail=%u first=\"%s\" line=%d failure_iteration=%u failure_phase=%s seed=%08x",
                    total_failures, first_failure, first_line, first_iteration,
                    first_iteration ? "sample" : "warmup", (u32)TEST_SEED);
            if (first_has_values) kprintf(" got=%08x want=%08x", first_got, first_want);
        }
        kputc('\n');
        report_runtime(t->name, runtime_start, warmup_ms, sample_ms);
    }
}

static void core_ud_probe(void) { __asm__ volatile("ud2"); }
static void core_watchdog_probe(void) { for (;;) { sti(); hlt(); } }

static u32 core_recovery_test(void) {
    CHECK_EQ(guard_call(core_ud_probe), GUARD_EXC(6));
    CHECK_EQ(g_exc.vec, 6);
    u32 deadline = g_wd_deadline;
    g_wd_deadline = pit_ticks + 2;
    CHECK_EQ(guard_call(core_watchdog_probe), GUARD_WATCHDOG);
    g_wd_deadline = deadline;
    CHECK_EQ(guard_call(core_ud_probe), GUARD_EXC(6));
    t_ops(3);
    return 3;
}

static const test_t tests_core[] = {
    { "core_fault_watchdog_recovery", core_recovery_test, 0 },
    { NULL, NULL, 0 }
};

/* ============================ main ============================ */
void kmain(void) {
    ser_init();
    vga_init();
    post(1);
    kprintf("CPUBURN-TEST %s start\n", CPUBURN_VERSION);
    kprintf("BUILD version=%s compiler=\"%s\"\n", CPUBURN_VERSION, __VERSION__);

    gdt_init();
    idt_init();
    load_tables();
    ltr_main();
    pic_init();
    sti();
    if (!timer_ready()) {
        kputs("FATAL PIT/PIC timer unavailable; legacy PC platform required\nDONE\n");
        for (;;) { cli(); hlt(); }
    }
    cpu_detect();
    calibrate();
    pci_probe();
    mem_init();
    post(2);

    kprintf("BOOT drive=%02x a20=%u e820=%u int15_88=%uK e801=%uK+%uK64\n", bootinfo[1], bootinfo[6], bootinfo[5], bootinfo[2], bootinfo[3], bootinfo[4]);
    kprintf("CPU cpuid=%u vendor=\"%s\" family=%u model=%u stepping=%u is486=%u fpu=%u feat_edx=%08x feat_ecx=%08x max_leaf=%u ext_max=%08x\n",
            cpu.has_cpuid, cpu.vendor, cpu.family, cpu.model, cpu.stepping, cpu.is486, cpu.has_fpu, cpu.feat_edx, cpu.feat_ecx, cpu.max_leaf, cpu.ext_max);
    kprintf("CPU mhz=%u tsc_per_ms=%llu hypervisor=%u hv_vendor=\"%s\" cr4=%u pci=%u serial=%u\n", cpu.mhz, cpu.tsc_per_ms, cpu.hv, cpu.hv_vendor, cr4_ok, pci_present, ser_ok);
    kprintf("CPU ext_edx=%08x 3dnow=%u 3dnowext=%u cyrix_emmi=%u\n",
            cpu.ext_edx, cpu.has_3dnow, cpu.has_3dnowext, cpu.has_emmi);
    kprintf("MEM ram_top=%08x arena_base=%08x arena_size=%08x\n", ram_top, arena_base, arena_size);
    kprintf("CFG rounds=%u burn_seconds=%u watchdog_seconds=%u heavy_scale=%u seed=%08x test_seconds=%u protocol=work1\n",
            ROUNDS, BURN_SECONDS, WATCHDOG_SECONDS, HEAVY_SCALE, (u32)TEST_SEED, TEST_SECONDS);
    if (*ONLY) kprintf("CFG only=%s\n", ONLY);

    if (cpu.is386) kprintf("WARN 386 detected: unsupported, results may be unreliable\n");

    u32 start_ms = ms_now();
    for (g_round = 1; g_round <= ROUNDS; g_round++) {
        paging_disable();
        run_tests(tests_core);
        run_tests(tests_int);
        run_tests(tests_corpus);
        run_tests(tests_fpu);
        run_tests(tests_fp_edges);
        run_tests(tests_mmx);
        run_tests(tests_vendor);
        run_tests(tests_pmode);
        run_tests(tests_heavy_pmode);
        run_tests(tests_chipset);
        paging_enable();
        run_tests(tests_paging);
        run_tests(tests_priority);
        run_tests(tests_restart_complex);
        run_tests(tests_heavy_paging);
        run_tests(tests_mem);
        run_tests(tests_sweeps);
        run_tests(tests_sys);
        run_tests(tests_heavy_state);
        if (g_round == ROUNDS) run_tests(tests_burn);
    }
    u32 runtime_total = ms_now() - start_ms;
    kprintf("SUMMARY rounds=%u tests=%u pass=%u fail=%u skip=%u fault=%u ms=%u\n", ROUNDS, tot_tests, tot_pass, tot_fail, tot_skip, tot_fault,
            runtime_total);
    kprintf("RUNTIME scope=suite total_ms=%u warmup_ms=%llu sample_ms=%llu overhead_ms=%llu suite_overhead_ms=%llu resolution_ms=10\n",
            runtime_total, runtime_warmup_ms, runtime_sample_ms, runtime_overhead_ms,
            (u64)runtime_total - runtime_warmup_ms - runtime_sample_ms - runtime_overhead_ms);
    kprintf("DONE\n");
    post(0xFF);
    cli();
    for (;;) { cli(); hlt(); }
}
