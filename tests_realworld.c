#include "kernel.h"

/* Workloads shaped like real programs that generate, load or patch code at
 * run time, or dispatch through tables: a tiering JIT with inline caches, a
 * packed-executable loader, an overlay manager, an in-place decryptor,
 * per-object call thunks and a threaded bytecode interpreter. Everything stays in RAM. Each
 * result is checked against a C reference that does not run generated code. */

#define NOINLINE __attribute__((noinline))

static u32 rw_rng(u32 *s) {
    u32 x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x ? x : 0x6D2B79F5u;
}
static inline u32 rol32(u32 x, u32 k) { k &= 31; return k ? (x << k) | (x >> (32 - k)) : x; }
static void rw_sync(void) { __asm__ volatile("jmp 1f\n1:" ::: "memory"); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 invocation;

/* ---- A small stack bytecode, its interpreter and an x86 translator ---- */

enum { BC_PUSH, BC_ARG, BC_GLOB, BC_ADD, BC_SUB, BC_XOR, BC_MUL, BC_SHL, BC_ROL };
#define BC_MAX 40
#define BC_GMAX 6
#define BC_NGLOB 8
#define BC_DEPTH 6
typedef struct { u8 n, op[BC_MAX]; u32 imm[BC_MAX]; u32 gmask; } prog_t;

/* Random valid program: never empty at a pop, ends with one value. */
static void bc_gen(prog_t *p, u32 *seed) {
    u32 n = 12 + rw_rng(seed) % (BC_MAX - 11), d = 0, globs = 0;
    p->n = (u8)n; p->gmask = 0;
    for (u32 i = 0; i < n; i++) {
        u32 rem = n - i, r = rw_rng(seed), op;
        int can_push = d < BC_DEPTH && d + 1 <= rem, can_un = d >= 1 && d <= rem, can_bin = d >= 2;
        if (can_push && ((!can_un && !can_bin) || r % 8 < 3)) {
            op = (r >> 8) % 3;
            if (op == BC_GLOB && globs >= BC_GMAX) op = BC_PUSH;
            d++;
        } else if (can_bin && (!can_un || r % 8 < 6)) {
            op = BC_ADD + (r >> 8) % 4; d--;
        } else {
            op = BC_SHL + (r >> 8) % 2;
        }
        p->op[i] = (u8)op;
        if (op == BC_PUSH) p->imm[i] = rw_rng(seed);
        else if (op == BC_GLOB) { p->imm[i] = rw_rng(seed) % BC_NGLOB; p->gmask |= 1u << p->imm[i]; globs++; }
        else if (op >= BC_SHL) p->imm[i] = 1 + rw_rng(seed) % 31;
        else p->imm[i] = 0;
    }
}

static u32 bc_interp(const prog_t *p, u32 arg, const u32 *glob) {
    u32 st[BC_DEPTH + 1], d = 0;
    for (u32 i = 0; i < p->n; i++) {
        u32 v = p->imm[i];
        switch (p->op[i]) {
        case BC_PUSH: st[d++] = v; break;
        case BC_ARG: st[d++] = arg; break;
        case BC_GLOB: st[d++] = glob[v]; break;
        case BC_ADD: d--; st[d - 1] += st[d]; break;
        case BC_SUB: d--; st[d - 1] -= st[d]; break;
        case BC_XOR: d--; st[d - 1] ^= st[d]; break;
        case BC_MUL: d--; st[d - 1] *= st[d]; break;
        case BC_SHL: st[d - 1] <<= v; break;
        default: st[d - 1] = rol32(st[d - 1], v); break;
        }
    }
    return st[0];
}

#define BC_CODE_MAX (4 + 6 * BC_MAX + 1)
/* cdecl u32 f(u32 arg). EAX holds the top of stack, the rest is on the x86
 * stack. A global is either an inline-cached immediate (its offset goes to
 * sites[], to be patched when the global changes) or, for globals in
 * 'memload', a load from glob[]. Returns the code length. */
static u32 bc_compile(const prog_t *p, u8 *c, const u32 *glob, u32 memload, u16 *sites, u8 *sglob, u32 *nsite) {
    u32 o = 0, d = 0, ns = 0;
    c[o++] = 0x8B; c[o++] = 0x4C; c[o++] = 0x24; c[o++] = 0x04; /* mov ecx,[esp+4] */
    for (u32 i = 0; i < p->n; i++) {
        u32 v = p->imm[i], op = p->op[i];
        if (op <= BC_GLOB && d++) c[o++] = 0x50; /* push eax */
        switch (op) {
        case BC_PUSH: c[o++] = 0xB8; put32(c + o, v); o += 4; break;
        case BC_ARG: c[o++] = 0x89; c[o++] = 0xC8; break;
        case BC_GLOB:
            if (memload >> v & 1) { c[o++] = 0xA1; put32(c + o, (u32)&glob[v]); o += 4; }
            else {
                c[o++] = 0xB8; put32(c + o, glob[v]);
                if (sites) { sites[ns] = (u16)o; sglob[ns] = (u8)v; ns++; }
                o += 4;
            }
            break;
        case BC_ADD: c[o++] = 0x5A; c[o++] = 0x01; c[o++] = 0xD0; d--; break;
        case BC_SUB: c[o++] = 0x5A; c[o++] = 0x29; c[o++] = 0xC2; c[o++] = 0x89; c[o++] = 0xD0; d--; break;
        case BC_XOR: c[o++] = 0x5A; c[o++] = 0x31; c[o++] = 0xD0; d--; break;
        case BC_MUL: c[o++] = 0x5A; c[o++] = 0x0F; c[o++] = 0xAF; c[o++] = 0xC2; d--; break;
        case BC_SHL: c[o++] = 0xC1; c[o++] = 0xE0; c[o++] = (u8)v; break;
        default: c[o++] = 0xC1; c[o++] = 0xC0; c[o++] = (u8)v; break;
        }
    }
    c[o++] = 0xC3;
    if (nsite) *nsite = ns;
    return o;
}

static NOINLINE u32 call_code(const u8 *code, u32 arg) {
    rw_sync();
    return ((u32 (*)(u32))code)(arg);
}

/* ---- rw_jit_tiering: interpreted tier, translation, inline caches,
 * deoptimisation of unstable globals and code-cache flushes ---- */

#define JF 96
#define JHOT 16
#define JCACHE 12288
#define JCALLS 16384
typedef struct { u8 *code; u16 calls, nsite; u16 site[BC_GMAX]; u8 sglob[BC_GMAX]; u8 memo_ok; u32 memo[8]; } jfn_t;
static prog_t jprog[JF];
static jfn_t jfn[JF];
static u32 jglob[BC_NGLOB];

static u32 rw_jit_tiering(void) {
    u8 *cache = arena_alloc(JCACHE, 4096);
    if (!cache) SKIP("arena");
    u32 seed = TEST_SEED ^ (++invocation * 0x9E3779B9u), args[8], hot[JHOT], gchanges[BC_NGLOB], mega = 0;
    for (u32 i = 0; i < JF; i++) { bc_gen(&jprog[i], &seed); kmemset(&jfn[i], 0, sizeof(jfn[i])); }
    for (u32 i = 0; i < 8; i++) args[i] = rw_rng(&seed);
    for (u32 i = 0; i < JHOT; i++) hot[i] = rw_rng(&seed) % JF;
    for (u32 i = 0; i < BC_NGLOB; i++) { jglob[i] = rw_rng(&seed); gchanges[i] = 0; }
    u8 *cp = cache;
    u32 sum = 0, compiles = 0, flushes = 0, patches = 0, deopts = 0, interp_calls = 0;
    for (u32 c = 0; c < JCALLS; c++) {
        if ((c & 63) == 63) {
            /* Globals 6 and 7 change often enough to be deoptimised. */
            u32 g = (c >> 6) % BC_NGLOB;
            if (c & 64) g = 6 + (c >> 7) % 2;
            jglob[g] = rw_rng(&seed);
            if (!(mega >> g & 1) && ++gchanges[g] > 24) {
                mega |= 1u << g;
                for (u32 f = 0; f < JF; f++)
                    if (jfn[f].code && jprog[f].gmask >> g & 1) { jfn[f].code = NULL; deopts++; }
            }
            for (u32 f = 0; f < JF; f++) {
                if (!(jprog[f].gmask >> g & 1)) continue;
                jfn[f].memo_ok = 0;
                if (!jfn[f].code) continue;
                for (u32 s = 0; s < jfn[f].nsite; s++)
                    if (jfn[f].sglob[s] == g) { put32(jfn[f].code + jfn[f].site[s], jglob[g]); patches++; }
            }
        }
        u32 r = rw_rng(&seed), f = (r & 3) ? hot[(r >> 2) % JHOT] : (r >> 2) % JF, ai = (r >> 12) & 7;
        jfn_t *F = &jfn[f];
        u32 expect;
        if (F->memo_ok >> ai & 1) expect = F->memo[ai];
        else { expect = bc_interp(&jprog[f], args[ai], jglob); F->memo[ai] = expect; F->memo_ok |= 1u << ai; }
        if (!F->code) {
            if (F->calls < 2) { F->calls++; interp_calls++; sum = csum_mix(sum, expect); continue; }
            if (cp + BC_CODE_MAX > cache + JCACHE) {
                for (u32 i = 0; i < JF; i++) jfn[i].code = NULL;
                cp = cache; flushes++;
            }
            u32 ns;
            u32 len = bc_compile(&jprog[f], cp, jglob, mega, F->site, F->sglob, &ns);
            F->nsite = (u16)ns; F->code = cp;
            cp += (len + 15) & ~15u; compiles++;
        }
        u32 got = call_code(F->code, args[ai]);
        CHECK_EQ(got, expect);
        sum = csum_mix(sum, got);
    }
    t_note("compiles=%u flushes=%u ic_patches=%u deopts=%u interpreted=%u", compiles, flushes, patches, deopts, interp_calls);
    t_work_unit("jit_call");
    t_ops(JCALLS);
    return sum;
}

/* ---- Program images: a main that calls 8 functions through relocated
 * absolute addresses, used by the loader and overlay workloads ---- */

#define IMG_FUNCS 8
#define IMG_VARIANTS 4
#define IMG_MAIN 160
#define IMG_MAX (IMG_MAIN + IMG_FUNCS * ((BC_CODE_MAX + 3) & ~3u))
#define IMG_ARGS 4
typedef struct {
    u8 img[IMG_MAX]; u32 size, nrel; u16 rel[IMG_FUNCS];
    u8 lz[IMG_MAX + IMG_MAX / 64 + 16]; u32 lzsize;
    u32 expect[IMG_ARGS];
} image_t;
static image_t images[IMG_VARIANTS];
static prog_t img_prog[IMG_VARIANTS][IMG_FUNCS];
static u32 img_glob[BC_NGLOB], img_args[IMG_ARGS];

/* main: ebx = 0; for each function: ebx = rol(ebx ^ f(arg), 5). */
static void image_build(image_t *im, prog_t *progs) {
    u8 *c = im->img;
    u32 o = 0, fo = IMG_MAIN;
    c[o++] = 0x53; c[o++] = 0x31; c[o++] = 0xDB;
    im->nrel = 0;
    for (u32 i = 0; i < IMG_FUNCS; i++) {
        c[o++] = 0xFF; c[o++] = 0x74; c[o++] = 0x24; c[o++] = 0x08; /* push dword [esp+8] */
        c[o++] = 0xBA; im->rel[im->nrel++] = (u16)o; put32(c + o, fo); o += 4; /* mov edx,image+fo */
        c[o++] = 0xFF; c[o++] = 0xD2;                       /* call edx */
        c[o++] = 0x83; c[o++] = 0xC4; c[o++] = 0x04;        /* add esp,4 */
        c[o++] = 0x31; c[o++] = 0xC3;                       /* xor ebx,eax */
        c[o++] = 0xC1; c[o++] = 0xC3; c[o++] = 0x05;        /* rol ebx,5 */
        fo += (bc_compile(&progs[i], c + fo, img_glob, 0, NULL, NULL, NULL) + 3) & ~3u;
    }
    c[o++] = 0x89; c[o++] = 0xD8; c[o++] = 0x5B; c[o++] = 0xC3;
    while (o < IMG_MAIN) c[o++] = 0xCC;
    im->size = fo;
    for (u32 a = 0; a < IMG_ARGS; a++) {
        u32 ebx = 0;
        for (u32 i = 0; i < IMG_FUNCS; i++) ebx = rol32(ebx ^ bc_interp(&progs[i], img_args[a], img_glob), 5);
        im->expect[a] = ebx;
    }
}

/* LZ77: control byte < 0x80 is a run of n+1 literals; otherwise a match of
 * (n & 0x7F) + 3 bytes at a 16-bit distance that follows. */
static u16 lz_head[1024];
static u32 lz_compress(const u8 *src, u32 n, u8 *dst) {
    u32 o = 0, i = 0, lit = 0;
    for (u32 h = 0; h < 1024; h++) lz_head[h] = 0xFFFF;
    while (i < n) {
        u32 best = 0, dist = 0;
        if (i + 3 <= n) {
            u32 h = ((src[i] << 6) ^ (src[i + 1] << 3) ^ src[i + 2]) & 1023;
            u32 cand = lz_head[h];
            lz_head[h] = (u16)i;
            if (cand != 0xFFFF) {
                u32 l = 0;
                while (i + l < n && l < 130 && src[cand + l] == src[i + l]) l++;
                if (l >= 3) { best = l; dist = i - cand; }
            }
        }
        if (best) {
            if (lit) { dst[o - lit - 1] = (u8)(lit - 1); lit = 0; }
            dst[o++] = (u8)(0x80 | (best - 3)); dst[o++] = (u8)dist; dst[o++] = (u8)(dist >> 8);
            i += best;
        } else {
            if (!lit) o++;
            dst[o++] = src[i++];
            if (++lit == 128) { dst[o - 129] = 127; lit = 0; }
        }
    }
    if (lit) dst[o - lit - 1] = (u8)(lit - 1);
    return o;
}
static u32 lz_expand(const u8 *src, u32 n, u8 *dst) {
    u32 i = 0, o = 0;
    while (i < n) {
        u32 c = src[i++];
        if (c < 0x80) { for (u32 k = 0; k <= c; k++) dst[o++] = src[i++]; }
        else {
            u32 len = (c & 0x7F) + 3, dist = src[i] | src[i + 1] << 8;
            i += 2;
            for (u32 k = 0; k < len; k++, o++) dst[o] = dst[o - dist];
        }
    }
    return o;
}

static void image_relocate(u8 *dst, const image_t *im) {
    for (u32 r = 0; r < im->nrel; r++) {
        u8 *p = dst + im->rel[r];
        put32(p, (p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24) + (u32)dst);
    }
}

static int images_setup(void) {
    u32 seed = TEST_SEED ^ 0x1A6E5u ^ (++invocation * 0x85EBCA6Bu);
    for (u32 i = 0; i < BC_NGLOB; i++) img_glob[i] = rw_rng(&seed);
    for (u32 a = 0; a < IMG_ARGS; a++) img_args[a] = rw_rng(&seed);
    for (u32 v = 0; v < IMG_VARIANTS; v++) {
        for (u32 i = 0; i < IMG_FUNCS; i++) bc_gen(&img_prog[v][i], &seed);
        image_build(&images[v], img_prog[v]);
        images[v].lzsize = lz_compress(images[v].img, images[v].size, images[v].lz);
        /* Round trip once in C, so the timed loader always sees valid streams. */
        u8 tmp[IMG_MAX];
        if (lz_expand(images[v].lz, images[v].lzsize, tmp) != images[v].size ||
            kmemcmp(tmp, images[v].img, images[v].size)) return 0;
    }
    return 1;
}

/* ---- rw_exe_loader: decompress a packed executable, apply its
 * relocations and run it, at changing load addresses ---- */
#define LOADS 1024
static u32 rw_exe_loader(void) {
    u8 *area = arena_alloc(32768, 4096);
    if (!area) SKIP("arena");
    CHECK(images_setup());
    static const u32 slots[6] = {0x0000, 0x0A30, 0x2004, 0x3FF0 - IMG_MAX / 2, 0x5550, 0x6800};
    u32 sum = 0, packed = 0, unpacked = 0;
    for (u32 r = 0; r < LOADS; r++) {
        image_t *im = &images[r % IMG_VARIANTS];
        u8 *base = area + slots[(r * 3 + r / 7) % 6];
        u32 n = lz_expand(im->lz, im->lzsize, base);
        CHECK_EQ(n, im->size);
        image_relocate(base, im);
        packed += im->lzsize; unpacked += n;
        for (u32 a = 0; a < IMG_ARGS; a++) {
            u32 got = call_code(base, img_args[a]);
            CHECK_EQ(got, im->expect[a]);
            sum = csum_mix(sum, got);
        }
    }
    t_note("packed=%u unpacked=%u", packed / LOADS, unpacked / LOADS);
    t_work_unit("image_load");
    t_ops(LOADS);
    return sum;
}

/* ---- rw_overlay: overlay manager swapping relocated images into one
 * region, with runs of calls between swaps ---- */
#define OVL_CALLS 8192
static u32 rw_overlay(void) {
    u8 *area = arena_alloc(8192 + IMG_VARIANTS * IMG_MAX, 4096);
    if (!area) SKIP("arena");
    CHECK(images_setup());
    u8 *region = area + 0x100, *store = area + 8192;
    for (u32 v = 0; v < IMG_VARIANTS; v++) {
        /* Stored already relocated for the region it is swapped into. */
        u8 *img = store + v * IMG_MAX;
        kmemcpy(img, images[v].img, images[v].size);
        for (u32 r = 0; r < images[v].nrel; r++) {
            u8 *p = img + images[v].rel[r];
            put32(p, (p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24) + (u32)region);
        }
    }
    u32 seed = TEST_SEED ^ invocation, resident = ~0u, swaps = 0, sum = 0, left = 0;
    for (u32 c = 0; c < OVL_CALLS; c++) {
        if (!left) {
            u32 v = rw_rng(&seed) % IMG_VARIANTS;
            left = 1 + rw_rng(&seed) % 12;
            if (v != resident) { kmemcpy(region, store + v * IMG_MAX, images[v].size); resident = v; swaps++; }
        }
        left--;
        u32 a = c % IMG_ARGS, got = call_code(region, img_args[a]);
        CHECK_EQ(got, images[resident].expect[a]);
        sum = csum_mix(sum, got);
    }
    t_note("swaps=%u", swaps);
    t_work_unit("overlay_call");
    t_ops(OVL_CALLS);
    return sum;
}

/* ---- rw_decrypt_run: decryptor stub that decodes the body behind it on
 * its own page, then falls into it; the body is re-encrypted with a new key
 * before every pass ---- */
#define CR_REG 8
#define CR_ROUNDS 2048
#define CR_STUB 24
static u32 rw_decrypt_run(void) {
    u8 *area = arena_alloc(8192, 4096);
    if (!area) SKIP("arena");
    u32 seed = TEST_SEED ^ 0xC0DEu ^ (++invocation * 0x27D4EB2Fu), glob[BC_NGLOB], args[4];
    prog_t prog[CR_REG];
    u8 plain[CR_REG][BC_CODE_MAX];
    u32 len[CR_REG], expect[CR_REG][4];
    for (u32 i = 0; i < BC_NGLOB; i++) glob[i] = rw_rng(&seed);
    for (u32 a = 0; a < 4; a++) args[a] = rw_rng(&seed);
    for (u32 r = 0; r < CR_REG; r++) {
        bc_gen(&prog[r], &seed);
        len[r] = bc_compile(&prog[r], plain[r], glob, 0, NULL, NULL, NULL);
        for (u32 a = 0; a < 4; a++) expect[r][a] = bc_interp(&prog[r], args[a], glob);
        u8 *s = area + r * 1024, *body = s + CR_STUB;
        s[0] = 0xB8; put32(s + 1, (u32)body);                 /* mov eax,body */
        s[5] = 0xB9; put32(s + 6, len[r]);                    /* mov ecx,len */
        s[10] = 0xBA;                                         /* mov edx,key */
        s[15] = 0x30; s[16] = 0x10;                           /* xor [eax],dl */
        s[17] = 0xC1; s[18] = 0xC2; s[19] = 0x03;             /* rol edx,3 */
        s[20] = 0x40; s[21] = 0x49; s[22] = 0x75; s[23] = 0xF7; /* inc eax; dec ecx; jnz */
        kmemcpy(body, plain[r], len[r]);
    }
    u32 sum = 0;
    for (u32 n = 0; n < CR_ROUNDS; n++) {
        u32 r = n % CR_REG, k = rw_rng(&seed);
        u8 *s = area + r * 1024, *body = s + CR_STUB;
        /* The body is plain after its last run; encrypt it under a new key. */
        for (u32 i = 0, kk = k; i < len[r]; i++, kk = rol32(kk, 3)) body[i] ^= (u8)kk;
        put32(s + 11, k);
        u32 a = n & 3, got = call_code(s, args[a]);
        CHECK_EQ(got, expect[r][a]);
        sum = csum_mix(sum, got);
        for (u32 j = 1; j < 4; j++) {
            got = call_code(body, args[(a + j) & 3]);
            CHECK_EQ(got, expect[r][(a + j) & 3]);
        }
    }
    for (u32 r = 0; r < CR_REG; r++) CHECK_EQ(kmemcmp(area + r * 1024 + CR_STUB, plain[r], len[r]), 0);
    t_work_unit("decrypt_pass");
    t_ops(CR_ROUNDS);
    return sum;
}

/* ---- rw_thunks: per-object call thunks (mov ecx,this; jmp method), as
 * window-procedure and callback thunks do, created and freed while others
 * on the same pages keep being called ---- */
#define TH_SLOTS 1024
#define TH_STEPS 2048
#define TH_CALLS 32
typedef struct { u32 id, kind; } obj_t;
static obj_t th_obj[TH_SLOTS];
static u32 th_ref(u32 kind, u32 id, u32 a) {
    switch (kind) {
    case 0: return id * 3 + a;
    case 1: return rol32(id ^ a, id & 31);
    case 2: return (id + 0x1234567u) * (a | 1);
    default: return ~id - a;
    }
}
static NOINLINE __attribute__((fastcall)) u32 th_m0(obj_t *o, u32 a) { return o->id * 3 + a; }
static NOINLINE __attribute__((fastcall)) u32 th_m1(obj_t *o, u32 a) { return rol32(o->id ^ a, o->id & 31); }
static NOINLINE __attribute__((fastcall)) u32 th_m2(obj_t *o, u32 a) { return (o->id + 0x1234567u) * (a | 1); }
static NOINLINE __attribute__((fastcall)) u32 th_m3(obj_t *o, u32 a) { return ~o->id - a; }
typedef u32 (__attribute__((fastcall)) *th_fn)(obj_t *, u32);
static const th_fn th_methods[4] = {th_m0, th_m1, th_m2, th_m3};
static void th_make(u8 *slot, obj_t *o) {
    slot[0] = 0xB9; put32(slot + 1, (u32)o);
    slot[5] = 0xE9; put32(slot + 6, (u32)th_methods[o->kind] - (u32)(slot + 10));
}
static NOINLINE u32 th_call(u8 *slot, u32 a) { rw_sync(); return ((th_fn)slot)(NULL, a); }
static u32 rw_thunks(void) {
    u8 *pool = arena_alloc(TH_SLOTS * 16, 4096);
    if (!pool) SKIP("arena");
    u32 seed = TEST_SEED ^ 0x7417u ^ (++invocation * 0xC2B2AE35u), sum = 0;
    for (u32 i = 0; i < TH_SLOTS * 16; i++) pool[i] = 0xCC;
    for (u32 i = 0; i < TH_SLOTS; i++) {
        th_obj[i].id = rw_rng(&seed); th_obj[i].kind = rw_rng(&seed) & 3;
        th_make(pool + i * 16, &th_obj[i]);
    }
    for (u32 s = 0; s < TH_STEPS; s++) {
        /* Calls cluster around a moving window, as event handling does. */
        u32 centre = rw_rng(&seed) % TH_SLOTS;
        for (u32 c = 0; c < TH_CALLS; c++) {
            u32 i = (centre + rw_rng(&seed) % 64) % TH_SLOTS, a = rw_rng(&seed);
            u32 got = th_call(pool + i * 16, a);
            CHECK_EQ(got, th_ref(th_obj[i].kind, th_obj[i].id, a));
            sum = csum_mix(sum, got);
        }
        for (u32 k = 0; k < 2; k++) {
            u32 i = (centre + rw_rng(&seed) % 64) % TH_SLOTS;
            th_obj[i].id = rw_rng(&seed); th_obj[i].kind = rw_rng(&seed) & 3;
            th_make(pool + i * 16, &th_obj[i]);
        }
    }
    t_work_unit("thunk_call");
    t_ops(TH_STEPS * TH_CALLS);
    return sum;
}

/* ---- rw_threaded_vm: register bytecode run by a computed-goto
 * interpreter (one indirect jump per instruction), checked against a
 * switch interpreter ---- */
enum { VM_LI, VM_ADD, VM_XOR, VM_ROL, VM_MUL, VM_LD, VM_ST, VM_DJNZ, VM_HALT };
typedef struct { u8 op, a, b, c; u32 imm; } vmins_t;
#define VM_BODY 28
#define VM_MEM 256
static vmins_t vm_prog[VM_BODY + 8];
static u32 vm_mem_ref[VM_MEM], vm_mem[VM_MEM];

static u32 vm_switch(const vmins_t *p, u32 *r, u32 *m) {
    u32 pc = 0, n = 0;
    for (;;) {
        const vmins_t *i = &p[pc++];
        n++;
        switch (i->op) {
        case VM_LI: r[i->a] = i->imm; break;
        case VM_ADD: r[i->a] += r[i->b]; break;
        case VM_XOR: r[i->a] ^= r[i->b]; break;
        case VM_ROL: r[i->a] = rol32(r[i->a], i->imm); break;
        case VM_MUL: r[i->a] *= r[i->b] | 1; break;
        case VM_LD: r[i->a] = m[r[i->b] % VM_MEM]; break;
        case VM_ST: m[r[i->b] % VM_MEM] = r[i->a]; break;
        case VM_DJNZ: if (--r[i->a]) pc = i->imm; break;
        default: return n;
        }
    }
}
static NOINLINE u32 vm_threaded(const vmins_t *p, u32 *r, u32 *m) {
    static void *const tbl[] = {&&li, &&add, &&xr, &&rl, &&mul, &&ld, &&st, &&djnz, &&halt};
    const vmins_t *i = p;
    u32 n = 0;
#define NEXT do { n++; goto *tbl[i->op]; } while (0)
    NEXT;
li: r[i->a] = i->imm; i++; NEXT;
add: r[i->a] += r[i->b]; i++; NEXT;
xr: r[i->a] ^= r[i->b]; i++; NEXT;
rl: r[i->a] = rol32(r[i->a], i->imm); i++; NEXT;
mul: r[i->a] *= r[i->b] | 1; i++; NEXT;
ld: r[i->a] = m[r[i->b] % VM_MEM]; i++; NEXT;
st: m[r[i->b] % VM_MEM] = r[i->a]; i++; NEXT;
djnz: i = --r[i->a] ? p + i->imm : i + 1; NEXT;
halt: return n;
#undef NEXT
}
static u32 rw_threaded_vm(void) {
    u32 seed = TEST_SEED ^ 0x7A11u ^ (++invocation * 0x165667B1u), n = 0, sum = 0;
    /* r7 counts the outer loop, r6 the inner one; the body uses r0..r5. */
    u32 o = 0;
    vm_prog[o++] = (vmins_t){VM_LI, 7, 0, 0, 24};
    vm_prog[o++] = (vmins_t){VM_LI, 6, 0, 0, 96};
    u32 inner = o;
    for (u32 k = 0; k < VM_BODY; k++) {
        u32 r = rw_rng(&seed), op = VM_LI + r % 7;
        vm_prog[o++] = (vmins_t){(u8)op, (u8)((r >> 4) % 6), (u8)((r >> 8) % 6), 0, op == VM_ROL ? 1 + (r >> 12) % 31 : rw_rng(&seed)};
    }
    vm_prog[o++] = (vmins_t){VM_DJNZ, 6, 0, 0, inner};
    vm_prog[o++] = (vmins_t){VM_LI, 6, 0, 0, 96};
    vm_prog[o++] = (vmins_t){VM_DJNZ, 7, 0, 0, inner};
    vm_prog[o++] = (vmins_t){VM_HALT, 0, 0, 0, 0};
    u32 rr[8] = {0}, rt[8] = {0};
    for (u32 i = 0; i < VM_MEM; i++) vm_mem_ref[i] = rw_rng(&seed);
    kmemcpy(vm_mem, vm_mem_ref, sizeof(vm_mem));
    u32 nref = vm_switch(vm_prog, rr, vm_mem_ref);
    for (u32 pass = 0; pass < 4; pass++) {
        u32 got = vm_threaded(vm_prog, rt, vm_mem);
        if (!pass) { CHECK_EQ(got, nref); for (u32 i = 0; i < 6; i++) CHECK_EQ(rt[i], rr[i]); CHECK_EQ(kmemcmp(vm_mem, vm_mem_ref, sizeof(vm_mem)), 0); }
        n += got;
        for (u32 i = 0; i < 6; i++) sum = csum_mix(sum, rt[i]);
    }
    t_work_unit("vm_instruction");
    t_ops(n);
    return sum;
}

const test_t tests_realworld[] = {
    {"rw_jit_tiering", rw_jit_tiering, REQ_486, 0},
    {"rw_exe_loader", rw_exe_loader, REQ_486, 0},
    {"rw_overlay", rw_overlay, REQ_486, 0},
    {"rw_decrypt_run", rw_decrypt_run, REQ_486, 0},
    {"rw_thunks", rw_thunks, REQ_486, 0},
    {"rw_threaded_vm", rw_threaded_vm, REQ_486, 0},
    {NULL, NULL, 0, 0},
};
