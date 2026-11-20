/* Integer paths that 86Box's new dynarec leaves to the interpreter (helper
   calls inside compiled blocks), and patterns that force block exits or
   page checks. doc/workloads.md names the 86Box function each record aims at.
   References never use the instruction under test. */
#include "kernel.h"

#define NOINLINE __attribute__((noinline))
static u32 rng(u32 *s) { u32 x=*s; x^=x<<13; x^=x>>17; x^=x<<5; return *s=x; }
static u32 rol32(u32 v,unsigned n) { return v<<n | v>>(32-n); }

/* One shared pool; a record regenerates its data only when it does not own
   the pool, so benchmark repeats after the warm-up do no setup. */
static u32 pool[16384];
static u32 pool_owner;
static int pool_claim(u32 id) { if(pool_owner==id) return 0; pool_owner=id; return 1; }

#define PRO "pushl %ebp\n pushl %esi\n pushl %edi\n pushl %ebx\n"
#define EPI "popl %ebx\n popl %edi\n popl %esi\n popl %ebp\n ret\n"
#define FN(n) ".type " n ",@function\n" n ":\n" PRO

/* ---- CMOVcc (0F 40-4F: NULL in recomp_opcodes_0f) vs the same in Jcc ---- */
extern void ie_cmov_sel(const s32 *a,const s32 *b,u32 n,u32 *out);
extern void ie_jcc_sel(const s32 *a,const s32 *b,u32 n,u32 *out);
#define SEL_HEAD "movl 20(%esp),%esi\n movl 24(%esp),%edi\n movl 28(%esp),%ecx\n xorl %ebx,%ebx\n xorl %ebp,%ebp\n pushl $0\n pushl $0\n 1: "
#define SEL_TAIL "addl $4,%esi\n addl $4,%edi\n decl %ecx\n jnz 1b\n popl %eax\n popl %edx\n movl 32(%esp),%ecx\n" \
                 "movl %ebx,(%ecx)\n movl %ebp,4(%ecx)\n movl %edx,8(%ecx)\n movl %eax,12(%ecx)\n" EPI
__asm__(".text\n"
FN("ie_cmov_sel") SEL_HEAD
"movl (%esi),%eax\n movl %eax,%edx\n cmpl (%edi),%eax\n cmovgl (%edi),%eax\n cmovll (%edi),%edx\n addl %eax,%ebx\n addl %edx,%ebp\n"
"movl (%esi),%eax\n movl (%edi),%edx\n testb $1,%al\n cmovnel %eax,%edx\n addl %edx,4(%esp)\n"
"movl %eax,%edx\n negl %edx\n cmovsl %eax,%edx\n addl %edx,(%esp)\n" SEL_TAIL
FN("ie_jcc_sel") SEL_HEAD
"movl (%esi),%eax\n movl (%edi),%edx\n cmpl %edx,%eax\n jle 2f\n movl %edx,%eax\n movl (%esi),%edx\n 2: addl %eax,%ebx\n addl %edx,%ebp\n"
"movl (%esi),%eax\n testb $1,%al\n jnz 3f\n movl (%edi),%eax\n 3: addl %eax,4(%esp)\n"
"movl (%esi),%eax\n testl %eax,%eax\n jns 4f\n negl %eax\n 4: addl %eax,(%esp)\n" SEL_TAIL);

static u32 sel_run(int cmov) {
    enum { N=4096, SEL_ROUNDS=16 };
    s32 *a=(s32 *)pool,*b=a+N;
    static u32 ref[4];
    if(pool_claim(1)) {
        u32 seed=0x5e1ec7u;
        for(unsigned i=0;i<N;i++) { a[i]=(s32)(rng(&seed)&0xffffff)-0x800000; b[i]=(s32)(rng(&seed)&0xffffff)-0x800000; }
        for(unsigned i=0;i<4;i++) ref[i]=0;
        for(unsigned i=0;i<N;i++) {
            s32 lo=a[i],hi=b[i];
            if(lo>hi) { lo=b[i]; hi=a[i]; }
            ref[0]+=lo; ref[1]+=hi; ref[2]+=(a[i]&1)?a[i]:b[i]; ref[3]+=a[i]<0?-a[i]:a[i];
        }
    }
    u32 out[4],sum=0;
    for(unsigned r=0;r<SEL_ROUNDS;r++) {
        if(cmov) ie_cmov_sel(a,b,N,out); else ie_jcc_sel(a,b,N,out);
        for(unsigned i=0;i<4;i++) { CHECK_EQ(out[i],ref[i]); sum=csum_mix(sum,out[i]); }
    }
    t_work_unit("element"); t_ops(N*SEL_ROUNDS); return sum;
}
static u32 int_cmov_select(void) { return sel_run(1); }
static u32 int_jcc_select(void) { return sel_run(0); }

/* ---- DIV/IDIV (ropF6/ropF7_16/ropF7_32 return 0 for /6 /7) ---- */
extern u64 ie_div32(const u32 *v,u32 n);
extern u64 ie_div16(const u16 *v,u32 n);
extern u64 ie_div8(const u8 *v,u32 n);
extern u64 ie_idiv(const u32 *v,const s32 *d32,const s16 *d16,const s8 *d8,u32 n);
#define DIV_HEAD "movl 20(%esp),%esi\n movl 24(%esp),%edi\n xorl %ebx,%ebx\n xorl %ebp,%ebp\n movl $10,%ecx\n"
#define DIV_TAIL "decl %edi\n jnz 1b\n movl %ebx,%eax\n movl %ebp,%edx\n" EPI
__asm__(".text\n"
FN("ie_div32") DIV_HEAD "1: movl (%esi),%eax\n 2: xorl %edx,%edx\n divl %ecx\n addl %edx,%ebx\n incl %ebp\n testl %eax,%eax\n jnz 2b\n addl $4,%esi\n" DIV_TAIL
FN("ie_div16") DIV_HEAD "1: movzwl (%esi),%eax\n 2: xorl %edx,%edx\n divw %cx\n addl %edx,%ebx\n incl %ebp\n testw %ax,%ax\n jnz 2b\n addl $2,%esi\n" DIV_TAIL
FN("ie_div8") DIV_HEAD "1: movzbl (%esi),%eax\n 2: divb %cl\n movzbl %ah,%edx\n addl %edx,%ebx\n incl %ebp\n movzbl %al,%eax\n testl %eax,%eax\n jnz 2b\n incl %esi\n" DIV_TAIL
FN("ie_idiv") "movl 20(%esp),%esi\n xorl %edi,%edi\n xorl %ebx,%ebx\n xorl %ebp,%ebp\n"
"1: movl (%esi,%edi,4),%eax\n cltd\n movl 24(%esp),%ecx\n idivl (%ecx,%edi,4)\n addl %eax,%ebx\n addl %edx,%ebp\n roll $1,%ebx\n"
"movw (%esi,%edi,4),%ax\n cwtd\n movl 28(%esp),%ecx\n idivw (%ecx,%edi,2)\n movswl %ax,%eax\n addl %eax,%ebx\n movswl %dx,%edx\n addl %edx,%ebp\n"
"movb (%esi,%edi,4),%al\n cbtw\n movl 32(%esp),%ecx\n idivb (%ecx,%edi)\n movsbl %ah,%edx\n addl %edx,%ebp\n movsbl %al,%eax\n addl %eax,%ebx\n"
"incl %edi\n cmpl 36(%esp),%edi\n jb 1b\n movl %ebx,%eax\n movl %ebp,%edx\n" EPI);

/* Shift-subtract division keeps DIV/IDIV out of the reference. */
static u32 udivr(u32 n,u32 d,u32 *r) {
    u32 q=0,rem=0;
    for(int i=31;i>=0;i--) { rem=rem<<1|(n>>i&1); if(rem>=d) { rem-=d; q|=1u<<i; } }
    *r=rem; return q;
}
static s32 sdivr(s32 n,s32 d,s32 *r) {
    u32 ur,q=udivr(n<0?0u-(u32)n:(u32)n,d<0?0u-(u32)d:(u32)d,&ur);
    *r=n<0?-(s32)ur:(s32)ur; return (n<0)!=(d<0)?-(s32)q:(s32)q;
}
static u64 digits_ref(u32 v) {
    u32 rs=0,n=0,r;
    do { v=udivr(v,10,&r); rs+=r; n++; } while(v);
    return (u64)n<<32|rs;
}
enum { DV_N=1024 };
static void div_data(void) {
    if(!pool_claim(2)) return;
    u32 seed=0xd1d1d1u,*v=pool;
    s32 *d32=(s32 *)(v+DV_N); s16 *d16=(s16 *)(d32+DV_N); s8 *d8=(s8 *)(d16+DV_N);
    for(unsigned i=0;i<DV_N;i++) {
        u32 x=rng(&seed); v[i]=x>>(x&15); /* mixed magnitudes, mixed digit counts */
        x=rng(&seed); d32[i]=(s32)(2+(x&0xffff))*((x>>16&1)?-1:1);
        d16[i]=(s16)((2+(x>>17&1023))*((x>>27&1)?-1:1));
        d8[i]=(s8)((2+(x>>28&7)+(x>>5&31)*2)*((x>>31)?-1:1)); /* |d8| in 2..71 */
    }
}
static u32 div_run(int w) {
    enum { REPS=4 };
    static u64 ref[3]; static u32 have;
    const u32 n=w==32?DV_N:w==16?2*DV_N:4*DV_N;
    div_data();
    if(!(have>>(w>>4)&1)) {
        u64 s=0;
        for(u32 i=0;i<n;i++) s+=digits_ref(w==32?pool[i]:w==16?((u16 *)pool)[i]:((u8 *)pool)[i]);
        ref[w>>4]=s; have|=1u<<(w>>4);
    }
    u64 r=0;
    for(unsigned k=0;k<REPS;k++) {
        r=w==32?ie_div32(pool,n):w==16?ie_div16((u16 *)pool,n):ie_div8((u8 *)pool,n);
        CHECK_EQ((u32)r,(u32)ref[w>>4]); CHECK_EQ((u32)(r>>32),(u32)(ref[w>>4]>>32));
    }
    t_work_unit("div"); t_ops((u32)(ref[w>>4]>>32)*REPS); return (u32)r;
}
static u32 int_div32_itoa(void) { return div_run(32); }
static u32 int_div16_itoa(void) { return div_run(16); }
static u32 int_div8_digits(void) { return div_run(8); }
static u32 int_idiv_hash(void) {
    enum { REPS=8 };
    static u32 refq,refr,have;
    div_data();
    s32 *d32=(s32 *)(pool+DV_N); s16 *d16=(s16 *)(d32+DV_N); s8 *d8=(s8 *)(d16+DV_N);
    if(!have) {
        u32 q=0,rs=0; s32 rr;
        for(unsigned i=0;i<DV_N;i++) {
            q+=sdivr((s32)pool[i],d32[i],&rr); rs+=rr; q=rol32(q,1);
            q+=sdivr((s16)pool[i],d16[i],&rr); rs+=rr;
            q+=sdivr((s8)pool[i],d8[i],&rr); rs+=rr;
        }
        refq=q; refr=rs; have=1;
    }
    u64 r=0;
    for(unsigned k=0;k<REPS;k++) {
        r=ie_idiv(pool,d32,d16,d8,DV_N);
        CHECK_EQ((u32)r,refq); CHECK_EQ((u32)(r>>32),refr);
    }
    t_work_unit("idiv"); t_ops(3*DV_N*REPS); return refq^refr;
}

/* ---- RCL/RCR by CL or imm (ropC0/ropC1/ropD2/ropD3 return 0) ---- */
extern u32 ie_rcx_cl(const u8 *cnt,u32 n,u32 *st);
extern u32 ie_rcx_imm(const u8 *cnt,u32 n,u32 *st);
#define RCX_FN(n,LD,C1,C2,C3,C4) FN(n) \
"movl 20(%esp),%esi\n movl 24(%esp),%edi\n movl 28(%esp),%ecx\n movl (%ecx),%eax\n movl 4(%ecx),%ebx\n movl 8(%ecx),%edx\n movl 12(%ecx),%ebp\n clc\n" \
"1: " LD "rcll " C1 ",%eax\n adcl %eax,%ebx\n rcrl " C2 ",%edx\n adcl %edx,%ebp\n rclw " C3 ",%ax\n rcrb " C4 ",%dl\n adcl %eax,%edx\n" \
"incl %esi\n decl %edi\n jnz 1b\n movl 28(%esp),%ecx\n movl %eax,(%ecx)\n movl %ebx,4(%ecx)\n movl %edx,8(%ecx)\n movl %ebp,12(%ecx)\n sbbl %eax,%eax\n" EPI
__asm__(".text\n" RCX_FN("ie_rcx_cl","movb (%esi),%cl\n","%cl","%cl","%cl","%cl")
                  RCX_FN("ie_rcx_imm","","$7","$13","$5","$3"));
static u32 rc_ref(u32 v,u32 *cf,unsigned w,unsigned c,int left) {
    u32 m=w==32?~0u:(1u<<w)-1,o;
    for(c&=31;c;c--) {
        if(left) { o=v>>(w-1)&1; v=((v<<1)|*cf)&m; }
        else { o=v&1; v=(v>>1)|(*cf<<(w-1)); }
        *cf=o;
    }
    return v;
}
static u32 adc_ref(u32 a,u32 b,u32 *cf) { u64 t=(u64)a+b+*cf; *cf=(u32)(t>>32); return (u32)t; }
static u32 rcx_run(int imm) {
    enum { N=4096, REPS=4 };
    u8 *cnt=(u8 *)pool;
    static u32 ref[2][5],have;
    if(pool_claim(3)) { u32 seed=0x7c1u; for(unsigned i=0;i<N;i++) cnt[i]=1+rng(&seed)%31u; }
    if(!(have>>imm&1)) {
        u32 a=0x12345678,b=0x9abcdef0,d=0x0f1e2d3c,p=0x4b5a6978,cf=0;
        for(unsigned k=0;k<REPS;k++) {
            cf=0; /* each call starts with CLC */
            for(unsigned i=0;i<N;i++) {
                unsigned c1=imm?7:cnt[i],c2=imm?13:cnt[i],c3=imm?5:cnt[i],c4=imm?3:cnt[i];
                a=rc_ref(a,&cf,32,c1,1); b=adc_ref(b,a,&cf);
                d=rc_ref(d,&cf,32,c2,0); p=adc_ref(p,d,&cf);
                a=(a&0xffff0000u)|rc_ref(a&0xffff,&cf,16,c3,1);
                d=(d&0xffffff00u)|rc_ref(d&0xff,&cf,8,c4,0);
                d=adc_ref(d,a,&cf);
            }
        }
        ref[imm][0]=a; ref[imm][1]=b; ref[imm][2]=d; ref[imm][3]=p; ref[imm][4]=0u-cf; have|=1u<<imm;
    }
    u32 st[4]={0x12345678,0x9abcdef0,0x0f1e2d3c,0x4b5a6978},cf=0;
    for(unsigned k=0;k<REPS;k++) cf=imm?ie_rcx_imm(cnt,N,st):ie_rcx_cl(cnt,N,st);
    for(unsigned i=0;i<4;i++) CHECK_EQ(st[i],ref[imm][i]);
    CHECK_EQ(cf,ref[imm][4]);
    t_work_unit("rotate"); t_ops(4*N*REPS); return st[0]^st[1]^st[2]^st[3];
}
static u32 int_rcx_cl(void) { return rcx_run(0); }
static u32 int_rcx_imm(void) { return rcx_run(1); }

/* ---- REPE/REPNE CMPS/SCAS, REP LODS (only REP MOVS/STOS are compiled) ---- */
extern u32 ie_strlen(const u8 *s,u32 n);
extern u32 ie_memchr(const u8 *s,u32 nblk,u32 key);
extern u32 ie_cmpsb(const u8 *a,const u8 *b,u32 nblk,u32 len);
extern u32 ie_cmpsd(const u8 *a,const u8 *b,u32 nblk,u32 len);
extern u32 ie_lods(u32 reps,const u8 *buf);
#define CMPS_FN(n,OP,SH) FN(n) "movl 28(%esp),%edx\n xorl %ebx,%ebx\n xorl %ebp,%ebp\n" \
"1: movl 20(%esp),%esi\n addl %ebp,%esi\n movl 24(%esp),%edi\n addl %ebp,%edi\n movl 32(%esp),%ecx\n " OP "\n jz 2f\n addl $0x10000,%ebx\n" \
"2: movl 32(%esp),%eax\n subl %ecx,%eax\n addl %eax,%ebx\n movl 32(%esp),%eax\n shll $" SH ",%eax\n addl %eax,%ebp\n decl %edx\n jnz 1b\n movl %ebx,%eax\n" EPI
__asm__(".text\n"
FN("ie_strlen") "movl 20(%esp),%edi\n movl 24(%esp),%edx\n xorl %ebx,%ebx\n xorl %eax,%eax\n"
"1: orl $-1,%ecx\n repne scasb\n notl %ecx\n decl %ecx\n addl %ecx,%ebx\n decl %edx\n jnz 1b\n movl %ebx,%eax\n" EPI
FN("ie_memchr") "movl 20(%esp),%esi\n movl 24(%esp),%edx\n movl 28(%esp),%eax\n xorl %ebx,%ebx\n"
"1: movl %esi,%edi\n movl $64,%ecx\n repne scasb\n jne 2f\n subl %esi,%edi\n addl %edi,%ebx\n 2: addl $64,%esi\n decl %edx\n jnz 1b\n movl %ebx,%eax\n" EPI
CMPS_FN("ie_cmpsb","repe cmpsb","0") CMPS_FN("ie_cmpsd","repe cmpsl","2")
FN("ie_lods") "movl 20(%esp),%edx\n xorl %ebx,%ebx\n"
"1: movl 24(%esp),%esi\n movl $4096,%ecx\n rep lodsb\n movzbl %al,%eax\n addl %eax,%ebx\n"
"movl 24(%esp),%esi\n movl $1024,%ecx\n rep lodsl\n addl %eax,%ebx\n decl %edx\n jnz 1b\n movl %ebx,%eax\n" EPI);

enum { SL_N=96, MC_N=256, CM_N=64, CM_LEN=256 };
static u32 int_repne_scasb_strlen(void) {
    enum { REPS=8 };
    u8 *s=(u8 *)pool; static u32 total;
    if(pool_claim(4)) {
        u32 seed=0x57e1u,o=0; total=0;
        for(unsigned i=0;i<SL_N;i++) {
            u32 len=rng(&seed)&511;
            for(u32 j=0;j<len;j++) s[o++]=1+rng(&seed)%255u;
            s[o++]=0; total+=len;
        }
    }
    for(unsigned k=0;k<REPS;k++) CHECK_EQ(ie_strlen(s,SL_N),total);
    t_work_unit("byte"); t_ops((total+SL_N)*REPS); return total;
}
static u32 int_repne_scasb_memchr(void) {
    enum { REPS=16, KEY=0xa5 };
    u8 *s=(u8 *)pool; static u32 total;
    if(pool_claim(5)) {
        u32 seed=0x3e3c4u; total=0;
        for(unsigned i=0;i<MC_N*64;i++) { u8 c=rng(&seed); s[i]=c==KEY?0:c; }
        for(unsigned i=0;i<MC_N;i++) { u32 p=16+rng(&seed)%48u; s[i*64+p]=KEY; total+=p+1; }
    }
    for(unsigned k=0;k<REPS;k++) CHECK_EQ(ie_memchr(s,MC_N,KEY),total);
    t_work_unit("byte"); t_ops(total*REPS); return total;
}
static u32 cmps_run(int dw) {
    enum { REPS=8 };
    u8 *a=(u8 *)pool,*b=a+CM_N*CM_LEN; u16 *mis=(u16 *)(b+CM_N*CM_LEN);
    static u32 ref[2],work[2];
    if(pool_claim(6)) {
        u32 seed=0xc3b5u;
        for(unsigned i=0;i<CM_N*CM_LEN;i++) a[i]=b[i]=rng(&seed);
        for(unsigned i=0;i<CM_N;i++) {
            u32 m=rng(&seed)%(CM_LEN+1);
            mis[i]=m; if(m<CM_LEN) b[i*CM_LEN+m]^=1+rng(&seed)%255u;
        }
        for(int w=0;w<2;w++) {
            u32 len=w?CM_LEN/4:CM_LEN,r=0,e=0;
            for(unsigned i=0;i<CM_N;i++) {
                u32 m=w?mis[i]/4:mis[i];
                if(mis[i]<CM_LEN) { r+=0x10000+m+1; e+=m+1; } else { r+=len; e+=len; }
            }
            ref[w]=r; work[w]=e;
        }
    }
    for(unsigned k=0;k<REPS;k++) CHECK_EQ(dw?ie_cmpsd(a,b,CM_N,CM_LEN/4):ie_cmpsb(a,b,CM_N,CM_LEN),ref[dw]);
    t_work_unit(dw?"dword":"byte"); t_ops(work[dw]*REPS); return ref[dw];
}
static u32 int_repe_cmpsb_memcmp(void) { return cmps_run(0); }
static u32 int_repe_cmpsd_memcmp(void) { return cmps_run(1); }
static u32 int_rep_lods(void) {
    enum { REPS=16 };
    u8 *s=(u8 *)pool;
    if(pool_claim(7)) { u32 seed=0x10d5u; for(unsigned i=0;i<1024;i++) pool[i]=rng(&seed); }
    u32 ref=REPS*(s[4095]+pool[1023]);
    CHECK_EQ(ie_lods(REPS,s),ref);
    t_work_unit("element"); t_ops(REPS*(4096+1024)); return ref;
}

/* ---- REP MOVSD slow paths in the compiled REP routine ---- */
static u32 get32(const u8 *p) { return p[0]|p[1]<<8|p[2]<<16|(u32)p[3]<<24; }
static void stosd(u8 *d,u32 v,u32 n) { __asm__ volatile("rep stosl" : "+D"(d),"+c"(n) : "a"(v) : "memory"); }
static void movsd(u8 *d,const u8 *s,u32 n,int df) {
    if(df) { d+=4*n-4; s+=4*n-4; __asm__ volatile("std\n rep movsl\n cld" : "+D"(d),"+S"(s),"+c"(n) :: "memory"); }
    else __asm__ volatile("rep movsl" : "+D"(d),"+S"(s),"+c"(n) :: "memory");
}
static NOINLINE u32 call_code(u8 *p,u32 c,void *di) {
    u32 r;
    __asm__ volatile("jmp 1f\n1: call *%3" : "=a"(r),"+c"(c),"+D"(di) : "b"(p) : "edx","memory","cc");
    return r;
}
static u32 movsd_run(u32 dst_off,u32 ndw,int df,u32 reps,int code) {
    u32 span=(ndw*4+4096+4095)&~4095u;
    u8 *base=arena_alloc(3*span,4096); if(!base) SKIP("arena_movsd");
    u8 *dst=base+dst_off,*src[2]={base+span,base+2*span};
    for(int k=0;k<2;k++) {
        stosd(src[k],k?0xa5c3a5c3u:0x5a3c5a3cu,ndw);
        ((u32 *)src[k])[0]=0x11111111u*(k+1); ((u32 *)src[k])[ndw-1]=0x22222222u*(k+1);
    }
    if(code) base[0]=0xc3;
    for(int i=0;i<4;i++) dst[4*ndw+i]=0xee;
    for(u32 r=0;r<reps;r++) {
        if(code) call_code(base,0,0);
        movsd(dst,src[r&1],ndw,df);
    }
    const u8 *last=src[(reps-1)&1];
    for(u32 i=0;i<ndw;i+=ndw>64?61:1) CHECK_EQ(get32(dst+4*i),get32(last+4*i));
    CHECK_EQ(get32(dst+4*ndw-4),get32(last+4*ndw-4));
    CHECK_EQ(get32(dst+4*ndw),0xeeeeeeeeu);
    t_work_unit("dword"); t_ops(ndw*reps); return get32(dst);
}
static u32 int_rep_movsd_aligned(void) { return movsd_run(0,256,0,256,0); }
static u32 int_rep_movsd_misaligned(void) { return movsd_run(1,256,0,256,0); }
static u32 int_rep_movsd_backward(void) { return movsd_run(0,256,1,256,0); }
static u32 int_rep_movsd_page(void) { return movsd_run(0,1024,0,64,0); }
static u32 int_rep_movsd_long(void) { return movsd_run(0,16384,0,16,0); }
static u32 int_rep_movsd_codepage(void) { return movsd_run(64,256,0,256,1); }
static u32 int_rep_movsd_codepage_backward(void) { return movsd_run(64,256,1,256,1); }

/* ---- BCD (27 DAA, 2F DAS, 37 AAA, 3F AAS, D4 AAM, D5 AAD: NULL) ---- */
extern u32 ie_daa_add(u8 *d,const u8 *a,const u8 *b,u32 n);
extern u32 ie_das_sub(u8 *d,const u8 *a,const u8 *b,u32 n);
extern u32 ie_aaa_add(u8 *d,const u8 *a,const u8 *b,u32 n);
extern u32 ie_aas_sub(u8 *d,const u8 *a,const u8 *b,u32 n);
extern u64 ie_aam_aad(u32 reps);
#define BCD_HEAD "movl 20(%esp),%edi\n movl 24(%esp),%esi\n movl 28(%esp),%ebx\n movl 32(%esp),%ecx\n clc\n"
#define BCDP_FN(n,OP,ADJ) FN(n) BCD_HEAD "1: movb (%esi),%al\n " OP " (%ebx),%al\n " ADJ "\n movb %al,(%edi)\n incl %esi\n incl %ebx\n incl %edi\n decl %ecx\n jnz 1b\n sbbl %eax,%eax\n" EPI
#define BCDA_FN(n,OP,ADJ) FN(n) BCD_HEAD "1: movb -1(%esi,%ecx),%al\n " OP " -1(%ebx,%ecx),%al\n " ADJ "\n movb %al,-1(%edi,%ecx)\n decl %ecx\n jnz 1b\n sbbl %eax,%eax\n" EPI
__asm__(".text\n" BCDP_FN("ie_daa_add","adcb","daa") BCDP_FN("ie_das_sub","sbbb","das")
        BCDA_FN("ie_aaa_add","adcb","aaa") BCDA_FN("ie_aas_sub","sbbb","aas")
FN("ie_aam_aad") "movl 20(%esp),%edx\n xorl %ebx,%ebx\n xorl %ebp,%ebp\n"
"1: movl $99,%ecx\n 2: movl %ecx,%eax\n aam\n addl %eax,%ebx\n aad\n addl %eax,%ebp\n decl %ecx\n jns 2b\n decl %edx\n jnz 1b\n movl %ebx,%eax\n movl %ebp,%edx\n" EPI);

/* Decimal digit adders: packed (two digits a byte, little endian) or an
   unpacked accumulator plus ASCII digits (most significant first). */
static u32 dec_digit(u32 x,u32 *c,int sub) {
    if(sub) { if((s32)x<0) { x+=10; *c=1; } else *c=0; }
    else if(x>9) { x-=10; *c=1; } else *c=0;
    return x;
}
static u32 bcdp_ref(u8 *acc,const u8 *x,u32 n,int sub) {
    u32 c=0;
    for(u32 i=0;i<n;i++) {
        u32 lo=sub?(acc[i]&15)-(x[i]&15)-c:(acc[i]&15)+(x[i]&15)+c; lo=dec_digit(lo,&c,sub);
        u32 hi=sub?(acc[i]>>4)-(x[i]>>4)-c:(acc[i]>>4)+(x[i]>>4)+c; hi=dec_digit(hi,&c,sub);
        acc[i]=hi<<4|lo;
    }
    return c;
}
static u32 bcda_ref(u8 *acc,const u8 *x,u32 n,int sub) {
    u32 c=0;
    for(u32 i=n;i--;) acc[i]=dec_digit(sub?acc[i]-(x[i]&15)-c:acc[i]+(x[i]&15)+c,&c,sub);
    return c;
}
enum { BCD_K=64, BCD_B=32 };
static u32 bcd_run(int ascii) {
    enum { REPS=4 };
    u8 *x=(u8 *)pool,*acc=x+BCD_K*BCD_B,*racc=acc+BCD_B;
    static u32 refc[2],refs[2],have;
    if(pool_claim(8+ascii)) {
        u32 seed=0xbcdu+ascii;
        for(unsigned i=0;i<BCD_K*BCD_B;i++) {
            u32 r=rng(&seed),lo=(r&0xffff)%10u,hi=(r>>16)%10u;
            x[i]=ascii?(u8)(0x30+lo):(u8)(hi<<4|lo);
        }
    }
    if(!(have>>ascii&1)) {
        u32 cs=0;
        for(unsigned i=0;i<BCD_B;i++) racc[i]=0;
        for(unsigned r=0;r<REPS;r++) for(unsigned k=0;k<BCD_K;k++) {
            const u8 *xi=x+k*BCD_B,*yi=x+((k*7+3)%BCD_K)*BCD_B;
            cs=cs*2+(ascii?bcda_ref(racc,xi,BCD_B,0):bcdp_ref(racc,xi,BCD_B,0));
            cs=cs*2+(ascii?bcda_ref(racc,yi,BCD_B,1):bcdp_ref(racc,yi,BCD_B,1));
        }
        u32 s=0; for(unsigned i=0;i<BCD_B;i++) s=csum_mix(s,racc[i]);
        refc[ascii]=cs; refs[ascii]=s; have|=1u<<ascii;
    }
    u32 cs=0;
    for(unsigned i=0;i<BCD_B;i++) acc[i]=0;
    for(unsigned r=0;r<REPS;r++) for(unsigned k=0;k<BCD_K;k++) {
        const u8 *xi=x+k*BCD_B,*yi=x+((k*7+3)%BCD_K)*BCD_B;
        cs=cs*2+(ascii?-ie_aaa_add(acc,acc,xi,BCD_B):-ie_daa_add(acc,acc,xi,BCD_B));
        cs=cs*2+(ascii?-ie_aas_sub(acc,acc,yi,BCD_B):-ie_das_sub(acc,acc,yi,BCD_B));
    }
    u32 s=0; for(unsigned i=0;i<BCD_B;i++) s=csum_mix(s,acc[i]);
    CHECK_EQ(cs,refc[ascii]); CHECK_EQ(s,refs[ascii]);
    t_work_unit(ascii?"digit":"byte"); t_ops(REPS*BCD_K*2*BCD_B); return s;
}
static u32 int_bcd_packed_daa_das(void) { return bcd_run(0); }
static u32 int_bcd_ascii_aaa_aas(void) { return bcd_run(1); }
static u32 int_bcd_aam_aad(void) {
    enum { REPS=64 };
    u32 q=0,r=0,sa=0,sd=0;
    for(u32 v=0;v<100;v++) { sa+=q<<8|r; sd+=v; if(++r==10) { r=0; q++; } }
    u64 got=ie_aam_aad(REPS);
    CHECK_EQ((u32)got,sa*REPS); CHECK_EQ((u32)(got>>32),sd*REPS);
    t_work_unit("aam_aad"); t_ops(100*REPS); return (u32)got;
}

/* ---- RDTSC (0F 31) and CPUID (0F A2): NULL in recomp_opcodes_0f ---- */
static u32 int_rdtsc_rate(void) {
    enum { N=1024 };
    u32 lo0,hi0,lo1,hi1;
    __asm__ volatile("rdtsc" : "=a"(lo0),"=d"(hi0));
    for(unsigned i=0;i<N;i++)
        __asm__ volatile("rdtsc\n rdtsc\n rdtsc\n rdtsc\n rdtsc\n rdtsc\n rdtsc\n rdtsc" ::: "eax","edx");
    __asm__ volatile("rdtsc" : "=a"(lo1),"=d"(hi1));
    CHECK(((u64)hi1<<32|lo1)>((u64)hi0<<32|lo0));
    t_work_unit("rdtsc"); t_ops(8*N); return 8*N;
}
static u32 int_cpuid_rate(void) {
    enum { N=1024 };
    u32 r0[4],a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(r0[0]),"=b"(r0[1]),"=c"(r0[2]),"=d"(r0[3]) : "a"(0));
    for(unsigned i=0;i<N;i++) {
        __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0));
        if(a!=r0[0] || b!=r0[1] || c!=r0[2] || d!=r0[3]) { t_failv("cpuid leaf 0 stable",__LINE__,a^b^c^d,r0[0]^r0[1]^r0[2]^r0[3]); return 0; }
    }
    t_work_unit("cpuid"); t_ops(N); return r0[1]^r0[2]^r0[3];
}

/* ---- Fallback density: BOUND (62, NULL, does not end the block) every D
   of 64 compiled register ALU ops per iteration ---- */
extern void ie_dens_none(u32 n,const u32 *bnd,u32 *st);
extern void ie_dens_4(u32 n,const u32 *bnd,u32 *st);
extern void ie_dens_16(u32 n,const u32 *bnd,u32 *st);
extern void ie_dens_64(u32 n,const u32 *bnd,u32 *st);
#define ALU4 "addl %ebx,%eax\n xorl %eax,%edx\n roll $5,%edx\n subl %edx,%ebx\n"
#define BND ".byte 0x62,0x0e\n" /* bound %ecx,(%esi) */
#define A16 ALU4 ALU4 ALU4 ALU4
#define A4B ALU4 BND
#define A16B A16 BND
#define DENS_FN(n,BODY) FN(n) "movl 20(%esp),%ecx\n movl 24(%esp),%esi\n movl 28(%esp),%edi\n movl (%edi),%eax\n movl 4(%edi),%ebx\n movl 8(%edi),%edx\n" \
"1: " BODY "decl %ecx\n jnz 1b\n movl %eax,(%edi)\n movl %ebx,4(%edi)\n movl %edx,8(%edi)\n" EPI
__asm__(".text\n" DENS_FN("ie_dens_none",A16 A16 A16 A16)
        DENS_FN("ie_dens_4",A4B A4B A4B A4B A4B A4B A4B A4B A4B A4B A4B A4B A4B A4B A4B A4B)
        DENS_FN("ie_dens_16",A16B A16B A16B A16B) DENS_FN("ie_dens_64",A16 A16 A16 A16B));
static u32 dens_run(void (*fn)(u32,const u32 *,u32 *)) {
    enum { N=4096 };
    static const u32 bnd[2]={0,0x7fffffff};
    static u32 ref[3],have;
    if(!have) {
        u32 a=1,b=0x9e3779b9u,d=0x7f4a7c15u;
        for(u32 i=0;i<N*16;i++) { a+=b; d^=a; d=rol32(d,5); b-=d; }
        ref[0]=a; ref[1]=b; ref[2]=d; have=1;
    }
    u32 st[3]={1,0x9e3779b9u,0x7f4a7c15u};
    fn(N,bnd,st);
    for(unsigned i=0;i<3;i++) CHECK_EQ(st[i],ref[i]);
    t_work_unit("alu_op"); t_ops(64*N); return st[0]^st[1]^st[2];
}
static u32 int_fallback_density_none(void) { return dens_run(ie_dens_none); }
static u32 int_fallback_density_d4(void) { return dens_run(ie_dens_4); }
static u32 int_fallback_density_d16(void) { return dens_run(ie_dens_16); }
static u32 int_fallback_density_d64(void) { return dens_run(ie_dens_64); }

/* ---- Code-page placement: false SMC and a loop straddling two pages ---- */
extern const u8 ie_smc_tpl[],ie_smc_tpl_end[],ie_split_tpl[],ie_split_tpl_end[];
__asm__(".text\n"
"ie_smc_tpl:\n xorl %eax,%eax\n 1: addl %ecx,%eax\n movl %eax,(%edi)\n xorl $0x5a5a5a5a,%eax\n decl %ecx\n jnz 1b\n ret\n ie_smc_tpl_end:\n"
"ie_split_tpl:\n xorl %eax,%eax\n xorl %edx,%edx\n 1: addl %ecx,%eax\n roll $3,%eax\n xorl %eax,%edx\n addl $0x9e3779b9,%edx\n subl %edx,%eax\n decl %ecx\n jnz 1b\n"
" xorl %edx,%eax\n ret\n ie_split_tpl_end:\n");
enum { CP_N=4096, CP_CALLS=16 };
static u8 *place(const u8 *t,const u8 *e,u32 off) {
    u8 *p=arena_alloc(8192+64,4096); if(!p) return 0;
    for(u32 i=0;i<(u32)(e-t);i++) p[off+i]=t[i];
    return p;
}
static u32 false_smc_run(u32 slot) {
    u8 *p=place(ie_smc_tpl,ie_smc_tpl_end,0); if(!p) SKIP("arena_smc");
    u32 ref=0,last=0;
    for(u32 c=CP_N;c;c--) { ref+=c; last=ref; ref^=0x5a5a5a5au; }
    for(unsigned k=0;k<CP_CALLS;k++) CHECK_EQ(call_code(p,CP_N,p+slot),ref);
    CHECK_EQ(*(volatile u32 *)(p+slot),last);
    t_work_unit("store"); t_ops(CP_N*CP_CALLS); return ref;
}
static u32 int_false_smc_same_chunk(void) { return false_smc_run(48); }
static u32 int_false_smc_same_page(void) { return false_smc_run(2048); }
static u32 int_false_smc_other_page(void) { return false_smc_run(4096+48); }
static u32 page_loop_run(int split) {
    /* the loop body starts 4 bytes into the template; split puts it 9 bytes before the page end */
    u8 *p=place(ie_split_tpl,ie_split_tpl_end,split?4096-4-9:256); if(!p) SKIP("arena_split");
    u32 a=0,d=0;
    for(u32 c=CP_N;c;c--) { a+=c; a=rol32(a,3); d^=a; d+=0x9e3779b9u; a-=d; }
    a^=d;
    for(unsigned k=0;k<CP_CALLS;k++) CHECK_EQ(call_code(p+(split?4096-4-9:256),CP_N,0),a);
    t_work_unit("iteration"); t_ops(CP_N*CP_CALLS); return a;
}
static u32 int_loop_page_split(void) { return page_loop_run(1); }
static u32 int_loop_page_inside(void) { return page_loop_run(0); }

/* ---- Branches and lazy flags ---- */
extern u32 ie_jcc_dd(const u8 *buf,u32 n,u32 reps);
extern void ie_adc256(u32 *acc,const u32 *x,u32 n);
extern u64 ie_flagmix(const u32 *tri,u32 n);
extern void ie_op16(const u16 *src,u16 *dst,u32 n,u32 *st);
#define ACW(o) "movl " o "(%esi),%eax\n adcl %eax," o "(%edi)\n"
#define SBW(o) "movl " o "(%esi),%eax\n sbbl %eax," o "-32(%edi)\n"
#define FLAGB "movzbl %cl,%ecx\n"
__asm__(".text\n"
FN("ie_jcc_dd") "movl 28(%esp),%edx\n xorl %ebx,%ebx\n"
"2: movl 20(%esp),%esi\n movl 24(%esp),%ecx\n 1: movb (%esi),%al\n testb $1,%al\n jz 3f\n incl %ebx\n 3: incl %esi\n decl %ecx\n jnz 1b\n decl %edx\n jnz 2b\n movl %ebx,%eax\n" EPI
FN("ie_adc256") "movl 20(%esp),%edi\n movl 24(%esp),%esi\n movl 28(%esp),%ecx\n"
"1: movl (%esi),%eax\n addl %eax,(%edi)\n" ACW("4") ACW("8") ACW("12") ACW("16") ACW("20") ACW("24") ACW("28")
"movl 32(%esi),%eax\n subl %eax,(%edi)\n" SBW("36") SBW("40") SBW("44") SBW("48") SBW("52") SBW("56") SBW("60")
"addl $32,%esi\n decl %ecx\n jnz 1b\n" EPI
FN("ie_flagmix") "movl 20(%esp),%esi\n pushl 24(%esp)\n xorl %ebx,%ebx\n xorl %ebp,%ebp\n"
"1: movl (%esi),%edi\n movl 4(%esi),%edx\n"
"addl %edx,%edi\n pushfl\n popl %ecx\n andl $0x8d5,%ecx\n addl %ecx,%ebx\n"
"subl %edx,%edi\n lahf\n movzbl %ah,%ecx\n andl $0xd5,%ecx\n addl %ecx,%ebp\n"
"xorl %edx,%edi\n setle %cl\n" FLAGB "addl %ecx,%ebx\n"
"incl %edi\n setg %cl\n" FLAGB "addl %ecx,%ebp\n"
"decl %edi\n setbe %cl\n" FLAGB "addl %ecx,%ebx\n"
"movl 8(%esi),%ecx\n andl $0x8d5,%ecx\n orl $0x202,%ecx\n pushl %ecx\n popfl\n"
"setg %al\n seta %ah\n adcl $0,%ebx\n movzwl %ax,%ecx\n addl %ecx,%ebp\n roll $1,%ebx\n"
"addl $12,%esi\n decl (%esp)\n jnz 1b\n popl %ecx\n movl %ebx,%eax\n movl %ebp,%edx\n" EPI
FN("ie_op16") "movl 20(%esp),%esi\n movl 24(%esp),%edi\n movl 28(%esp),%ecx\n movl 32(%esp),%eax\n movl (%eax),%ebx\n movl 4(%eax),%edx\n movl 8(%eax),%ebp\n"
"1: movw (%esi),%ax\n addw %ax,%bx\n xorw %bx,%dx\n rolw $3,%dx\n subw %dx,%bx\n adcw %ax,%bp\n movw %dx,(%edi)\n shrw $1,%ax\n xorw %ax,%bp\n"
"addl $2,%esi\n addl $2,%edi\n decl %ecx\n jnz 1b\n movl 32(%esp),%eax\n movl %ebx,(%eax)\n movl %edx,4(%eax)\n movl %ebp,8(%eax)\n" EPI);

static u32 int_jcc_data_dependent(void) {
    enum { N=4096, REPS=16 };
    u8 *s=(u8 *)pool; static u32 odd;
    if(pool_claim(10)) { u32 seed=0x0dd5u; odd=0; for(unsigned i=0;i<N;i++) { s[i]=rng(&seed); odd+=s[i]&1; } }
    CHECK_EQ(ie_jcc_dd(s,N,REPS),odd*REPS);
    t_work_unit("element"); t_ops(N*REPS); return odd;
}
static u32 int_flags_adc_sbb_chain(void) {
    enum { N=255, REPS=16 };
    static u32 ref[8];
    if(pool_claim(11)) {
        u32 seed=0xadc5bbu; for(unsigned i=0;i<(N+1)*8;i++) pool[i]=rng(&seed);
        /* acc = REPS*(x[0]-x[N]) mod 2^256; the sum telescopes */
        u32 diff[8],b=0;
        for(unsigned i=0;i<8;i++) { u32 x=pool[i],y=pool[N*8+i]; u32 r=x-y-b; b=x<y || (b && x==y); diff[i]=r; }
        for(unsigned i=0;i<8;i++) ref[i]=0;
        for(unsigned r=0;r<REPS;r++) { u32 c=0; for(unsigned i=0;i<8;i++) ref[i]=adc_ref(ref[i],diff[i],&c); }
    }
    u32 acc[8]={0};
    for(unsigned r=0;r<REPS;r++) ie_adc256(acc,pool,N);
    u32 s=0;
    for(unsigned i=0;i<8;i++) { CHECK_EQ(acc[i],ref[i]); s=csum_mix(s,acc[i]); }
    t_work_unit("limb_op"); t_ops(16*N*REPS); return s;
}
#define FCF 0x001u
#define FPF 0x004u
#define FAF 0x010u
#define FZF 0x040u
#define FSF 0x080u
#define FOF 0x800u
static u32 szp(u32 r) { u32 p=r&255; p^=p>>4; p&=15; return (r?0:FZF)|(r>>31?FSF:0)|((0x9669u>>p&1)?FPF:0); }
static u32 int_flags_pushf_lahf_setcc(void) {
    enum { N=1024, REPS=8 };
    u32 *t=pool; static u32 ra,rb;
    if(pool_claim(12)) {
        u32 seed=0xf1a95u;
        for(unsigned i=0;i<N;i++) {
            u32 a=rng(&seed),b=rng(&seed);
            switch(i&7) {
            case 0: b=a; break;                 /* xor gives zero */
            case 1: b=a^0x7fffffffu; break;     /* inc overflows */
            case 2: a=0x7fffffffu; b=1; break;  /* add overflows */
            case 3: b=0u-a; break;              /* add gives zero, carry */
            case 4: a&=0xff; b&=0x0f; break;    /* auxiliary carries */
            }
            t[3*i]=a; t[3*i+1]=b; t[3*i+2]=rng(&seed);
        }
        u32 x=0,y=0;
        for(unsigned i=0;i<N;i++) {
            u32 a=t[3*i],b=t[3*i+1],r=a+b,f;
            f=szp(r)|(r<a?FCF:0)|((~(a^b)&(a^r))>>31?FOF:0)|((a^b^r)&16?FAF:0); x+=f;
            f=szp(a)|(r<b?FCF:0)|((r^b^a)&16?FAF:0); y+=f&0xd5;
            u32 v=a^b; x+=(!v || v>>31);
            u32 w=v+1; y+=(w && ((w>>31)==(v==0x7fffffffu)));
            x+=(v==0);
            f=t[3*i+2]&0x8d5;
            u32 g=!(f&FZF) && !(f&FSF)==!(f&FOF),ab=!(f&(FCF|FZF));
            x+=f&FCF; y+=g|ab<<8; x=rol32(x,1);
        }
        ra=x; rb=y;
    }
    u64 r=0;
    for(unsigned k=0;k<REPS;k++) r=ie_flagmix(t,N);
    CHECK_EQ((u32)r,ra); CHECK_EQ((u32)(r>>32),rb);
    t_work_unit("element"); t_ops(N*REPS); return ra^rb;
}
static u32 int_op16_prefixed(void) {
    enum { N=2048, REPS=16 };
    u16 *src=(u16 *)pool,*dst=src+N;
    static u32 ref[4];
    if(pool_claim(13)) {
        u32 seed=0x16b17u; for(unsigned i=0;i<N;i++) src[i]=rng(&seed);
        u32 bx=0x1234,dx=0x5678,bp=0x9abc,s=0;
        for(unsigned k=0;k<REPS;k++) for(unsigned i=0;i<N;i++) {
            u32 ax=src[i],c;
            bx=(bx+ax)&0xffff; dx^=bx; dx=(dx<<3|dx>>13)&0xffff;
            c=bx<dx; bx=(bx-dx)&0xffff; bp=(bp+ax+c)&0xffff;
            if(k==REPS-1) s=csum_mix(s,dx);
            bp^=ax>>1;
        }
        ref[0]=0xa5a50000u|bx; ref[1]=0x5a5a0000u|dx; ref[2]=0xc3c30000u|bp; ref[3]=s;
    }
    u32 st[3]={0xa5a51234u,0x5a5a5678u,0xc3c39abcu};
    for(unsigned k=0;k<REPS;k++) ie_op16(src,dst,N,st);
    u32 s=0; for(unsigned i=0;i<N;i++) s=csum_mix(s,dst[i]);
    for(unsigned i=0;i<3;i++) CHECK_EQ(st[i],ref[i]);
    CHECK_EQ(s,ref[3]);
    t_work_unit("element"); t_ops(N*REPS); return s;
}

const test_t tests_int_ext[]={
    {"int_cmov_select",int_cmov_select,REQ_CMOV},
    {"int_jcc_select",int_jcc_select,REQ_486},
    {"int_div32_itoa",int_div32_itoa,REQ_486},
    {"int_div16_itoa",int_div16_itoa,REQ_486},
    {"int_div8_digits",int_div8_digits,REQ_486},
    {"int_idiv_hash",int_idiv_hash,REQ_486},
    {"int_rcx_cl",int_rcx_cl,REQ_486},
    {"int_rcx_imm",int_rcx_imm,REQ_486},
    {"int_repne_scasb_strlen",int_repne_scasb_strlen,REQ_486},
    {"int_repne_scasb_memchr",int_repne_scasb_memchr,REQ_486},
    {"int_repe_cmpsb_memcmp",int_repe_cmpsb_memcmp,REQ_486},
    {"int_repe_cmpsd_memcmp",int_repe_cmpsd_memcmp,REQ_486},
    {"int_rep_lods",int_rep_lods,REQ_486},
    {"int_rep_movsd_aligned",int_rep_movsd_aligned,REQ_486},
    {"int_rep_movsd_misaligned",int_rep_movsd_misaligned,REQ_486},
    {"int_rep_movsd_backward",int_rep_movsd_backward,REQ_486},
    {"int_rep_movsd_page",int_rep_movsd_page,REQ_486},
    {"int_rep_movsd_long",int_rep_movsd_long,REQ_486},
    {"int_rep_movsd_codepage",int_rep_movsd_codepage,REQ_486,T_TAG_SMC},
    {"int_rep_movsd_codepage_backward",int_rep_movsd_codepage_backward,REQ_486,T_TAG_SMC},
    {"int_bcd_packed_daa_das",int_bcd_packed_daa_das,REQ_486},
    {"int_bcd_ascii_aaa_aas",int_bcd_ascii_aaa_aas,REQ_486},
    {"int_bcd_aam_aad",int_bcd_aam_aad,REQ_486},
    {"int_rdtsc_rate",int_rdtsc_rate,REQ_TSC},
    {"int_cpuid_rate",int_cpuid_rate,REQ_CPUID},
    {"int_fallback_density_none",int_fallback_density_none,REQ_486},
    {"int_fallback_density_d4",int_fallback_density_d4,REQ_486},
    {"int_fallback_density_d16",int_fallback_density_d16,REQ_486},
    {"int_fallback_density_d64",int_fallback_density_d64,REQ_486},
    {"int_false_smc_same_chunk",int_false_smc_same_chunk,REQ_486},
    {"int_false_smc_same_page",int_false_smc_same_page,REQ_486},
    {"int_false_smc_other_page",int_false_smc_other_page,REQ_486},
    {"int_loop_page_split",int_loop_page_split,REQ_486},
    {"int_loop_page_inside",int_loop_page_inside,REQ_486},
    {"int_jcc_data_dependent",int_jcc_data_dependent,REQ_486},
    {"int_flags_adc_sbb_chain",int_flags_adc_sbb_chain,REQ_486},
    {"int_flags_pushf_lahf_setcc",int_flags_pushf_lahf_setcc,REQ_486},
    {"int_op16_prefixed",int_op16_prefixed,REQ_486},
    {0,0,0}
};
