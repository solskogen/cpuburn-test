#include "kernel.h"

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

static u32 ref_load(const u8 *p, u32 n) { u32 v = 0; for (u32 i = 0; i < n; i++) v |= (u32)p[i] << (8 * i); return v; }
static void ref_store(u8 *p, u32 n, u32 v) { for (u32 i = 0; i < n; i++) p[i] = v >> (8 * i); }
static u32 mem_unaligned(void) {
    u8 *p = arena_alloc(8192, 4096), *r = arena_alloc(8192, 4096);
    if (!p || !r) SKIP("arena too small");
    for (u32 i = 0; i < 8192; i++) p[i] = r[i] = i ^ (i >> 8);
    for (u32 region = 0; region < 2; region++) {
        u32 base = region ? 4096 - 64 : 0;
        for (u32 j = 0; j < 128; j++) {
            u32 off = base + j, v = j * 0xABC123u + 0x176423;
            *(volatile u32 *)(p + off) = v; ref_store(r + off, 4, v);
            CHECK_EQ(*(volatile u32 *)(p + off), ref_load(r + off, 4));
            *(volatile u16 *)(p + off + 1) = v >> 8; ref_store(r + off + 1, 2, v >> 8);
            CHECK_EQ(*(volatile u32 *)(p + off), ref_load(r + off, 4));
            *(volatile u8 *)(p + off + 2) = (u8)~v; ref_store(r + off + 2, 1, ~v);
            CHECK_EQ(*(volatile u32 *)(p + off), ref_load(r + off, 4));
            *(volatile u32 *)(p + off + 1) = ~v; ref_store(r + off + 1, 4, ~v);
            CHECK_EQ(*(volatile u16 *)(p + off + 2), ref_load(r + off + 2, 2));
            CHECK_EQ(*(volatile u32 *)(p + off), ref_load(r + off, 4));
        }
    }
    CHECK_EQ(kmemcmp(p, r, 8192), 0); t_ops(256 * 9); return ref_load(r + 4095, 4);
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

const test_t tests_mem[] = {
    {"mem_walk", mem_walk, REQ_PAGING},
    {"mem_bandwidth", mem_bandwidth, REQ_PAGING},
    {"mem_unaligned", mem_unaligned, REQ_PAGING},
    {"mem_lock", mem_lock, REQ_PAGING | REQ_486},
    {"mem_cache_ctl", mem_cache_ctl, REQ_PAGING | REQ_486 | REQ_NOHV},
    {0, 0, 0}
};
