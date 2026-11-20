#include "kernel.h"

#ifndef HEAVY_SCALE
#define HEAVY_SCALE 8
#endif
#ifndef TEST_SEED
#define TEST_SEED 0x86B04A21u
#endif

#define SW_PAGE 4096u
#define SW_MAX_CODE (4u * 1024u * 1024u)
#define SW_MAX_MEM  (4u * 1024u * 1024u)
#define SW_MEM_ALIGN_COUNT 67u
#define SW_MAX_SLOTS (32u * HEAVY_SCALE)

static u32 sw_random(u32 x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x ? x : 0xA5366B4Du;
}

static u64 sw_elapsed(u64 start, u64 end) {
    return end >= start ? end - start : 0;
}

static u32 sw_value(u32 size, u32 align, u32 width, u32 i) {
    u32 x = TEST_SEED ^ size ^ (align * 0x9E3779B9u) ^
            (width * 0x85EBCA6Bu) ^ (i * 0xC2B2AE35u);
    return sw_random(x);
}

static void sw_put32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 sw_ref_load(const u8 *p, u32 n) {
    if (n > 4) n = 4;
    u32 v = 0;
    for (u32 i = 0; i < n; i++) v |= (u32)p[i] << (i * 8);
    return v;
}

static void sw_ref_store(u8 *p, u32 n, u32 v) {
    if (n > 4) n = 4;
    for (u32 i = 0; i < n; i++) p[i] = (u8)(v >> (i * 8));
}

static void sw_readwrite8(volatile u8 *p, u32 lo, u32 hi, u32 *got_lo, u32 *got_hi) {
    __asm__ volatile("movl %3,(%0); movl %4,4(%0); movl (%0),%1; movl 4(%0),%2"
                     : "+r"(p), "=&r"(*got_lo), "=&r"(*got_hi)
                     : "r"(lo), "r"(hi)
                     : "memory");
}

static u32 sw_readwrite1(volatile u8 *p, u32 v) {
    u32 got;
    __asm__ volatile("movb %b1,(%2); movzbl (%2),%0"
                     : "=&r"(got) : "q"(v), "r"(p) : "memory");
    return got;
}

static u32 sw_readwrite2(volatile u8 *p, u32 v) {
    u32 got;
    __asm__ volatile("movw %w1,(%2); movzwl (%2),%0"
                     : "=&r"(got) : "r"(v), "r"(p) : "memory");
    return got;
}

static u32 sw_readwrite4(volatile u8 *p, u32 v) {
    u32 got;
    __asm__ volatile("movl %1,(%2); movl (%2),%0"
                     : "=&r"(got) : "r"(v), "r"(p) : "memory");
    return got;
}

static void sw_serialize_code(void) {
    /* A taken near jump discards the i486 prefetch queue after code writes. */
    __asm__ volatile("jmp 1f\n1:" ::: "memory");
}

static u32 sw_align_at(u32 i) {
    if (i < 64) return i;
    return 4093u + i - 64u;
}

static u32 sw_decode_make(u8 *p, u32 input, u32 add16, u32 add32, u32 xor32) {
    u32 n = 0;
    p[n++] = 0xB8; sw_put32(p + n, input); n += 4; /* mov eax,imm32 */
    p[n++] = 0x66; p[n++] = 0x05;                 /* add ax,imm16 */
    p[n++] = (u8)add16; p[n++] = (u8)(add16 >> 8);
    p[n++] = 0x67; p[n++] = 0x40;                 /* address-size inc eax */
    p[n++] = 0x2E; p[n++] = 0x40;                 /* CS override inc eax */
    p[n++] = 0x66; p[n++] = 0x90;                 /* prefixed one-byte nop */
    p[n++] = 0x67; p[n++] = 0x90;
    p[n++] = 0x2E; p[n++] = 0x90;
    p[n++] = 0x3E; p[n++] = 0x90;
    p[n++] = 0xA8; p[n++] = 0x01;                 /* test al,1 */
    p[n++] = 0x74; p[n++] = 0x07;                 /* jz over add+jmp */
    p[n++] = 0x05; sw_put32(p + n, add32); n += 4; /* odd path */
    p[n++] = 0xEB; p[n++] = 0x05;
    p[n++] = 0x35; sw_put32(p + n, xor32); n += 4; /* even path */
    /* Loops in the generated code, ECX times, while EAX matches ESI */
    p[n++] = 0x39; p[n++] = 0xF0;                 /* cmp eax,esi */
    p[n++] = 0x75; p[n++] = 0x03;                 /* jne to ret */
    p[n++] = 0x49;                                /* dec ecx */
    p[n++] = 0x75; p[n] = (u8)(0 - (n + 1)); n++; /* jnz to the mov */
    p[n++] = 0xC3;
    return n;
}

static u32 sw_decode_expected(u32 input, u32 add16, u32 add32, u32 xor32) {
    u32 v = (input & 0xFFFF0000u) | ((input + add16) & 0xFFFFu);
    v += 2;
    return (v & 1) ? v + add32 : v ^ xor32;
}

static u32 sw_decode_alignment(void) {
    u8 *code = arena_alloc(3 * SW_PAGE, SW_PAGE);
    if (!code) SKIP("arena too small for three-page decode window");
    u32 loops = 128u * HEAVY_SCALE;
    u64 min_cycles = ~(u64)0, max_cycles = 0;
    u32 min_offset = 0, max_offset = 0, max_path = 0, executed = 0, nbytes = 0;
    u32 sum = 0;

    for (u32 a = 0; a < SW_MEM_ALIGN_COUNT; a++) {
        u32 off = sw_align_at(a);
        for (u32 path = 0; path < 2; path++) {
            u32 add16 = (TEST_SEED ^ (off * 0x51u) ^ (path * 0xA21u)) & 0xFFFEu;
            u32 add32 = sw_random(TEST_SEED + off * 19u + path);
            u32 xor32 = sw_random(TEST_SEED ^ (off * 37u) ^ (path * 0x1357u));
            u32 input = 0xA1048000u | path;
            u8 *entry = code + SW_PAGE + off;
            nbytes = sw_decode_make(entry, input, add16, add32, xor32);
            sw_serialize_code();
            u32 want = sw_decode_expected(input, add16, add32, xor32);
            u32 got, left = loops;
            u64 start = stamp();
            __asm__ volatile("call *%2" : "=a"(got), "+c"(left) : "r"(entry), "S"(want) : "memory", "cc");
            u32 ran_instr = (loops - left) * (path ? 16u : 15u) + 1u;
            if (left || got != want) {
                t_note("seed=%08x id=decode-align offset=%u path=%u operand=%08x expected=%08x got=%08x call=%u",
                       (u32)TEST_SEED, off, path, input, want, got, loops - left);
                t_ops(ran_instr);
                CHECK_EQ(got, want);
                CHECK_EQ(left, 0);
                return sum;
            }
            sum += got * loops;
            u64 elapsed = sw_elapsed(start, stamp());
            if (elapsed < min_cycles) { min_cycles = elapsed; min_offset = off; }
            if (elapsed > max_cycles) { max_cycles = elapsed; max_offset = off; max_path = path; }
            executed += ran_instr;
            t_ops(ran_instr);
        }
    }
    t_note("seed=%08x id=decode-align offsets=0..63,4093..4095 count=%u page_cross=%u code_bytes=%u loops_per_path=%u min_cycles=%llu min_offset=%u max_cycles=%llu max_offset=%u max_path=%u instr=%u",
           (u32)TEST_SEED, SW_MEM_ALIGN_COUNT, 3u, nbytes, loops,
           min_cycles, min_offset, max_cycles, max_offset, max_path, executed);
    return sum;
}

static void sw_make_work_fn(u8 *p, u32 imm, u32 add) {
    p[0] = 0xB8; sw_put32(p + 1, imm);
    p[5] = 0x05; sw_put32(p + 6, add);
    p[10] = 0xC3;
}

static u32 sw_work_value(u32 set_size, u32 i, u32 pass) {
    return sw_random((u32)TEST_SEED ^ set_size * 0x9E3779B9u ^
                     i * 0x85EBCA6Bu ^ pass * 0xC2B2AE35u);
}

/* 4 KiB to 4 MiB of 11-byte functions, one every 256 bytes. The cold record
 * rewrites a function before each call (code write, taken jump, call: a
 * recompile every time); the warm one writes them all once, checks each, and
 * then calls them all in turn at least 16 times, each result against the
 * value kept from that check. */
static u32 sw_wants[SW_MAX_CODE / 256u];

static u8 *sw_worksets_code(u32 *max_code) {
    *max_code = SW_MAX_CODE;
    while (*max_code > 4096u && *max_code + 8192u > arena_size)
        *max_code >>= 1;
    if (*max_code < 4096u) return 0;
    return arena_alloc(*max_code + 8192u, SW_PAGE);
}

static const u32 sw_workset_sizes[] = {
    4096u, 16384u, 65536u, 262144u, 1048576u, 4194304u
};

static u32 sw_decode_worksets_cold(void) {
    u32 max_code;
    u8 *code = sw_worksets_code(&max_code);
    if (!code) SKIP("arena too small for executable working sets");
    u32 completed = 0, max_seen = 0, sum = 0, instructions = 0;
    for (u32 s = 0; s < sizeof sw_workset_sizes / sizeof sw_workset_sizes[0]; s++) {
        u32 size = sw_workset_sizes[s];
        if (size > max_code) continue;
        u32 entries = size / 256u;
        u32 iterations = 4096u * HEAVY_SCALE;
        if (iterations < entries) iterations = entries;
        u32 bad = 0, bad_idx = 0, bad_want = 0, bad_got = 0;
        u64 start = stamp();
        for (u32 i = 0; i < iterations; i++) {
            u32 idx = i % entries, imm = sw_work_value(size, idx, i + 1u);
            u32 add = sw_work_value(size, idx, i + 0x10001u);
            u8 *p = code + idx * 256u;
            sw_make_work_fn(p, imm, add);
            sw_serialize_code();
            u32 got = ((u32 (*)(void))p)();
            u32 want = imm + add;
            if (!bad && got != want) { bad = 1; bad_idx = idx; bad_want = want; bad_got = got; }
            sum += got;
        }
        u64 cold = sw_elapsed(start, stamp());
        t_ops(iterations * 3u);
        if (bad) {
            t_note("seed=%08x id=decode-working-set size=%u index=%u operand=%08x expected=%08x got=%08x path=recompiled",
                   (u32)TEST_SEED, size, bad_idx, bad_idx, bad_want, bad_got);
            CHECK_EQ(bad_got, bad_want);
            return sum;
        }
        t_note("seed=%08x id=decode-working-set bytes=%u entries=%u iterations=%u cold_rewrite_jump=%llu cold_per_1000_calls=%llu instr=%u unit=%s",
               (u32)TEST_SEED, size, entries, iterations,
               cold, cold * 1000u / iterations, iterations * 3u, stamp_is_tsc ? "tsc_cycles" : "pit_counts");
        instructions += iterations * 3u;
        completed++;
        max_seen = size;
    }
    if (!completed) SKIP("arena too small for executable working sets");
    t_note("seed=%08x id=decode-working-set sets=%u max_bytes=%u actual_sizes=4K..%u arena=%u recompile=code-write+taken-jump",
           (u32)TEST_SEED, completed, max_seen, max_seen, arena_size);
    t_work_unit("rewrite_and_call");
    return sum ^ instructions;
}

static u32 sw_decode_worksets_warm(void) {
    u32 max_code;
    u8 *code = sw_worksets_code(&max_code);
    if (!code) SKIP("arena too small for executable working sets");
    u32 completed = 0, max_seen = 0, sum = 0, instructions = 0;
    for (u32 s = 0; s < sizeof sw_workset_sizes / sizeof sw_workset_sizes[0]; s++) {
        u32 size = sw_workset_sizes[s];
        if (size > max_code) continue;
        u32 entries = size / 256u;
        u32 iterations = 4096u * HEAVY_SCALE;
        if (iterations < 16u * entries) iterations = 16u * entries;

        for (u32 i = 0; i < entries; i++) {
            u32 imm = sw_work_value(size, i, 0);
            u32 add = sw_work_value(size, i, 0x10000u);
            sw_make_work_fn(code + i * 256u, imm, add);
            sw_wants[i] = imm + add;
        }
        sw_serialize_code();
        for (u32 i = 0; i < entries; i++) {
            u32 want = sw_wants[i];
            u32 got = ((u32 (*)(void))(code + i * 256u))();
            if (got != want) {
                t_note("seed=%08x id=decode-working-set size=%u index=%u operand=%08x expected=%08x got=%08x path=warmup",
                       (u32)TEST_SEED, size, i, i, want, got);
                CHECK_EQ(got, want);
                return sum;
            }
        }
        t_ops(entries * 3u);
        u32 warm_bad = 0, warm_idx = 0, warm_want = 0, warm_got = 0;
        u64 start = stamp();
        for (u32 i = 0, idx = 0; i < iterations; i++) {
            u32 got = ((u32 (*)(void))(code + idx * 256u))();
            if (got != sw_wants[idx] && !warm_bad) {
                warm_bad = 1; warm_idx = idx; warm_want = sw_wants[idx]; warm_got = got;
            }
            sum += got;
            if (++idx == entries) idx = 0;
        }
        u64 warm = sw_elapsed(start, stamp());
        t_ops(iterations * 3u);
        if (warm_bad) {
            t_note("seed=%08x id=decode-working-set size=%u index=%u operand=%08x expected=%08x got=%08x path=warm",
                   (u32)TEST_SEED, size, warm_idx, warm_idx, warm_want, warm_got);
            CHECK_EQ(warm_got, warm_want);
            return sum;
        }
        t_note("seed=%08x id=decode-working-set bytes=%u entries=%u passes=%u iterations=%u warm_reuse=%llu warm_per_1000_calls=%llu warmup_calls=%u instr=%u unit=%s",
               (u32)TEST_SEED, size, entries, iterations / entries, iterations,
               warm, warm * 1000u / iterations, entries,
               iterations * 3u + entries * 3u, stamp_is_tsc ? "tsc_cycles" : "pit_counts");
        instructions += iterations * 3u + entries * 3u;
        completed++;
        max_seen = size;
    }
    if (!completed) SKIP("arena too small for executable working sets");
    t_note("seed=%08x id=decode-working-set sets=%u max_bytes=%u actual_sizes=4K..%u arena=%u warm=reuse min_passes=16",
           (u32)TEST_SEED, completed, max_seen, max_seen, arena_size);
    t_work_unit("call");
    return sum ^ instructions;
}

/* One asm loop over the slots per width, no call per access: each slot's
 * store then load back, as sw_readwrite* did. */
typedef struct { u32 addr, lo, hi, glo, ghi; } sw_slot_t;
static sw_slot_t sw_slots[SW_MAX_SLOTS];

static void sw_mem_pass(u32 width, u32 count) {
    sw_slot_t *sp = sw_slots;
    if (width == 1)
        __asm__ volatile("1: movl (%0),%%edx\n movl 4(%0),%%eax\n movb %%al,(%%edx)\n movzbl (%%edx),%%eax\n"
                         " movl %%eax,12(%0)\n addl $20,%0\n decl %1\n jnz 1b"
                         : "+r"(sp), "+r"(count) :: "eax", "edx", "memory", "cc");
    else if (width == 2)
        __asm__ volatile("1: movl (%0),%%edx\n movl 4(%0),%%eax\n movw %%ax,(%%edx)\n movzwl (%%edx),%%eax\n"
                         " movl %%eax,12(%0)\n addl $20,%0\n decl %1\n jnz 1b"
                         : "+r"(sp), "+r"(count) :: "eax", "edx", "memory", "cc");
    else if (width == 4)
        __asm__ volatile("1: movl (%0),%%edx\n movl 4(%0),%%eax\n movl %%eax,(%%edx)\n movl (%%edx),%%eax\n"
                         " movl %%eax,12(%0)\n addl $20,%0\n decl %1\n jnz 1b"
                         : "+r"(sp), "+r"(count) :: "eax", "edx", "memory", "cc");
    else
        __asm__ volatile("1: movl (%0),%%edx\n movl 4(%0),%%eax\n movl %%eax,(%%edx)\n movl 8(%0),%%eax\n movl %%eax,4(%%edx)\n"
                         " movl (%%edx),%%eax\n movl %%eax,12(%0)\n movl 4(%%edx),%%eax\n movl %%eax,16(%0)\n"
                         " addl $20,%0\n decl %1\n jnz 1b"
                         : "+r"(sp), "+r"(count) :: "eax", "edx", "memory", "cc");
}

static u32 sw_decode_memory(void) {
    u32 max_size = SW_MAX_MEM;
    while (max_size > 1024u &&
           2u * (max_size + 16384u) + 8192u > arena_size)
        max_size >>= 1;
    if (max_size < 1024u) {
        t_note("seed=%08x id=memory-align requested=1K..4M actual=none arena=%u",
               (u32)TEST_SEED, arena_size);
        SKIP("arena too small for memory sweep and bytewise reference");
    }
    u8 *data = arena_alloc(max_size + 16384u, SW_PAGE);
    u8 *ref = arena_alloc(max_size + 16384u, SW_PAGE);
    if (!data || !ref) SKIP("arena too small for memory buffers");

    static const u32 widths[] = { 1, 2, 4, 8 };
    u32 sum = 0, total_ops = 0, size_count = 0;
    const u32 passes = 8;
    for (u32 size = 1024u; size <= max_size; size <<= 1) {
        u64 min_cost = ~(u64)0, max_cost = 0;
        u32 min_align = 0, max_align = 0, max_width = 0;
        u64 max_width_cost = 0;
        for (u32 ai = 0; ai < SW_MEM_ALIGN_COUNT; ai++) {
            u32 align = sw_align_at(ai);
            u64 align_cost = 0;
            u64 width_cost[4];
            for (u32 wi = 0; wi < 4; wi++) {
                u32 width = widths[wi];
                u32 positions = (size - width) / 64u + 1u;
                u32 count = positions < SW_MAX_SLOTS ? positions : SW_MAX_SLOTS;
                u8 *dp = data + SW_PAGE * 2u + align;
                u8 *rp = ref + SW_PAGE * 2u + align;
                for (u32 i = 0; i < count; i++) {
                    u32 pos = (u32)(((u64)i * positions) / count) * 64u;
                    u32 v = sw_value(size, align, width, i);
                    u32 hi = ~v ^ 0xA53C9E17u;
                    sw_slots[i].addr = (u32)(dp + pos); sw_slots[i].lo = v;
                    sw_slots[i].hi = hi; sw_slots[i].ghi = 0;
                    dp[pos - 1] = rp[pos - 1] = 0xA5;
                    dp[pos + width] = rp[pos + width] = 0xA5;
                    sw_ref_store(rp + pos, width, v);
                    if (width == 8) sw_ref_store(rp + pos + 4, 4, hi);
                }
                u64 start = stamp();
                for (u32 pass = 0; pass < passes; pass++) sw_mem_pass(width, count);
                u64 elapsed = sw_elapsed(start, stamp());
                u64 per = elapsed * 1000u / ((u64)count * passes);
                width_cost[wi] = per;
                align_cost += per;
                total_ops += count * 2u * passes;
                t_ops(count * 2u * passes);
                for (u32 i = 0; i < count; i++) {
                    u32 at = sw_slots[i].addr - (u32)dp, v = sw_slots[i].lo;
                    u32 want = sw_ref_load(rp + at, width);
                    u32 want_hi = width == 8 ? sw_ref_load(rp + at + 4, 4) : 0;
                    u32 got = sw_slots[i].glo, got_hi = sw_slots[i].ghi;
                    sum += got ^ got_hi;
                    for (u32 b = 0; b < width; b++) {
                        if (dp[at + b] != rp[at + b]) {
                            t_note("seed=%08x id=memory-align size=%u align=%u width=%u index=%u operand=%08x expected=%02x got=%02x",
                                   (u32)TEST_SEED, size, align, width, i, v,
                                   rp[at + b], dp[at + b]);
                            CHECK_EQ(dp[at + b], rp[at + b]);
                            return sum;
                        }
                    }
                    if (got != want || got_hi != want_hi) {
                        t_note("seed=%08x id=memory-align size=%u align=%u width=%u index=%u operand=%08x expected=%08x:%08x got=%08x:%08x",
                               (u32)TEST_SEED, size, align, width, i, v,
                               want_hi, want, got_hi, got);
                        CHECK_EQ(got, want);
                        CHECK_EQ(got_hi, want_hi);
                        return sum;
                    }
                    if (dp[at - 1] != 0xA5 || dp[at + width] != 0xA5) {
                        t_note("seed=%08x id=memory-align size=%u align=%u width=%u index=%u guard_before=%02x guard_after=%02x",
                               (u32)TEST_SEED, size, align, width, i,
                               dp[at - 1], dp[at + width]);
                        CHECK_EQ(dp[at - 1], 0xA5);
                        CHECK_EQ(dp[at + width], 0xA5);
                        return sum;
                    }
                }
            }
            u64 avg = align_cost / 4u;
            if (avg < min_cost) { min_cost = avg; min_align = align; }
            if (avg > max_cost) {
                max_cost = avg; max_align = align; max_width = widths[0];
                max_width_cost = width_cost[0];
                for (u32 wi = 1; wi < 4; wi++)
                    if (width_cost[wi] > max_width_cost) {
                        max_width_cost = width_cost[wi];
                        max_width = widths[wi];
                    }
            }
        }
        t_note("seed=%08x id=memory-align bytes=%u alignments=%u offsets=0..63,4093..4095 widths=1,2,4,8 samples_per_case<=%u timing_passes=%u min_cycles_per_1000_accesses=%llu min_offset=%u max_cycles_per_1000_accesses=%llu worst_offset=%u worst_width=%u crossings=cacheline+page ops_cumulative=%u unit=%s timed=asm_load_store_loop setup+bytewise_reference_checks=outside_stamp",
               (u32)TEST_SEED, size, SW_MEM_ALIGN_COUNT, SW_MAX_SLOTS, passes,
               min_cost, min_align, max_cost, max_align, max_width, total_ops,
               stamp_is_tsc ? "tsc_cycles" : "pit_counts");
        size_count++;
        if (size == max_size) break;
    }
    t_note("seed=%08x id=memory-align sizes=1K..%u actual_sizes=%u alignments_each=%u widths=1,2,4,8 reference=bytewise seed=%08x ops=%u timing_passes=%u unit=%s",
           (u32)TEST_SEED, max_size, size_count, SW_MEM_ALIGN_COUNT,
           (u32)TEST_SEED, total_ops, passes, stamp_is_tsc ? "tsc_cycles" : "pit_counts");
    return sum;
}

const test_t tests_sweeps[] = {
    { "heavy_decode_align", sw_decode_alignment, REQ_PAGING | REQ_486 },
    { "heavy_decode_worksets_cold", sw_decode_worksets_cold, REQ_PAGING | REQ_486 },
    { "heavy_decode_worksets_warm", sw_decode_worksets_warm, REQ_PAGING | REQ_486 },
    { "heavy_mem_align", sw_decode_memory, REQ_PAGING | REQ_486 },
    { NULL, NULL, 0 }
};
