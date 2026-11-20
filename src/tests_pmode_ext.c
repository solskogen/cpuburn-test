/* Emulator-path benchmarks, protected mode with paging OFF. */
#include "kernel.h"

/* Fast A20 gate (port 0x92) toggled 2n times, reading physical 0x171000
 * after every change: with A20 masked it aliases 0x71000. The kernel, IDT
 * and GDT live above 1 MiB, so the loop is copied to 0x70000 and runs with
 * interrupts off and no segment loads; it re-enables A20 before returning.
 * In: ECX=n, BL=port value with A20 off, BH=with A20 on. Out: EAX=sum. */
extern const u8 xa_blob[], xa_blob_end[];
__asm__(".text\n.globl xa_blob,xa_blob_end\n"
        "xa_blob: pushl %esi\n xorl %esi,%esi\n"
        "1: movb %bl,%al\n outb %al,$0x92\n addl 0x171000,%esi\n"
        " movb %bh,%al\n outb %al,$0x92\n addl 0x171000,%esi\n"
        " decl %ecx\n jnz 1b\n movl %esi,%eax\n popl %esi\n ret\n"
        "xa_blob_end:\n");
#define XA_CODE 0x70000u
#define XA_LO 0x71000u

static u32 a20_run(u32 n, u32 off, u32 on) {
    u32 r, f = rd_eflags(), b = off | on << 8;
    cli();
    __asm__ volatile("call *%3" : "=a"(r), "+c"(n), "+b"(b) : "r"(XA_CODE) : "memory", "cc");
    wr_eflags(f);
    return r;
}

static u32 pm_a20_toggle(void) {
    const u32 n = 4096;
    u8 v = inb(0x92), save[64];
    u32 len = (u32)(xa_blob_end - xa_blob);
    if (v == 0xFF) SKIP("no port 0x92");
    if (!(v & 2)) SKIP("A20 not enabled through port 0x92"); /* restoring could mask it */
    if (len > sizeof save) SKIP("A20 loop too large");
    volatile u32 *lo = (volatile u32 *)XA_LO, *hi = (volatile u32 *)(XA_LO + 0x100000);
    u32 off = v & 0xFCu, on = (v & 0xFEu) | 2, keep = *lo, h = *hi; /* bit 0 is fast reset */
    kmemcpy(save, (void *)XA_CODE, len);
    kmemcpy((void *)XA_CODE, xa_blob, len);
    *lo = ~h;
    u32 probe = a20_run(1, off, on);
    u32 sum = probe == 2 * h ? 0 : a20_run(n, off, on);
    outb(0x92, v & 0xFEu);
    *lo = keep;
    kmemcpy((void *)XA_CODE, save, len);
    if (probe == 2 * h) SKIP("port 0x92 does not gate A20");
    CHECK_EQ(probe, 0xFFFFFFFFu); /* ~h with A20 masked, h with it on */
    CHECK_EQ(sum, n * 0xFFFFFFFFu);
    t_work_unit("a20_toggle");
    t_ops(2 * n);
    return sum;
}

const test_t tests_pmode_ext[] = {
    { "pm_a20_toggle", pm_a20_toggle, 0 },
    { 0, 0, 0 }
};
