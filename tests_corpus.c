#include "kernel.h"

#define C_CF 1u
#define C_PF 4u
#define C_AF 16u
#define C_ZF 64u
#define C_SF 128u
#define C_OF 2048u
#define C_FLAGS (C_CF | C_PF | C_AF | C_ZF | C_SF | C_OF)
#define C_LENGTH 32u
#define C_POINTS (C_LENGTH / 8u)

typedef struct { u32 reg[4], flags; } snapshot_t;
typedef struct { u32 op, reg, width, operand; } instruction_t;

static u32 next(u32 *state) {
    u32 x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

static u32 width_mask(u32 width) {
    return width == 32 ? ~0u : (1u << width) - 1;
}

static u32 result_flags(u32 value, u32 width) {
    u32 parity = 0;
    for (u32 bit = 0; bit < 8; bit++) parity ^= (value >> bit) & 1;
    return (parity ? 0 : C_PF) | (value ? 0 : C_ZF) |
           ((value >> (width - 1)) ? C_SF : 0);
}

/* Bit-serial arithmetic deliberately avoids using the instruction under test
 * as the reference for its result and carry/borrow. */
static u32 add_bits(u32 a, u32 b, u32 cin, u32 width, u32 *carry) {
    u32 result = 0, c = cin;
    for (u32 bit = 0; bit < width; bit++) {
        u32 x = (a >> bit) & 1, y = (b >> bit) & 1;
        result |= (x ^ y ^ c) << bit;
        c = (x & y) | (x & c) | (y & c);
    }
    *carry = c;
    return result;
}

static void reference(snapshot_t *s, u32 *defined, instruction_t ins) {
    u32 w = ins.width, mask = width_mask(w), sign = 1u << (w - 1);
    u32 a = s->reg[ins.reg] & mask, b = ins.operand & mask;
    u32 r = a, f = s->flags, carry = 0, overflow = 0, old_cf = f & C_CF;
    if (ins.op <= 7 || ins.op == 11 || ins.op == 12) {
        int sub = ins.op == 3 || ins.op == 5 || ins.op == 7 || ins.op == 12;
        if (ins.op == 1) r = a | b;
        else if (ins.op == 4) r = a & b;
        else if (ins.op == 6) r = a ^ b;
        else {
            u32 cin = (ins.op == 2 || ins.op == 3) ? old_cf : 0;
            if (ins.op == 11 || ins.op == 12) b = 1;
            r = add_bits(a, sub ? (~b & mask) : b, sub ? !cin : cin, w, &carry);
            if (sub) carry = !carry;
            overflow = (sub ? (a ^ b) : ~(a ^ b)) & (a ^ r) & sign;
        }
        f = result_flags(r, w);
        if (ins.op == 1 || ins.op == 4 || ins.op == 6) {
            *defined = C_FLAGS & ~C_AF;
        } else {
            f |= carry | (((a ^ b ^ r) & 16) ? C_AF : 0) | (overflow ? C_OF : 0);
            *defined = C_FLAGS;
            if (ins.op == 11 || ins.op == 12) f = (f & ~C_CF) | old_cf;
        }
    } else if (ins.op == 8 || ins.op == 9) {
        u32 count = ins.operand;
        for (u32 i = 0; i < count; i++) {
            if (ins.op == 8) { carry = (r >> (w - 1)) & 1; r = ((r << 1) | carry) & mask; }
            else { carry = r & 1; r = (r >> 1) | (carry << (w - 1)); }
        }
        f = (f & ~C_CF) | carry;
        if (count == 1) {
            overflow = ins.op == 8 ? ((r >> (w - 1)) ^ carry) :
                        ((r >> (w - 1)) ^ (r >> (w - 2)));
            f = (f & ~C_OF) | ((overflow & 1) ? C_OF : 0);
            *defined |= C_OF;
        } else *defined &= ~C_OF;
        *defined |= C_CF;
    } else r = (~a) & mask;
    if (ins.op != 7) s->reg[ins.reg] = (s->reg[ins.reg] & ~mask) | r;
    s->flags = f;
}

static void byte(u8 **p, u8 v) { *(*p)++ = v; }
static void dword(u8 **p, u32 v) {
    for (u32 i = 0; i < 4; i++) byte(p, v >> (i * 8));
}

static void emit_instruction(u8 **p, instruction_t ins) {
    if (ins.width == 16) byte(p, 0x66);
    if (ins.op <= 7) {
        byte(p, ins.width == 8 ? 0x80 : 0x81);
        byte(p, 0xC0 | (ins.op << 3) | ins.reg);
        for (u32 i = 0; i < ins.width / 8; i++) byte(p, ins.operand >> (i * 8));
    } else if (ins.op == 8 || ins.op == 9) {
        byte(p, ins.width == 8 ? 0xC0 : 0xC1);
        byte(p, 0xC0 | ((ins.op - 8) << 3) | ins.reg);
        byte(p, ins.operand);
    } else if (ins.op == 10) {
        byte(p, ins.width == 8 ? 0xF6 : 0xF7);
        byte(p, 0xD0 | ins.reg);
    } else {
        byte(p, ins.width == 8 ? 0xFE : 0xFF);
        byte(p, 0xC0 | ((ins.op - 11) << 3) | ins.reg);
    }
}

static void emit_snapshot(u8 **p, snapshot_t *output) {
    for (u32 reg = 0; reg < 4; reg++) {
        byte(p, 0x89); byte(p, 0x05 | (reg << 3));
        dword(p, (u32)&output->reg[reg]);
    }
    byte(p, 0x50); byte(p, 0x9C); byte(p, 0x58);
    byte(p, 0xA3); dword(p, (u32)&output->flags); byte(p, 0x58);
}

static void synchronize(void) {
    if (cpu.has_cpuid) {
        u32 a, b, c, d;
        cpuid(0, &a, &b, &c, &d);
    } else __asm__ volatile("jmp 1f\n1:" ::: "memory");
}

static u32 instruction_corpus(void) {
    u8 *storage = arena_alloc(8192, 4096);
    if (!storage) SKIP("instruction corpus needs 8 KiB owned arena");
    snapshot_t actual[C_POINTS], expected[C_POINTS], state;
    instruction_t sequence[C_LENGTH];
    u32 masks[C_POINTS], initial[4], rng = TEST_SEED, checksum = 0;
    u32 coverage[13][3] = {{0}};
    const u32 blocks = HEAVY_SCALE * 2048u, reuses = 8;
    for (u32 block = 0; block < blocks; block++) {
        u32 vector_seed = rng, defined = C_FLAGS;
        u8 *code = storage + (block & 63), *p = code;
        byte(&p, 0x9C); byte(&p, 0x53);
        for (u32 reg = 0; reg < 4; reg++) {
            initial[reg] = state.reg[reg] = next(&rng);
            byte(&p, 0xB8 + reg); dword(&p, initial[reg]);
        }
        state.flags = 0;
        byte(&p, 0x68); dword(&p, 0x202); byte(&p, 0x9D);
        for (u32 i = 0; i < C_LENGTH; i++) {
            instruction_t ins;
            ins.op = next(&rng) % 13;
            ins.reg = next(&rng) & 3;
            u32 width_index = next(&rng) % 3;
            ins.width = 8u << width_index;
            coverage[ins.op][width_index]++;
            ins.operand = next(&rng);
            if (ins.op == 8 || ins.op == 9) ins.operand = 1 + ins.operand % (ins.width - 1);
            sequence[i] = ins;
            reference(&state, &defined, ins);
            emit_instruction(&p, ins);
            if ((i & 7) == 7) {
                u32 point = i / 8;
                expected[point] = state; masks[point] = defined;
                emit_snapshot(&p, &actual[point]);
            }
        }
        byte(&p, 0x5B); byte(&p, 0x9D); byte(&p, 0xC3);
        CHECK((u32)(p - storage) < 8192);
        synchronize();
        for (u32 reuse = 0; reuse < reuses; reuse++) {
            ((void (*)(void))code)();
            for (u32 point = 0; point < C_POINTS; point++) {
                int bad = ((actual[point].flags ^ expected[point].flags) & masks[point]) != 0;
                for (u32 reg = 0; reg < 4; reg++)
                    bad |= actual[point].reg[reg] != expected[point].reg[reg];
                if (bad) {
                    t_note("seed=%08x vector_seed=%08x block=%u reuse=%u point=%u offset=%u flags=%08x/%08x mask=%08x",
                           (u32)TEST_SEED, vector_seed, block, reuse, point,
                           block & 63, actual[point].flags, expected[point].flags, masks[point]);
                    for (u32 reg = 0; reg < 4; reg++)
                        t_note("reg=%u initial=%08x got=%08x want=%08x", reg, initial[reg],
                               actual[point].reg[reg], expected[point].reg[reg]);
                    for (u32 i = 0; i <= point * 8 + 7; i++)
                        t_note("instruction=%u group=%u reg=%u width=%u operand=%08x",
                               i, sequence[i].op, sequence[i].reg, sequence[i].width, sequence[i].operand);
                    t_fail("generated instruction sequence differs from bit-serial reference", __LINE__);
                    return checksum;
                }
            }
        }
        checksum = csum_mix(checksum, actual[C_POINTS - 1].reg[0]);
        t_ops(C_LENGTH * reuses);
    }
    for (u32 op = 0; op < 13; op++)
        for (u32 width = 0; width < 3; width++) CHECK(coverage[op][width] != 0);
    t_note("seed=%08x blocks=%u instructions_per_block=%u reuses=%u checkpoints=4 widths=8/16/32 groups=13 offsets=0..63 defined_flags_only=1",
           (u32)TEST_SEED, blocks, C_LENGTH, reuses);
    return checksum;
}

const test_t tests_corpus[] = {
    {"corpus_integer_sequences", instruction_corpus, REQ_486},
    {0, 0, 0}
};
