#include "kernel.h"

#define MEM_NOINLINE __attribute__((noinline))

static u32 rng(u32 *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static u32 mem_walk(void) {
    u32 bytes = arena_size > 0x200000 ? 0x200000 : arena_size & ~4095u;
    volatile u32 *p = arena_alloc(bytes, 4096);
    if (!p || bytes < 4096) SKIP("arena too small");
    for (u32 i = 0; i < 32; i++) {
        p[0] = 1u << i; CHECK_EQ(p[0], 1u << i);
        p[0] = ~(1u << i); CHECK_EQ(p[0], ~(1u << i));
    }
    u32 n = bytes / 4;
    p[0] = 0x13579BDF;
    for (u32 i = 1; i < n; i <<= 1) p[i] = i ^ 0xA53C9621;
    CHECK_EQ(p[0], 0x13579BDF);
    for (u32 i = 1; i < n; i <<= 1) {
        CHECK_EQ(p[i], i ^ 0xA53C9621);
        p[i] = ~(i ^ 0xA53C9621);
        for (u32 j = 1; j < n; j <<= 1)
            CHECK_EQ(p[j], j == i ? ~(j ^ 0xA53C9621) : j ^ 0xA53C9621);
        p[i] = i ^ 0xA53C9621;
    }
    u32 patterns[] = {0x55555555, 0xAAAAAAAA, 0, 0xFFFFFFFF};
    u32 sum = 0;
    for (u32 k = 0; k < 5; k++) {
        u32 state = 0x17BFA321;
        for (u32 i = 0; i < n; i++) p[i] = k == 4 ? rng(&state) : patterns[k];
        state = 0x17BFA321;
        for (u32 i = 0; i < n; i++) {
            u32 want = k == 4 ? rng(&state) : patterns[k], got = p[i];
            CHECK_EQ(got, want); sum = csum_mix(sum, got);
        }
    }
    t_ops(n * 10); return sum;
}

static volatile u32 mem_sink;
static u32 mem_bandwidth(void) {
    u32 max = 0x400000;
    while (max > 1024 && max * 2 + 8192 > arena_size) max >>= 1;
    u32 *a = arena_alloc(max, 4096), *b = arena_alloc(max, 4096);
    if (!a || !b) SKIP("arena too small");
    u32 sum = 0;
    for (u32 size = 1024; size <= max; size <<= 1) {
        u32 n = size / 4, passes = 0x40000 / size;
        if (passes < 2) passes = 2;
        for (u32 i = 0; i < n; i++) a[i] = i ^ size;
        u64 st = stamp();
        for (u32 k = 0; k < passes; k++) for (u32 i = 0; i < n; i++) sum += ((volatile u32 *)a)[i];
        u64 read = stamp() - st; st = stamp();
        for (u32 k = 0; k < passes; k++) for (u32 i = 0; i < n; i++) ((volatile u32 *)b)[i] = i;
        u64 write = stamp() - st; st = stamp();
        for (u32 k = 0; k < passes; k++) {
            u32 *src = a, *dst = b, cnt = n;
            __asm__ volatile("cld; rep movsl" : "+S"(src), "+D"(dst), "+c"(cnt) :: "memory");
        }
        u64 copy = stamp() - st;
        for (u32 i = 0; i < n; i++) CHECK_EQ(b[i], a[i]);
        st = stamp();
        for (u32 k = 0; k < passes; k++) {
            u32 *dst = b, cnt = n;
            __asm__ volatile("cld; rep stosl" : "+D"(dst), "+c"(cnt) : "a"(0xAA551234) : "memory");
        }
        u64 fill = stamp() - st;
        CHECK_EQ(b[0], 0xAA551234); CHECK_EQ(b[n - 1], 0xAA551234);
        t_note("bytes=%u unit=%s read=%llu write=%llu movsd=%llu stosd=%llu per_dword", size,
               stamp_is_tsc ? "cycles" : "pit", read / (n * passes), write / (n * passes), copy / (n * passes), fill / (n * passes));
        u32 state = 0x731BACD5;
        for (u32 i = 0; i < n; i++) b[i] = i;
        for (u32 i = n - 1; i; i--) {
            u32 j = rng(&state) % (i + 1), swap = b[i]; b[i] = b[j]; b[j] = swap;
        }
        for (u32 i = 0; i < n; i++) a[b[i]] = (u32)&a[b[(i + 1) & (n - 1)]];
        volatile u32 *p = a; st = stamp();
        for (u32 i = 0; i < 16384; i++) p = (volatile u32 *)*p;
        mem_sink = (u32)p;
        t_note("pointer_chase bytes=%u latency=%llu unit=%s", size, (stamp() - st) / 16384, stamp_is_tsc ? "cycles" : "pit");
        t_ops(n * passes * 4 + 16384);
    }
    for (u32 stride = 4; stride <= 4096; stride <<= 1) {
        u32 n = max / stride; u64 st = stamp();
        for (u32 k = 0; k < 4; k++) for (u32 i = 0; i < max / 4; i += stride / 4) sum += ((volatile u32 *)a)[i];
        t_note("strided bytes=%u stride=%u per_access=%llu unit=%s", max, stride, (stamp() - st) / (n * 4), stamp_is_tsc ? "cycles" : "pit");
        t_ops(n * 4);
    }
    mem_sink = sum; return sum;
}

/* Unaligned load/store mix: 4 stores and 5 loads per offset. The byte-loop
   reference runs once (per variant) on a scratch buffer; timed invocations
   compare a checksum of every readback and of the final bytes against it. */
#define UA_GUARD 16u
static u32 ref_load(const u8 *p, u32 n) { u32 v = 0; for (u32 i = 0; i < n; i++) v |= (u32)p[i] << (8 * i); return v; }
static void ref_store(u8 *p, u32 n, u32 v) { for (u32 i = 0; i < n; i++) p[i] = v >> (8 * i); }
static u32 ua_value(u32 j, u32 pass) { return (j * 0xABC123u + 0x176423u) ^ (pass * 0x9E3779B9u); }
static u32 ua_span_lo(u32 base) { return (base - UA_GUARD) & ~3u; }
static u32 ua_span_hi(u32 base, u32 count) { return (base + count + 4u + UA_GUARD + 3u) & ~3u; }
static u32 ua_pattern(u32 i) {
    u32 d = 0;
    for (u32 k = 0; k < 4; k++) d |= (u32)(u8)((i + k) ^ ((i + k) >> 8) ^ 0x5Au) << (8 * k);
    return d;
}
static void ua_init(u8 *p, u32 base, u32 count) {
    for (u32 i = ua_span_lo(base); i < ua_span_hi(base, count); i += 4)
        *(volatile u32 *)(p + i) = ua_pattern(i);
}
static MEM_NOINLINE u32 ua_hw(u8 *p, u32 base, u32 count, u32 pass, u32 s) {
    for (u32 j = 0; j < count; j++) {
        u32 off = base + j, v = ua_value(j, pass);
        *(volatile u32 *)(p + off) = v;
        s = csum_mix(s, *(volatile u32 *)(p + off));
        *(volatile u16 *)(p + off + 1) = v >> 8;
        s = csum_mix(s, *(volatile u32 *)(p + off));
        *(volatile u8 *)(p + off + 2) = (u8)~v;
        s = csum_mix(s, *(volatile u32 *)(p + off));
        *(volatile u32 *)(p + off + 1) = ~v;
        s = csum_mix(s, *(volatile u16 *)(p + off + 2));
        s = csum_mix(s, *(volatile u32 *)(p + off));
    }
    return s;
}
static u32 ua_ref(u8 *r, u32 base, u32 count, u32 pass, u32 s) {
    for (u32 j = 0; j < count; j++) {
        u32 off = base + j, v = ua_value(j, pass);
        ref_store(r + off, 4, v);
        s = csum_mix(s, ref_load(r + off, 4));
        ref_store(r + off + 1, 2, v >> 8);
        s = csum_mix(s, ref_load(r + off, 4));
        ref_store(r + off + 2, 1, ~v);
        s = csum_mix(s, ref_load(r + off, 4));
        ref_store(r + off + 1, 4, ~v);
        s = csum_mix(s, ref_load(r + off + 2, 2));
        s = csum_mix(s, ref_load(r + off, 4));
    }
    return s;
}
static u32 ua_bytes_sum(const u8 *lo_page, const u8 *hi_page, u32 base, u32 count, u32 s) {
    for (u32 i = ua_span_lo(base); i < ua_span_hi(base, count); i++)
        s = csum_mix(s, i < 4096u ? lo_page[i] : hi_page[i - 4096u]);
    return s;
}

#define UA_PLAIN_PASSES (16u * HEAVY_SCALE)
static const u32 ua_plain_base[2] = { 64u, 4096u - 64u };
static u32 mem_unaligned(void) {
    static u32 ref_sum, ref_bytes;
    static int ref_ok;
    u8 *p = arena_alloc(8192, 4096);
    if (!p) SKIP("arena too small");
    if (!ref_ok) {
        u8 *r = arena_alloc(8192, 4096);
        if (!r) SKIP("arena too small");
        u32 sum = 0, bytes = 0;
        for (u32 g = 0; g < 2; g++) ua_init(r, ua_plain_base[g], 128);
        for (u32 pass = 0; pass < UA_PLAIN_PASSES; pass++)
            for (u32 g = 0; g < 2; g++) sum = ua_ref(r, ua_plain_base[g], 128, pass, sum);
        for (u32 g = 0; g < 2; g++) bytes = ua_bytes_sum(r, r + 4096, ua_plain_base[g], 128, bytes);
        ref_sum = sum;
        ref_bytes = bytes;
        ref_ok = 1;
    }
    u32 sum = 0, bytes = 0;
    for (u32 g = 0; g < 2; g++) ua_init(p, ua_plain_base[g], 128);
    for (u32 pass = 0; pass < UA_PLAIN_PASSES; pass++)
        for (u32 g = 0; g < 2; g++) sum = ua_hw(p, ua_plain_base[g], 128, pass, sum);
    for (u32 g = 0; g < 2; g++) bytes = ua_bytes_sum(p, p + 4096, ua_plain_base[g], 128, bytes);
    CHECK_EQ(sum, ref_sum);
    CHECK_EQ(bytes, ref_bytes);
    t_ops(UA_PLAIN_PASSES * 256u * 9u);
    t_work_unit("access");
    return sum ^ bytes;
}

/* Offsets 4093-4095 of a two-page window whose pages map to non-contiguous
   physical frames (page 0 -> frame 2, page 1 -> frame 0), so every access
   needs two translations. Frames 1 and 3 are sentinels. */
#define UA_SPLIT_VA 0x5C000000u
#define UA_SPLIT_BASE 4093u
#define UA_SPLIT_COUNT 3u
#define UA_SPLIT_PASSES (1024u * HEAVY_SCALE)
static u32 mem_unaligned_page_split(void) {
    static u32 ref_sum, ref_bytes;
    static int ref_ok;
    const u32 pde = UA_SPLIT_VA >> 22;
    if (pg_dir[pde]) SKIP("scratch page-directory slot is occupied");
    u32 *pd = arena_alloc(4096, 4096), *pt = arena_alloc(4096, 4096);
    u8 *frames = arena_alloc(4 * 4096, 4096);
    if (!pd || !pt || !frames) SKIP("arena too small");
    if (!ref_ok) {
        u8 *r = arena_alloc(8192, 4096);
        if (!r) SKIP("arena too small");
        u32 sum = 0;
        ua_init(r, UA_SPLIT_BASE, UA_SPLIT_COUNT);
        for (u32 pass = 0; pass < UA_SPLIT_PASSES; pass++)
            sum = ua_ref(r, UA_SPLIT_BASE, UA_SPLIT_COUNT, pass, sum);
        ref_bytes = ua_bytes_sum(r, r + 4096, UA_SPLIT_BASE, UA_SPLIT_COUNT, 0);
        ref_sum = sum;
        ref_ok = 1;
    }
    u8 *lo = frames + 2 * 4096, *hi = frames;
    kmemcpy(pd, pg_dir, 4096);
    kmemset(pt, 0, 4096);
    kmemset(frames + 4096, 0xA5, 4096);
    kmemset(frames + 3 * 4096, 0x5A, 64);
    pt[0] = (u32)lo | 3u;
    pt[1] = (u32)hi | 3u;
    pd[pde] = (u32)pt | 3u;
    u32 old_cr3 = rd_cr3();
    wr_cr3((u32)pd);
    u8 *w = (u8 *)UA_SPLIT_VA;
    u32 sum = 0;
    ua_init(w, UA_SPLIT_BASE, UA_SPLIT_COUNT);
    for (u32 pass = 0; pass < UA_SPLIT_PASSES; pass++)
        sum = ua_hw(w, UA_SPLIT_BASE, UA_SPLIT_COUNT, pass, sum);
    wr_cr3(old_cr3);
    u32 bytes = ua_bytes_sum(lo, hi, UA_SPLIT_BASE, UA_SPLIT_COUNT, 0);
    CHECK_EQ(sum, ref_sum);
    CHECK_EQ(bytes, ref_bytes);
    u32 sentinel_bad = 0;
    for (u32 i = 0; i < 4096; i++) sentinel_bad += frames[4096 + i] != 0xA5;
    for (u32 i = 0; i < 64; i++) sentinel_bad += frames[3 * 4096 + i] != 0x5A;
    CHECK_EQ(sentinel_bad, 0);
    t_ops(UA_SPLIT_PASSES * UA_SPLIT_COUNT * 9u);
    t_work_unit("access");
    t_note("passes=%u offsets=4093-4095 page0_frame=%08x page1_frame=%08x", UA_SPLIT_PASSES, (u32)lo, (u32)hi);
    return sum ^ bytes;
}

static volatile u32 lock_word;
static volatile u64 *lock_wide;
static void cx8_op(void) {
    u32 lo = 0x12345678, hi = 0x9ABCDEF0; u8 ok;
    __asm__ volatile("lock cmpxchg8b %1; sete %0" : "=q"(ok), "+m"(*lock_wide), "+a"(lo), "+d"(hi) : "b"(0x87654321), "c"(0x0FEDCBA9) : "memory", "cc");
    CHECK_EQ(ok, 1); CHECK_EQ((u32)*lock_wide, 0x87654321); CHECK_EQ((u32)(*lock_wide >> 32), 0x0FEDCBA9);
    lo = 0; hi = 0;
    __asm__ volatile("lock cmpxchg8b %1; sete %0" : "=q"(ok), "+m"(*lock_wide), "+a"(lo), "+d"(hi) : "b"(1), "c"(2) : "memory", "cc");
    CHECK_EQ(ok, 0); CHECK_EQ(lo, 0x87654321); CHECK_EQ(hi, 0x0FEDCBA9);
}
static void cache_flush(void) { __asm__ volatile("wbinvd" ::: "memory"); }
static u32 mem_lock(void) {
    lock_word = 0;
    for (u32 i = 0; i < 4096; i++) __asm__ volatile("lock addl $1,%0" : "+m"(lock_word) :: "memory", "cc");
    CHECK_EQ(lock_word, 4096);
    u32 v = 7; __asm__ volatile("xchgl %0,%1" : "+r"(v), "+m"(lock_word) :: "memory");
    CHECK_EQ(v, 4096); CHECK_EQ(lock_word, 7);
    v = 5; __asm__ volatile("lock xaddl %0,%1" : "+r"(v), "+m"(lock_word) :: "memory", "cc");
    CHECK_EQ(v, 7); CHECK_EQ(lock_word, 12);
    u32 expected = 12; u8 ok;
    __asm__ volatile("lock cmpxchgl %3,%1; sete %0" : "=q"(ok), "+m"(lock_word), "+a"(expected) : "r"(19) : "memory", "cc");
    CHECK_EQ(ok, 1); CHECK_EQ(lock_word, 19);
    expected = 0;
    __asm__ volatile("lock cmpxchgl %3,%1; sete %0" : "=q"(ok), "+m"(lock_word), "+a"(expected) : "r"(20) : "memory", "cc");
    CHECK_EQ(ok, 0); CHECK_EQ(expected, 19);
    __asm__ volatile("lock btsl $20,%0" : "+m"(lock_word) :: "memory", "cc"); CHECK_EQ(lock_word, 19 | (1 << 20));
    lock_wide = arena_alloc(8, 8);
    if (cpu_test_feature(REQ_CX8) && lock_wide) { *lock_wide = 0x9ABCDEF012345678ull; CHECK_EQ(guard_call(cx8_op), 0); }
    else t_note("CMPXCHG8B unavailable");
    int rc = guard_call(cache_flush); t_note("WBINVD rc=%x", rc);
    CHECK(rc == 0 || rc == GUARD_EXC(6) || rc == GUARD_EXC(13));
    for (u32 i = 0; i < 1024; i++) invlpg((void *)&lock_word);
    t_ops(5120); return lock_word;
}

static volatile u32 *cache_page;
static volatile u32 cache_cd, cache_nw;
static void cache_window(void) {
    __asm__ volatile("wbinvd" ::: "memory");
    wr_cr0((rd_cr0() | 0x40000000u) & ~0x20000000u);
    __asm__ volatile("wbinvd" ::: "memory");
    cache_cd = rd_cr0();
    for (u32 i = 0; i < 128; i++) cache_page[i] = i ^ 0xA617B2CC;
    for (u32 i = 0; i < 128; i++) mem_sink ^= cache_page[i];
    /* NW=1 is legal only while CD=1. Keep both uncached windows bounded. */
    wr_cr0(rd_cr0() | 0x20000000u);
    cache_nw = rd_cr0();
    cache_page[128] = 0xB714325A;
    __asm__ volatile("wbinvd" ::: "memory");
}
static u32 mem_cache_ctl(void) {
    cache_page = arena_alloc(4096, 4096); if (!cache_page) SKIP("arena too small");
    u32 cr0 = rd_cr0(), flags = rd_eflags(); cli();
    int rc = guard_call(cache_window);
    wr_cr0(cr0); /* Restore before reporting errors, even if WBINVD was intercepted. */
    int flush_rc = guard_call(cache_flush); wr_eflags(flags);
    if (rc || flush_rc) SKIP("cache-control instruction trapped");
    CHECK_EQ(cache_cd & 0x60000000u, 0x40000000u);
    CHECK_EQ(cache_nw & 0x60000000u, 0x60000000u);
    for (u32 i = 0; i < 128; i++) CHECK_EQ(cache_page[i], i ^ 0xA617B2CC);
    CHECK_EQ(cache_page[128], 0xB714325A); CHECK_EQ(rd_cr0() & 0x60000000u, cr0 & 0x60000000u);
    return cache_page[128];
}

/* An integer and call loop over a small buffer, as firmware runs it: the
   CPU's caches off (CR0.CD set, NW clear, as an Award BIOS's setup screen
   runs). Checked against the same loop with the caches on. */
static MEM_NOINLINE u32 uncached_step(u32 *buf, u32 v) {
    u32 i = v & 63;
    buf[i] = buf[i] * 3 + v;
    return v ^ (buf[(i + 17) & 63] << 1);
}
static MEM_NOINLINE u32 uncached_loop(u32 *buf, u32 n) {
    u32 v = 0x6a09e667u;
    for (u32 i = 0; i < 64; i++) buf[i] = i * 0x9E3779B1u;
    for (u32 i = 0; i < n; i++) {
        v = uncached_step(buf, v + i);
        if (v & 1) v = (v >> 1) ^ 0xedb88320u; else v >>= 1;
    }
    return v ^ buf[v & 63];
}
static u32 mem_cache_off_loop(void) {
    u32 *buf = arena_alloc(64 * 4, 64); if (!buf) SKIP("arena too small");
    const u32 n = 20000;
    static u32 ref;
    static int ref_ok;
    if (!ref_ok) {
        ref = uncached_loop(buf, n);
        ref_ok = 1;
    }
    u32 cr0 = rd_cr0();
    wr_cr0((cr0 | 0x40000000u) & ~0x20000000u);
    u32 got = uncached_loop(buf, n);
    u32 during = rd_cr0();
    wr_cr0(cr0);
    CHECK_EQ(during & 0x60000000u, 0x40000000u);
    CHECK_EQ(got, ref);
    t_ops(n * 12);
    return got;
}

/* A boot loader's console, as NTLDR's menu redraws it while it waits: a line
   printed one character at a time into text-mode memory through a print
   routine and a cursor routine, each on a page of its own, the cursor's
   address worked out with an IMUL. Written to the text screen's second page,
   which is not shown, and read back. */
static volatile u8 *text_at;
static u32 text_base, text_row, text_col;
static MEM_NOINLINE __attribute__((aligned(4096))) void text_set_cursor(u32 row, u32 col) {
    u32 a;
    if (row > 24) row = 24;
    if (col > 79) col = 79;
    __asm__ volatile("imul $80,%1,%0" : "=r"(a) : "r"(row));
    text_at  = (volatile u8 *)(text_base + (a + col) * 2);
    text_row = row;
    text_col = col;
}
static MEM_NOINLINE __attribute__((aligned(4096))) void text_print_char(u8 c, u8 attr) {
    if (c == '\n') {
        text_set_cursor(text_row + 1, 0);
        return;
    }
    text_at[0] = c;
    text_at[1] = attr;
    text_set_cursor(text_row, text_col + 1);
}
static u32 mem_text_print(void) {
    static const char line[] = "Seconds until highlighted choice will be started automatically: 30";
    const u32 len = sizeof(line) - 1, passes = 400;
    u32 sum = 0;
    if (bda_read16(0x63) == 0x3B4) SKIP("monochrome adapter: no second text page");
    text_base = 0xB9000;
    for (u32 p = 0; p < passes; p++) {
        text_set_cursor(p % 25, 0);
        for (u32 i = 0; i < len; i++) text_print_char((u8) line[i], (u8) (0x07 + (p & 7)));
        text_print_char('\n', 0x07);
        sum = csum_mix(sum, text_row * 80 + text_col);
    }
    volatile u8 *row = (volatile u8 *)(text_base + ((passes - 1) % 25) * 160);
    for (u32 i = 0; i < len; i++) {
        CHECK_EQ(row[i * 2], (u8) line[i]);
        CHECK_EQ(row[i * 2 + 1], (u8) (0x07 + ((passes - 1) & 7)));
    }
    t_ops(passes * (len + 1));
    return sum ^ row[0];
}

const test_t tests_mem[] = {
    {"mem_walk", mem_walk, REQ_PAGING, T_CHECK},
    {"mem_bandwidth", mem_bandwidth, REQ_PAGING, T_CHECK},
    {"mem_unaligned", mem_unaligned, REQ_PAGING},
    {"mem_unaligned_page_split", mem_unaligned_page_split, REQ_PAGING},
    {"mem_lock", mem_lock, REQ_PAGING | REQ_486, T_CHECK},
    {"mem_cache_ctl", mem_cache_ctl, REQ_PAGING | REQ_486 | REQ_NOHV, T_CHECK},
    {"mem_cache_off_loop", mem_cache_off_loop, REQ_486 | REQ_NOHV},
    {"mem_text_print", mem_text_print, REQ_PAGING, T_CHECK},
    {0, 0, 0}
};
