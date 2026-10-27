#include "kernel.h"

static u8 cmos_read(u8 reg) { outb(0x70, reg & 0x7f); io_delay(); return inb(0x71); }
static void cmos_write(u8 reg, u8 val) { outb(0x70, reg & 0x7f); io_delay(); outb(0x71, val); }
static volatile u32 rtc_irqs;
static void rtc_hook(regs_t *r) { (void)r; if (cmos_read(0x0c) & 0x40) rtc_irqs++; }
static u8 pic_reg(u16 port, u8 cmd) { outb(port, cmd); return inb(port); }

static u32 cs_pic(void) {
    u8 m = inb(0x21), s = inb(0xa1), ma, sa;
    if (m == 0xff && s == 0xff) SKIP("PIC unreadable; IRQ0 left untouched");
    u32 flags = rd_eflags();
    cli();
    outb(0x21, 0xfa); outb(0xa1, 0xfe);
    ma = inb(0x21); sa = inb(0xa1);
    outb(0x21, 0xfe); outb(0xa1, 0xff);
    wr_eflags(flags);
    CHECK_EQ(ma, 0xfa); CHECK_EQ(sa, 0xfe);
    t_note("IMR saved=%02x/%02x loop=%02x/%02x restored=fe/ff IRR=%02x/%02x ISR=%02x/%02x",
           m, s, ma, sa, pic_reg(0x20, 0x0a), pic_reg(0xa0, 0x0a),
           pic_reg(0x20, 0x0b), pic_reg(0xa0, 0x0b));
    /* IRQ7/15 stay masked: inspect the spurious-IRQ ISR bits, never synthesize an edge. */
    t_note("spurious ISR7/15=%u/%u", !!(pic_reg(0x20, 0x0b) & 0x80), !!(pic_reg(0xa0, 0x0b) & 0x80));
    outb(0x20, 0x67); outb(0xa0, 0x67);
    u8 a = cmos_read(0x0a), b = cmos_read(0x0b);
    if (a == 0xff || b == 0xff) SKIP("RTC absent; PIC register checks complete");
    /* A stopped divider or SET mode belongs to firmware; do not start/reconfigure it. */
    if ((a & 0x70) != 0x20 || (b & 0x80)) SKIP("RTC stopped or SET mode; cascade not changed");
    flags = rd_eflags(); cli();
    int_hook[0x28] = rtc_hook; rtc_irqs = 0;
    cmos_write(0x0a, (a & 0x70) | 6);
    cmos_write(0x0b, b | 0x40);
    (void)cmos_read(0x0c);
    outb(0xa1, 0xfe); outb(0x21, 0xfa);
    wr_eflags(flags);
    u32 start = pit_ticks;
    while (pit_ticks - start < 10) hlt();
    cli();
    outb(0xa1, 0xff); outb(0x21, 0xfe);
    cmos_write(0x0b, b); cmos_write(0x0a, a & 0x7f);
    (void)cmos_read(0x0c);
    int_hook[0x28] = 0;
    wr_eflags(flags);
    u32 n = rtc_irqs, ms=(pit_ticks-start)*10;
    /* Judge against the PIT interval actually elapsed, so late host scheduling lengthens the window rather than failing. */
    u32 expect=ms*1024/1000, lo=expect/2, hi=expect+expect/2+2;
    t_note("cascade IRQ8 periodic count=%u PIT_ms=%u expected=%u allowed=%u..%u RTC_A/B_restored=%02x/%02x", n, ms, expect, lo, hi, a, b);
    if (!n) t_skip("RTC periodic IRQ not delivered/routed");
    else CHECK(n >= lo && n <= hi);
    return n;
}

static u16 pit_count(u8 ch) {
    u32 f = rd_eflags(); cli();
    outb(0x43, ch << 6);
    u16 c = inb(0x40 + ch); c |= (u16)inb(0x40 + ch) << 8;
    wr_eflags(f); return c;
}
static u32 cs_pit(void) {
    u16 a = pit_count(0), b;
    for (u32 i=0; i<64; i++) (void)inb(0x61);
    b = pit_count(0);
    t_note("ch0 latch=%u,%u delta_mod11932=%u reload=11932", a,b,(a+11932-b)%11932);
    if (a == 0xffff && b == 0xffff) SKIP("PIT unreadable");
    CHECK(a <= 11932 && b <= 11932);
    u32 f=rd_eflags(); cli();
    /* Latch status only, not the count: do not leave a stale ch0 count latch. */
    outb(0x43, 0xe2); u8 status=inb(0x40);
    wr_eflags(f);
    t_note("ch0 readback status=%02x mode/access=%02x supported=%u", status,status&0x3f,(status&0x3f)==0x34);
    u8 saved=inb(0x61);
    if (saved==0xff) SKIP("port61 absent; ch0 checks complete");
    u32 changes=0; u8 last=saved;
    u32 start=pit_ticks;
    for (u32 i=0; i<200000 && pit_ticks-start<2; i++) {
        u8 v=inb(0x61); if ((v^last)&0x10) changes++; last=v;
    }
    t_note("refresh bit4 edges=%u observation_PIT_ms=%u (nominal edge ~15us)",changes,(pit_ticks-start)*10);
    for (u32 mode=0; mode<=4; mode++) {
        outb(0x61, saved&0xfc);
        outb(0x43, 0xb0 | (mode<<1));
        outb(0x42, 0xa9); outb(0x42, 0x04); /* 1193 counts ~= 1 ms */
        u8 before=inb(0x61);
        u64 t0=stamp(); u32 tick0=pit_ticks;
        outb(0x61, (saved&0xfc)|1);
        u32 edges=0, loops=0; last=inb(0x61); u8 first=last;
        /* One-shots end on OUT=1; periodic modes sample for two PIT ticks. */
        while (++loops<200000 && pit_ticks-tick0<2) {
            u8 v=inb(0x61); if ((v^last)&0x20) edges++; last=v;
            if ((mode==0 || mode==1 || mode==4) && (v&0x20) && edges) break;
        }
        u64 dt=stamp()-t0;
        t_note("ch2 mode=%u count=1193 OUT_before/first/last=%u/%u/%u edges=%u polls=%u PIT_ms=%u stamp_delta=%llu unit=%s",
               mode,!!(before&0x20),!!(first&0x20),!!(last&0x20),edges,loops,(pit_ticks-tick0)*10,dt,stamp_is_tsc?"TSC":"PIT");
        if (mode==0 && edges && stamp_is_tsc)
            t_note("ch2 estimated_CPU_MHz=%u (poll overhead included)",(u32)(dt/1000));
    }
    outb(0x61, saved&~2u);
    t_note("port61 saved=%02x restored=%02x speaker forced off; ch2 mode not owned by core",saved,inb(0x61));
    return csum_mix(changes, a<<16|b);
}

static u32 rtc_number(u8 n,u8 b) { return (b&4)?n:(n>>4)*10+(n&15); }
static u32 cs_cmos_rtc(void) {
    u8 c[64]; u32 sum=0, hash=0;
    for (u32 i=0;i<64;i++) { c[i]=cmos_read(i); hash=csum_mix(hash,c[i]); }
    if (c[0x0a]==0xff && c[0x0b]==0xff) SKIP("CMOS unreadable");
    for (u32 i=0;i<64;i+=8)
        t_note("CMOS %02x: %02x %02x %02x %02x %02x %02x %02x %02x",i,c[i],c[i+1],c[i+2],c[i+3],c[i+4],c[i+5],c[i+6],c[i+7]);
    for (u32 i=0x10;i<=0x2d;i++) sum+=c[i];
    t_note("format=%s seconds=%u battery_valid=%u floppy=%02x equipment=%02x checksum stored=%04x calculated=%04x (vendor-dependent)",
           (c[0xb]&4)?"binary":"BCD",rtc_number(c[0],c[0xb]),!!(c[0xd]&0x80),c[0x10],c[0x14],c[0x2e]*256+c[0x2f],sum&0xffff);
    t_note("memory base_KB=%u extended_KB=%u ext2_KB=%u above16M_64KB=%u ram_top=%08x",
           c[0x15]+256*c[0x16],c[0x17]+256*c[0x18],c[0x30]+256*c[0x31],c[0x34]+256*c[0x35],ram_top);
    u32 start=pit_ticks, first=0, second=0, uip=0, polls=0; u8 sec=c[0];
    while (pit_ticks-start<250) {
        u8 a=cmos_read(0xa); polls++; if (a&0x80) { uip++; continue; }
        u8 now=cmos_read(0);
        if (now!=sec) {
            sec=now;
            if (!first) first=pit_ticks;
            else { second=pit_ticks; break; }
        }
    }
    t_note("RTC edge_wait_ms=%u full_second_interval_ms=%u edges=%u UIP_high_samples=%u/%u timeout_ms=2500",
           (pit_ticks-start)*10,second?(second-first)*10:0,second?2:(first?1:0),uip,polls);
    if (!first) t_skip("RTC seconds did not advance within 2.5s");
    return hash;
}

static u32 cs_kbc_a20(void) {
    u8 status=inb(0x64); u32 drained=0;
    if (status!=0xff) while (drained<32 && (inb(0x64)&1)) { (void)inb(0x60); drained++; }
    u8 p=inb(0x92);
    t_note("KBC status=%02x drained=%u no commands; port92=%02x A20_gate=%s",status,drained,p,
           p==0xff?"unreadable":((p&2)?"on":"off_or_other_source"));
    if (paging_on || (rd_cr0()&0x80000000u)) SKIP("A20 alias test needs paging off");
    /* Both addresses are test-owned usable RAM. 2 MiB alignment makes their
       only differing address bit A20, without touching the kernel or firmware. */
    u8 *block=arena_alloc(0x100004,0x200000);
    if (!block) SKIP("insufficient owned RAM for A20 alias check");
    volatile u32 *lo=(volatile u32 *)block, *hi=(volatile u32 *)(block+0x100000);
    u32 f=rd_eflags(); cli();
    u32 l=*lo,h=*hi;
    *lo=0x14253647; *hi=0x89abcdef;
    u32 low=*lo,high=*hi;
    *hi=h; *lo=l;
    wr_eflags(f);
    t_note("A20 ON alias physical=%08x/%08x values=%08x/%08x distinct=%u saved_bytes_restored; gate writes=none; OFF coverage omitted on all platforms",
           (u32)lo,(u32)hi,low,high,low==0x14253647 && high==0x89abcdef);
    CHECK_EQ(low,0x14253647); CHECK_EQ(high,0x89abcdef);
    return low^high;
}

static u32 cs_dma(void) {
    /* Read two bytes per register without clearing the shared byte flip-flop.
       Byte order is intentionally unknown; the pair leaves its phase unchanged.
       Active channels can advance between reads: this is observational only. */
    u8 status=inb(0x08), high_status=inb(0xd0);
    u8 a0=inb(0x02),a1=inb(0x02),c0=inb(0x03),c1=inb(0x03),page=inb(0x83);
    u8 ha0=inb(0xc4),ha1=inb(0xc4),hc0=inb(0xc6),hc1=inb(0xc6),hp=inb(0x8b);
    t_note("DMA8 status=%02x ch1 address_raw=%02x/%02x count_raw=%02x/%02x page=%02x",status,a0,a1,c0,c1,page);
    t_note("DMA16 status=%02x ch5 address_raw=%02x/%02x count_raw=%02x/%02x page=%02x",high_status,ha0,ha1,hc0,hc1,hp);
    t_note("DMA read-only; status reads clear TC flags; pair byte order unknown; no flip-flop/mask/mode/address/count/page writes; loopback and active transfers omitted for hardware safety");
    if(status==0xff && high_status==0xff && a0==0xff && a1==0xff && c0==0xff && c1==0xff && page==0xff &&
       ha0==0xff && ha1==0xff && hc0==0xff && hc1==0xff && hp==0xff) SKIP("DMA unreadable/absent");
    return csum_mix((u32)status|((u32)high_status<<8),page|((u32)hp<<8));
}

static u32 cs_shadow_rom(void) {
    u32 hash=0;
    for (u32 base=0xc0000;base<0x100000;base+=0x4000) {
        volatile u8 *p=(volatile u8 *)base; u32 h=0,ones=0,zeros=0;
        for(u32 i=0;i<0x4000;i++) { u8 b=p[i]; h=csum_mix(h,b); ones+=b==0xff; zeros+=b==0; }
        /* Reserved ISA holes can contain device MMIO even when every byte reads zero.
           A store cannot be proven reversible from readback: never write unknown mappings. */
        t_note("map=%05x-%05x kind=%s hash=%08x zeros=%u ff=%u first=%02x%02x%02x%02x write_probe=disabled_safety",
               base,base+0x3fff,ones==0x4000?"open_or_FF_ROM":(zeros==0x4000?"zero_RAM_or_MMIO":"ROM_shadow_or_MMIO_readonly"),h,zeros,ones,p[0],p[1],p[2],p[3]);
        hash=csum_mix(hash,h);
    }
    volatile u8 *v=(volatile u8 *)0xffff0; char date[9];
    for(u32 i=0;i<8;i++) { u8 ch=v[5+i]; date[i]=(ch>=32&&ch<=126)?ch:'.'; } date[8]=0;
    t_note("reset_vector=%02x %02x %02x %02x %02x BIOS_date=%s",v[0],v[1],v[2],v[3],v[4],date);
    t_note("BDA COM1=%04x equipment=%04x conventional_KB=%u VGA_A0000=%08x VGA_B8000=%08x",
           *(volatile u16 *)0x400,*(volatile u16 *)0x410,*(volatile u16 *)0x413,*(volatile u32 *)0xa0000,*(volatile u32 *)0xb8000);
    return hash;
}

static void pci_dump(u8 bus,u8 dev,u8 fn) {
    for(u32 r=0;r<256;r+=16)
        t_note("PCI %u:%u.%u cfg%02x %08x %08x %08x %08x",bus,dev,fn,r,
               pci_read32(bus,dev,fn,r),pci_read32(bus,dev,fn,r+4),pci_read32(bus,dev,fn,r+8),pci_read32(bus,dev,fn,r+12));
}
static u32 cs_pci(void) {
    u32 buses=1,n=0,hash=0;
    for(u32 bus=0;bus<4;bus++) {
        if (!(buses&(1u<<bus))) continue;
        for(u32 dev=0;dev<32;dev++) {
            u32 id=pci_read32(bus,dev,0,0); if((id&0xffff)==0xffff) continue;
            u32 hdr=pci_read32(bus,dev,0,0x0c); u32 funcs=(hdr&0x800000)?8:1;
            for(u32 fn=0;fn<funcs;fn++) {
                id=pci_read32(bus,dev,fn,0); if((id&0xffff)==0xffff) continue;
                CHECK_EQ(pci_read32(bus,dev,fn,0),id);
                u32 cl=pci_read32(bus,dev,fn,8),h=pci_read32(bus,dev,fn,0x0c),type=(h>>16)&0x7f;
                u32 flags=rd_eflags(); cli();
                outl(0xcf8,0x80000000u|(bus<<16)|(dev<<11)|(fn<<8));
                u8 lo=inb(0xcfc),hi=inb(0xcfd); u16 vendor=inw(0xcfc),device=inw(0xcfe);
                outl(0xcf8,0); wr_eflags(flags);
                CHECK_EQ(lo,id&255); CHECK_EQ(hi,(id>>8)&255); CHECK_EQ(vendor,id&65535); CHECK_EQ(device,id>>16);
                t_note("PCI %u:%u.%u vendor=%04x device=%04x class=%02x subclass=%02x progif=%02x rev=%02x header=%02x subsystem=%08x",
                       bus,dev,fn,id&65535,id>>16,cl>>24,(cl>>16)&255,(cl>>8)&255,cl&255,(h>>16)&255,type==0?pci_read32(bus,dev,fn,0x2c):0);
                u32 bars=type==0?6:(type==1?2:0);
                for(u32 r=0;r<bars;r++) t_note("PCI %u:%u.%u BAR%u=%08x (not sized)",bus,dev,fn,r,pci_read32(bus,dev,fn,0x10+r*4));
                if ((!bus&&!dev&&!fn) || (cl>>16)==0x0601) pci_dump(bus,dev,fn);
                if (!bus && type==1 && (cl>>16)==0x0604) {
                    u32 b=pci_read32(bus,dev,fn,0x18),sec=(b>>8)&255;
                    t_note("bridge buses primary=%u secondary=%u subordinate=%u",b&255,sec,(b>>16)&255);
                    if(sec && sec<=3) buses|=1u<<sec;
                }
                n++; hash=csum_mix(hash,id);
            }
        }
    }
    t_note("PCI enumerated=%u buses_mask=%x config writes=none CF8 restored=0",n,buses);
    if(!n) t_skip("no readable PCI devices");
    return hash;
}

static u64 apic_base;
static u32 apic_regs[4];
static void apic_msr_read(void) { apic_base=rdmsr(0x1b); }
static void apic_mmio_read(void) {
    volatile u32 *p=(volatile u32 *)0xfee00000;
    apic_regs[0]=p[0x20/4]; apic_regs[1]=p[0x30/4]; apic_regs[2]=p[0x80/4]; apic_regs[3]=p[0xf0/4];
}
static u32 cs_apic(void) {
    int rc=guard_call(apic_msr_read);
    t_note("CPUID_APIC=%u APIC_BASE_guard=%x",cpu.has_apic,rc);
    if(rc) SKIP("APIC base MSR unreadable");
    t_note("IA32_APIC_BASE=%016llx enabled=%u x2apic=%u BSP=%u",apic_base,!!(apic_base&0x800),!!(apic_base&0x400),!!(apic_base&0x100));
    if(!(apic_base&0x800) || (apic_base&0x400) || (apic_base&0xffffffffff000ull)!=0xfee00000ull) SKIP("LAPIC MMIO disabled or nonstandard base/mode");
    if(guard_call(apic_mmio_read) || apic_regs[1]==0xffffffff || !apic_regs[1]) SKIP("LAPIC MMIO unreadable");
    t_note("LAPIC ID=%08x version=%08x TPR=%08x SVR=%08x read-only",apic_regs[0],apic_regs[1],apic_regs[2],apic_regs[3]);
    return apic_regs[1];
}
static u32 cs_misc_ports(void) {
    u8 p=inb(0x61),a=inb(0x92);
    t_note("port61=%02x NMI_parity=%u IOCHK=%u port92=%02x COM1 untouched",p,!!(p&0x80),!!(p&0x40),a);
    u16 com2=*(volatile u16 *)0x402,lpt=*(volatile u16 *)0x408;
    if(com2==0x2f8 && inb(0x2fd)!=0xff) {
        u8 saved=inb(0x2ff); outb(0x2ff,0x5a); u8 r1=inb(0x2ff); outb(0x2ff,0xa5); u8 r2=inb(0x2ff); outb(0x2ff,saved);
        t_note("COM2 scratch=%02x/%02x restored=%02x",r1,r2,saved);
    } else t_note("COM2 scratch skipped: BDA=%04x absent/unidentified UART",com2);
    /* LPT data drives external pins. Restoring does not undo a peripheral's action. */
    if(lpt==0x378) t_note("LPT data=%02x loopback not written: external-device safety",inb(0x378));
    else t_note("LPT skipped: BDA=%04x",lpt);
    return p|((u32)a<<8);
}

const test_t tests_chipset[] = {
    {"cs_pic",cs_pic,0}, {"cs_pit",cs_pit,0}, {"cs_cmos_rtc",cs_cmos_rtc,0},
    {"cs_kbc_a20",cs_kbc_a20,0}, {"cs_dma",cs_dma,0}, {"cs_shadow_rom",cs_shadow_rom,0},
    {"cs_pci",cs_pci,REQ_PCI}, {"cs_apic",cs_apic,REQ_APIC|REQ_MSR},
    {"cs_misc_ports",cs_misc_ports,0}, {0,0,0}
};
