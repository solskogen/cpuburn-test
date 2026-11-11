/* Integer conformance tests. Undefined architectural flags are deliberately masked. */
#include "kernel.h"

#define CF 0x001u
#define PF 0x004u
#define AF 0x010u
#define ZF 0x040u
#define SF 0x080u
#define OF 0x800u
#define ARITH (CF|PF|AF|ZF|SF|OF)
#define LOGIC (CF|PF|ZF|SF|OF)
#define NOINLINE __attribute__((noinline))
#if __has_attribute(noclone)
#define NOCLONE __attribute__((noclone))
#else
#define NOCLONE
#endif
static u32 rng(u32 *s) { u32 x=*s; x^=x<<13; x^=x>>17; x^=x<<5; return *s=x; }
static u32 maskof(unsigned w) { return w==32 ? ~0u : (1u<<w)-1; }
static u32 szp(u32 x, unsigned w) {
    u32 p=x&255; p^=p>>4; p&=15;
    return (!x?ZF:0) | ((x>>(w-1))?SF:0) | ((0x9669u>>p&1)?PF:0);
}
enum { A_ADD,A_ADC,A_SUB,A_SBB,A_CMP,A_AND,A_OR,A_XOR,A_TEST,A_NEG,A_NOT,A_INC,A_DEC };
#define BINARY_CASE(N,OP,S,R) case N: __asm__ volatile("btl $0,%3; " OP S " " R "2," R "0; pushfl; popl %1" : "+a"(a), "=m"(*f) : "d"(b), "r"(cin) : "cc", "memory"); break
#define UNARY_CASE(N,OP,S,R) case N: __asm__ volatile("btl $0,%2; " OP S " " R "0; pushfl; popl %1" : "+a"(a), "=m"(*f) : "r"(cin) : "cc", "memory"); break
#define ALU_FN(W,S,R) \
static NOINLINE u32 alu##W(unsigned op,u32 a,u32 b,u32 cin,u32 *f) { \
 switch(op) { \
 BINARY_CASE(A_ADD,"add",S,R); BINARY_CASE(A_ADC,"adc",S,R); \
 BINARY_CASE(A_SUB,"sub",S,R); BINARY_CASE(A_SBB,"sbb",S,R); \
 BINARY_CASE(A_CMP,"cmp",S,R); BINARY_CASE(A_AND,"and",S,R); \
 BINARY_CASE(A_OR,"or",S,R); BINARY_CASE(A_XOR,"xor",S,R); BINARY_CASE(A_TEST,"test",S,R); \
 UNARY_CASE(A_NEG,"neg",S,R); UNARY_CASE(A_NOT,"not",S,R); \
 UNARY_CASE(A_INC,"inc",S,R); UNARY_CASE(A_DEC,"dec",S,R); \
 } return a&maskof(W); }
ALU_FN(8,"b","%b")
ALU_FN(16,"w","%w")
ALU_FN(32,"l","%")
static u32 alu_ref(unsigned op,u32 a,u32 b,u32 cin,unsigned w,u32 *flags) {
    u32 m=maskof(w),sign=1u<<(w-1),r,cf=0,ov=0,af=0,keep=0;
    a&=m; b&=m;
    if(op==A_NOT) { *flags=0; return (~a)&m; }
    if(op==A_NEG) { b=a; a=0; op=A_SUB; }
    if(op==A_INC || op==A_DEC) { keep=cin; cin=0; b=1; op=op==A_INC?A_ADD:A_SUB; keep|=2; }
    if(op==A_ADD || op==A_ADC) {
        if(op==A_ADD) cin=0;
        r=(a+b+cin)&m; cf=(r<a || (cin && r==a));
        ov=(~(a^b)&(a^r)&sign)!=0; af=((a^b^r)&16)!=0;
    } else if(op==A_SUB || op==A_SBB || op==A_CMP) {
        if(op!=A_SBB) cin=0;
        r=(a-b-cin)&m; cf=a<b || (cin && a==b);
        ov=((a^b)&(a^r)&sign)!=0; af=((a^b^r)&16)!=0;
    } else if(op==A_AND || op==A_TEST) r=a&b;
    else if(op==A_OR) r=a|b;
    else r=a^b;
    if(keep&2) cf=keep&1;
    *flags=szp(r,w) | cf | (ov?OF:0) | (af?AF:0);
    return (op==A_CMP || op==A_TEST)?a:r;
}
static u32 run_alu(unsigned w,unsigned op,u32 a,u32 b,u32 c,u32 *f) {
    return w==8?alu8(op,a,b,c,f):w==16?alu16(op,a,b,c,f):alu32(op,a,b,c,f);
}
/* CF read back by SETC, JC and ADC right after the instruction that set it:
   logic ops, INC and DEC (which keep it), rotates and shifts by constant
   counts (some past the width), and 8- and 16-bit ADC and SBB with the
   carry in set or clear; each against PUSHF after the same instruction. */
static u32 int_cf_consumers(void) {
    static const u32 edge[]={0,1,0x7f,0x80,0xff,0x7fff,0x8000,0xffff,0x7fffffffu,0x80000000u,0xfffffffeu,0xffffffffu};
    u32 seed=0xcf5e7c0du,sum=0;
    const unsigned rounds=1u<<10;
    for(unsigned i=0;i<rounds;i++) {
        u32 a=rng(&seed),b=rng(&seed),r=rng(&seed);
        if(r&1) a=edge[(r>>1)%12];
        if(r&0x20) b=edge[(r>>6)%12];
        u32 cin=(r>>12)&1;
#define CFT(P) do { \
            u32 x,y=b,z,f; u8 o1,o2; \
            x=a; __asm__ volatile("btl $0,%[c]; " P "; setc %[o]" : [x]"+q"(x),[o]"=q"(o1) : [y]"q"(y),[c]"r"(cin) : "cc"); \
            x=a; __asm__ volatile("btl $0,%[c]; " P "; jc 1f; movb $0,%[o]; jmp 2f; 1: movb $1,%[o]; 2:" : [x]"+q"(x),[o]"=q"(o2) : [y]"q"(y),[c]"r"(cin) : "cc"); \
            x=a; __asm__ volatile("btl $0,%[c]; " P "; movl $0,%[z]; adcl $0,%[z]" : [x]"+q"(x),[z]"=&r"(z) : [y]"q"(y),[c]"r"(cin) : "cc"); \
            x=a; __asm__ volatile("btl $0,%[c]; " P "; pushfl; popl %[f]" : [x]"+q"(x),[f]"=r"(f) : [y]"q"(y),[c]"r"(cin) : "cc"); \
            CHECK_EQ(o1,f&1); CHECK_EQ(o2,f&1); CHECK_EQ(z,f&1); \
            sum=csum_mix(sum,x^(f&0x8d5)); \
        } while(0)
        CFT("andb %b[y],%b[x]");
        CFT("orb %b[y],%b[x]");
        CFT("xorb %b[y],%b[x]");
        CFT("testb %b[y],%b[x]");
        CFT("incb %b[x]");
        CFT("decb %b[x]");
        CFT("rolb $1,%b[x]");
        CFT("rolb $3,%b[x]");
        CFT("rolb $7,%b[x]");
        CFT("rorb $1,%b[x]");
        CFT("rorb $3,%b[x]");
        CFT("rorb $7,%b[x]");
        CFT("shlb $1,%b[x]");
        CFT("shlb $3,%b[x]");
        CFT("shlb $7,%b[x]");
        CFT("shlb $8,%b[x]");
        CFT("shlb $9,%b[x]");
        CFT("shrb $1,%b[x]");
        CFT("shrb $3,%b[x]");
        CFT("shrb $7,%b[x]");
        CFT("shrb $8,%b[x]");
        CFT("shrb $9,%b[x]");
        CFT("sarb $1,%b[x]");
        CFT("sarb $3,%b[x]");
        CFT("sarb $7,%b[x]");
        CFT("sarb $8,%b[x]");
        CFT("sarb $9,%b[x]");
        CFT("adcb %b[y],%b[x]");
        CFT("sbbb %b[y],%b[x]");
        CFT("andw %w[y],%w[x]");
        CFT("orw %w[y],%w[x]");
        CFT("xorw %w[y],%w[x]");
        CFT("testw %w[y],%w[x]");
        CFT("incw %w[x]");
        CFT("decw %w[x]");
        CFT("rolw $1,%w[x]");
        CFT("rolw $3,%w[x]");
        CFT("rolw $15,%w[x]");
        CFT("rorw $1,%w[x]");
        CFT("rorw $3,%w[x]");
        CFT("rorw $15,%w[x]");
        CFT("shlw $1,%w[x]");
        CFT("shlw $3,%w[x]");
        CFT("shlw $15,%w[x]");
        CFT("shlw $16,%w[x]");
        CFT("shlw $17,%w[x]");
        CFT("shrw $1,%w[x]");
        CFT("shrw $3,%w[x]");
        CFT("shrw $15,%w[x]");
        CFT("shrw $16,%w[x]");
        CFT("shrw $17,%w[x]");
        CFT("sarw $1,%w[x]");
        CFT("sarw $3,%w[x]");
        CFT("sarw $15,%w[x]");
        CFT("sarw $16,%w[x]");
        CFT("sarw $17,%w[x]");
        CFT("adcw %w[y],%w[x]");
        CFT("sbbw %w[y],%w[x]");
        CFT("andl %[y],%[x]");
        CFT("orl %[y],%[x]");
        CFT("xorl %[y],%[x]");
        CFT("testl %[y],%[x]");
        CFT("incl %[x]");
        CFT("decl %[x]");
        CFT("roll $1,%[x]");
        CFT("roll $3,%[x]");
        CFT("roll $31,%[x]");
        CFT("rorl $1,%[x]");
        CFT("rorl $3,%[x]");
        CFT("rorl $31,%[x]");
        CFT("shll $1,%[x]");
        CFT("shll $3,%[x]");
        CFT("shll $31,%[x]");
        CFT("shrl $1,%[x]");
        CFT("shrl $3,%[x]");
        CFT("shrl $31,%[x]");
        CFT("sarl $1,%[x]");
        CFT("sarl $3,%[x]");
        CFT("sarl $31,%[x]");
#undef CFT
    }
    t_ops(rounds*79*4); return sum;
}
/* The carry an ADC or SBB leaves, read through an INC or DEC after it
   (which keeps CF) and SETC, at each width, with the carry in set or clear
   and operands at the edges of the range. */
static u32 int_adc_carry_incdec(void) {
    static const u32 edge[]={0,1,0x7f,0x80,0xff,0x7fff,0x8000,0xffff,0x7fffffffu,0x80000000u,0xfffffffeu,0xffffffffu};
    u32 seed=0xadcc0de5u,sum=0;
    const unsigned rounds=1u<<12;
    for(unsigned i=0;i<rounds;i++) {
        u32 a=rng(&seed),b=rng(&seed),r=rng(&seed);
        if(r&1) a=edge[(r>>1)%12];
        if(r&0x20) b=edge[(r>>6)%12];
        u32 cin=(r>>12)&1,d=r;
        for(unsigned w=8;w<=32;w*=2) {
            u32 m=w==32?0xffffffffu:((1u<<w)-1);
            u32 want[2]={(u32)(((u64)(a&m)+(b&m)+cin)>>w)&1,((u64)(a&m)<(u64)(b&m)+cin)?1:0};
            for(unsigned k=0;k<4;k++) {
                u32 x=a; u8 c;
#define ADCC(OP,ID) __asm__ volatile("btl $0,%[c]; " OP "; " ID " %[d]; setc %[o]" : [x]"+q"(x),[d]"+r"(d),[o]"=q"(c) : [b]"q"(b),[c]"r"(cin) : "cc")
                if(w==8) { if(k==0) ADCC("adcb %b[b],%b[x]","decl"); else if(k==1) ADCC("adcb %b[b],%b[x]","incl"); else if(k==2) ADCC("sbbb %b[b],%b[x]","decl"); else ADCC("sbbb %b[b],%b[x]","incl"); }
                else if(w==16) { if(k==0) ADCC("adcw %w[b],%w[x]","decl"); else if(k==1) ADCC("adcw %w[b],%w[x]","incl"); else if(k==2) ADCC("sbbw %w[b],%w[x]","decl"); else ADCC("sbbw %w[b],%w[x]","incl"); }
                else { if(k==0) ADCC("adcl %[b],%[x]","decl"); else if(k==1) ADCC("adcl %[b],%[x]","incl"); else if(k==2) ADCC("sbbl %[b],%[x]","decl"); else ADCC("sbbl %[b],%[x]","incl"); }
#undef ADCC
                CHECK_EQ(c,want[k>>1]);
                sum=csum_mix(sum,x^c);
            }
        }
    }
    t_ops(rounds*3*2*4); return sum;
}
/* A bit-stream reader's shift, as Windows 98's 16-bit code does it as it
   boots: DX:AX (or EDX:EAX) shifted left by CL one bit at a time with ADD
   and ADC, each ADC taking the carry the ADD before it left. */
static u32 int_adc_shift_loop(void) {
    u32 sum=0,seed=0xadc5f17u;
    const unsigned rounds=1u<<13;
    for(unsigned i=0;i<rounds;i++) {
        u32 a=rng(&seed),d=rng(&seed),r=rng(&seed),ra=a,rd=d;
        /* 32 shifts of 1-15 bits each, as a reader pulls codes off the stream */
        if(r&0x100) {
            u64 v=((u64)d<<32)|a;
            for(unsigned k=0;k<32;k++) {
                u32 c=1+((r>>(k&15))%15),n=c;
                __asm__ volatile("orb %%cl,%%cl; jz 2f; 1: addl %%eax,%%eax; adcl %%edx,%%edx; decb %%cl; jnz 1b; 2:"
                                 : "+a"(ra),"+d"(rd),"+c"(n) :: "cc");
                v<<=c;
            }
            CHECK_EQ(ra,(u32)v); CHECK_EQ(rd,(u32)(v>>32));
        } else {
            u32 v=((d&0xffff)<<16)|(a&0xffff);
            for(unsigned k=0;k<32;k++) {
                u32 c=1+((r>>(k&15))%15),n=c;
                __asm__ volatile("orb %%cl,%%cl; jz 2f; 1: addw %%ax,%%ax; adcw %%dx,%%dx; decb %%cl; jnz 1b; 2:"
                                 : "+a"(ra),"+d"(rd),"+c"(n) :: "cc");
                v<<=c;
            }
            CHECK_EQ(ra,(a&0xffff0000u)|(v&0xffff)); CHECK_EQ(rd,(d&0xffff0000u)|(v>>16));
        }
        sum=csum_mix(sum,ra^rd);
    }
    t_ops(rounds*32*8*4); return sum;
}
/* A copy through far pointers, as Windows 98's 16-bit decompressor does it
   as it boots: each byte loads both pointers with LES and puts ES back from
   memory, six segment register loads a byte, and steps the offsets. */
static u32 int_far_ptr_copy(void) {
    u8 *src=arena_alloc(8192,16),*dst=arena_alloc(8192,16);
    if(!src || !dst) SKIP("arena_far_ptr_copy");
    gdt_set(GDT_SCRATCH_FIRST+2,(u32)src,8191,0x92,0x4);
    gdt_set(GDT_SCRATCH_FIRST+3,(u32)dst,8191,0x92,0x4);
    static volatile struct __attribute__((packed)) { u32 off; u16 sel; } sp,dp;
    static volatile u16 es_save;
    u32 sum=0,seed=0xfa2c0b1u;
    const unsigned rounds=256;
    for(unsigned r=0;r<rounds;r++) {
        u32 n=1024+(rng(&seed)&3071),so=rng(&seed)&4095,dof=rng(&seed)&4095,cnt=n;
        for(unsigned i=0;i<n;i++) src[so+i]=(u8)rng(&seed);
        sp.off=so; sp.sel=(GDT_SCRATCH_FIRST+2)<<3;
        dp.off=dof; dp.sel=(GDT_SCRATCH_FIRST+3)<<3;
        __asm__ volatile("movw %%es,%0\n"
                         "1: lesl %1,%%ebx; movb %%es:(%%ebx),%%al; movw %0,%%es\n"
                         "lesl %2,%%ebx; movb %%al,%%es:(%%ebx); movw %0,%%es\n"
                         "lesl %1,%%ebx; movw %0,%%es\n"
                         "addl $1,%1; sbbl %%edx,%%edx; addl $1,%2; adcl $0,%%edx\n"
                         "decl %%ecx; jnz 1b"
                         : "+m"(es_save),"+m"(sp),"+m"(dp),"+c"(cnt) :: "eax","ebx","edx","memory","cc");
        CHECK_EQ(kmemcmp(dst+dof,src+so,n),0);
        sum=csum_mix(sum,dst[dof]^n);
    }
    t_ops(rounds*2048*12); return sum;
}
/* SHA-1 as Windows 98 runs it as it first boots, checking file signatures:
   the message schedule's byte swap and expansion loops are its own, two
   words a pass, each pass one trip round a short loop. */
extern void sha1_sched(u32 *w,const u32 *blk);
__asm__(".text\n"
".type sha1_sched,@function\nsha1_sched:\n"
"pushl %ebp\n pushl %esi\n pushl %edi\n pushl %ebx\n"
"movl 24(%esp),%edx\n movl 20(%esp),%ecx\n pushl %ecx\n"
"movl $16,%esi\n movl $0xff00ff00,%edi\n movl $0xff00ff,%ebp\n leal -4(%ecx),%ecx\n"
"1: movl (%edx),%eax\n addl $4,%edx\n movl %eax,%ebx\n andl %edi,%eax\n andl %ebp,%ebx\n addl $4,%ecx\n"
"roll $8,%eax\n rorl $8,%ebx\n xorl %ebx,%eax\n decl %esi\n movl %eax,(%ecx)\n jne 1b\n"
"popl %ecx\n leal 8(%ecx),%ecx\n movl $32,%edx\n"
"2: movl -8(%ecx),%eax\n movl -4(%ecx),%ebx\n movl (%ecx),%esi\n movl 4(%ecx),%edi\n xorl %esi,%eax\n xorl %edi,%ebx\n"
"movl 0x18(%ecx),%esi\n movl 0x1c(%ecx),%edi\n xorl %esi,%eax\n xorl %edi,%ebx\n"
"movl 0x2c(%ecx),%esi\n movl 0x30(%ecx),%edi\n xorl %esi,%eax\n addl $8,%ecx\n roll $1,%eax\n xorl %edi,%ebx\n roll $1,%ebx\n"
"movl %eax,0x30(%ecx)\n decl %edx\n movl %ebx,0x34(%ecx)\n jne 2b\n"
"popl %ebx\n popl %edi\n popl %esi\n popl %ebp\n ret\n");
static inline u32 rol32(u32 x,unsigned n) { return (x<<n)|(x>>(32-n)); }
static void sha1_block(u32 *h,const u32 *blk,int ref) {
    u32 w[80];
    if(ref) {
        for(unsigned i=0;i<16;i++) { u32 v=blk[i]; w[i]=(v>>24)|((v>>8)&0xff00)|((v<<8)&0xff0000)|(v<<24); }
        for(unsigned i=16;i<80;i++) w[i]=rol32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    } else sha1_sched(w,blk);
    u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],t;
    for(unsigned i=0;i<20;i++) { t=rol32(a,5)+((b&c)|(~b&d))+e+0x5a827999+w[i]; e=d; d=c; c=rol32(b,30); b=a; a=t; }
    for(unsigned i=20;i<40;i++) { t=rol32(a,5)+(b^c^d)+e+0x6ed9eba1+w[i]; e=d; d=c; c=rol32(b,30); b=a; a=t; }
    for(unsigned i=40;i<60;i++) { t=rol32(a,5)+((b&c)|(b&d)|(c&d))+e+0x8f1bbcdc+w[i]; e=d; d=c; c=rol32(b,30); b=a; a=t; }
    for(unsigned i=60;i<80;i++) { t=rol32(a,5)+(b^c^d)+e+0xca62c1d6+w[i]; e=d; d=c; c=rol32(b,30); b=a; a=t; }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}
static void sha1_init(u32 *h) { h[0]=0x67452301; h[1]=0xefcdab89; h[2]=0x98badcfe; h[3]=0x10325476; h[4]=0xc3d2e1f0; }
static u32 int_sha1(void) {
    const unsigned blocks=4096;
    u32 *buf=arena_alloc(blocks*64,16);
    if(!buf) SKIP("arena_sha1");
    /* "abc", padded: the known digest a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d */
    static const u32 abc[16]={0x80636261,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x18000000};
    u32 h[5],hr[5],seed=0x5a1f00du;
    sha1_init(h); sha1_block(h,abc,0);
    CHECK_EQ(h[0],0xa9993e36); CHECK_EQ(h[1],0x4706816a); CHECK_EQ(h[2],0xba3e2571); CHECK_EQ(h[3],0x7850c26c); CHECK_EQ(h[4],0x9cd0d89d);
    for(unsigned i=0;i<blocks*16;i++) buf[i]=rng(&seed);
    sha1_init(h); sha1_init(hr);
    for(unsigned i=0;i<blocks;i++) sha1_block(h,buf+i*16,0);
    for(unsigned i=0;i<blocks/16;i++) sha1_block(hr,buf+i*16,1);
    /* the reference pass covers the first sixteenth; its digest must match a fresh fast pass over the same */
    u32 hf[5]; sha1_init(hf);
    for(unsigned i=0;i<blocks/16;i++) sha1_block(hf,buf+i*16,0);
    for(unsigned i=0;i<5;i++) CHECK_EQ(hf[i],hr[i]);
    t_ops(blocks*1800); return h[0]^h[1]^h[2]^h[3]^h[4];
}
/* Bignum arithmetic as Windows 98 runs it as it first boots, checking
   signatures with RSA: a multiply-accumulate row four words to a pass, each
   word MUL, ADD and two ADC $0 that take the carries into the high half. */
extern u32 bn_mul_add4(u32 *r,const u32 *a,u32 n,u32 w);
extern u32 bn_mul_sub4(u32 *r,const u32 *a,u32 n,u32 w);
#define BN_STEP(o,OP) "movl " o "(%esi,%ebp,4),%eax\n mull %ebx\n addl %eax,%ecx\n movl " o "(%edi,%ebp,4),%eax\n adcl $0,%edx\n" \
                      OP " %ecx,%eax\n adcl $0,%edx\n movl %eax," o "(%edi,%ebp,4)\n movl %edx,%ecx\n"
#define BN_FN(name,OP) \
".type " name ",@function\n" name ":\n pushl %ebp\n pushl %esi\n pushl %edi\n pushl %ebx\n" \
"movl 20(%esp),%edi\n movl 24(%esp),%esi\n movl 32(%esp),%ebx\n xorl %ecx,%ecx\n xorl %ebp,%ebp\n" \
"1: " BN_STEP("0",OP) BN_STEP("4",OP) BN_STEP("8",OP) BN_STEP("12",OP) \
"addl $4,%ebp\n cmpl 28(%esp),%ebp\n jb 1b\n movl %ecx,%eax\n popl %ebx\n popl %edi\n popl %esi\n popl %ebp\n ret\n"
__asm__(".text\n" BN_FN("bn_mul_add4","addl") BN_FN("bn_mul_sub4","subl"));
static u32 bn_ref(u32 *r,const u32 *a,u32 n,u32 w,int sub) {
    u32 c=0;
    for(u32 i=0;i<n;i++) {
        u64 p=(u64)a[i]*w+c;
        if(sub) { u32 lo=(u32)p; c=(u32)(p>>32)+(r[i]<lo); r[i]-=lo; }
        else { u64 s=p+r[i]; r[i]=(u32)s; c=(u32)(s>>32); }
    }
    return c;
}
static u32 int_bn_muladd(void) {
    enum { N=32 };
    static u32 a[N],b[N],r[2*N],rr[2*N];
    u32 sum=0,seed=0xb16a0d0u;
    const unsigned rounds=512;
    for(unsigned k=0;k<rounds;k++) {
        for(unsigned i=0;i<N;i++) { a[i]=rng(&seed); b[i]=rng(&seed); }
        if(k&1) for(unsigned i=0;i<2*N;i++) r[i]=rr[i]=rng(&seed);
        else for(unsigned i=0;i<2*N;i++) r[i]=rr[i]=0;
        /* a product row by row, or (odd rounds) one taken off r, as a reduction does */
        for(unsigned i=0;i<N;i++) {
            u32 c=(k&1)?bn_mul_sub4(r+i,a,N,b[i]):bn_mul_add4(r+i,a,N,b[i]);
            u32 cr=bn_ref(rr+i,a,N,b[i],k&1);
            CHECK_EQ(c,cr);
            if(k&1) { r[i+N]-=c; rr[i+N]-=cr; } else { r[i+N]=c; rr[i+N]=cr; }
        }
        for(unsigned i=0;i<2*N;i++) CHECK_EQ(r[i],rr[i]);
        sum=csum_mix(sum,r[N]^r[2*N-1]);
    }
    t_ops(rounds*N*N*9); return sum;
}
/* The same with DS, ES and SS as Windows 98 gives a Win32 program: base 0,
   expand-down, big, the first page left out so that a null pointer faults.
   Every access then has a lower bound to pass. */
static u32 with_xdown_segs(u32 (*fn)(void)) {
    const u16 sel=(GDT_SCRATCH_FIRST+4)<<3;
    u16 ods,oes,oss;
    gdt_set(GDT_SCRATCH_FIRST+4,0,0xfff,0x96,0x4);
    __asm__ volatile("movw %%ds,%0; movw %%es,%1; movw %%ss,%2" : "=m"(ods),"=m"(oes),"=m"(oss));
    __asm__ volatile("movw %0,%%ds; movw %0,%%es; movw %0,%%ss" :: "r"(sel) : "memory");
    u32 r=fn();
    __asm__ volatile("movw %0,%%ds; movw %1,%%es; movw %2,%%ss" :: "m"(ods),"m"(oes),"m"(oss) : "memory");
    return r;
}
static u32 int_sha1_xdown(void) { return with_xdown_segs(int_sha1); }
static u32 int_bn_muladd_xdown(void) { return with_xdown_segs(int_bn_muladd); }
/* A large executable footprint, compiled once and run again and again, as
   Windows 98's first boot runs its drivers and setup code: many distinct
   short functions (loads and stores over 64 KiB of data, moves, ALU, LEA,
   PUSH/POP, a shift, INC and a skipped INC) in a fixed shuffled order, so the host code for them
   all is in use at once. Chained, each ends with a direct JMP to the next,
   as straight-line code reaches the next; called, each is reached by its
   own CALL through a table and returns, as through a dispatch table. */
extern void cf_run(u32 *st,u32 *data,void **table,u32 n);
__asm__(".text\n"
".type cf_run,@function\ncf_run:\n"
"pushl %ebp\n pushl %esi\n pushl %edi\n pushl %ebx\n"
"movl 20(%esp),%ecx\n movl 24(%esp),%esi\n movl 28(%esp),%edi\n movl 32(%esp),%ebp\n"
"pushl %ecx\n movl (%ecx),%eax\n movl 4(%ecx),%ebx\n movl 8(%ecx),%edx\n"
"1: call *(%edi)\n addl $4,%edi\n decl %ebp\n jnz 1b\n"
"popl %ecx\n movl %eax,(%ecx)\n movl %ebx,4(%ecx)\n movl %edx,8(%ecx)\n"
"popl %ebx\n popl %edi\n popl %esi\n popl %ebp\n ret\n");
enum { CF_FN=64, CF_DATA=16384 };
/* One function's code from seed, built at code to run at at, ending with a
   JMP to next (a RET if next is NULL); or, code NULL, its effect on st and
   data */
static void cf_gen(u32 seed,u8 *code,const u8 *at,const u8 *next,u32 *st,u32 *data) {
    /* A mix like compiled code's: loads and stores, register moves, ALU,
       LEA, a byte load, PUSH/POP, a shift, INC and a skipped INC */
    static const u8 mix[16]={0,0,0,1,1,2,3,4,5,6,7,8,9,10,11,12};
    static const u8 len[13]={6,6,6,2,5,7,2,2,5,2,7,3,1};
    u32 p=0,a=st?st[0]:0,b=st?st[1]:0,d=st?st[2]:0;
    for(;;) {
        u32 r=rng(&seed),imm=rng(&seed),off=(imm%CF_DATA)*4,k=1+(r>>8)%31;
        unsigned op=mix[r%16];
        if(p+len[op]>CF_FN-5) break;
        if(code) {
            u8 *c=code+p;
            switch(op) {
            case 0: c[0]=0x8b; c[1]=0x86; kmemcpy(c+2,&off,4); break;               /* mov eax,[esi+off] */
            case 1: c[0]=0x89; c[1]=0x86; kmemcpy(c+2,&off,4); break;               /* mov [esi+off],eax */
            case 2: c[0]=0x03; c[1]=0x9e; kmemcpy(c+2,&off,4); break;               /* add ebx,[esi+off] */
            case 3: c[0]=0x89; c[1]=0xda; break;                                    /* mov edx,ebx */
            case 4: c[0]=0x05; kmemcpy(c+1,&imm,4); break;                          /* add eax,imm */
            case 5: c[0]=0x8d; c[1]=0x94; c[2]=0x58; kmemcpy(c+3,&imm,4); break;    /* lea edx,[eax+ebx*2+imm] */
            case 6: c[0]=0x29; c[1]=0xc2; break;                                    /* sub edx,eax */
            case 7: c[0]=0x31; c[1]=0xd0; break;                                    /* xor eax,edx */
            case 8: c[0]=0xa8; c[1]=0x01; c[2]=0x74; c[3]=0x01; c[4]=0x43; break;   /* test al,1; jz; inc ebx */
            case 9: c[0]=0x50; c[1]=0x5b; break;                                    /* push eax; pop ebx */
            case 10: c[0]=0x0f; c[1]=0xb6; c[2]=0x9e; kmemcpy(c+3,&off,4); break;   /* movzx ebx,byte [esi+off] */
            case 11: c[0]=0xc1; c[1]=0xe0; c[2]=k; break;                           /* shl eax,k */
            case 12: c[0]=0x40; break;                                              /* inc eax */
            }
        } else {
            switch(op) {
            case 0: a=data[off/4]; break;
            case 1: data[off/4]=a; break;
            case 2: b+=data[off/4]; break;
            case 3: d=b; break;
            case 4: a+=imm; break;
            case 5: d=a+b*2+imm; break;
            case 6: d-=a; break;
            case 7: a^=d; break;
            case 8: if(a&1) b++; break;
            case 9: b=a; break;
            case 10: b=data[off/4]&0xff; break;
            case 11: a<<=k; break;
            case 12: a++; break;
            }
        }
        p+=len[op];
    }
    if(!code) { st[0]=a; st[1]=b; st[2]=d; return; }
    if(next) { u32 rel=(u32)next-(u32)(at+p+5); code[p]=0xe9; kmemcpy(code+p+1,&rel,4); p+=5; }
    else code[p++]=0xc3;
    while(p<CF_FN) code[p++]=0xcc;
}
static u32 code_footprint(u32 nfn,u32 passes,u32 seed0,int chain) {
    u8 *code=arena_alloc(nfn*CF_FN,4096);
    u32 *data=arena_alloc(CF_DATA*4,4096),*ref=arena_alloc(CF_DATA*4,4096);
    void **table=arena_alloc(nfn*4,4);
    u32 *order=arena_alloc(nfn*4,4);
    if(!code || !data || !ref || !table || !order) SKIP("arena_code_footprint");
    u32 seed=seed0;
    for(u32 i=0;i<nfn;i++) order[i]=i;
    for(u32 i=nfn-1;i>0;i--) { u32 j=rng(&seed)%(i+1),t=order[i]; order[i]=order[j]; order[j]=t; }
    /* Each function built aside and written only where the code differs: an
       invocation after another one finds its code in place and runs it as
       compiled, rather than rewriting it with the same bytes */
    for(u32 i=0;i<nfn;i++) {
        const u8 *next=(chain && i+1<nfn)?code+order[i+1]*CF_FN:NULL;
        u8 fn[CF_FN],*at=code+order[i]*CF_FN;
        cf_gen(seed0+order[i]*0x9e3779b9u,fn,at,next,NULL,NULL);
        if(kmemcmp(at,fn,CF_FN)) kmemcpy(at,fn,CF_FN);
        table[i]=at;
    }
    for(u32 i=0;i<CF_DATA;i++) data[i]=ref[i]=rng(&seed);
    u32 st[3]={1,2,3},sr[3]={1,2,3},calls=chain?1:nfn;
    /* the first pass checked against the model, function by function in the same order */
    cf_run(st,data,table,calls);
    for(u32 i=0;i<nfn;i++) cf_gen(seed0+order[i]*0x9e3779b9u,NULL,NULL,NULL,sr,ref);
    for(u32 i=0;i<3;i++) CHECK_EQ(st[i],sr[i]);
    CHECK_EQ(kmemcmp(data,ref,CF_DATA*4),0);
    for(u32 p=1;p<passes;p++) cf_run(st,data,table,calls);
    t_ops(passes*nfn*12); return st[0]^st[1]^st[2]^data[seed%CF_DATA];
}
static u32 int_code_footprint_128k(void) { return code_footprint(2048,256,0xc0de128u,1); }
static u32 int_code_footprint_512k(void) { return code_footprint(8192,64,0xc0de512u,1); }
static u32 int_code_footprint_calls(void) { return code_footprint(2048,256,0xc0dec11u,0); }
static u32 int_alu(void) {
    u32 seed=0x12345678,sum=0;
    static const u32 edge[]={0,1,0xffffffffu,0x80000000u,0x7fffffffu,0xffffu,0xffu,0x80u};
    for(unsigned i=0;i<4096;i++) {
        u32 a=rng(&seed),b=rng(&seed),f,rf;
        if(i<64) { a=edge[i&7]; b=edge[i>>3]; }
        for(unsigned w=8;w<=32;w*=2) for(unsigned op=0;op<=A_DEC;op++) {
            u32 v=run_alu(w,op,a,b,i&1,&f),r=alu_ref(op,a,b,i&1,w,&rf);
            CHECK_EQ(v,r); sum=csum_mix(sum,v);
        }
        u32 x,y,z;
        __asm__ volatile("leal 13(%1,%2,4),%0" : "=r"(x) : "r"(a),"r"(b));
        CHECK_EQ(x,a+4*b+13);
        x=a; y=b; __asm__ volatile("xchgl %0,%1" : "+r"(x),"+r"(y)); CHECK_EQ(x,b); CHECK_EQ(y,a);
        __asm__ volatile("movzbl %b1,%0" : "=r"(x):"q"(a)); CHECK_EQ(x,(u8)a);
        __asm__ volatile("movsbl %b1,%0" : "=r"(x):"q"(a)); CHECK_EQ(x,(s32)(s8)a);
        __asm__ volatile("movzwl %w1,%0" : "=r"(x):"r"(a)); CHECK_EQ(x,(u16)a);
        __asm__ volatile("movswl %w1,%0" : "=r"(x):"r"(a)); CHECK_EQ(x,(s32)(s16)a);
        x=a; y=b; z=~a;
        __asm__ volatile("addl $1,%0; adcl $0,%1; adcl $0,%2" : "+r"(x),"+r"(y),"+r"(z)::"cc");
        CHECK_EQ(x,a+1); CHECK_EQ(y,b+(a==~0u)); CHECK_EQ(z,~a+(a==~0u && b==~0u));
        __asm__ volatile("subl $1,%0; sbbl $0,%1; sbbl $0,%2" : "+r"(x),"+r"(y),"+r"(z)::"cc");
        CHECK_EQ(x,a); CHECK_EQ(y,b); CHECK_EQ(z,~a);
        u64 wide=((u64)a<<32)|b, add=((u64)b<<32)|a,got;
        x=b;y=a;
        __asm__ volatile("addl %2,%0; adcl %3,%1" : "+&r"(x),"+&r"(y):"r"(a),"r"(b):"cc");
        got=((u64)y<<32)|x; CHECK(got==wide+add);
    }
    t_ops(4096u*80); return sum;
}
static u32 int_flags_boundaries(void) {
    static const u32 edge[] = {0, 1, ~0u, 0x80000000u, 0x7fffffffu, 0xffffu, 0xffu, 0x80u};
    u32 seed = TEST_SEED, sum = 0;
    for (u32 i = 0; i < 256u * HEAVY_SCALE; i++) {
        u32 a = rng(&seed), b = rng(&seed);
        if (i < 64) { a = edge[i & 7]; b = edge[i >> 3]; }
        for (u32 sub = 0; sub < 2; sub++) {
            u32 out[4] = {0}, got = a, rf;
#define FLAG_BOUNDARY(OP) __asm__ volatile( \
    OP " %%edx,%%eax; jmp 1f; 1: call 2f; jmp 3f; 2: ret; 3:" \
    "pushfl; popl (%%edi);" \
    "setc 4(%%edi); seto 5(%%edi); setz 6(%%edi); sets 7(%%edi);" \
    "movl $0x13579bdf,%%ebx; adcl $0,%%ebx; movl %%ebx,8(%%edi);" \
    "pushl (%%edi); popfl;" \
    "movl $0x2468ace0,%%ebx; sbbl $0,%%ebx; movl %%ebx,12(%%edi)" \
    : "+a"(got) : "d"(b), "D"(out) : "ebx", "cc", "memory")
            if (sub) FLAG_BOUNDARY("subl");
            else FLAG_BOUNDARY("addl");
#undef FLAG_BOUNDARY
            u32 want = alu_ref(sub ? A_SUB : A_ADD, a, b, 0, 32, &rf);
            if (got != want || (out[0] & ARITH) != rf ||
                out[1] != ((rf & CF ? 1u : 0) | (rf & OF ? 0x100u : 0) |
                           (rf & ZF ? 0x10000u : 0) | (rf & SF ? 0x1000000u : 0)) ||
                out[2] != 0x13579bdfu + (rf & CF) ||
                out[3] != 0x2468ace0u - (rf & CF)) {
                t_note("seed=%08x case=%u sub=%u a=%08x b=%08x", TEST_SEED, i, sub, a, b);
                CHECK_EQ(got, want); CHECK_EQ(out[0] & ARITH, rf);
                CHECK_EQ(out[1], (rf & CF ? 1u : 0) | (rf & OF ? 0x100u : 0) |
                         (rf & ZF ? 0x10000u : 0) | (rf & SF ? 0x1000000u : 0));
                CHECK_EQ(out[2], 0x13579bdfu + (rf & CF));
                CHECK_EQ(out[3], 0x2468ace0u - (rf & CF));
                return sum;
            }
            sum = csum_mix(sum, got ^ rf);
        }
    }
    t_ops(256u * HEAVY_SCALE * 2u); return sum;
}

static u32 int_partial_boundaries(void) {
    u32 seed = TEST_SEED, sum = 0;
    for (u32 i = 0; i < 1024u * HEAVY_SCALE; i++) {
        u32 input = rng(&seed), value = rng(&seed), got = input;
        u32 operand = value;
        u32 snapshots[3];
        __asm__ volatile(
            "movb %%dl,%%al; jmp 1f; 1: movl %%eax,(%%edi);"
            "movb %%dh,%%ah; call 2f; jmp 3f; 2: ret; 3: movl %%eax,4(%%edi);"
            "notl %%edx; movw %%dx,%%ax; jmp 4f; 4: movl %%eax,8(%%edi);"
            "movzbl %%ah,%%ecx; xorl %%ecx,%%eax"
            : "+a"(got), "+d"(operand) : "D"(snapshots) : "ecx", "cc", "memory");
        u32 low = (input & 0xffffff00u) | (value & 0xff);
        u32 word = (input & 0xffff0000u) | (value & 0xffff);
        u32 replaced = (input & 0xffff0000u) | (~value & 0xffff);
        if (snapshots[0] != low || snapshots[1] != word ||
            snapshots[2] != replaced || got != (replaced ^ ((~value >> 8) & 0xff))) {
            t_note("seed=%08x case=%u input=%08x value=%08x", TEST_SEED, i, input, value);
            CHECK_EQ(snapshots[0], low); CHECK_EQ(snapshots[1], word);
            CHECK_EQ(snapshots[2], replaced);
            CHECK_EQ(got, replaced ^ ((~value >> 8) & 0xff));
            return sum;
        }
        sum = csum_mix(sum, got);
    }
    t_ops(1024u * HEAVY_SCALE); return sum;
}

static u32 int_flags_exhaustive(void) {
    u32 sum=0,f,rf;
    u16 result_flags[256];
    for(unsigned i=0;i<256;i++) result_flags[i]=szp(i,8);
    for(u32 a=0;a<256;a++) for(u32 b=0;b<256;b++) {
        u32 observed[7],expected[4];u8 values[7];
        u32 add=a+b,adc=add+1,sub=(a-b)&255,sbb=(a-b-1)&255;
        u32 ra=add&255,rc=adc&255;
        expected[0]=result_flags[ra]|(add>>8)|((a^b^ra)&AF)|((~(a^b)&(a^ra)&128)<<4);
        expected[1]=result_flags[rc]|(adc>>8)|((a^b^rc)&AF)|((~(a^b)&(a^rc)&128)<<4);
        expected[2]=result_flags[sub]|(a<b)|((a^b^sub)&AF)|(((a^b)&(a^sub)&128)<<4);
        expected[3]=result_flags[sbb]|(a<b+1)|((a^b^sbb)&AF)|(((a^b)&(a^sbb)&128)<<4);
        /* Batch the seven observations so dispatch/calls do not dwarf the ALU work. */
        __asm__ volatile(
            "movl %%esi,%%eax; addb %%dl,%%al; movb %%al,0(%%ebx); pushfl; popl 0(%%edi);"
            "movl %%esi,%%eax; clc; adcb %%dl,%%al; movb %%al,1(%%ebx); pushfl; popl 4(%%edi);"
            "movl %%esi,%%eax; stc; adcb %%dl,%%al; movb %%al,2(%%ebx); pushfl; popl 8(%%edi);"
            "movl %%esi,%%eax; subb %%dl,%%al; movb %%al,3(%%ebx); pushfl; popl 12(%%edi);"
            "movl %%esi,%%eax; clc; sbbb %%dl,%%al; movb %%al,4(%%ebx); pushfl; popl 16(%%edi);"
            "movl %%esi,%%eax; stc; sbbb %%dl,%%al; movb %%al,5(%%ebx); pushfl; popl 20(%%edi);"
            "movl %%esi,%%eax; cmpb %%dl,%%al; movb %%al,6(%%ebx); pushfl; popl 24(%%edi)"
            ::"S"(a),"d"(b),"D"(observed),"b"(values):"eax","cc","memory");
        CHECK_EQ(values[0],ra); CHECK_EQ(values[1],ra); CHECK_EQ(values[2],rc);
        CHECK_EQ(values[3],sub);CHECK_EQ(values[4],sub);CHECK_EQ(values[5],sbb);CHECK_EQ(values[6],a);
        CHECK_EQ(observed[0]&ARITH,expected[0]);CHECK_EQ(observed[1]&ARITH,expected[0]);
        CHECK_EQ(observed[2]&ARITH,expected[1]);CHECK_EQ(observed[3]&ARITH,expected[2]);
        CHECK_EQ(observed[4]&ARITH,expected[2]);CHECK_EQ(observed[5]&ARITH,expected[3]);CHECK_EQ(observed[6]&ARITH,expected[2]);
        for(unsigned j=0;j<7;j++) sum+=observed[j]&ARITH;
    }
    for(unsigned w=8;w<=32;w*=2) for(u32 i=0;i<512;i++) {
        u32 a=(i<256)?i:(0xffffffffu-(i-256)),b=0x80008081u*i;
        for(unsigned op=0;op<=A_DEC;op++) for(unsigned c=0;c<2;c++) {
            u32 v=run_alu(w,op,a,b,c,&f),r=alu_ref(op,a,b,c,w,&rf);
            CHECK_EQ(v,r);
            if(op!=A_NOT) CHECK_EQ(f&((op>=A_AND && op<=A_TEST)?LOGIC:ARITH),rf&((op>=A_AND && op<=A_TEST)?LOGIC:ARITH));
        }
    }
    for(u32 i=0;i<256;i++) {
        u32 a=i<<8, got;
        __asm__ volatile("sahf; lahf; movzbl %%ah,%0" : "=r"(got),"+a"(a)::"cc");
        CHECK_EQ(got,(i&(CF|PF|AF|ZF|SF))|2);
    }
    u32 old=rd_eflags(),image;
    __asm__ volatile("pushl %1; popfl; pushfl; popl %0; pushl %2; popfl" : "=&r"(image):"r"((old&~ARITH)|ARITH),"r"(old):"cc","memory");
    CHECK_EQ(image&ARITH,ARITH); CHECK(image&2);
    t_ops(500000); return sum;
}

/* Each expected #DE checks the fault address, not just the vector. */
static void divide_fault(unsigned kind) {
    u32 site=0, before=g_exc.count;
    exc_expect(0,2);
    if(kind==0) __asm__ volatile("movl $1,%%eax; xorl %%edx,%%edx; xorl %%ecx,%%ecx; leal 1f,%0; 1: divl %%ecx" : "=S"(site)::"eax","edx","ecx","cc");
    else if(kind==1) __asm__ volatile("xorl %%eax,%%eax; movl $1,%%edx; movl $1,%%ecx; leal 1f,%0; 1: divl %%ecx" : "=S"(site)::"eax","edx","ecx","cc");
    else __asm__ volatile("movl $0x80000000,%%eax; cltd; movl $-1,%%ecx; leal 1f,%0; 1: idivl %%ecx" : "=S"(site)::"eax","edx","ecx","cc");
    exc_expect_clear(); CHECK_EQ(g_exc.count,before+1); CHECK_EQ(g_exc.vec,0); CHECK_EQ(g_exc.eip,site);
}
static regs_t divide_snapshot;
static u32 divide_hits;
static void divide_state_hook(regs_t *r) {
    kmemcpy(&divide_snapshot, r, sizeof divide_snapshot);
    divide_hits++;
    r->eip += 2;
}
static u32 int_divide_state(void) {
    isr_hook_t saved = int_hook[0];
    u32 sum = 0;
    int_hook[0] = divide_state_hook;
    for (u32 i = 0; i < 128u * HEAVY_SCALE; i++) {
        for (u32 kind = 0; kind < 3; kind++) {
            u32 a = kind == 2 ? 0x80000000u : 0x12345678u;
            u32 d = kind == 0 ? 0 : kind == 1 ? 1 : ~0u;
            u32 divisor = kind == 0 ? 0 : kind == 1 ? 1 : ~0u;
            u32 site, before = divide_hits;
            u32 initial_a = a, initial_d = d;
            if (kind == 2)
                __asm__ volatile("leal 1f,%0; 1: idivl %%ecx"
                                 : "=S"(site), "+a"(a), "+d"(d) : "c"(divisor) : "cc", "memory");
            else
                __asm__ volatile("leal 1f,%0; 1: divl %%ecx"
                                 : "=S"(site), "+a"(a), "+d"(d) : "c"(divisor) : "cc", "memory");
            if (divide_hits != before + 1 || divide_snapshot.eip != site ||
                divide_snapshot.eax != initial_a || divide_snapshot.edx != initial_d ||
                divide_snapshot.ecx != divisor || a != initial_a || d != initial_d) {
                t_note("case=%u kind=%u site=%08x dividend=%08x:%08x divisor=%08x",
                       i, kind, site, initial_d, initial_a, divisor);
                CHECK_EQ(divide_hits, before + 1); CHECK_EQ(divide_snapshot.eip, site);
                CHECK_EQ(divide_snapshot.eax, initial_a); CHECK_EQ(divide_snapshot.edx, initial_d);
                CHECK_EQ(divide_snapshot.ecx, divisor);
                CHECK_EQ(a, initial_a); CHECK_EQ(d, initial_d);
                int_hook[0] = saved; return sum;
            }
            sum = csum_mix(sum, a ^ d);
        }
    }
    int_hook[0] = saved;
    t_ops(128u * HEAVY_SCALE * 3u); return sum;
}
static u32 int_muldiv(void) {
    u32 seed=0x31415926,sum=0;
    for(unsigned i=0;i<8192;i++) {
        u32 a=rng(&seed),b=rng(&seed)|1,lo,hi,q,r;
        __asm__ volatile("mull %3" : "=a"(lo),"=d"(hi):"a"(a),"rm"(b):"cc");
        CHECK((((u64)hi<<32)|lo)==(u64)a*b); sum^=hi+lo;
        __asm__ volatile("imull %3" : "=a"(lo),"=d"(hi):"a"(a),"rm"(b):"cc");
        CHECK((((u64)hi<<32)|lo)==(u64)((s64)(s32)a*(s32)b));
        lo=a; __asm__ volatile("imull %1,%0" : "+r"(lo):"r"(b):"cc"); CHECK_EQ(lo,a*b);
        __asm__ volatile("imull $-13,%1,%0" : "=r"(lo):"r"(a):"cc"); CHECK_EQ(lo,a*(u32)-13);
        u16 ax=(u8)a;
        __asm__ volatile("mulb %b1" : "+a"(ax):"q"(b):"cc"); CHECK_EQ(ax,(u8)a*(u8)b);
        ax=(u8)a; __asm__ volatile("imulb %b1" : "+a"(ax):"q"(b):"cc"); CHECK_EQ(ax,(u16)((s8)a*(s8)b));
        u16 al,ah;
        __asm__ volatile("mulw %w3" : "=a"(al),"=d"(ah):"a"((u16)a),"rm"((u16)b):"cc"); CHECK_EQ(((u32)ah<<16)|al,(u16)a*(u32)(u16)b);
        __asm__ volatile("imulw %w3" : "=a"(al),"=d"(ah):"a"((u16)a),"rm"((u16)b):"cc"); CHECK_EQ(((u32)ah<<16)|al,(s16)a*(s32)(s16)b);
        al=a; __asm__ volatile("imulw %w1,%0" : "+r"(al):"r"(b):"cc"); CHECK_EQ(al,(u16)(a*b));
        __asm__ volatile("imulw $17,%w1,%0" : "=r"(al):"r"(a):"cc"); CHECK_EQ(al,(u16)(17*a));
        hi=(a>>16)%b;
        __asm__ volatile("divl %4" : "=a"(q),"=d"(r):"a"(a),"d"(hi),"rm"(b):"cc");
        CHECK((u64)q*b+r==((u64)hi<<32)+a); CHECK(r<b);
        s32 sa=(s32)a/2,sd=(s16)b; if(!sd) sd=1;
        __asm__ volatile("cltd; idivl %3" : "=a"(q),"=&d"(r):"0"(sa),"rm"(sd):"cc");
        CHECK((s64)(s32)q*sd+(s32)r==sa); CHECK_EQ((s32)q,sa/sd); CHECK_EQ((s32)r,sa%sd);
        ax=(u8)a; u8 db=(u8)b;
        __asm__ volatile("divb %1" : "+a"(ax):"q"(db):"cc"); CHECK_EQ(ax&255,(u8)a/db); CHECK_EQ(ax>>8,(u8)a%db);
        s8 byte_n=(s8)a/2; ax=(u16)(s16)byte_n;
        __asm__ volatile("idivb %1" : "+a"(ax):"q"(db):"cc");
        CHECK_EQ((s8)ax,byte_n/(s8)db); CHECK_EQ((s8)(ax>>8),byte_n%(s8)db);
        al=(u16)a; ah=0; u16 dw=(u16)b;
        __asm__ volatile("divw %w4" : "=a"(al),"=d"(ah):"0"(al),"1"(ah),"rm"(dw):"cc"); CHECK_EQ(al,(u16)a/dw); CHECK_EQ(ah,(u16)a%dw);
        s16 sn=(s16)a/2,ds=(s16)b;
        __asm__ volatile("cwd; idivw %w3" : "=a"(al),"=&d"(ah):"0"(sn),"rm"(ds):"cc"); CHECK_EQ((s16)al,sn/ds); CHECK_EQ((s16)ah,sn%ds);
    }
    for(unsigned k=0;k<3;k++) divide_fault(k);
    t_ops(8192*23); return sum;
}

enum { S_SHL,S_SHR,S_SAR,S_ROL,S_ROR,S_RCL,S_RCR };
#define SHIFT_CASE(N,OP,S,R) case N: __asm__ volatile("pushl $0xa03; popfl; " OP S " %%cl," R "0; pushfl; popl %1" : "+a"(a),"=m"(*f):"c"(n):"cc","memory"); break
#define SHIFT_FN(W,S,R) \
static NOINLINE u32 shift##W(unsigned op,u32 a,u32 n,u32 *f) { \
 switch(op) { SHIFT_CASE(S_SHL,"shl",S,R); SHIFT_CASE(S_SHR,"shr",S,R); SHIFT_CASE(S_SAR,"sar",S,R); \
 SHIFT_CASE(S_ROL,"rol",S,R); SHIFT_CASE(S_ROR,"ror",S,R); SHIFT_CASE(S_RCL,"rcl",S,R); SHIFT_CASE(S_RCR,"rcr",S,R); } return a&maskof(W); }
SHIFT_FN(8,"b","%b")
SHIFT_FN(16,"w","%w")
SHIFT_FN(32,"l","%")
#define SHIFT_FIXED_CASE(N,OP,S,R) case N: \
 if(n==1) { __asm__ volatile("pushl $0xa03; popfl; " OP S " " R "0; pushfl; popl %1" : "+a"(a),"=m"(*f)::"cc","memory"); } \
 else { __asm__ volatile("pushl $0xa03; popfl; " OP S " $7," R "0; pushfl; popl %1" : "+a"(a),"=m"(*f)::"cc","memory"); } break
#define SHIFT_FIXED_FN(W,S,R) \
static NOINLINE u32 shift_fixed##W(unsigned op,u32 a,unsigned n,u32 *f) { \
 switch(op) { SHIFT_FIXED_CASE(S_SHL,"shl",S,R); SHIFT_FIXED_CASE(S_SHR,"shr",S,R); SHIFT_FIXED_CASE(S_SAR,"sar",S,R); \
 SHIFT_FIXED_CASE(S_ROL,"rol",S,R); SHIFT_FIXED_CASE(S_ROR,"ror",S,R); SHIFT_FIXED_CASE(S_RCL,"rcl",S,R); SHIFT_FIXED_CASE(S_RCR,"rcr",S,R); } return a&maskof(W); }
SHIFT_FIXED_FN(8,"b","%b")
SHIFT_FIXED_FN(16,"w","%w")
SHIFT_FIXED_FN(32,"l","%")
static u32 shift_ref(unsigned op,u32 a,unsigned n,unsigned w,u32 *cf) {
    u32 m=maskof(w),top=1u<<(w-1),c=1;
    a&=m; n&=31;
    if(op==S_RCL || op==S_RCR) n%=w+1;
    while(n--) {
        u32 next;
        if(op==S_SHL || op==S_ROL || op==S_RCL) {
            next=(a&top)!=0; a=(a<<1)&m;
            if(op==S_ROL) a|=next;
            if(op==S_RCL) a|=c;
        } else {
            next=a&1; u32 sign=a&top; a>>=1;
            if(op==S_SAR) a|=sign;
            if(op==S_ROR && next) a|=top;
            if(op==S_RCR && c) a|=top;
        }
        c=next;
    }
    *cf=c; return a;
}
static u32 int_shift_rot(void) {
    u32 seed=0x6ac1e2,sum=0;
    for(unsigned i=0;i<64;i++) for(unsigned w=8;w<=32;w*=2) for(unsigned op=0;op<7;op++) for(unsigned n=0;n<=40;n++) {
        u32 a=rng(&seed),f,c,r=shift_ref(op,a,n,w,&c);
        u32 v=w==8?shift8(op,a,n,&f):w==16?shift16(op,a,n,&f):shift32(op,a,n,&f);
        CHECK_EQ(v,r);
        if(n==1 || n==7) {
            u32 ff,fixed=w==8?shift_fixed8(op,a,n,&ff):w==16?shift_fixed16(op,a,n,&ff):shift_fixed32(op,a,n,&ff);
            CHECK_EQ(fixed,v); CHECK_EQ(ff&(n==1?(CF|OF):CF),f&(n==1?(CF|OF):CF));
        }
        unsigned k=n&31;
        if(op>=S_ROL || k<w || op==S_SAR) CHECK_EQ(f&CF,c);
        if(k==1) {
            u32 ov;
            if(op==S_SHR) ov=a>>(w-1)&1;
            else if(op==S_SAR) ov=0;
            else if(op==S_ROR || op==S_RCR) ov=(r>>(w-1)^r>>(w-2))&1;
            else ov=((r>>(w-1))^c)&1;
            CHECK_EQ((f&OF)!=0,ov);
        }
        if(!k) CHECK_EQ(f&(CF|OF),CF|OF);
        if(op<=S_SAR && k) CHECK_EQ(f&(SF|ZF|PF),szp(r,w));
        sum+=v;
    }
    for(unsigned i=0;i<512;i++) for(unsigned n=0;n<=40;n++) {
        u32 a=rng(&seed),b=rng(&seed),v=a,f,k=n&31;
        __asm__ volatile("shldl %%cl,%2,%0; pushfl; popl %1" : "+r"(v),"=m"(f):"r"(b),"c"(n):"cc");
        u32 r=k?(a<<k)|(b>>(32-k)):a; CHECK_EQ(v,r);
        if(k) CHECK_EQ(f&CF,(a>>(32-k))&1);
        if(k==1) CHECK_EQ(!!(f&OF),((r^a)>>31));
        v=a; __asm__ volatile("shrdl %%cl,%2,%0; pushfl; popl %1" : "+r"(v),"=m"(f):"r"(b),"c"(n):"cc");
        r=k?(a>>k)|(b<<(32-k)):a; CHECK_EQ(v,r);
        if(k) CHECK_EQ(f&CF,(a>>(k-1))&1);
        if(k==1) CHECK_EQ(!!(f&OF),((r^a)>>31));
        u16 x=a,y=b;
        __asm__ volatile("shldw %%cl,%2,%0; pushfl; popl %1" : "+r"(x),"=m"(f):"r"(y),"c"(n):"cc");
        if(k<=16) { CHECK_EQ(x,k?(u16)(((u32)(u16)a<<k)|((u16)b>>(16-k))):(u16)a); if(k) CHECK_EQ(f&CF,((u16)a>>(16-k))&1); if(k==1) CHECK_EQ(!!(f&OF),((x^(u16)a)>>15)&1); }
        x=a; __asm__ volatile("shrdw %%cl,%2,%0; pushfl; popl %1" : "+r"(x),"=m"(f):"r"(y),"c"(n):"cc");
        if(k<=16) { CHECK_EQ(x,k?(u16)(((u16)a>>k)|((u32)(u16)b<<(16-k))):(u16)a); if(k) CHECK_EQ(f&CF,((u16)a>>(k-1))&1); if(k==1) CHECK_EQ(!!(f&OF),((x^(u16)a)>>15)&1); }
    }
    u32 x=0x12345678; u16 y=0x1234; u8 z=0x81;
    __asm__ volatile("shll $1,%0; shrl $3,%0; roll $7,%0; rorl $7,%0" : "+r"(x)::"cc"); CHECK_EQ(x,0x048d159e);
    __asm__ volatile("shlw $1,%0; shrw $3,%0; rolw $7,%0; rorw $7,%0" : "+r"(y)::"cc"); CHECK_EQ(y,0x048d);
    __asm__ volatile("shlb $1,%0; shrb $1,%0; rolb $3,%0; rorb $3,%0" : "+q"(z)::"cc"); CHECK_EQ(z,1);
    t_ops(64*3*7*41+512*41*4); return sum;
}

#define CC_LIST(M) M(o,0) M(no,1) M(b,2) M(ae,3) M(e,4) M(ne,5) M(be,6) M(a,7) M(s,8) M(ns,9) M(p,10) M(np,11) M(l,12) M(ge,13) M(le,14) M(g,15)
static int condition(unsigned n,u32 f) {
    unsigned o=!!(f&OF),c=!!(f&CF),z=!!(f&ZF),s=!!(f&SF),p=!!(f&PF);
    switch(n) { case 0:return o;case 1:return !o;case 2:return c;case 3:return !c;case 4:return z;case 5:return !z;case 6:return c||z;case 7:return !(c||z);case 8:return s;case 9:return !s;case 10:return p;case 11:return !p;case 12:return s!=o;case 13:return s==o;case 14:return z||(s!=o);default:return !z&&(s==o); }
}
#define SET_CC(CC,N) __asm__ volatile("pushl %1; popfl; set" #CC " %0" : "=qm"(out[N]):"r"(f):"cc","memory");
static u32 int_bits_misc(void) {
    u32 sum=0,seed=0x89123;
    for(unsigned i=0;i<2048;i++) {
        u32 a=rng(&seed),x,f;
        if(i<32) a=1u<<i;
        __asm__ volatile("bsfl %2,%0; pushfl; popl %1" : "=r"(x),"=m"(f):"r"(a):"cc");
        unsigned lo=0,hi=31; while(!(a&(1u<<lo))) lo++; while(!(a&(1u<<hi))) hi--;
        CHECK_EQ(x,lo); CHECK(!(f&ZF));
        __asm__ volatile("bsrl %2,%0; pushfl; popl %1" : "=r"(x),"=m"(f):"r"(a):"cc"); CHECK_EQ(x,hi); CHECK(!(f&ZF));
        __asm__ volatile("bsfl %2,%0; pushfl; popl %1" : "=r"(x),"=m"(f):"r"(0):"cc"); CHECK(f&ZF);
        __asm__ volatile("bsrl %2,%0; pushfl; popl %1" : "=r"(x),"=m"(f):"r"(0):"cc"); CHECK(f&ZF);
        x=a; __asm__ volatile("bswap %0" : "+r"(x)); CHECK_EQ(x,(a<<24)|((a&0xff00)<<8)|((a>>8)&0xff00)|(a>>24));
        for(unsigned bit=0;bit<40;bit++) {
            u32 c,old=a,mask=1u<<(bit&31);
            __asm__ volatile("btl %2,%1; setc %b0" : "=q"(c):"r"(a),"r"(bit):"cc"); CHECK_EQ(c&255,!!(a&mask));
            __asm__ volatile("btsl %2,%0; setc %b1" : "+r"(old),"=q"(c):"r"(bit):"cc"); CHECK_EQ(old,a|mask); CHECK_EQ(c&255,!!(a&mask));
            old=a; __asm__ volatile("btrl %2,%0; setc %b1" : "+r"(old),"=q"(c):"r"(bit):"cc"); CHECK_EQ(old,a&~mask); CHECK_EQ(c&255,!!(a&mask));
            old=a; __asm__ volatile("btcl %2,%0; setc %b1" : "+r"(old),"=q"(c):"r"(bit):"cc"); CHECK_EQ(old,a^mask); CHECK_EQ(c&255,!!(a&mask));
        }
        u32 mem=a,reg=7;
        __asm__ volatile("lock; xaddl %0,%1" : "+r"(reg),"+m"(mem)::"cc","memory"); CHECK_EQ(reg,a); CHECK_EQ(mem,a+7);
        x=mem; reg=0x10203040;
        __asm__ volatile("lock; cmpxchgl %3,%1; pushfl; popl %2" : "+a"(x),"+m"(mem),"=r"(f):"r"(reg):"cc","memory"); CHECK(f&ZF); CHECK_EQ(mem,reg);
        x=~reg;
        __asm__ volatile("lock; cmpxchgl %3,%1; pushfl; popl %2" : "+a"(x),"+m"(mem),"=r"(f):"r"(a):"cc","memory"); CHECK(!(f&ZF)); CHECK_EQ(x,reg); CHECK_EQ(mem,reg);
        sum^=mem;
    }
    u32 bits[20]; for(unsigned i=0;i<20;i++) bits[i]=0;
    /* The large positive and negative offsets are checked against a byte model. */
    for(unsigned i=0;i<20;i++) bits[i]=0;
    for(s32 bit=-32;bit<512;bit++) {
        u8 c; __asm__ volatile("lock; btsl %2,(%1); setc %0" : "=q"(c):"r"(bits+1),"r"(bit):"cc","memory"); CHECK_EQ(c,0);
    }
    for(unsigned i=0;i<17;i++) CHECK_EQ(bits[i],~0u);
    CHECK_EQ(bits[17],0);
    for(s32 bit=-32;bit<512;bit++) {
        u8 c;
        __asm__ volatile("btl %2,(%1); setc %0" : "=q"(c):"r"(bits+1),"r"(bit):"cc","memory");CHECK_EQ(c,1);
        __asm__ volatile("lock; btrl %2,(%1); setc %0" : "=q"(c):"r"(bits+1),"r"(bit):"cc","memory");CHECK_EQ(c,1);
        __asm__ volatile("btl %2,(%1); setc %0" : "=q"(c):"r"(bits+1),"r"(bit):"cc","memory");CHECK_EQ(c,0);
        __asm__ volatile("lock; btcl %2,(%1); setc %0" : "=q"(c):"r"(bits+1),"r"(bit):"cc","memory");CHECK_EQ(c,0);
        __asm__ volatile("lock; btcl %2,(%1); setc %0" : "=q"(c):"r"(bits+1),"r"(bit):"cc","memory");CHECK_EQ(c,1);
    }
    for(unsigned i=0;i<20;i++) CHECK_EQ(bits[i],0);

    for(unsigned k=0;k<32;k++) {
        u32 f=0x202|((k&1)?CF:0)|((k&2)?ZF:0)|((k&4)?SF:0)|((k&8)?OF:0)|((k&16)?PF:0);
        u8 out[16]; CC_LIST(SET_CC)
        for(unsigned n=0;n<16;n++) CHECK_EQ(out[n],condition(n,f));
    }
    u8 table[256]; for(unsigned i=0;i<256;i++) table[i]=255-i;
    for(unsigned i=0;i<256;i++) { u32 a=i; __asm__ volatile("xlatb" : "+a"(a):"b"(table):"memory"); CHECK_EQ(a&255,255-i); }
    for(unsigned i=0;i<256;i++) {
        u32 a=i,d;
        __asm__ volatile("cbtw; cwtl; cltd" : "+a"(a),"=d"(d)); CHECK_EQ(a,(s32)(s8)i); CHECK_EQ(d,((s8)i<0)?~0u:0);
        u16 ax=(u16)(i*257),dx; __asm__ volatile("cwd" : "+a"(ax),"=d"(dx)); CHECK_EQ(dx,((s16)ax<0)?65535:0);
    }
    s32 bounds[2]={-4,4}; u32 site,before=g_exc.count;
    exc_expect(5,2);
    __asm__ volatile("movl $5,%%eax; leal 1f,%0; 1: bound %%eax,(%1)" : "=S"(site):"d"(bounds):"eax","cc","memory");
    exc_expect_clear(); CHECK_EQ(g_exc.count,before+1); CHECK_EQ(g_exc.vec,5); CHECK_EQ(g_exc.eip,site);
    before=g_exc.count;
    /* INTO is a trap: saved EIP already points after the instruction. */
    exc_expect(4,0);
    __asm__ volatile("movb $127,%%al; addb $1,%%al; into; 1: leal 1b,%0" : "=r"(site)::"eax","cc");
    exc_expect_clear(); CHECK_EQ(g_exc.count,before+1); CHECK_EQ(g_exc.vec,4); CHECK_EQ(g_exc.eip,site);
    t_ops(2048*180+2048); return sum;
}
static u32 int_cx8(void) {
    volatile u64 value __attribute__((aligned(8)))=0x1234567887654321ull;
    u32 a=0x87654321,d=0x12345678,f;
    __asm__ volatile("lock; cmpxchg8b %2; pushfl; popl %0" : "=S"(f),"+a"(a),"+m"(value),"+d"(d):"b"(0xaabbccddu),"c"(0x11223344u):"cc","memory");
    CHECK(f&ZF); CHECK(value==0x11223344aabbccddull);
    a=0;d=0;
    __asm__ volatile("lock; cmpxchg8b %2; pushfl; popl %0" : "=S"(f),"+a"(a),"+m"(value),"+d"(d):"b"(7),"c"(8):"cc","memory");
    CHECK(!(f&ZF)); CHECK_EQ(a,0xaabbccdd); CHECK_EQ(d,0x11223344); CHECK(value==0x11223344aabbccddull);
    t_ops(2); return a^d;
}
#define CMOV_CC(CC,N) do { u32 v=0xabcdef01; __asm__ volatile("pushl %2; popfl; cmov" #CC " %1,%0" : "+r"(v):"r"(0x12345678),"r"(f):"cc","memory"); CHECK_EQ(v,condition(N,f)?0x12345678:0xabcdef01); sum^=v; } while(0);
static u32 int_cmov(void) {
    u32 sum=0;
    for(unsigned k=0;k<32;k++) { u32 f=0x202|((k&1)?CF:0)|((k&2)?ZF:0)|((k&4)?SF:0)|((k&8)?OF:0)|((k&16)?PF:0); CC_LIST(CMOV_CC) }
    t_ops(512); return sum;
}

static NOINLINE u32 bcd_run(unsigned op,u32 ax,u32 flags,u32 *f) {
#define BCD_CASE(N,I) case N: __asm__ volatile("pushl %2; popfl; " I "; pushfl; popl %1" : "+a"(ax),"=m"(*f):"r"(flags):"cc","memory"); break
    switch(op) { BCD_CASE(0,"daa"); BCD_CASE(1,"das"); BCD_CASE(2,"aaa"); BCD_CASE(3,"aas"); BCD_CASE(4,"aam $10"); BCD_CASE(5,"aad $10"); BCD_CASE(6,"aam $7"); BCD_CASE(7,"aad $7"); }
    return ax&65535;
}
static u32 int_bcd(void) {
    u32 sum=0,f;
    for(unsigned op=0;op<2;op++) for(unsigned ah=0;ah<1;ah++) for(unsigned a=0;a<256;a++) for(unsigned bits=0;bits<4;bits++) {
        u32 ax=(ah<<8)|a, flags=0x202|((bits&1)?AF:0)|((bits&2)?CF:0),r=ax,cf=0,af=0;
        {
            u32 low=a,oldcf=!!(flags&CF);
            if((a&15)>9 || (flags&AF)) {
                af=1;
                if(op==0) { cf=oldcf || a+6>255; low=(a+6)&255; }
                else { cf=oldcf || a<6; low=(a-6)&255; }
            }
            if(a>0x99 || oldcf) { low=(low+(op==0?0x60:-0x60))&255; cf=1; }
            else if(op==0) cf=0;
            r=(ax&0xff00)|low;
        }
        u32 got=bcd_run(op,ax,flags,&f),mask=CF|AF,rf=(cf?CF:0)|(af?AF:0);
        if(op<2) { mask|=SF|ZF|PF; rf|=szp(r&255,8); }
        CHECK_EQ(got,r); CHECK_EQ(f&mask,rf); sum+=got;
    }
    for(u32 ax=0;ax<65536;ax++) for(unsigned bits=0;bits<4;bits++) {
        u32 flags=0x202|((bits&1)?AF:0)|((bits&2)?CF:0);
        u32 adjust=((ax&15)>9 || (flags&AF))?0x106:0;
        u32 plus=(ax+adjust)&0xff0f,minus=(ax-adjust)&0xff0f,rf=adjust?(CF|AF):0;
        u32 got=ax;
        __asm__ volatile("pushl %2; popfl; aaa; pushfl; popl %1" : "+a"(got),"=m"(f):"r"(flags):"cc","memory");
        CHECK_EQ(got&65535,plus);CHECK_EQ(f&(CF|AF),rf);sum+=got&65535;
        got=ax;
        __asm__ volatile("pushl %2; popfl; aas; pushfl; popl %1" : "+a"(got),"=m"(f):"r"(flags):"cc","memory");
        CHECK_EQ(got&65535,minus);CHECK_EQ(f&(CF|AF),rf);sum+=got&65535;
    }
    for(unsigned op=4;op<8;op++) for(u32 ax=0;ax<((op&1)?65536u:256u);ax++) {
        unsigned base=op<6?10:7,a=ax&255,ah=ax>>8,r;
        if(!(op&1)) r=((a/base)<<8)|(a%base); else r=(a+ah*base)&255;
        u32 got=bcd_run(op,ax,0x202,&f);
        CHECK_EQ(got,r); CHECK_EQ(f&(SF|ZF|PF),szp(r&255,8)); sum^=got;
    }
    u32 site,before=g_exc.count;
    exc_expect(0,2);
    __asm__ volatile("leal 1f,%0; 1: .byte 0xd4,0" : "=r"(site)::"eax","cc");
    exc_expect_clear(); CHECK_EQ(g_exc.count,before+1); CHECK_EQ(g_exc.vec,0); CHECK_EQ(g_exc.eip,site);
    t_ops(2*256*4+2*256*256*4+2*256+2*65536); return sum;
}
static u32 int_string(void) {
    u8 *p=arena_alloc(65536,16); if(!p) SKIP("arena_64k");
    u8 *src=p,*dst=p+16384,*ref=p+32768,*save=p+49152;
    const u32 lens[]={0,1,2,3,15,31,255,1023,4093};
    u32 sum=0;
    for(unsigned round=0;round<1;round++) for(unsigned w=1;w<=4;w*=2) for(unsigned li=0;li<sizeof(lens)/sizeof(lens[0]);li++) {
        u32 n=lens[li],bytes=n*w;
        for(u32 j=0;j<16384;j++) { src[j]=(u8)(j*37+(j>>4)+round); dst[j]=ref[j]=0xa5; }
        for(u32 j=0;j<bytes;j++) ref[j+3]=src[j+1];
        const u8 *s=src+1; u8 *d=dst+3; u32 c=n;
        if(w==1) __asm__ volatile("cld; rep movsb" : "+S"(s),"+D"(d),"+c"(c)::"memory","cc");
        else if(w==2) __asm__ volatile("cld; rep movsw" : "+S"(s),"+D"(d),"+c"(c)::"memory","cc");
        else __asm__ volatile("cld; rep movsl" : "+S"(s),"+D"(d),"+c"(c)::"memory","cc");
        CHECK_EQ(kmemcmp(dst,ref,16384),0); CHECK_EQ(c,0); CHECK(s==src+1+bytes); CHECK(d==dst+3+bytes);
        for(u32 j=0;j<16384;j++) dst[j]=ref[j]=0xa5;
        s=src+1+(n?(n-1)*w:0); d=dst+3+(n?(n-1)*w:0); c=n;
        /* CLD is in the same asm: no C code, calls, or compiler spills execute with DF set. */
        if(w==1) __asm__ volatile("std; rep movsb; cld" : "+S"(s),"+D"(d),"+c"(c)::"memory","cc");
        else if(w==2) __asm__ volatile("std; rep movsw; cld" : "+S"(s),"+D"(d),"+c"(c)::"memory","cc");
        else __asm__ volatile("std; rep movsl; cld" : "+S"(s),"+D"(d),"+c"(c)::"memory","cc");
        for(u32 j=0;j<bytes;j++) ref[j+3]=src[j+1];
        CHECK_EQ(kmemcmp(dst,ref,16384),0);
        d=dst+3; c=n; u32 a=0x78563412;
        if(w==1) __asm__ volatile("rep stosb" : "+D"(d),"+c"(c):"a"(a):"memory");
        else if(w==2) __asm__ volatile("rep stosw" : "+D"(d),"+c"(c):"a"(a):"memory");
        else __asm__ volatile("rep stosl" : "+D"(d),"+c"(c):"a"(a):"memory");
        for(u32 j=0;j<bytes;j++) ref[j+3]=(u8)(a>>(8*(j%w)));
        CHECK_EQ(kmemcmp(dst,ref,16384),0);
        s=src+1;c=n;a=0xabcdef55;
        __asm__ volatile("rep lodsb" : "+S"(s),"+c"(c),"+a"(a)::"memory"); CHECK_EQ(a&255,n?src[n]:0x55); CHECK(s==src+1+n); CHECK_EQ(c,0);
        /* Compare overlap semantics to sequential byte loops, not memmove. */
        for(u32 j=0;j<512;j++) dst[j]=ref[j]=(u8)j;
        for(u32 j=0;j<257;j++) ref[j+3]=ref[j+1];
        s=dst+1;d=dst+3;c=257; __asm__ volatile("rep movsb" : "+S"(s),"+D"(d),"+c"(c)::"memory"); CHECK_EQ(kmemcmp(dst,ref,512),0);
        for(s32 j=256;j>=0;j--) ref[j+1]=ref[j+3];
        s=dst+259;d=dst+257;c=257; __asm__ volatile("std; rep movsb; cld" : "+S"(s),"+D"(d),"+c"(c)::"memory","cc"); CHECK_EQ(kmemcmp(dst,ref,512),0);
        if(n) sum=csum_mix(sum,dst[n&511]);
    }
    for(u32 n=0;n<=513;n++) {
        for(u32 j=0;j<1024;j++) src[j]=dst[j]=(u8)(j*13);
        u32 pos=n/2,c=n,f; const u8 *s=src; u8 *d=dst;
        if(n) dst[pos]^=1;
        __asm__ volatile("pushl $0x242; popfl; repe cmpsb; pushfl; popl %3" : "+S"(s),"+D"(d),"+c"(c),"=m"(f)::"memory","cc");
        CHECK_EQ(c,n?n-pos-1:0); CHECK_EQ(!!(f&ZF),n?0:1); CHECK_EQ((u32)(s-src),n?pos+1:0);
        for(u32 j=0;j<n;j++) dst[j]=0x44;
        if(n) dst[pos]=0x77;
        d=dst;c=n;
        __asm__ volatile("pushl $0x202; popfl; repne scasb; pushfl; popl %2" : "+D"(d),"+c"(c),"=m"(f):"a"(0x77):"memory","cc");
        CHECK_EQ(c,n?n-pos-1:0); CHECK_EQ(!!(f&ZF),n?1:0);
        d=dst;c=n;
        __asm__ volatile("pushl $0x242; popfl; repe scasb; pushfl; popl %2" : "+D"(d),"+c"(c),"=m"(f):"a"(0x44):"memory","cc");
        CHECK_EQ(c,n?n-pos-1:0); CHECK_EQ(!!(f&ZF),n?0:1);
        for(u32 j=0;j<n;j++) dst[j]=src[j]^1;
        if(n) dst[pos]=src[pos];
        s=src;d=dst;c=n;
        __asm__ volatile("pushl $0x202; popfl; repne cmpsb; pushfl; popl %3" : "+S"(s),"+D"(d),"+c"(c),"=m"(f)::"memory","cc");
        CHECK_EQ(c,n?n-pos-1:0); CHECK_EQ(!!(f&ZF),n?1:0);
    }
    /* A byte-for-byte sentinel also catches a zero-count store. */
    for(unsigned i=0;i<16;i++) save[i]=dst[i];
    u8 *d=dst;u32 c=0; __asm__ volatile("rep stosl" : "+D"(d),"+c"(c):"a"(0):"memory"); CHECK_EQ(kmemcmp(dst,save,16),0);
    t_ops(3000000); return sum;
}

/* Out-of-line stack probes leave the C ABI and original stack intact. */
extern u32 int_stack_probe(u32);
extern u32 int_ret4_probe(u32);
__asm__(".text\n"
".type int_stack_probe,@function\nint_stack_probe:\n"
"enter $32,$0\n pushal\n movl $0x11223344,%eax\n pushl %eax\n popl %ecx\n cmpl %eax,%ecx\n jne 1f\n"
"pushw $0x7654\n popw %ax\n cmpw $0x7654,%ax\n jne 1f\n"
"pushaw\n popaw\n pushl $-7\n popl %edx\n cmpl $-7,%edx\n jne 1f\n pushl 8(%ebp)\n popl %edx\n cmpl 8(%ebp),%edx\n jne 1f\n push %cs\n popl %edx\n cmpw $8,%dx\n jne 1f\n push %ds\n pop %ds\n pushfl\n popfl\n popal\n movl 8(%ebp),%eax\n xorl $0x55aa55aa,%eax\n leave\n ret\n"
"1: popal\n movl $0,%eax\n leave\n ret\n"
".type int_ret4_probe,@function\nint_ret4_probe:\n movl 4(%esp),%eax\n addl $7,%eax\n ret $4\n");
static NOINLINE u32 fib(u32 n) { if(n<2) return n; return fib(n-1)+fib(n-2); }
static NOINLINE u32 branch_vm(u32 x,u32 rounds) {
    for(u32 i=0;i<rounds;i++) {
        switch((x^i)&15) {
        case 0:x+=13;break;case 1:x^=0x127;break;case 2:x=x*3+1;break;case 3:x=(x>>1)|(x<<31);break;
        case 4:x-=71;break;case 5:x^=x>>7;break;case 6:x+=i;break;case 7:x=~x;break;
        case 8:x*=17;break;case 9:x^=0xdeadbeef;break;case 10:x+=x>>3;break;case 11:x-=i;break;
        case 12:x=(x<<5)|(x>>27);break;case 13:x^=i*31;break;case 14:x+=0x12345;break;default:x*=7;break;
        }
    } return x;
}
static u32 vm_ref(u32 x,u32 rounds) {
    for(u32 i=0;i<rounds;i++) {
        u32 k=(x^i)&15;
        if(k==0)x+=13;else if(k==1)x^=0x127;else if(k==2)x=x*3+1;else if(k==3)x=(x>>1)|(x<<31);
        else if(k==4)x-=71;else if(k==5)x^=x>>7;else if(k==6)x+=i;else if(k==7)x=~x;
        else if(k==8)x*=17;else if(k==9)x^=0xdeadbeef;else if(k==10)x+=x>>3;else if(k==11)x-=i;
        else if(k==12)x=(x<<5)|(x>>27);else if(k==13)x^=i*31;else if(k==14)x+=0x12345;else x*=7;
    } return x;
}
static u32 int_prefix_addr(void) {
    u8 *p=arena_alloc(8192,16); if(!p) SKIP("arena_prefix");
    for(unsigned i=0;i<8192;i++) p[i]=(u8)(i*29);
    u32 expected=(u32)p[16]|((u32)p[17]<<8)|((u32)p[18]<<16)|((u32)p[19]<<24),v;
    gdt_set(GDT_SCRATCH_FIRST,(u32)p,8191,0x92,0x4);
    /* FS gives 16-bit effective offsets a safe arena-backed segment. */
    __asm__ volatile("push %%fs; movw $0x40,%%ax; movw %%ax,%%fs; .byte 0x64,0x67,0x8b,0x00; pop %%fs" : "=a"(v):"b"(7),"S"(9):"memory"); CHECK_EQ(v,expected);
    __asm__ volatile("push %%fs; movw $0x40,%%ax; movw %%ax,%%fs; xorl %%eax,%%eax; .byte 0x64,0x67,0x66,0x8b,0x00; pop %%fs" : "=a"(v):"b"(7),"S"(9):"memory","cc"); CHECK_EQ(v,expected&65535);
    /* Exercise all eight 16-bit ModRM addressing encodings, including BP/SS default. */
    u32 got[8];
    __asm__ volatile("push %%fs; movw $0x40,%%ax; movw %%ax,%%fs; pushl %%ebp; movl $5,%%ebp;"
        ".byte 0x64,0x67,0x8b,0x00; movl %%eax,0(%%edx);"
        ".byte 0x64,0x67,0x8b,0x01; movl %%eax,4(%%edx);"
        ".byte 0x64,0x67,0x8b,0x02; movl %%eax,8(%%edx);"
        ".byte 0x64,0x67,0x8b,0x03; movl %%eax,12(%%edx);"
        ".byte 0x64,0x67,0x8b,0x04; movl %%eax,16(%%edx);"
        ".byte 0x64,0x67,0x8b,0x05; movl %%eax,20(%%edx);"
        ".byte 0x64,0x67,0x8b,0x06,0x10,0x00; movl %%eax,24(%%edx);"
        ".byte 0x64,0x67,0x8b,0x07; movl %%eax,28(%%edx); popl %%ebp; pop %%fs"
        :"=&a"(v):"b"(7),"S"(9),"D"(11),"d"(got):"memory");
    const u32 offsets[]={16,18,14,16,9,11,16,7};
    for(unsigned i=0;i<8;i++) { u32 off=offsets[i],r=0; for(unsigned j=0;j<4;j++) r|=(u32)p[off+j]<<(8*j); CHECK_EQ(got[i],r); }
    u32 si=0xabcd0010,di=0x98760100,c=0x11110010;
    __asm__ volatile("push %%ds; push %%es; movw $0x40,%%ax; movw %%ax,%%ds; movw %%ax,%%es; .byte 0xf3,0x67,0xa4; pop %%es; pop %%ds" : "+S"(si),"+D"(di),"+c"(c)::"eax","memory");
    CHECK_EQ(si,0xabcd0020); CHECK_EQ(di,0x98760110); CHECK_EQ(c,0x11110000); CHECK_EQ(kmemcmp(p+16,p+256,16),0);
#define SEG_READ(S) __asm__ volatile("movl %%" #S ":(%1),%0" : "=r"(v):"r"(p+16):"memory"); CHECK_EQ(v,expected)
    SEG_READ(ds); SEG_READ(es); SEG_READ(fs); SEG_READ(gs); SEG_READ(ss); SEG_READ(cs);
    v=(u32)(p+16);
    __asm__ volatile(".rept 13; .byte 0x3e; .endr; .byte 0x8b,0x00" : "+a"(v)::"memory"); CHECK_EQ(v,expected);
    v=(u32)(p+16);
    __asm__ volatile(".byte 0x26,0x64,0x3e,0x66,0x8b,0x00" : "+a"(v)::"memory"); CHECK_EQ(v&65535,expected&65535);
    for(unsigned scale=1;scale<=8;scale*=2) {
        u32 *base=(u32 *)(p+512),idx=13;
        if(scale==1) __asm__ volatile("movl 7(%1,%2,1),%0" : "=r"(v):"r"(base),"r"(idx):"memory");
        else if(scale==2) __asm__ volatile("movl 7(%1,%2,2),%0" : "=r"(v):"r"(base),"r"(idx):"memory");
        else if(scale==4) __asm__ volatile("movl 7(%1,%2,4),%0" : "=r"(v):"r"(base),"r"(idx):"memory");
        else __asm__ volatile("movl 7(%1,%2,8),%0" : "=r"(v):"r"(base),"r"(idx):"memory");
        u32 r=0;for(unsigned j=0;j<4;j++)r|=(u32)p[512+7+13*scale+j]<<(8*j); CHECK_EQ(v,r);
    }
    for(unsigned i=0;i<256;i++) {
        CHECK_EQ(int_stack_probe(i),i^0x55aa55aa);
        __asm__ volatile("pushl %1; call int_ret4_probe" : "=a"(v):"r"(i):"ecx","edx","cc","memory"); CHECK_EQ(v,i+7);
    }
    u32 n=7,acc=0;
    __asm__ volatile("1: incl %0; loop 1b" : "+r"(acc),"+c"(n)::"cc"); CHECK_EQ(acc,7); CHECK_EQ(n,0);
    n=7;acc=0;
    __asm__ volatile("xorl %%eax,%%eax; 1: leal 1(%0),%0; loope 1b" : "+r"(acc),"+c"(n)::"eax","cc"); CHECK_EQ(acc,7); CHECK_EQ(n,0);
    n=7;acc=0;
    __asm__ volatile("orl $1,%%eax; 1: leal 1(%0),%0; loopne 1b" : "+r"(acc),"+c"(n)::"eax","cc"); CHECK_EQ(acc,7); CHECK_EQ(n,0);
    n=0;v=5; __asm__ volatile("jecxz 1f; movl $99,%0; 1:" : "+r"(v):"c"(n)); CHECK_EQ(v,5);
    /* Register and memory indirect jump targets are local, known-safe labels. */
    void *target;
    __asm__ volatile("movl $1f,%0; jmp *%0; movl $0,%0; 1:" : "=&r"(target)); CHECK(target!=NULL);
    __asm__ volatile("movl $1f,%0; jmp *%0; int3; 1:" : "=m"(target)::"memory");
    u32 (*fp)(u32)=int_stack_probe;
    __asm__ volatile("pushl $19; call *%1; addl $4,%%esp" : "=a"(v):"r"(fp):"ecx","edx","cc","memory"); CHECK_EQ(v,19^0x55aa55aa);
    CHECK_EQ(fib(24),46368);
    u32 sum=branch_vm(0x12345678,131072); CHECK_EQ(sum,vm_ref(0x12345678,131072));
    t_ops(131072*8+242785); return sum;
}

static void put32(volatile u8 *p,u32 v) { for(unsigned i=0;i<4;i++) p[i]=(u8)(v>>(8*i)); }
static void smc_sync(void) { __asm__ volatile("jmp 1f\n1:" ::: "memory"); }
static NOINLINE u32 exec_code(u8 *p) { smc_sync(); return ((u32 (*)(void))p)(); }
static u32 int_smc(void) {
    u8 *area=arena_alloc(16384,4096); if(!area) SKIP("arena_smc");
    const unsigned offsets[]={1,13,29,61,4093,8191};
    u32 sum=0,seed=0x91378;
    for(unsigned k=0;k<sizeof(offsets)/sizeof(offsets[0]);k++) {
        u8 *p=area+offsets[k]; volatile u8 *writer=p; /* distinct C pointers, same physical bytes */
        writer[0]=0xb8;writer[5]=0xc3;
        gdt_set(GDT_SCRATCH_FIRST,(u32)p,63,0x92,0x4);
        for(unsigned i=0;i<128;i++) {
            u32 value=0x89abcdefu^i;
            __asm__ volatile("push %%fs; movw $0x40,%%dx; movw %%dx,%%fs; movl %0,%%fs:1; pop %%fs" ::"r"(value):"edx","memory");
            CHECK_EQ(exec_code(p),value);
        }
        for(unsigned i=0;i<2048;i++) {
            u32 a=rng(&seed);put32(writer+1,a); CHECK_EQ(exec_code(p),a);
            writer[5]=0x05;put32(writer+6,0x01020304);writer[10]=0xc3;
            CHECK_EQ(exec_code(p),a+0x01020304);
            writer[5]=0x35; CHECK_EQ(exec_code(p),a^0x01020304);
            writer[5]=0xc3; sum=csum_mix(sum,a);
            /* Data writes in the executable page must not discard pending patches. */
            ((volatile u32 *)(area+12288))[i&255]=a;
        }
        /* Rewrite the next MOV's immediate, then branch to flush a 486 prefetch queue. */
        p[0]=0xc7;p[1]=0x05;put32(p+2,(u32)(p+13)); p[10]=0xeb;p[11]=0;p[12]=0xb8;p[17]=0xc3;
        for(unsigned i=0;i<512;i++) { u32 a=rng(&seed);put32(p+6,a);put32(p+13,~a);CHECK_EQ(exec_code(p),a); }
        /* Running code changes the opcode on its own loop backedge. */
        p[0]=0xb9; p[5]=0x31;p[6]=0xc0;p[7]=0x40;p[8]=0xc6;p[9]=0x05;
        put32(p+10,(u32)(p+7));p[14]=0x48;p[15]=0xeb;p[16]=0;p[17]=0xe2;p[18]=(u8)-12;p[19]=0xc3;
        for(unsigned n=1;n<=128;n++) { put32(p+1,n);p[7]=0x40;CHECK_EQ(exec_code(p),2-n); }
        /* A patched counted arithmetic loop catches stale translations of immediates. */
        p[0]=0x31;p[1]=0xc0;p[2]=0xb9;p[7]=0x01;p[8]=0xc8;p[9]=0xe2;p[10]=(u8)-4;p[11]=0xc3;
        for(unsigned n=1;n<=256;n++) { put32(p+3,n);CHECK_EQ(exec_code(p),n*(n+1)/2); }
    }
    t_ops(6*(2048*3+512+128*64+256*128)); return sum;
}

static NOINLINE u32 exec_code_eax(u8 *p,u32 a) {
    u32 r; smc_sync();
    __asm__ volatile("call *%1" : "=a"(r) : "r"(p), "0"(a) : "ecx","edx","cc","memory");
    return r;
}
/* A misaligned word or dword store that starts just before a 64-byte
   boundary, or a page's end, and changes only bytes past it: the opcode and
   immediate of a snippet starting at the boundary, in pages that hold
   compiled code (so the stores take a recompiler's tracked path). The change
   must be seen past the boundary, not only in the chunk or page the store
   starts in, which itself is left as it was. */
static NOINLINE NOCLONE u32 smc_split(unsigned size,unsigned boundary) {
    static const u8 ops[6]={0xb8,0x05,0x2d,0x35,0x0d,0x25}; /* MOV ADD SUB XOR OR AND EAX,imm32 */
    u8 *area=arena_alloc(8192,4096); if(!area) SKIP("arena_smc_split");
    u8 *p=area+boundary; volatile u8 *writer=p;
    u32 sum=0,seed=0x5b117000u^(size<<16)^boundary;
    area[0]=0xc3; /* code in the first page too */
    for(unsigned off=1;off<size;off++) {
        u8 *at=p-off;
        for(unsigned i=0;i<off;i++) at[i]=(u8)(0xa5+i);
        writer[0]=0xb8; put32(writer+1,0x01020304); writer[5]=0xc3;
        for(unsigned i=0;i<32;i++) { exec_code(area); CHECK_EQ(exec_code_eax(p,0),0x01020304); }
        for(unsigned i=0;i<1024;i++) {
            u32 r=rng(&seed),v=0;
            for(unsigned b=0;b<off;b++) v|=(u32)at[b]<<(8*b);
            v|=(u32)ops[r%6]<<(8*off);
            for(unsigned b=off+1;b<size;b++) v|=((r>>(8*b))&0xffu)<<(8*b);
            if(size==2) __asm__ volatile("movw %w1,(%0)" ::"r"(at),"r"(v):"memory");
            else __asm__ volatile("movl %1,(%0)" ::"r"(at),"r"(v):"memory");
            u32 imm=p[1]|((u32)p[2]<<8)|((u32)p[3]<<16)|((u32)p[4]<<24),a=0x13579bdfu^i,want;
            switch(p[0]) {
                case 0xb8: want=imm; break;
                case 0x05: want=a+imm; break;
                case 0x2d: want=a-imm; break;
                case 0x35: want=a^imm; break;
                case 0x0d: want=a|imm; break;
                default:   want=a&imm; break;
            }
            CHECK_EQ(exec_code_eax(p,a),want); sum=csum_mix(sum,want);
        }
    }
    t_ops((size-1)*(32*2+1024*3)); return sum;
}
static u32 int_smc_split_w_chunk(void) { return smc_split(2,1024); }
static u32 int_smc_split_d_chunk(void) { return smc_split(4,1024); }
static u32 int_smc_split_w_page(void) { return smc_split(2,4096); }
static u32 int_smc_split_d_page(void) { return smc_split(4,4096); }

/* REP STOS rewrites code that has run: the string ops store whole runs at a
   time, and code compiled from the bytes they replace must not run again.
   Bytes, words and dwords, forwards and backwards, some across a page. Each
   body runs 256 times, so a recompiler that compiles it runs it as compiled
   code. */
static void rep_fill(u8 *dst,u32 bytes,u32 val,unsigned size,int back) {
    u32 count=bytes/size;
    if(back) dst+=bytes-size;
    if(size==1) {
        if(back) __asm__ volatile("std; rep stosb; cld" : "+D"(dst),"+c"(count) : "a"(val) : "memory","cc");
        else __asm__ volatile("rep stosb" : "+D"(dst),"+c"(count) : "a"(val) : "memory");
    } else if(size==2) {
        if(back) __asm__ volatile("std; rep stosw; cld" : "+D"(dst),"+c"(count) : "a"(val) : "memory","cc");
        else __asm__ volatile("rep stosw" : "+D"(dst),"+c"(count) : "a"(val) : "memory");
    } else {
        if(back) __asm__ volatile("std; rep stosl; cld" : "+D"(dst),"+c"(count) : "a"(val) : "memory","cc");
        else __asm__ volatile("rep stosl" : "+D"(dst),"+c"(count) : "a"(val) : "memory");
    }
}
static u32 int_smc_rep_stos(void) {
    static const u8 fills[3]={0x40,0x48,0x90}; /* INC EAX, DEC EAX, NOP */
    u8 *area=arena_alloc(8192,4096); if(!area) SKIP("arena_smc_rep_stos");
    u32 sum=0,seed=0x5705c0deu;
    for(unsigned round=0;round<256;round++) {
        u32 r=rng(&seed);
        unsigned size=1u<<(r%3),back=(r>>2)&1;
        u32 bytes=(((r>>3)%200)+size)&~(size-1);
        u8 *p=area+(((r>>12)&1) ? 4096-2-(bytes/2) : ((r>>13)&63));
        u8 v=fills[(r>>20)%3];
        u32 val=v*0x01010101u;
        int per=v==0x40?1:v==0x48?-1:0;
        p[0]=0x31; p[1]=0xc0; p[2+bytes]=0xc3;
        rep_fill(p+2,bytes,val,size,back);
        for(unsigned i=0;i<256;i++) CHECK_EQ(exec_code(p),(u32)(per*(int)bytes));
        sum=csum_mix(sum,bytes^(v<<16));
    }
    t_ops(256*256); return sum;
}

/* A hot loop shares its 4K page with a snippet patched every k calls: the
   dynarec must not slow the loop down for the snippet's sake. */
static u32 smc_mixed_page(unsigned k,unsigned h) {
    static const u8 loop[]={0xb9,0x40,0,0,0, 0x31,0xc0, 0x01,0xc8, 0xc1,0xc0,0x03,
                            0x35,0x5a,0x5a,0x5a,0x5a, 0x49, 0x75,0xf3, 0xc3};
    u8 *page=arena_alloc(4096,4096); if(!page) SKIP("arena_smc_mixed");
    u8 *snip=page+2048;
    for(unsigned i=0;i<sizeof(loop);i++) page[i]=loop[i];
    snip[0]=0xb8;
    for(unsigned i=0;i<15;i++) { snip[5+5*i]=0x05; put32(snip+6+5*i,0x01010101); }
    snip[80]=0xc3;
    u32 ref=0; for(u32 c=64;c;c--) { ref+=c; ref=(ref<<3)|(ref>>29); ref^=0x5a5a5a5a; }
    u32 sum=0,seed=0x5eed1234;
    const unsigned patches=2048;
    for(unsigned p=0;p<patches;p++) {
        u32 a=rng(&seed); put32(snip+1,a);
        for(unsigned i=0;i<k;i++) CHECK_EQ(exec_code(snip),a+15u*0x01010101u);
        for(unsigned i=0;i<h;i++) CHECK_EQ(exec_code(page),ref);
        sum=csum_mix(sum,a);
    }
    t_ops(patches*(k*16+h*64*5)); return sum;
}
static u32 int_smc_mixed_k2_h1(void) { return smc_mixed_page(2,1); }
static u32 int_smc_mixed_k2_h16(void) { return smc_mixed_page(2,16); }
static u32 int_smc_mixed_k8_h1(void) { return smc_mixed_page(8,1); }
static u32 int_smc_mixed_k8_h16(void) { return smc_mixed_page(8,16); }

/* A snippet of n ADDs is patched every k calls: sweeps the patch rate
   around the dynarec's compile-or-interpret threshold. */
/* Keep the parameter sweep from duplicating these large bodies at -O3. */
static NOINLINE NOCLONE u32 smc_rate(unsigned k,unsigned n) {
    u8 *snip=arena_alloc(4096,4096); if(!snip) SKIP("arena_smc_rate");
    snip[0]=0xb8;
    for(unsigned i=0;i<n;i++) { snip[5+5*i]=0x05; put32(snip+6+5*i,0x01010101); }
    snip[5+5*n]=0xc3;
    u32 sum=0,seed=0x7a7e0000u^(k<<8)^n;
    const unsigned calls=16384;
    for(unsigned c=0;c<calls;c+=k) {
        u32 a=rng(&seed); put32(snip+1,a);
        for(unsigned i=0;i<k;i++) CHECK_EQ(exec_code(snip),a+n*0x01010101u);
        sum=csum_mix(sum,a);
    }
    t_ops(((calls+k-1)/k)*k*(n+2)); return sum;
}
/* As smc_rate, but each patch also turns a random one of the ADDs into ADD,
   SUB or XOR, so the block's code itself changes and is rarely seen twice. */
static NOINLINE NOCLONE u32 smc_rate_op(unsigned k,unsigned n) {
    static const u8 ops[3]={0x05,0x2d,0x35};
    u8 *snip=arena_alloc(4096,4096); if(!snip) SKIP("arena_smc_rate");
    snip[0]=0xb8;
    for(unsigned i=0;i<n;i++) { snip[5+5*i]=0x05; put32(snip+6+5*i,0x01010101); }
    snip[5+5*n]=0xc3;
    u32 sum=0,seed=0x0b7e0000u^(k<<8)^n;
    const unsigned calls=16384;
    for(unsigned c=0;c<calls;c+=k) {
        u32 a=rng(&seed),r=rng(&seed); put32(snip+1,a); snip[5+5*((r>>8)%n)]=ops[r%3];
        u32 want=a;
        for(unsigned i=0;i<n;i++) { u8 o=snip[5+5*i]; want=o==0x05?want+0x01010101u:o==0x2d?want-0x01010101u:want^0x01010101u; }
        for(unsigned i=0;i<k;i++) CHECK_EQ(exec_code(snip),want);
        sum=csum_mix(sum,want);
    }
    t_ops(((calls+k-1)/k)*k*(n+2)); return sum;
}
/* As smc_rate_op, but each patch picks from ADD, OR, AND, SUB, XOR, ADC, SBB and
   CMP EAX,imm32: the same length and form, the last three outside the set a
   recompiler may treat as one, ADC and SBB taking the carry the instruction
   before left. */
static NOINLINE NOCLONE u32 smc_alu_ops(unsigned k,unsigned n) {
    static const u8 ops[8]={0x05,0x0d,0x25,0x2d,0x35,0x15,0x1d,0x3d};
    u8 *snip=arena_alloc(4096,4096); if(!snip) SKIP("arena_smc_alu_ops");
    snip[0]=0xb8;
    for(unsigned i=0;i<n;i++) { snip[5+5*i]=0x05; put32(snip+6+5*i,0x89abcdefu*(i+1)); }
    snip[5+5*n]=0xc3;
    u32 sum=0,seed=0xa1d0f000u^(k<<8)^n;
    const unsigned calls=8192;
    for(unsigned c=0;c<calls;c+=k) {
        /* Not the first: ADC and SBB there would take the caller's carry */
        u32 a=rng(&seed),r=rng(&seed); put32(snip+1,a); snip[5+5*(1+(r>>8)%(n-1))]=ops[(r>>4)%8];
        u32 want=a,cf=0;
        for(unsigned i=0;i<n;i++) {
            u8 o=snip[5+5*i]; u32 imm=0x89abcdefu*(i+1),res;
            switch(o) {
                case 0x05: res=want+imm; cf=res<want; want=res; break;
                case 0x0d: want|=imm; cf=0; break;
                case 0x25: want&=imm; cf=0; break;
                case 0x2d: cf=want<imm; want-=imm; break;
                case 0x35: want^=imm; cf=0; break;
                case 0x15: { u64 t=(u64)want+imm+cf; cf=(u32)(t>>32); want=(u32)t; } break;
                case 0x1d: { u64 t=(u64)want-imm-cf; cf=(u32)((t>>32)&1); want=(u32)t; } break;
                default:   cf=want<imm; break; /* CMP */
            }
        }
        for(unsigned i=0;i<k;i++) CHECK_EQ(exec_code(snip),want);
        sum=csum_mix(sum,want);
    }
    t_ops(((calls+k-1)/k)*k*(n+2)); return sum;
}
/* Patches flip opcodes between ADD, OR, AND, SUB and XOR EAX,imm32, which a
   recompiler may compile to read the opcode as it runs; every eighth puts
   ADC, SBB or CMP in instead, which such code must not run as one of those,
   and the next one puts the family back. Each version runs 32 times. */
static u32 int_smc_alu_family_exit(void) {
    static const u8 fam[5]={0x05,0x0d,0x25,0x2d,0x35},other[3]={0x15,0x1d,0x3d};
    const unsigned n=8,k=32;
    u8 *snip=arena_alloc(4096,4096); if(!snip) SKIP("arena_smc_alu_family");
    snip[0]=0xb8;
    for(unsigned i=0;i<n;i++) { snip[5+5*i]=0x05; put32(snip+6+5*i,0x89abcdefu*(i+1)); }
    snip[5+5*n]=0xc3;
    u32 sum=0,seed=0xfa417e00u;
    for(unsigned round=0;round<2048;round++) {
        u32 a=rng(&seed),r=rng(&seed);
        /* Not the first: ADC and SBB there would take the caller's carry */
        unsigned at=5+5*(1+(r>>8)%(n-1));
        put32(snip+1,a);
        snip[at]=((round&7)==7) ? other[(r>>4)%3] : fam[(r>>4)%5];
        u32 want=a,cf=0;
        for(unsigned i=0;i<n;i++) {
            u8 o=snip[5+5*i]; u32 imm=0x89abcdefu*(i+1),res;
            switch(o) {
                case 0x05: res=want+imm; cf=res<want; want=res; break;
                case 0x0d: want|=imm; cf=0; break;
                case 0x25: want&=imm; cf=0; break;
                case 0x2d: cf=want<imm; want-=imm; break;
                case 0x35: want^=imm; cf=0; break;
                case 0x15: { u64 t=(u64)want+imm+cf; cf=(u32)(t>>32); want=(u32)t; } break;
                case 0x1d: { u64 t=(u64)want-imm-cf; cf=(u32)((t>>32)&1); want=(u32)t; } break;
                default:   cf=want<imm; break;
            }
        }
        for(unsigned i=0;i<k;i++) CHECK_EQ(exec_code(snip),want);
        /* Back to the family, so the next rounds run compiled again */
        if((round&7)==7) snip[at]=fam[r%5];
        sum=csum_mix(sum,want);
    }
    t_ops(2048*k*(n+2)); return sum;
}
static u32 int_smc_alu_ops_k1_n8(void) { return smc_alu_ops(1,8); }
static u32 int_smc_alu_ops_k4_n16(void) { return smc_alu_ops(4,16); }
static u32 int_smc_alu_ops_k16_n8(void) { return smc_alu_ops(16,8); }
static u32 int_smc_alu_ops_k16_n32(void) { return smc_alu_ops(16,32); }
static u32 int_smc_rate_k1_n16(void) { return smc_rate(1,16); }
static u32 int_smc_rate_k2_n16(void) { return smc_rate(2,16); }
static u32 int_smc_rate_k4_n16(void) { return smc_rate(4,16); }
static u32 int_smc_rate_k8_n16(void) { return smc_rate(8,16); }
static u32 int_smc_rate_k12_n16(void) { return smc_rate(12,16); }
static u32 int_smc_rate_k15_n16(void) { return smc_rate(15,16); }
static u32 int_smc_rate_k16_n16(void) { return smc_rate(16,16); }
static u32 int_smc_rate_k17_n16(void) { return smc_rate(17,16); }
static u32 int_smc_rate_k20_n16(void) { return smc_rate(20,16); }
static u32 int_smc_rate_k24_n16(void) { return smc_rate(24,16); }
static u32 int_smc_rate_k32_n16(void) { return smc_rate(32,16); }
static u32 int_smc_rate_k64_n16(void) { return smc_rate(64,16); }
static u32 int_smc_rate_k1_n256(void) { return smc_rate(1,256); }
static u32 int_smc_rate_k2_n256(void) { return smc_rate(2,256); }
static u32 int_smc_rate_k4_n256(void) { return smc_rate(4,256); }
static u32 int_smc_rate_k8_n256(void) { return smc_rate(8,256); }
static u32 int_smc_rate_k12_n256(void) { return smc_rate(12,256); }
static u32 int_smc_rate_k15_n256(void) { return smc_rate(15,256); }
static u32 int_smc_rate_k16_n256(void) { return smc_rate(16,256); }
static u32 int_smc_rate_k17_n256(void) { return smc_rate(17,256); }
static u32 int_smc_rate_k20_n256(void) { return smc_rate(20,256); }
static u32 int_smc_rate_k24_n256(void) { return smc_rate(24,256); }
static u32 int_smc_rate_k32_n256(void) { return smc_rate(32,256); }
static u32 int_smc_rate_k64_n256(void) { return smc_rate(64,256); }
static u32 int_smc_rate_op_k1_n16(void) { return smc_rate_op(1,16); }
static u32 int_smc_rate_op_k2_n16(void) { return smc_rate_op(2,16); }
static u32 int_smc_rate_op_k4_n16(void) { return smc_rate_op(4,16); }
static u32 int_smc_rate_op_k8_n16(void) { return smc_rate_op(8,16); }
static u32 int_smc_rate_op_k12_n16(void) { return smc_rate_op(12,16); }
static u32 int_smc_rate_op_k15_n16(void) { return smc_rate_op(15,16); }
static u32 int_smc_rate_op_k16_n16(void) { return smc_rate_op(16,16); }
static u32 int_smc_rate_op_k17_n16(void) { return smc_rate_op(17,16); }
static u32 int_smc_rate_op_k20_n16(void) { return smc_rate_op(20,16); }
static u32 int_smc_rate_op_k24_n16(void) { return smc_rate_op(24,16); }
static u32 int_smc_rate_op_k32_n16(void) { return smc_rate_op(32,16); }
static u32 int_smc_rate_op_k64_n16(void) { return smc_rate_op(64,16); }
static u32 int_smc_rate_op_k1_n256(void) { return smc_rate_op(1,256); }
static u32 int_smc_rate_op_k2_n256(void) { return smc_rate_op(2,256); }
static u32 int_smc_rate_op_k4_n256(void) { return smc_rate_op(4,256); }
static u32 int_smc_rate_op_k8_n256(void) { return smc_rate_op(8,256); }
static u32 int_smc_rate_op_k12_n256(void) { return smc_rate_op(12,256); }
static u32 int_smc_rate_op_k15_n256(void) { return smc_rate_op(15,256); }
static u32 int_smc_rate_op_k16_n256(void) { return smc_rate_op(16,256); }
static u32 int_smc_rate_op_k17_n256(void) { return smc_rate_op(17,256); }
static u32 int_smc_rate_op_k20_n256(void) { return smc_rate_op(20,256); }
static u32 int_smc_rate_op_k24_n256(void) { return smc_rate_op(24,256); }
static u32 int_smc_rate_op_k32_n256(void) { return smc_rate_op(32,256); }
static u32 int_smc_rate_op_k64_n256(void) { return smc_rate_op(64,256); }
static u32 int_smc_rate_op_k2_n8(void) { return smc_rate_op(2,8); }
static u32 int_smc_rate_op_k4_n8(void) { return smc_rate_op(4,8); }
static u32 int_smc_rate_op_k6_n8(void) { return smc_rate_op(6,8); }
static u32 int_smc_rate_op_k8_n8(void) { return smc_rate_op(8,8); }
static u32 int_smc_rate_op_k10_n8(void) { return smc_rate_op(10,8); }
static u32 int_smc_rate_op_k12_n8(void) { return smc_rate_op(12,8); }
static u32 int_smc_rate_op_k14_n8(void) { return smc_rate_op(14,8); }
static u32 int_smc_rate_op_k16_n8(void) { return smc_rate_op(16,8); }
static u32 int_smc_rate_op_k2_n32(void) { return smc_rate_op(2,32); }
static u32 int_smc_rate_op_k4_n32(void) { return smc_rate_op(4,32); }
static u32 int_smc_rate_op_k6_n32(void) { return smc_rate_op(6,32); }
static u32 int_smc_rate_op_k8_n32(void) { return smc_rate_op(8,32); }
static u32 int_smc_rate_op_k10_n32(void) { return smc_rate_op(10,32); }
static u32 int_smc_rate_op_k12_n32(void) { return smc_rate_op(12,32); }
static u32 int_smc_rate_op_k14_n32(void) { return smc_rate_op(14,32); }
static u32 int_smc_rate_op_k16_n32(void) { return smc_rate_op(16,32); }
static u32 int_smc_rate_op_k2_n48(void) { return smc_rate_op(2,48); }
static u32 int_smc_rate_op_k4_n48(void) { return smc_rate_op(4,48); }
static u32 int_smc_rate_op_k6_n48(void) { return smc_rate_op(6,48); }
static u32 int_smc_rate_op_k8_n48(void) { return smc_rate_op(8,48); }
static u32 int_smc_rate_op_k10_n48(void) { return smc_rate_op(10,48); }
static u32 int_smc_rate_op_k12_n48(void) { return smc_rate_op(12,48); }
static u32 int_smc_rate_op_k14_n48(void) { return smc_rate_op(14,48); }
static u32 int_smc_rate_op_k16_n48(void) { return smc_rate_op(16,48); }

/* Patched code in the last usable RAM, where chipsets that remap the RAM
   behind 0xA0000-0xFFFFF to the top of memory put it. */
static u8 *smc_top_page;
static u32 smc_top_sum;
static void smc_top_body(void) {
    u32 sum=0,seed=0x70b5eed;
    for(unsigned k=0;k<4;k++) {
        u8 *p=smc_top_page+k*0x1000+(k*37&0xff);
        p[0]=0xb8; p[5]=0xc3;
        for(unsigned i=0;i<4096;i++) {
            u32 a=rng(&seed); put32(p+1,a); CHECK_EQ(exec_code(p),a);
            p[5]=0x35; put32(p+6,0x0f0f0f0f); p[10]=0xc3; CHECK_EQ(exec_code(p),a^0x0f0f0f0f);
            p[5]=0xc3; sum=csum_mix(sum,a);
        }
    }
    smc_top_sum=sum;
}
static u32 int_smc_ram_top(void) {
    if(!arena_size) SKIP("arena_smc_top");
    smc_top_page=(u8 *)(arena_end+0x8000);
    /* The BIOS may not report the remapped RAM: use it if RAM answers just
       above the reported top (below the 15M ISA hole). Only where no E820
       record claims it, since firmware often puts ACPI tables/NVS directly
       after usable RAM, and the original contents are put back afterwards. */
    u32 top=ram_top&~0xfffu,above=0;
    u8 *saved=0;
    if(top>=0x100000 && top+0x4000<=0xf00000 && !e820_claims(top,0x4000)
       && (saved=arena_alloc(0x4000,16))) {
        kmemcpy(saved,(void *)top,0x4000);
        volatile u32 *a=(volatile u32 *)top,*b=(volatile u32 *)(top+0x3ffc);
        *a=0x5aa5c33c; *b=0xa55a3cc3;
        if(*a==0x5aa5c33c && *b==0xa55a3cc3) { *a=~0x5aa5c33cu; if(*a==~0x5aa5c33cu) { smc_top_page=(u8 *)top; above=1; } }
        if(!above) { kmemcpy((void *)top,saved,0x4000); saved=0; }
    }
    t_note("code=%08x above_reported_top=%u ram_top=%08x",(u32)smc_top_page,above,ram_top);
    smc_top_sum=0;
    u32 rc=(u32)guard_call(smc_top_body);
    if(saved) kmemcpy((void *)top,saved,0x4000);
    /* Report an unexpected fault as the test's FAULT, after restoring. */
    if(rc) guard_recover(rc);
    t_ops(4*4096*2); return smc_top_sum;
}

static volatile u32 debug_hits,debug_ip,debug_flags;
static void debug_hook(regs_t *r) { debug_hits++;debug_ip=r->eip;debug_flags=r->eflags;r->eflags&=~0x100u; }
static volatile u32 shadow_hits,shadow_ip,shadow_marker,shadow_bad;
static void shadow_hook(regs_t *r) { shadow_hits++; shadow_ip=r->eip; if(!shadow_marker) shadow_bad++; }
static u32 int_cpuid_cr(void) {
    u32 sum=0,old=rd_eflags(),image;
    /* Restore flags in the same asm, especially AC and NT, before calling C. */
    const u32 masks[]={1u<<18,1u<<21,0x4000,0x3000,0x400};
    for(unsigned i=0;i<sizeof(masks)/sizeof(masks[0]);i++) {
        u32 m=masks[i],n=old^m;
        if(m==(1u<<18) && !cpu.is486 && !(cpu_required_features() & REQ_AC)) continue;
        __asm__ volatile("pushl %1; popfl; pushfl; popl %0; pushl %2; popfl" : "=&r"(image):"r"(n),"r"(old):"cc","memory");
        if(m!=(1u<<21) || cpu.has_cpuid) CHECK_EQ((image^old)&m,m);
        CHECK(image&2); sum^=image&m;
    }
    u32 cr0=rd_cr0(),cr2=rd_cr2(),cr3=rd_cr3(),v;
    wr_cr0(cr0);CHECK_EQ(rd_cr0(),cr0);
    __asm__ volatile("movl %1,%%cr2; movl %%cr2,%0; movl %2,%%cr2" : "=&r"(v):"r"(0x12345000),"r"(cr2):"memory"); CHECK_EQ(v,0x12345000);CHECK_EQ(rd_cr2(),cr2);
    wr_cr3(cr3);CHECK_EQ(rd_cr3(),cr3);
    __asm__ volatile("pushl %1; popfl; pushfl; popl %0; pushl %2; popfl" : "=&r"(image):"r"(old&~0x200u),"r"(old):"cc","memory"); CHECK(!(image&0x200));
    __asm__ volatile("pushl %1; popfl; pushfl; popl %0; pushl %2; popfl" : "=&r"(image):"r"(old|0x200u),"r"(old):"cc","memory"); CHECK(image&0x200);
    int_hook[1]=debug_hook;debug_hits=0;
    u32 after;
    __asm__ volatile("pushfl; orl $0x100,(%%esp); popfl; nop; 1: leal 1b,%0" : "=r"(after)::"cc","memory");
    int_hook[1]=0;CHECK_EQ(debug_hits,1);CHECK_EQ(debug_ip,after);CHECK(debug_flags&0x100);CHECK(!(rd_eflags()&0x100));
    u32 start=pit_ticks;
    __asm__ volatile("sti; hlt" ::: "memory"); CHECK(pit_ticks!=start);
    /* Queue IRQ0 while CLI is active, then make the shadowed instruction observable. */
    cli();
    shadow_hits=shadow_bad=shadow_marker=0;int_hook[0x20]=shadow_hook;
    start=pit_ticks;outb(0x20,0x0a);
    unsigned pending=0;
    for(u32 i=0;i<10000000;i++) if(inb(0x20)&1) {pending=1;break;}
    u32 stopped=pit_ticks,disabled=rd_eflags();
    __asm__ volatile("sti; incl %0; cli; sti; hlt" : "+m"(shadow_marker)::"cc","memory");
    int_hook[0x20]=0;
    /* Without a latched IRQ0 the STI below is not shadowing anything. */
    CHECK(pending);
    CHECK_EQ(stopped,start);CHECK(!(disabled&0x200));CHECK_EQ(shadow_marker,1);CHECK_EQ(shadow_bad,0);CHECK(shadow_hits>0);
    t_note("sti_shadow pending=%u irq_hits=%u return_eip=%08x",pending,shadow_hits,shadow_ip);
    if(cpu.has_cpuid) {
        u32 a,b,c,d,a2,b2,c2,d2;
        cpuid(0,&a,&b,&c,&d);cpuid(0,&a2,&b2,&c2,&d2);
        CHECK_EQ(a,cpu.max_leaf);CHECK_EQ(a,a2);CHECK_EQ(b,b2);CHECK_EQ(c,c2);CHECK_EQ(d,d2);
        char vendor[13];for(unsigned i=0;i<4;i++) { vendor[i]=b>>(i*8);vendor[i+4]=d>>(i*8);vendor[i+8]=c>>(i*8); } vendor[12]=0;
        CHECK_EQ(kmemcmp(vendor,cpu.vendor,12),0);t_note("cpuid.0=%08x %08x %08x %08x vendor=%s",a,b,c,d,vendor);
        if(cpu.max_leaf>=1) {
            cpuid(1,&a,&b,&c,&d);cpuid(1,&a2,&b2,&c2,&d2);CHECK_EQ(a,a2);CHECK_EQ(b,b2);CHECK_EQ(c,c2);CHECK_EQ(d,d2);CHECK_EQ(c,cpu.feat_ecx);CHECK_EQ(d,cpu.feat_edx);
            t_note("cpuid.1=%08x %08x %08x %08x",a,b,c,d);sum^=a^b^c^d;
        }
        if(cpu.max_leaf>=2) {
            cpuid(2,&a,&b,&c,&d);unsigned count=a&255;t_note("cpuid.2 count=%u descriptors=%08x %08x %08x %08x",count,a,b,c,d);
            /* AMD may return an empty leaf; Intel may cycle descriptor sets. */
            for(unsigned i=1;i<count;i++) { cpuid(2,&a2,&b2,&c2,&d2);t_note("cpuid.2[%u]=%08x %08x %08x %08x",i,a2,b2,c2,d2); }
            cpuid(2,&a2,&b2,&c2,&d2);CHECK_EQ(a,a2);CHECK_EQ(b,b2);CHECK_EQ(c,c2);CHECK_EQ(d,d2);
        }
        if(cpu.ext_max) {
            cpuid(0x80000000,&a,&b,&c,&d);CHECK_EQ(a,cpu.ext_max);t_note("cpuid.extmax=%08x",a);
            u32 last=cpu.ext_max<0x80000004u?cpu.ext_max:0x80000004u;
            for(u32 leaf=0x80000001;leaf<=last;leaf++) { cpuid(leaf,&a,&b,&c,&d);cpuid(leaf,&a2,&b2,&c2,&d2);CHECK_EQ(a,a2);CHECK_EQ(b,b2);CHECK_EQ(c,c2);CHECK_EQ(d,d2);t_note("cpuid.%08x=%08x %08x %08x %08x",leaf,a,b,c,d); }
        }
    } else t_note("CPUID unavailable; control/flags probes still executed");
    wr_eflags(old);t_ops(100);return sum;
}

/* Reflected bit CRC versus a separately constructed nibble table. */
static u32 crc_bit(u32 crc,u8 byte) { crc^=byte; for(unsigned k=0;k<8;k++) crc=(crc>>1)^((0u-(crc&1))&0xedb88320u); return crc; }
static u64 mul64_ref(u64 a,u64 b) { u64 r=0;while(b) {if(b&1)r+=a;a<<=1;b>>=1;}return r; }
static u64 div64_ref(u64 n,u32 d,u32 *rem) {
    u64 q=0,r=0;for(unsigned k=64;k;k--) {r=(r<<1)|((n>>(k-1))&1);if(r>=d){r-=d;q|=1ull<<(k-1);}}*rem=(u32)r;return q;
}
static u32 int_alu_burn(void) {
    u32 *a=arena_alloc(1024,16);u8 *copy=arena_alloc(1024,16);if(!a||!copy)SKIP("arena_burn");
    u32 tab[16];for(unsigned i=0;i<16;i++){u32 x=i;for(unsigned j=0;j<4;j++)x=(x>>1)^((0u-(x&1))&0xedb88320u);tab[i]=x;}
    u32 seed=0x12345678,lcg=0x2468ace0,crc=~0u,ref=~0u,sum=0;
    for(unsigned i=0;i<16384;i++) {
        u32 old=seed,x=rng(&seed),r=old^(old<<13);r^=r>>17;r^=r<<5;CHECK_EQ(x,r);
        u64 l=(u64)lcg*1664525+1013904223u;lcg=lcg*1664525u+1013904223u;CHECK_EQ(lcg,(u32)l);
        crc=crc_bit(crc,(u8)x); ref^=(u8)x;ref=(ref>>4)^tab[ref&15];ref=(ref>>4)^tab[ref&15];CHECK_EQ(crc,ref);
        if(!(i&31)) {
            u64 n=((u64)x<<32)|lcg,b=((u64)lcg<<32)|x;
            u64 product=n*b;CHECK(product==mul64_ref(n,b));
            u32 divisor=(lcg>>1)|1,rem;u64 quotient=n/divisor;
            CHECK(quotient==div64_ref(n,divisor,&rem));CHECK_EQ((u32)(n%divisor),rem);CHECK(quotient*divisor+rem==n);
            u32 before_xor=0,before_sum=0;
            for(unsigned j=0;j<64;j++){a[j]=rng(&seed);before_xor^=a[j];before_sum+=a[j];}
            for(unsigned j=1;j<64;j++){u32 v=a[j],k=j;while(k && a[k-1]>v){a[k]=a[k-1];k--;}a[k]=v;}
            u32 after_xor=0,after_sum=0;for(unsigned j=0;j<64;j++){if(j)CHECK(a[j-1]<=a[j]);after_xor^=a[j];after_sum+=a[j];}
            CHECK_EQ(after_xor,before_xor);CHECK_EQ(after_sum,before_sum);
            kmemcpy(copy,a,256);CHECK_EQ(kmemcmp(copy,a,256),0);for(unsigned j=0;j<256;j++)CHECK_EQ(copy[j],((u8 *)a)[j]);
            sum=csum_mix(sum,(u32)product^(u32)quotient^a[31]);
        }
    }
    sum=csum_mix(sum,crc^seed^lcg);
    /* Independently generated with byte CRC, unbounded integer arithmetic and sorted() in Python. */
    CHECK_EQ(sum,0x6fffd0bcu);
    t_ops(16384*45+512*64*32);return sum;
}

/* Correctness checks, run once as in accuracy mode and left out of
   benchmark-only builds (run_checks()) */
const test_t tests_int_check[]={
    {"int_cf_consumers",int_cf_consumers,REQ_486},
    {0,0,0}
};

const test_t tests_int[]={
    {"int_flags_boundaries",int_flags_boundaries,REQ_486},
    {"int_partial_boundaries",int_partial_boundaries,REQ_486},
    {"int_divide_state",int_divide_state,REQ_486},
    {"int_alu",int_alu,REQ_486},
    {"int_adc_shift_loop",int_adc_shift_loop,REQ_486},
    {"int_adc_carry_incdec",int_adc_carry_incdec,REQ_486},
    {"int_far_ptr_copy",int_far_ptr_copy,REQ_486},
    {"int_sha1",int_sha1,REQ_486},
    {"int_bn_muladd",int_bn_muladd,REQ_486},
    {"int_sha1_xdown",int_sha1_xdown,REQ_486},
    {"int_bn_muladd_xdown",int_bn_muladd_xdown,REQ_486},
    {"int_code_footprint_128k",int_code_footprint_128k,REQ_486},
    {"int_code_footprint_512k",int_code_footprint_512k,REQ_486},
    {"int_code_footprint_calls",int_code_footprint_calls,REQ_486},
    {"int_flags_exhaustive",int_flags_exhaustive,REQ_486},
    {"int_muldiv",int_muldiv,REQ_486},
    {"int_shift_rot",int_shift_rot,REQ_486},
    {"int_bits_misc",int_bits_misc,REQ_486},
    {"int_cx8",int_cx8,REQ_CX8},
    {"int_cmov",int_cmov,REQ_CMOV},
    {"int_bcd",int_bcd,REQ_486},
    {"int_string",int_string,REQ_486},
    {"int_prefix_addr",int_prefix_addr,REQ_486},
    {"int_smc",int_smc,REQ_486},
    {"int_smc_split_w_chunk",int_smc_split_w_chunk,REQ_486},
    {"int_smc_split_d_chunk",int_smc_split_d_chunk,REQ_486},
    {"int_smc_split_w_page",int_smc_split_w_page,REQ_486},
    {"int_smc_split_d_page",int_smc_split_d_page,REQ_486},
    {"int_smc_rep_stos",int_smc_rep_stos,REQ_486},
    {"int_smc_alu_family_exit",int_smc_alu_family_exit,REQ_486},
    {"int_smc_alu_ops_k1_n8",int_smc_alu_ops_k1_n8,REQ_486},
    {"int_smc_alu_ops_k4_n16",int_smc_alu_ops_k4_n16,REQ_486},
    {"int_smc_alu_ops_k16_n8",int_smc_alu_ops_k16_n8,REQ_486},
    {"int_smc_alu_ops_k16_n32",int_smc_alu_ops_k16_n32,REQ_486},
    {"int_smc_mixed_k2_h1",int_smc_mixed_k2_h1,REQ_486},
    {"int_smc_mixed_k2_h16",int_smc_mixed_k2_h16,REQ_486},
    {"int_smc_mixed_k8_h1",int_smc_mixed_k8_h1,REQ_486},
    {"int_smc_mixed_k8_h16",int_smc_mixed_k8_h16,REQ_486},
    {"int_smc_ram_top",int_smc_ram_top,REQ_486},
    {"int_smc_rate_k1_n16",int_smc_rate_k1_n16,REQ_486},
    {"int_smc_rate_k2_n16",int_smc_rate_k2_n16,REQ_486},
    {"int_smc_rate_k4_n16",int_smc_rate_k4_n16,REQ_486},
    {"int_smc_rate_k8_n16",int_smc_rate_k8_n16,REQ_486},
    {"int_smc_rate_k12_n16",int_smc_rate_k12_n16,REQ_486},
    {"int_smc_rate_k15_n16",int_smc_rate_k15_n16,REQ_486},
    {"int_smc_rate_k16_n16",int_smc_rate_k16_n16,REQ_486},
    {"int_smc_rate_k17_n16",int_smc_rate_k17_n16,REQ_486},
    {"int_smc_rate_k20_n16",int_smc_rate_k20_n16,REQ_486},
    {"int_smc_rate_k24_n16",int_smc_rate_k24_n16,REQ_486},
    {"int_smc_rate_k32_n16",int_smc_rate_k32_n16,REQ_486},
    {"int_smc_rate_k64_n16",int_smc_rate_k64_n16,REQ_486},
    {"int_smc_rate_k1_n256",int_smc_rate_k1_n256,REQ_486},
    {"int_smc_rate_k2_n256",int_smc_rate_k2_n256,REQ_486},
    {"int_smc_rate_k4_n256",int_smc_rate_k4_n256,REQ_486},
    {"int_smc_rate_k8_n256",int_smc_rate_k8_n256,REQ_486},
    {"int_smc_rate_k12_n256",int_smc_rate_k12_n256,REQ_486},
    {"int_smc_rate_k15_n256",int_smc_rate_k15_n256,REQ_486},
    {"int_smc_rate_k16_n256",int_smc_rate_k16_n256,REQ_486},
    {"int_smc_rate_k17_n256",int_smc_rate_k17_n256,REQ_486},
    {"int_smc_rate_k20_n256",int_smc_rate_k20_n256,REQ_486},
    {"int_smc_rate_k24_n256",int_smc_rate_k24_n256,REQ_486},
    {"int_smc_rate_k32_n256",int_smc_rate_k32_n256,REQ_486},
    {"int_smc_rate_k64_n256",int_smc_rate_k64_n256,REQ_486},
    {"int_smc_rate_op_k1_n16",int_smc_rate_op_k1_n16,REQ_486},
    {"int_smc_rate_op_k2_n16",int_smc_rate_op_k2_n16,REQ_486},
    {"int_smc_rate_op_k4_n16",int_smc_rate_op_k4_n16,REQ_486},
    {"int_smc_rate_op_k8_n16",int_smc_rate_op_k8_n16,REQ_486},
    {"int_smc_rate_op_k12_n16",int_smc_rate_op_k12_n16,REQ_486},
    {"int_smc_rate_op_k15_n16",int_smc_rate_op_k15_n16,REQ_486},
    {"int_smc_rate_op_k16_n16",int_smc_rate_op_k16_n16,REQ_486},
    {"int_smc_rate_op_k17_n16",int_smc_rate_op_k17_n16,REQ_486},
    {"int_smc_rate_op_k20_n16",int_smc_rate_op_k20_n16,REQ_486},
    {"int_smc_rate_op_k24_n16",int_smc_rate_op_k24_n16,REQ_486},
    {"int_smc_rate_op_k32_n16",int_smc_rate_op_k32_n16,REQ_486},
    {"int_smc_rate_op_k64_n16",int_smc_rate_op_k64_n16,REQ_486},
    {"int_smc_rate_op_k1_n256",int_smc_rate_op_k1_n256,REQ_486},
    {"int_smc_rate_op_k2_n256",int_smc_rate_op_k2_n256,REQ_486},
    {"int_smc_rate_op_k4_n256",int_smc_rate_op_k4_n256,REQ_486},
    {"int_smc_rate_op_k8_n256",int_smc_rate_op_k8_n256,REQ_486},
    {"int_smc_rate_op_k12_n256",int_smc_rate_op_k12_n256,REQ_486},
    {"int_smc_rate_op_k15_n256",int_smc_rate_op_k15_n256,REQ_486},
    {"int_smc_rate_op_k16_n256",int_smc_rate_op_k16_n256,REQ_486},
    {"int_smc_rate_op_k17_n256",int_smc_rate_op_k17_n256,REQ_486},
    {"int_smc_rate_op_k20_n256",int_smc_rate_op_k20_n256,REQ_486},
    {"int_smc_rate_op_k24_n256",int_smc_rate_op_k24_n256,REQ_486},
    {"int_smc_rate_op_k32_n256",int_smc_rate_op_k32_n256,REQ_486},
    {"int_smc_rate_op_k64_n256",int_smc_rate_op_k64_n256,REQ_486},
    {"int_smc_rate_op_k2_n8",int_smc_rate_op_k2_n8,REQ_486},
    {"int_smc_rate_op_k4_n8",int_smc_rate_op_k4_n8,REQ_486},
    {"int_smc_rate_op_k6_n8",int_smc_rate_op_k6_n8,REQ_486},
    {"int_smc_rate_op_k8_n8",int_smc_rate_op_k8_n8,REQ_486},
    {"int_smc_rate_op_k10_n8",int_smc_rate_op_k10_n8,REQ_486},
    {"int_smc_rate_op_k12_n8",int_smc_rate_op_k12_n8,REQ_486},
    {"int_smc_rate_op_k14_n8",int_smc_rate_op_k14_n8,REQ_486},
    {"int_smc_rate_op_k16_n8",int_smc_rate_op_k16_n8,REQ_486},
    {"int_smc_rate_op_k2_n32",int_smc_rate_op_k2_n32,REQ_486},
    {"int_smc_rate_op_k4_n32",int_smc_rate_op_k4_n32,REQ_486},
    {"int_smc_rate_op_k6_n32",int_smc_rate_op_k6_n32,REQ_486},
    {"int_smc_rate_op_k8_n32",int_smc_rate_op_k8_n32,REQ_486},
    {"int_smc_rate_op_k10_n32",int_smc_rate_op_k10_n32,REQ_486},
    {"int_smc_rate_op_k12_n32",int_smc_rate_op_k12_n32,REQ_486},
    {"int_smc_rate_op_k14_n32",int_smc_rate_op_k14_n32,REQ_486},
    {"int_smc_rate_op_k16_n32",int_smc_rate_op_k16_n32,REQ_486},
    {"int_smc_rate_op_k2_n48",int_smc_rate_op_k2_n48,REQ_486},
    {"int_smc_rate_op_k4_n48",int_smc_rate_op_k4_n48,REQ_486},
    {"int_smc_rate_op_k6_n48",int_smc_rate_op_k6_n48,REQ_486},
    {"int_smc_rate_op_k8_n48",int_smc_rate_op_k8_n48,REQ_486},
    {"int_smc_rate_op_k10_n48",int_smc_rate_op_k10_n48,REQ_486},
    {"int_smc_rate_op_k12_n48",int_smc_rate_op_k12_n48,REQ_486},
    {"int_smc_rate_op_k14_n48",int_smc_rate_op_k14_n48,REQ_486},
    {"int_smc_rate_op_k16_n48",int_smc_rate_op_k16_n48,REQ_486},
    {"int_cpuid_cr",int_cpuid_cr,REQ_486},
    {"int_alu_burn",int_alu_burn,REQ_486},
    {0,0,0}
};
