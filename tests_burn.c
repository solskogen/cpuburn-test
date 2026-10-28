#include "kernel.h"

enum { CAT_INT, CAT_X87, CAT_MMX, CAT_MEM, CAT_PAGE, CAT_INT90, CAT_CALL, CAT_COUNT };
static const char *const categories[CAT_COUNT]={"integer_crc","x87","mmx","memory","remap_invlpg","int90","call_ret"};
static u64 category_time[CAT_COUNT];
static u32 baseline[CAT_COUNT],iterations,bytes,seconds,elapsed,start_tick,checksum;
static u8 *src,*dst;
static u32 *scratch_pt,*frames;
static volatile u32 soft_count;
static int first_iteration;

static void soft_hook(regs_t *r) { (void)r; soft_count++; }
static __attribute__((noinline)) u32 recur(u32 n,u32 seed) {
    if(!n) return seed^0x18273645;
    u32 child=recur(n-1,seed*1664525u+1013904223u);
    __asm__ volatile("" : "+r"(child));
    return csum_mix(child,n);
}
static u32 integer_work(void) {
    u32 x=0x12345678,crc=0xffffffff;
    for(u32 i=0;i<128;i++) {
        x=x*1664525u+1013904223u; crc^=x;
        for(u32 j=0;j<8;j++) crc=(crc>>1)^((0u-(crc&1))&0xedb88320u);
    }
    return x^crc;
}
static u32 x87_work(void) {
    s32 result=0;
    __asm__ volatile("fldz");
    for(s32 i=0;i<32;i++) __asm__ volatile("fildl %0; faddp" :: "m"(i));
    __asm__ volatile("fistpl %0" : "=m"(result));
    CHECK_EQ(result,496); return result;
}
static u32 mmx_work(void) {
    u64 step=0x0000000300000007ull,result;
    __asm__ volatile("pxor %%mm0,%%mm0" ::: "memory");
    for(u32 i=0;i<32;i++) __asm__ volatile("paddd %0,%%mm0" :: "m"(step) : "memory");
    __asm__ volatile("movq %%mm0,%0; emms" : "=m"(result) :: "memory");
    CHECK_EQ((u32)result,224); CHECK_EQ((u32)(result>>32),96);
    return (u32)result^(u32)(result>>32);
}
static u32 memory_work(void) {
    kmemcpy(dst,src,bytes);
    CHECK_EQ(kmemcmp(dst,src,bytes),0);
    u32 h=0; for(u32 i=0;i<bytes;i+=64) h=csum_mix(h,dst[i]);
    return h;
}
static u32 page_work(void) {
    if(!scratch_pt) return 0;
    volatile u32 *window=(volatile u32 *)0x50000000;
    u32 sum=0;
    for(u32 pass=0;pass<2;pass++) for(u32 i=0;i<16;i++) {
        u32 frame=(i+pass)&15;
        scratch_pt[i]=(u32)frames+frame*4096+3;
        invlpg((void *)(0x50000000+i*4096));
        u32 got=window[i*1024];
        CHECK_EQ(got,0xa5310000u+frame);
        sum=csum_mix(sum,got);
        window[i*1024+1]=got^0xfedcba98;
        CHECK_EQ(frames[frame*1024+1],got^0xfedcba98);
    }
    return sum;
}
static u32 int_work(void) {
    u32 before=soft_count;
    for(u32 i=0;i<16;i++) __asm__ volatile("int $0x90" ::: "memory","cc");
    u32 n=soft_count-before; CHECK_EQ(n,16); return n;
}
static void record(u32 cat,u64 begin,u32 value) {
    category_time[cat]+=stamp()-begin;
    if(first_iteration) baseline[cat]=value;
    else CHECK_EQ(value,baseline[cat]);
    checksum=csum_mix(checksum,value);
}
static void burn_body(void) {
    u32 heartbeat=500;
    first_iteration=1;
    start_tick=pit_ticks;
    do {
        u64 t=stamp(); u32 v=integer_work(); record(CAT_INT,t,v);
        if(cpu_test_feature(REQ_FPU)) { t=stamp(); v=x87_work(); record(CAT_X87,t,v); }
        if(cpu_test_feature(REQ_MMX)) { t=stamp(); v=mmx_work(); record(CAT_MMX,t,v); }
        t=stamp(); v=memory_work(); record(CAT_MEM,t,v);
        t=stamp(); v=page_work(); record(CAT_PAGE,t,v);
        t=stamp(); v=int_work(); record(CAT_INT90,t,v);
        t=stamp(); v=recur(12,0x87654321); record(CAT_CALL,t,v);
        first_iteration=0; iterations++;
        elapsed=pit_ticks-start_tick;
        if(elapsed>=heartbeat) {
            t_note("heartbeat elapsed_ms=%u target_seconds=%u iterations=%u checksum=%08x",elapsed*10,seconds,iterations,checksum);
            heartbeat=elapsed+500;
        }
    } while(elapsed<seconds*100);
}
static u32 burn_mixed(void) {
    if(g_round!=ROUNDS) SKIP("burn runs only in final round");
    if(WATCHDOG_SECONDS<=10 || BURN_SECONDS<=0) SKIP("no positive burn duration below watchdog margin");
    seconds=BURN_SECONDS;
    if(seconds>(u32)(WATCHDOG_SECONDS-10)) seconds=WATCHDOG_SECONDS-10;
    if(seconds>0x7fffffffu/100) seconds=0x7fffffffu/100;
    iterations=elapsed=checksum=soft_count=0;
    kmemset(category_time,0,sizeof category_time);
    bytes=16384; src=dst=NULL;
    while(bytes>=256) {
        src=arena_alloc(bytes*2,16);
        if(src) { dst=src+bytes; break; }
        bytes/=2;
    }
    if(!dst) SKIP("insufficient arena for even 256-byte copy buffers");
    for(u32 i=0;i<bytes;i++) src[i]=(u8)(i*37u+(i>>8));
    scratch_pt=arena_alloc(4096,4096);
    frames=arena_alloc(16*4096,4096);
    u32 saved_pde=pg_dir[0x50000000>>22];
    if(saved_pde&1) { scratch_pt=NULL; t_note("scratch VA occupied: remap omitted rather than disturb existing mapping"); }
    if(!frames) scratch_pt=NULL;
    if(scratch_pt) {
        kmemset(scratch_pt,0,4096);
        for(u32 i=0;i<16;i++) frames[i*1024]=0xa5310000u+i;
        pg_dir[0x50000000>>22]=(u32)scratch_pt|3;
        wr_cr3(rd_cr3());
    }
    isr_hook_t old_hook=int_hook[0x90];
    idt_set_gate(0x90,isr_stub[0x90],SEL_KCODE,0x8e);
    int_hook[0x90]=soft_hook;
    if(cpu_test_feature(REQ_FPU)) __asm__ volatile("fninit");
    t_note("duration=%u watchdog=%u copy_bytes=%u remap_pages=%u fpu=%u mmx=%u timing=%s",
           seconds,WATCHDOG_SECONDS,bytes,scratch_pt?16:0,cpu_test_feature(REQ_FPU),cpu_test_feature(REQ_MMX),stamp_is_tsc?"TSC_cycles":"PIT_counts");
    /* Recover locally so a workload exception cannot strand our temporary PDE. */
    int rc=guard_call(burn_body);
    pg_dir[0x50000000>>22]=saved_pde;
    wr_cr3(rd_cr3());
    for(u32 i=0;i<16;i++) invlpg((void *)(0x50000000+i*4096));
    int_hook[0x90]=old_hook;
    if(cpu_test_feature(REQ_MMX)) __asm__ volatile("emms");
    CHECK_EQ(rc,GUARD_OK);
    u32 ticks=pit_ticks-start_tick;
    t_note("complete iterations=%u elapsed_ms=%u iterations_per_sec=%u checksum=%08x PDE_restored=%08x",
           iterations,ticks*10,ticks?(u32)((u64)iterations*100/ticks):0,checksum,saved_pde);
    for(u32 i=0;i<CAT_COUNT;i++) t_note("category=%s total_stamp=%llu per_iteration=%llu baseline=%08x",
           categories[i],category_time[i],iterations?category_time[i]/iterations:0,baseline[i]);
    u64 ops=(u64)iterations*(128*10+bytes*2+32+32+32+16+12);
    t_ops(ops);
    return checksum;
}
const test_t tests_burn[] = { {"burn_mixed",burn_mixed,REQ_PAGING|REQ_486}, {0,0,0} };
