/* All floating-point computation is explicit x87; C handles raw encodings only. */
#include "kernel.h"
typedef struct { u32 w[3]; } ext80;
static const ext80 eone={{0,0x80000000,0x3fff}}, etwo={{0,0x80000000,0x4000}},
    epi={{0x2168c235,0xc90fdaa2,0x4000}};
static void init(void) { __asm__ volatile("fninit" ::: "memory"); }
static u16 status(void) { u16 sw; __asm__ volatile("fnstsw %0":"=m"(sw)); return sw; }
static void control(u16 cw) { __asm__ volatile("fldcw %0"::"m"(cw):"memory"); }
static void ext_check(const ext80 *a,const ext80 *b) {
    CHECK_EQ(a->w[0],b->w[0]); CHECK_EQ(a->w[1],b->w[1]); CHECK_EQ(a->w[2]&0xffff,b->w[2]&0xffff);
}
static u32 ext_sum(u32 s,const ext80 *a) { return csum_mix(csum_mix(csum_mix(s,a->w[0]),a->w[1]),a->w[2]&0xffff); }
static void show(const char *name,const ext80 *v) { t_note("%s=%04x:%08x%08x",name,v->w[2]&0xffff,v->w[1],v->w[0]); }
/* Error bound: 2^-55 relative (absolute at zero), or 2^-50 for TCG
   whose transcendental implementation rounds through host double precision. */
static int tcg(void) { return cpu.hv && !kmemcmp(cpu.hv_vendor,"TCGTCGTCGTCG",12); }
static void near(const char *name,const ext80 *got,const ext80 *want) {
    ext80 error={{0}}, tolerance={{0}}; u16 sw; s32 power=tcg()?-50:-55;
    const ext80 *scale=(want->w[1]|want->w[0])?want:&eone;
    __asm__ volatile("fldt %2; fldt %3; fsubrp %%st,%%st(1); fabs; fstpt %0;"
                     "fildl %4; fldt %5; fabs; fscale; fstpt %1; fstp %%st(0)"
                     : "=m"(error),"=m"(tolerance) : "m"(*got),"m"(*want),"m"(power),"m"(*scale) : "memory");
    __asm__ volatile("fldt %1; fldt %2; fcompp; fnstsw %0" : "=m"(sw) : "m"(tolerance),"m"(error) : "memory");
    show(name,got);
    CHECK((sw&0x4500)==0x0100 || (sw&0x4500)==0x4000);
}
#define RESULT(code,want) do { u64 r; __asm__ volatile(code "\n fstpl %0" : "=m"(r) : "m"(a),"m"(b),"m"(f),"m"(d),"m"(i16),"m"(i32) : "memory"); CHECK(r==(want)); } while(0)
static u32 fpu_arith(void) {
    s32 a=8,b=2,i32=2; s16 i16=2; u32 f=0x40000000; u64 d=0x4000000000000000ull;
    init();
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fadd %%st(1),%%st; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x4024000000000000ull); }
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fadd %%st,%%st(1); fxch; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x4024000000000000ull); }
    RESULT("fildl %1; fildl %2; faddp %%st,%%st(1)",0x4024000000000000ull);
    RESULT("fildl %1; fadds %3",0x4024000000000000ull);
    RESULT("fildl %1; faddl %4",0x4024000000000000ull);
    RESULT("fildl %1; fiadds %5",0x4024000000000000ull);
    RESULT("fildl %1; fiaddl %6",0x4024000000000000ull);
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fsub %%st(1),%%st; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0xc018000000000000ull); }
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fsub %%st,%%st(1); fxch; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0xc018000000000000ull); }
    RESULT("fildl %1; fildl %2; fsubp %%st,%%st(1)",0xc018000000000000ull);
    RESULT("fildl %1; fsubs %3",0x4018000000000000ull);
    RESULT("fildl %1; fsubl %4",0x4018000000000000ull);
    RESULT("fildl %1; fisubs %5",0x4018000000000000ull);
    RESULT("fildl %1; fisubl %6",0x4018000000000000ull);
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fsubr %%st(1),%%st; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x4018000000000000ull); }
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fsubr %%st,%%st(1); fxch; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x4018000000000000ull); }
    RESULT("fildl %1; fildl %2; fsubrp %%st,%%st(1)",0x4018000000000000ull);
    RESULT("fildl %1; fsubrs %3",0xc018000000000000ull);
    RESULT("fildl %1; fsubrl %4",0xc018000000000000ull);
    RESULT("fildl %1; fisubrs %5",0xc018000000000000ull);
    RESULT("fildl %1; fisubrl %6",0xc018000000000000ull);
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fmul %%st(1),%%st; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x4030000000000000ull); }
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fmul %%st,%%st(1); fxch; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x4030000000000000ull); }
    RESULT("fildl %1; fildl %2; fmulp %%st,%%st(1)",0x4030000000000000ull);
    RESULT("fildl %1; fmuls %3",0x4030000000000000ull);
    RESULT("fildl %1; fmull %4",0x4030000000000000ull);
    RESULT("fildl %1; fimuls %5",0x4030000000000000ull);
    RESULT("fildl %1; fimull %6",0x4030000000000000ull);
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fdiv %%st(1),%%st; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x3fd0000000000000ull); }
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fdiv %%st,%%st(1); fxch; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x3fd0000000000000ull); }
    RESULT("fildl %1; fildl %2; fdivp %%st,%%st(1)",0x3fd0000000000000ull);
    RESULT("fildl %1; fdivs %3",0x4010000000000000ull);
    RESULT("fildl %1; fdivl %4",0x4010000000000000ull);
    RESULT("fildl %1; fidivs %5",0x4010000000000000ull);
    RESULT("fildl %1; fidivl %6",0x4010000000000000ull);
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fdivr %%st(1),%%st; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x4010000000000000ull); }
    { u64 r; __asm__ volatile("fildl %1; fildl %2; fdivr %%st,%%st(1); fxch; fstpl %0; fstp %%st(0)" : "=m"(r) : "m"(a),"m"(b) : "memory"); CHECK(r==0x4010000000000000ull); }
    RESULT("fildl %1; fildl %2; fdivrp %%st,%%st(1)",0x4010000000000000ull);
    RESULT("fildl %1; fdivrs %3",0x3fd0000000000000ull);
    RESULT("fildl %1; fdivrl %4",0x3fd0000000000000ull);
    RESULT("fildl %1; fidivrs %5",0x3fd0000000000000ull);
    RESULT("fildl %1; fidivrl %6",0x3fd0000000000000ull);
    {
        static const u16 pc[]={0x007f,0x027f,0x037f};
        static const ext80 thirds[]={{{0,0xaaaaab00,0x3ffd}},{{0xaaaaa800,0xaaaaaaaa,0x3ffd}},{{0xaaaaaaab,0xaaaaaaaa,0x3ffd}}};
        s32 three=3;
        for(unsigned i=0;i<3;++i) {
            ext80 e={{0}}; u32 s; u64 r;
            control(pc[i]);
            __asm__ volatile("fld1; fidivl %3; fsts %0; fstl %1; fstpt %2" : "=m"(s),"=m"(r),"=m"(e) : "m"(three) : "memory");
            CHECK_EQ(s,0x3eaaaaab); CHECK(r==(i?0x3fd5555555555555ull:0x3fd5555560000000ull)); ext_check(&e,&thirds[i]);
        }
        control(0x037f);
    }
    { u64 r; s32 four=4;
      __asm__ volatile("fildl %1; fsqrt; fchs; fabs; fstpl %0":"=m"(r):"m"(four):"memory"); CHECK(r==0x4000000000000000ull);
      __asm__ volatile("fildl %1; fildl %2; fxch; fstpl %0; fstp %%st(0)":"=m"(r):"m"(a),"m"(b):"memory"); CHECK(r==0x4020000000000000ull);
    }
    t_ops(150); return 0x3eaaaaab;
}
#undef RESULT

static u32 fpu_formats(void) {
    static const u32 singles[]={0,0x80000000,1,0x007fffff,0x00800000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc12345};
    static const u64 doubles[]={0,0x8000000000000000ull,1,0xfffffffffffffull,0x10000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8123456789abcull};
    static const ext80 extended[]={{{0,0,0}},{{0,0,0x8000}},{{1,0,0}},{{0xffffffff,0x7fffffff,0}},{{0,0x80000000,1}},{{0,0x80000000,0x3fff}},{{0,0x80000000,0x7fff}},{{0x12345678,0xc0000000,0x7fff}}};
    static const s64 ints[]={0,1,-1,32767,-32768,2147483647ll,(-2147483647ll-1),0x123456789abcdefll,(-0x7fffffffffffffffll-1)};
    u32 sum=0; init();
    for(unsigned i=0;i<sizeof(singles)/sizeof(*singles);++i) { u32 r; __asm__ volatile("flds %1; fstps %0":"=m"(r):"m"(singles[i]):"memory"); CHECK_EQ(r,singles[i]); sum=csum_mix(sum,r); }
    for(unsigned i=0;i<sizeof(doubles)/sizeof(*doubles);++i) { u64 r; __asm__ volatile("fldl %1; fstpl %0":"=m"(r):"m"(doubles[i]):"memory"); CHECK(r==doubles[i]); sum=csum_mix(sum,(u32)r); }
    for(unsigned i=0;i<sizeof(extended)/sizeof(*extended);++i) { ext80 r={{0}}; __asm__ volatile("fldt %1; fstpt %0":"=m"(r):"m"(extended[i]):"memory"); ext_check(&r,&extended[i]); sum=ext_sum(sum,&r); }
    for(unsigned i=0;i<sizeof(ints)/sizeof(*ints);++i) {
        s64 r; __asm__ volatile("fildll %1; fistpll %0":"=m"(r):"m"(ints[i]):"memory"); CHECK(r==ints[i]);
        if(i<5) { s16 v=(s16)ints[i],o,p; __asm__ volatile("filds %2; fists %0; fistps %1":"=m"(o),"=m"(p):"m"(v):"memory"); CHECK_EQ((u16)o,(u16)v); CHECK_EQ((u16)p,(u16)v); }
        if(i<7) { s32 v=(s32)ints[i],o,p; __asm__ volatile("fildl %2; fistl %0; fistpl %1":"=m"(o),"=m"(p):"m"(v):"memory"); CHECK_EQ(o,v); CHECK_EQ(p,v); }
    }
    { const u8 bcd[10]={0x78,0x56,0x34,0x12,0x90,0x78,0x56,0x34,0x12,0}; u8 out[10]; s64 n=123456789012345678ll,r;
      __asm__ volatile("fbld %2; fistpll %0; fildll %3; fbstp %1":"=m"(r),"=m"(out):"m"(bcd),"m"(n):"memory"); CHECK(r==n); CHECK(!kmemcmp(out,bcd,10));
      n=-n; __asm__ volatile("fildll %1; fbstp %0":"=m"(out):"m"(n):"memory"); CHECK_EQ(out[9],0x80); CHECK(!kmemcmp(out,bcd,9));
      __asm__ volatile("fbld %1; fistpll %0":"=m"(r):"m"(out):"memory"); CHECK(r==n);
    }
    {
        static const ext80 constants[]={{{0,0x80000000,0x3fff}},{{0,0,0}},{{0x2168c235,0xc90fdaa2,0x4000}},{{0x5c17f0bc,0xb8aa3b29,0x3fff}},{{0xcd1b8afe,0xd49a784b,0x4000}},{{0xfbcff799,0x9a209a84,0x3ffd}},{{0xd1cf79ac,0xb17217f7,0x3ffe}}};
        ext80 r={{0}};
#define CONSTANT(n,k) __asm__ volatile(n "; fstpt %0":"=m"(r)::"memory"); ext_check(&r,&constants[k]);
        CONSTANT("fld1",0) CONSTANT("fldz",1) CONSTANT("fldpi",2) CONSTANT("fldl2e",3) CONSTANT("fldl2t",4) CONSTANT("fldlg2",5) CONSTANT("fldln2",6)
#undef CONSTANT
    }
    t_ops(150); init(); return sum;
}
static u32 fpu_compare(void) {
    u16 sw; u32 sum=0; init();
    for(unsigned k=0;k<3;++k) {
        s32 a=k==0?1:k==1?3:2,b=2; u32 s=0x40000000; u64 d=0x4000000000000000ull; s16 h=2;
        u16 want=k==0?0x100:k==1?0:0x4000;
#define COMP(code) do { __asm__ volatile("fninit; fildl %1; fildl %2; " code "; fnstsw %0":"=m"(sw):"m"(b),"m"(a),"m"(s),"m"(d),"m"(h):"memory"); CHECK_EQ(sw&0x4500,want); sum=csum_mix(sum,sw); } while(0)
        COMP("fcom %%st(1)"); COMP("fcomp %%st(1)"); COMP("fcompp"); COMP("fucom %%st(1)"); COMP("fucomp %%st(1)"); COMP("fucompp");
        COMP("fcoms %3"); COMP("fcomps %3"); COMP("fcoml %4"); COMP("fcompl %4"); COMP("ficoms %5"); COMP("ficomps %5"); COMP("ficoml %1"); COMP("ficompl %1");
#undef COMP
    }
    { u64 nan=0x7ff8000000000000ull;
#define UNORDERED(op) __asm__ volatile("fninit; fld1; fldl %1; " op "; fnstsw %0":"=m"(sw):"m"(nan):"memory"); CHECK_EQ(sw&0x4500,0x4500);
      UNORDERED("fcom %%st(1)") UNORDERED("fcomp %%st(1)") UNORDERED("fcompp") UNORDERED("fucom %%st(1)") UNORDERED("fucomp %%st(1)") UNORDERED("fucompp")
#undef UNORDERED
    }
    for(s32 i=-1;i<=1;++i) { __asm__ volatile("fninit; fildl %1; ftst; fnstsw %0":"=m"(sw):"m"(i):"memory"); CHECK_EQ(sw&0x4500,i<0?0x100:i?0:0x4000); }
    { static const ext80 classes[]={{{0,0,0}},{{1,0,0}},{{0,0x80000000,0x3fff}},{{0,0x80000000,0x7fff}},{{0,0xc0000000,0x7fff}}};
      static const u16 flags[]={0x4000,0x4400,0x400,0x500,0x100};
      for(unsigned i=0;i<5;++i) for(unsigned sign=0;sign<2;++sign) {
          ext80 v=classes[i]; v.w[2]|=sign<<15;
          __asm__ volatile("fninit; fldt %1; fxam; fnstsw %0":"=m"(sw):"m"(v):"memory"); CHECK_EQ(sw&0x4700,flags[i]|(sign<<9));
      }
      __asm__ volatile("fninit; fxam; fnstsw %0":"=m"(sw)::"memory"); CHECK_EQ(sw&0x4500,0x4100);
    }
    init(); t_ops(100); return sum;
}
static u32 fpu_round(void) {
    static const u64 inputs[]={0x3fe0000000000000ull,0x3ff8000000000000ull,0x4004000000000000ull,0xbfe0000000000000ull,0xbff8000000000000ull,0xc004000000000000ull,0x3ff000001ad7f29bull};
    static const s32 answers[4][7]={{0,2,2,0,-2,-2,1},{0,1,2,-1,-2,-3,1},{1,2,3,0,-1,-2,2},{0,1,2,0,-1,-2,1}};
    static const u64 integral[]={0,0x3ff0000000000000ull,0x4000000000000000ull,0x4008000000000000ull};
    u32 sum=0; init();
    for(unsigned mode=0;mode<4;++mode) {
        control(0x037f|(mode<<10));
        for(unsigned i=0;i<7;++i) {
            s32 got; u64 rounded;
            __asm__ volatile("fldl %2; fistpl %0; fldl %2; frndint; fstpl %1":"=m"(got),"=m"(rounded):"m"(inputs[i]):"memory");
            CHECK_EQ(got,answers[mode][i]);
            s32 n=answers[mode][i]; u64 want=integral[n<0?-n:n];
            if(n<0 || ((inputs[i]>>63)&&n==0)) want|=1ull<<63;
            CHECK(rounded==want); sum=csum_mix(sum,(u32)got);
        }
    }
    { static const u16 pc[]={0x007f,0x027f,0x037f};
      static const ext80 want24[]={{{0,0x80000000,0x4017}},{{0,0x80000080,0x4017}},{{0,0x80000080,0x4017}}};
      static const ext80 want53[]={{{0,0x80000000,0x4034}},{{0,0x80000000,0x4034}},{{0x400,0x80000000,0x4034}}};
      u64 big24=0x4170000000000000ull,big53=0x4340000000000000ull; ext80 r={{0}};
      for(unsigned i=0;i<3;++i) {
          control(pc[i]); __asm__ volatile("fldl %1; fld1; faddp; fstpt %0":"=m"(r):"m"(big24):"memory"); ext_check(&r,&want24[i]);
          __asm__ volatile("fldl %1; fld1; faddp; fstpt %0":"=m"(r):"m"(big53):"memory"); ext_check(&r,&want53[i]);
      }
    }
    init(); t_ops(70); return sum;
}
static u32 fpu_trans(void) {
    static const ext80 sinhalf={{0x582f7f44,0xf57743a2,0x3ffd}},
        coshalf={{0xdbea7cee,0xe0a94032,0x3ffe}},
        tanhalf={{0x9a3a5219,0x8bda7adf,0x3ffe}},
        quarterpi={{0x2168c235,0xc90fdaa2,0x3ffe}},
        three={{0,0xc0000000,0x4000}}, onehalf={{0,0xc0000000,0x3fff}},
        minushalf={{0,0x80000000,0xbffe}}, sixteen={{0,0x80000000,0x4003}};
    u64 half=0x3fe0000000000000ull, fivehalf=0x4016000000000000ull;
    s32 eight=8,two=2,power=3; ext80 r={{0}},s={{0}}; u32 sum=0;
    init();
    if(tcg()) t_note("TCG transcendental tolerance=2^-50 relative (native/86Box=2^-55)");
    __asm__ volatile("fldl %1; fsin; fstpt %0":"=m"(r):"m"(half):"memory"); near("fsin_half",&r,&sinhalf); sum=ext_sum(sum,&r);
    __asm__ volatile("fldl %1; fcos; fstpt %0":"=m"(r):"m"(half):"memory"); near("fcos_half",&r,&coshalf);
    __asm__ volatile("fldl %2; fsincos; fstpt %0; fstpt %1":"=m"(r),"=m"(s):"m"(half):"memory"); near("fsincos_cos",&r,&coshalf); near("fsincos_sin",&s,&sinhalf);
    __asm__ volatile("fldt %1; fmul %%st,%%st; fldt %2; fmul %%st,%%st; faddp; fstpt %0":"=m"(r):"m"(r),"m"(s):"memory"); near("sin2_cos2",&r,&eone);
    __asm__ volatile("fldl %1; fptan; fstp %%st(0); fstpt %0":"=m"(r):"m"(half):"memory"); near("fptan_half",&r,&tanhalf);
    __asm__ volatile("fld1; fld1; fpatan; fstpt %0":"=m"(r)::"memory"); near("fpatan_1_1",&r,&quarterpi);
    __asm__ volatile("fld1; f2xm1; fstpt %0":"=m"(r)::"memory"); near("f2xm1_one",&r,&eone);
    __asm__ volatile("fld1; fildl %1; fyl2x; fstpt %0":"=m"(r):"m"(eight):"memory"); near("fyl2x_8",&r,&three);
    { u64 quarter=0x3fd0000000000000ull; ext80 answer={{0x68dc57f2,0xa4d3c25e,0x3ffd}};
      __asm__ volatile("fld1; fldl %1; fyl2xp1; fstpt %0":"=m"(r):"m"(quarter):"memory"); near("fyl2xp1_quarter",&r,&answer); }
    __asm__ volatile("fildl %1; fldl %2; fprem; fstpt %0; fstp %%st(0)":"=m"(r):"m"(two),"m"(fivehalf):"memory"); near("fprem_5.5_2",&r,&onehalf);
    __asm__ volatile("fildl %1; fldl %2; fprem1; fstpt %0; fstp %%st(0)":"=m"(r):"m"(two),"m"(fivehalf):"memory"); near("fprem1_5.5_2",&r,&minushalf);
    __asm__ volatile("fildl %1; fildl %2; fscale; fstpt %0; fstp %%st(0)":"=m"(r):"m"(power),"m"(two):"memory"); near("fscale_2_3",&r,&sixteen);
    __asm__ volatile("fildl %2; fxtract; fstpt %0; fstpt %1":"=m"(r),"=m"(s):"m"(eight):"memory"); near("fxtract_significand",&r,&eone); near("fxtract_exponent",&s,&three);
    { ext80 huge={{0,0x80000000,0x403f}}; u16 sw;
      __asm__ volatile("fldt %2; fsin; fnstsw %0; fstpt %1":"=m"(sw),"=m"(r):"m"(huge):"memory"); CHECK_EQ(sw&0x400,0x400); ext_check(&r,&huge); show("fsin_2pow64",&r);
    }
    t_ops(80); return sum;
}

static const ext80 maxext={{0xffffffff,0xffffffff,0x7ffe}}, minext={{1,0,0}};
static const u32 denorm32=1;
static const s32 divisor3=3,divisor2=2;
static u16 exception_status;
static void invalid_op(void) { __asm__ volatile("fldz; fldz; fdivp; fwait" ::: "memory"); }
static void denormal_op(void) { __asm__ volatile("flds %0; fwait"::"m"(denorm32):"memory"); }
static void zerodiv_op(void) { __asm__ volatile("fldz; fld1; fdiv %%st(1),%%st; fwait" ::: "memory"); }
static void overflow_op(void) { __asm__ volatile("fldt %0; fadd %%st,%%st; fwait"::"m"(maxext):"memory"); }
static void underflow_op(void) { __asm__ volatile("fldt %0; fidivl %1; fwait"::"m"(minext),"m"(divisor2):"memory"); }
static void precision_op(void) { __asm__ volatile("fld1; fidivl %0; fwait"::"m"(divisor3):"memory"); }
static void fault_fnop(void) { __asm__ volatile("fnop":::"memory"); }
static void fault_wait(void) { __asm__ volatile("fwait":::"memory"); }
static u32 fpu_exceptions(void) {
    static void (*const triggers[])(void)={invalid_op,denormal_op,zerodiv_op,overflow_op,underflow_op,precision_op};
    u32 sum=0;
    for(unsigned i=0;i<6;++i) {
        init(); triggers[i](); u16 sw=status(); CHECK(sw&(1u<<i)); sum=csum_mix(sum,sw);
        if(i==2) { ext80 r={{0}},inf={{0,0x80000000,0x7fff}}; __asm__ volatile("fstpt %0":"=m"(r)::"memory"); ext_check(&r,&inf); }
        __asm__ volatile("fnclex"); CHECK_EQ(status()&0xff,0);
        init(); control(0x037f & ~(1u<<i));
        u32 count=g_exc.count; int rc=guard_call(triggers[i]);
        __asm__ volatile("fnstsw %0; fnclex; fninit":"=m"(exception_status)::"memory");
        CHECK_EQ(rc,GUARD_EXC(16)); CHECK_EQ(g_exc.vec,16); CHECK_EQ(g_exc.count,count+1); CHECK(exception_status&(1u<<i)); CHECK(exception_status&0x80);
    }
    { u32 env[7],again[7]; u8 saved[108] __attribute__((aligned(16))); u16 cw; ext80 r={{0}};
      init(); __asm__ volatile("fnstenv %0":"=m"(env)::"memory"); CHECK_EQ(env[0]&0xffff,0x037f); CHECK_EQ(env[1]&0xffff,0); CHECK_EQ(env[2]&0xffff,0xffff);
      control(0x077f);
      __asm__ volatile("fld1; fldz; fnstenv %0":"=m"(env)::"memory"); CHECK_EQ(env[1]&0x3800,0x3000); CHECK_EQ(env[2]&0xffff,0x1fff);
      init(); __asm__ volatile("fldenv %2; fnstcw %0; fnstenv %1":"=m"(cw),"=m"(again):"m"(env):"memory");
      CHECK_EQ(cw,0x077f); CHECK_EQ(again[1]&0xffff,env[1]&0xffff); CHECK_EQ(again[2]&0xffff,env[2]&0xffff);
      /* FLDENV restores tags, not registers; the following FRSTOR restores both. */
      init(); __asm__ volatile("fld1; fldz; fnsave %0; fnstenv %1":"=m"(saved),"=m"(again)::"memory");
      CHECK_EQ(again[2]&0xffff,0xffff); CHECK_EQ(*(u16 *)(saved+8),0x1fff);
      CHECK_EQ(*(u32 *)(saved+28),0); CHECK_EQ(*(u32 *)(saved+32),0); CHECK_EQ(*(u16 *)(saved+36),0);
      CHECK_EQ(*(u32 *)(saved+38),0); CHECK_EQ(*(u32 *)(saved+42),0x80000000); CHECK_EQ(*(u16 *)(saved+46),0x3fff);
      __asm__ volatile("frstor %1; fstpt %0":"=m"(r):"m"(saved):"memory"); CHECK_EQ(r.w[0]|r.w[1]|(r.w[2]&0xffff),0);
      __asm__ volatile("fstpt %0":"=m"(r)::"memory"); ext_check(&r,&eone);
      /* FNSTENV's 32-bit layout is 7 dwords, with 32-bit instruction/data offsets. */
      { u32 ip; u64 data=0x4000000000000000ull;
        __asm__ volatile("fninit; fldl %2; 1: fld1; fnstenv %0; leal 1b,%1":"=m"(env),"=r"(ip):"m"(data):"memory");
        t_note("env28 cw=%04x sw=%04x tw=%04x fip=%08x fcs=%04x fdp=%08x fds=%04x",env[0]&0xffff,env[1]&0xffff,env[2]&0xffff,env[3],env[4]&0xffff,env[5],env[6]&0xffff);
        /* New Intel parts may clear FDP/FIP unless an exception is pending. */
        if(env[3]) CHECK_EQ(env[3],ip);
        if(env[5]) CHECK_EQ(env[5],(u32)&data);
      }
    }
    { u32 env[7]; u16 sw;
      __asm__ volatile("finit; fnop; fwait; fld1; fldz; fldt %1; fnstenv %0":"=m"(env):"m"(maxext):"memory");
      /* maxext is finite/valid, then replace it with infinity (special tag). */
      init(); { ext80 inf={{0,0x80000000,0x7fff}}; __asm__ volatile("fld1; fldz; fldt %1; fnstenv %0":"=m"(env):"m"(inf):"memory"); }
      CHECK_EQ(env[2]&0xffff,0x1bff);
      __asm__ volatile("ffree %%st(0); fnstenv %0":"=m"(env)::"memory"); CHECK_EQ(env[2]&0xffff,0x1fff);
      __asm__ volatile("fincstp; fnstsw %0":"=m"(sw)::"memory"); CHECK_EQ(sw&0x3800,0x3000);
      __asm__ volatile("fdecstp; fnstsw %0":"=m"(sw)::"memory"); CHECK_EQ(sw&0x3800,0x2800);
      init();
    }
    { u32 old=rd_cr0();
      for(unsigned i=0;i<3;++i) {
          u32 count=g_exc.count; wr_cr0(i==2?(old|4)&~8u:(old|10)&~4u);
          int rc=guard_call(i==1?fault_wait:fault_fnop);
          wr_cr0(old);
          CHECK_EQ(rc,GUARD_EXC(7)); CHECK_EQ(g_exc.vec,7); CHECK_EQ(g_exc.count,count+1);
      }
    }
    init(); t_ops(120); return sum;
}

/* Stack faults (#IS): IE and SF, C1 set for overflow and clear for underflow.
   Masked, the result is the real indefinite (stored as a double,
   0xfff8000000000000); unmasked, the stack, TOP and tags are left alone and
   ES is set. Each case runs a few times so a recompiler sees it compiled. */
#define STACK_INDEFINITE 0xfff8000000000000ull
static unsigned empty_tags(u32 tw) { unsigned m=0; for(unsigned i=0;i<8;++i) if(((tw>>(2*i))&3)==3) m|=1u<<i; return m; }
static u32 fpu_stack_cases(void) {
    u32 sum=0, env[7]; u16 sw; u64 a,b; static const u64 qint=0x0123456789abcdefull;
    /* masked FADD ST,ST(2) with ST(2) empty: ST(0) becomes the indefinite */
    init(); __asm__ volatile("fld1; fadd %%st(2),%%st; fnstsw %0; fstpl %1":"=m"(sw),"=m"(a)::"memory");
    CHECK_EQ(sw&0x3a41,0x3841); CHECK(a==STACK_INDEFINITE); sum=csum_mix(sum,sw);
    /* masked FCHS of an empty ST(0) */
    init(); __asm__ volatile("fchs; fnstsw %0; fstpl %1":"=m"(sw),"=m"(a)::"memory");
    CHECK_EQ(sw&0x3a41,0x0041); CHECK(a==STACK_INDEFINITE); sum=csum_mix(sum,sw);
    /* masked FCOMPP of two empty registers: unordered, and both pops */
    init(); __asm__ volatile("fcompp; fnstenv %0":"=m"(env)::"memory");
    CHECK_EQ(env[1]&0x7f41,0x5541); CHECK_EQ(env[2]&0xffff,0xffff); sum=csum_mix(sum,env[1]&0xffff);
    /* masked FXCH with ST(1) empty: the indefinite is swapped in */
    init(); __asm__ volatile("fld1; fxch %%st(1); fnstenv %0; fstpl %1; fstpl %2":"=m"(env),"=m"(a),"=m"(b)::"memory");
    CHECK_EQ(env[1]&0x3a41,0x3841); CHECK_EQ(empty_tags(env[2]),0x7e);
    CHECK(a==STACK_INDEFINITE); CHECK(b==0x3ff0000000000000ull); sum=csum_mix(sum,env[1]&0xffff);
    /* masked FLD ST(1) with ST(1) empty: the indefinite is pushed */
    init(); __asm__ volatile("fld1; fld %%st(1); fnstsw %0; fstpl %1; fstpl %2":"=m"(sw),"=m"(a),"=m"(b)::"memory");
    CHECK_EQ(sw&0x3a41,0x3041); CHECK(a==STACK_INDEFINITE); CHECK(b==0x3ff0000000000000ull);
    /* masked FLD ST(0) onto a full stack: overflow, the indefinite is pushed */
    init(); __asm__ volatile("fld1; fld1; fld1; fld1; fld1; fld1; fldz; fld1; fld %%st(1); fnstsw %0; fstpl %1; fstpl %2":"=m"(sw),"=m"(a),"=m"(b)::"memory");
    CHECK_EQ(sw&0x3a41,0x3a41); CHECK(a==STACK_INDEFINITE); CHECK(b==0x3ff0000000000000ull); sum=csum_mix(sum,sw);
    /* masked FILD m64 onto a full stack: the indefinite, not the integer */
    init(); __asm__ volatile("fld1; fld1; fld1; fld1; fld1; fld1; fld1; fld1; fildll %2; fnstsw %0; fstpl %1":"=m"(sw),"=m"(a):"m"(qint):"memory");
    CHECK_EQ(sw&0x3a41,0x3a41); CHECK(a==STACK_INDEFINITE);
    /* FFREEP ST(1) leaves both registers empty: eight pushes fit */
    init(); __asm__ volatile(".byte 0xd9,0xe8,0xd9,0xe8,0xdf,0xc1; fld1; fld1; fld1; fld1; fld1; fld1; fld1; fld1; fnstsw %0":"=m"(sw)::"memory");
    CHECK_EQ(sw&0x41,0);
    /* FXAM of an empty register: C3 and C0, C1 its stale sign */
    init(); __asm__ volatile("fld1; fchs; ffree %%st(0); fxam; fnstsw %0":"=m"(sw)::"memory");
    CHECK_EQ(sw&0x4700,0x4300); CHECK_EQ(sw&0x41,0);
    /* unmasked underflow: ES, the stack and its tags as they were */
    init(); control(0x037e);
    __asm__ volatile("fld1; fadd %%st(2),%%st; fnstenv %0; fnclex; fstpl %1":"=m"(env),"=m"(a)::"memory");
    CHECK_EQ(env[1]&0xbaff,0xb8c1); CHECK_EQ(empty_tags(env[2]),0x7f); CHECK(a==0x3ff0000000000000ull);
    sum=csum_mix(sum,env[1]&0xffff);
    /* unmasked overflow: TOP and ST(0) untouched */
    init(); control(0x037e);
    __asm__ volatile("fld1; fld1; fld1; fld1; fld1; fld1; fld1; fld1; fldz; fnstenv %0; fnclex; fstpl %1":"=m"(env),"=m"(a)::"memory");
    CHECK_EQ(env[1]&0xbaff,0x82c1); CHECK_EQ(empty_tags(env[2]),0); CHECK(a==0x3ff0000000000000ull);
    sum=csum_mix(sum,env[1]&0xffff);
    /* unmasked FCOMI of an empty register leaves ZF, PF and CF alone */
    if(cpu.has_cmov) {
        u32 fl;
        init(); control(0x037e);
        __asm__ volatile("fld1; pushfl; orl $0x45,(%%esp); popfl; fcomi %%st(2),%%st; pushfl; popl %0; fnstsw %1; fnclex":"=r"(fl),"=m"(sw)::"memory","cc");
        CHECK_EQ(fl&0x45,0x45); CHECK_EQ(sw&0xc1,0xc1);
        init();
        __asm__ volatile("fld1; pushfl; andl $-70,(%%esp); popfl; fcomi %%st(2),%%st; pushfl; popl %0":"=r"(fl)::"memory","cc");
        CHECK_EQ(fl&0x45,0x45);
    }
    init(); return sum;
}

static u32 fpu_stack(void) {
    u16 overflow,underflow; u32 sum=0;
    init(); for(unsigned i=0;i<9;++i) __asm__ volatile("fld1"); overflow=status();
    init(); __asm__ volatile("fstp %%st(0); fnstsw %0":"=m"(underflow)::"memory");
    init(); t_ops(10);
    t_note("stack_overflow_sw=%04x stack_underflow_sw=%04x",overflow,underflow);
    if(tcg() && ((overflow&0x241)!=0x241 || (underflow&0x241)!=0x41))
        SKIP("TCG does not implement x87 stack faults; raw status reported");
    CHECK_EQ(overflow&0x241,0x241); CHECK_EQ(underflow&0x241,0x41);
    if(!tcg()) for(unsigned pass=0;pass<4;++pass) sum=fpu_stack_cases();
    t_note("stack_cases=%08x",sum);
    return ((u32)overflow<<16)|underflow;
}

static u32 fpu_p6(void) {
    u32 sum=0; init();
    for(unsigned kind=0;kind<4;++kind) for(unsigned rel=0;rel<4;++rel) {
        static const u64 vals[]={0x3fe0000000000000ull,0x4000000000000000ull,0x3ff0000000000000ull,0x7ff8000000000000ull};
        u32 flags=0, want=rel==0?1:rel==1?0:rel==2?0x40:0x45;
/* OF, SF and AF preset: FCOMI clears them */
#define FCOMI(n) __asm__ volatile("fninit; fld1; fldl %1; pushfl; orl $0x890,(%%esp); popfl; " n " %%st(1),%%st; pushfl; popl %0":"=m"(flags):"m"(vals[rel]):"memory","cc")
        switch(kind) { case 0: FCOMI("fcomi"); break; case 1: FCOMI("fcomip"); break; case 2: FCOMI("fucomi"); break; default: FCOMI("fucomip"); break; }
#undef FCOMI
        CHECK_EQ(flags&0x8d5,want);
        CHECK_EQ(status()&0x3800,(kind&1)?0x3800:0x3000);
        CHECK_EQ(status()&1,(rel==3 && kind<2)?1:0); sum=csum_mix(sum,flags&0x8d5);
    }
    for(unsigned kind=0;kind<8;++kind) for(unsigned take=0;take<2;++take) {
        static const u32 bits[]={1,0x40,0x41,4,1,0x40,0x41,4};
        u32 flags=(take^(kind>=4))?bits[kind]:0; u64 out;
#define FCMOV(n) __asm__ volatile("fninit; fldt %2; fld1; pushfl; popl %%eax; andl $-70,%%eax; orl %1,%%eax; pushl %%eax; popfl; " n " %%st(1),%%st; fstpl %0; fstp %%st(0)":"=m"(out):"r"(flags),"m"(etwo):"eax","memory","cc")
        switch(kind) {case 0: FCMOV("fcmovb");break;case 1: FCMOV("fcmove");break;case 2: FCMOV("fcmovbe");break;case 3: FCMOV("fcmovu");break;case 4: FCMOV("fcmovnb");break;case 5: FCMOV("fcmovne");break;case 6: FCMOV("fcmovnbe");break;default: FCMOV("fcmovnu");break;}
#undef FCMOV
        CHECK(out==(take?0x4000000000000000ull:0x3ff0000000000000ull));
    }
    init(); t_ops(100); return sum;
}
static u32 fpu_fxsr(void) {
    struct fxarea { u8 bytes[512]; };
    u8 *a=arena_alloc(512,16),*b=arena_alloc(512,16);
    if(!a||!b) SKIP("aligned FXSR arena unavailable");
#define FXMEM(p) (*(struct fxarea *)(p))
    kmemset(a,0,512); kmemset(b,0,512); init(); control(0x077f);
    __asm__ volatile("fld1; fldt %1; fxsave %0":"=m"(FXMEM(a)):"m"(etwo):"memory");
    CHECK_EQ(*(u16 *)a,0x077f); CHECK_EQ(*(u16 *)(a+2)&0x3800,0x3000); CHECK_EQ(a[4],0xc0);
    CHECK_EQ(*(u32 *)(a+32),0); CHECK_EQ(*(u32 *)(a+36),0x80000000); CHECK_EQ(*(u16 *)(a+40),0x4000);
    CHECK_EQ(*(u16 *)(a+56),0x3fff);
    init(); __asm__ volatile("fxrstor %1; fxsave %0":"=m"(FXMEM(b)):"m"(FXMEM(a)):"memory");
    CHECK_EQ(*(u16 *)b,*(u16 *)a); CHECK_EQ(*(u16 *)(b+2),*(u16 *)(a+2)); CHECK_EQ(b[4],a[4]);
    CHECK(!kmemcmp(a+32,b+32,10)); CHECK(!kmemcmp(a+48,b+48,10));
    if(cpu.has_mmx) {
        u64 q=0x0123456789abcdefull,out;
        __asm__ volatile("movq %1,%%mm0; movq %%mm0,%%mm1; movq %%mm0,%%mm2; movq %%mm0,%%mm3; movq %%mm0,%%mm4; movq %%mm0,%%mm5; movq %%mm0,%%mm6; movq %%mm0,%%mm7; fxsave %0; emms":"=m"(FXMEM(a)):"m"(q):"memory");
        CHECK_EQ(a[4],0xff); CHECK_EQ(*(u16 *)(a+2)&0x3800,0);
        for(unsigned i=0;i<8;++i) {
            if(!tcg()) CHECK_EQ(*(u16 *)(a+32+16*i+8),0xffff);
        }
        __asm__ volatile("fxrstor %1; movq %%mm0,%0; emms":"=m"(out):"m"(FXMEM(a)):"memory"); CHECK(out==q);
    }
#undef FXMEM
    init(); t_ops(40); return *(u32 *)(a+32);
}
static u64 from_int(s32 n) { u64 r; __asm__ volatile("fildl %1; fstpl %0":"=m"(r):"m"(n):"memory"); return r; }
#define DBIN(name,op) static u64 name(u64 a,u64 b) { u64 r; __asm__ volatile("fldl %1; " op " %2; fstpl %0":"=m"(r):"m"(a),"m"(b):"memory"); return r; }
DBIN(add,"faddl") DBIN(sub,"fsubl") DBIN(mul,"fmull") DBIN(divide,"fdivl")
#undef DBIN
static u64 root(u64 a) { u64 r; __asm__ volatile("fldl %1; fsqrt; fstpl %0":"=m"(r):"m"(a):"memory"); return r; }
static void double_near(u64 got,u64 want,unsigned ulps) {
    CHECK((got>>63)==(want>>63)); CHECK((got>want?got-want:want-got)<=ulps);
}
static u64 arctan_series(u64 x) {
    u64 x2=mul(x,x),term=x,sum=x;
    for(s32 k=1;k<24;++k) { term=mul(term,x2); u64 v=divide(term,from_int(2*k+1)); sum=k&1?sub(sum,v):add(sum,v); }
    return sum;
}
static u32 fpu_burn(void) {
    const u64 one=0x3ff0000000000000ull,two=0x4000000000000000ull,half=0x3fe0000000000000ull;
    u64 *a=arena_alloc(24*24*8,16),*b=arena_alloc(24*24*8,16),*c=arena_alloc(24*24*8,16);
    if(!a||!b||!c) SKIP("arena allocation unavailable");
    u32 sum=0,escaped=0; init(); control(0x027f);
    for(s32 i=0;i<24;++i) for(s32 j=0;j<24;++j) { a[i*24+j]=from_int((i+j)%7-3); b[i*24+j]=from_int((i*3+j)%5-2); }
    for(unsigned repeat=0;repeat<16;++repeat) {
        for(s32 i=0;i<24;++i) for(s32 j=0;j<24;++j) {
            u64 acc=0; s32 exact=0;
            for(s32 k=0;k<24;++k) {
                acc=add(acc,mul(a[i*24+k],b[k*24+j])); exact+=((i+k)%7-3)*((k*3+j)%5-2);
            }
            c[i*24+j]=acc;
            /* Exact integer products/sums are a separate known-answer oracle. */
            CHECK(acc==from_int(exact)); sum=csum_mix(sum,(u32)acc^(u32)(acc>>32));
        }
    }
    t_ops(16*24*24*24*2);
    for(s32 py=0;py<32;++py) for(s32 px=0;px<48;++px) {
        u64 cr=divide(from_int(px-32),from_int(16)),ci=divide(from_int(py-16),from_int(16));
        u64 x=0,y=0; unsigned k;
        for(k=0;k<64;++k) {
            u64 xx=mul(x,x),yy=mul(y,y),magnitude=add(xx,yy);
            if(magnitude>0x4010000000000000ull) break;
            y=add(mul(two,mul(x,y)),ci); x=add(sub(xx,yy),cr);
        }
        if(k<64) ++escaped;
        sum=csum_mix(csum_mix(sum,k),(u32)x^(u32)y); t_ops(k*9);
    }
    CHECK(escaped>0 && escaped<48*32); t_note("mandelbrot escaped=%u pixels=%u",escaped,48*32);
    u64 p=0,e=0,nr=0;
    for(unsigned repeat=0;repeat<128;++repeat) {
        p=sub(mul(from_int(16),arctan_series(divide(one,from_int(5)))),mul(from_int(4),arctan_series(divide(one,from_int(239)))));
        e=one; u64 term=one;
        for(s32 i=1;i<=20;++i) { term=divide(term,from_int(i)); e=add(e,term); }
        nr=from_int(3);
        for(unsigned i=0;i<12;++i) nr=mul(half,add(nr,divide(from_int(7),nr)));
        sum=csum_mix(sum,(u32)p^(u32)e^(u32)nr);
    }
    double_near(p,0x400921fb54442d18ull,12); double_near(e,0x4005bf0a8b145769ull,8); double_near(nr,0x40052a7fa9d2f8eaull,2);
    t_note("series_pi=%llx series_e=%llx newton_sqrt7=%llx",p,e,nr); t_ops(128*(2*23*4+20*2+12*3+10));
    for(unsigned repeat=0;repeat<8192;++repeat) {
        u64 v=from_int(8);
        for(s32 k=6;k>=0;--k) v=add(mul(v,half),from_int(k+1));
        CHECK(v==0x400f600000000000ull); sum=csum_mix(sum,(u32)(v>>32));
    }
    t_ops(8192*14);
    { u64 x[8],vel[8],force[8],dt=divide(one,from_int(1024));
      for(s32 i=0;i<8;++i) { x[i]=divide(from_int(i-4),from_int(8)); vel[i]=0; }
      for(unsigned step=0;step<128;++step) {
          for(unsigned i=0;i<8;++i) {
              force[i]=0;
              for(unsigned j=0;j<8;++j) if(i!=j) {
                  u64 dx=sub(x[j],x[i]),r2=add(mul(dx,dx),one),r3=mul(r2,root(r2));
                  force[i]=add(force[i],divide(dx,r3));
              }
          }
          for(unsigned i=0;i<8;++i) { vel[i]=add(vel[i],mul(force[i],dt)); x[i]=add(x[i],mul(vel[i],dt)); }
      }
      for(unsigned i=0;i<8;++i) { CHECK(((x[i]>>52)&0x7ff)<0x401); sum=csum_mix(sum,(u32)x[i]^(u32)(x[i]>>32)); }
      t_ops(128*(56*7+8*4));
    }
    { ext80 out={{0}}; u64 quarter=0x3fd0000000000000ull; control(0x037f);
      for(unsigned i=0;i<4096;++i) {
          __asm__ volatile("fldl %1; fsincos; fmul %%st,%%st; fxch; fmul %%st,%%st; faddp; fsqrt; fld1; fpatan; fstpt %0":"=m"(out):"m"(quarter):"memory");
          sum=ext_sum(sum,&out);
      }
      ext80 quarterpi=epi; quarterpi.w[2]-=2; near("whetstone_atan_identity",&out,&quarterpi); t_ops(4096*8);
    }
    init(); return sum;
}
const test_t tests_fpu[] = {
    {"fpu_arith",fpu_arith,REQ_FPU}, {"fpu_formats",fpu_formats,REQ_FPU},
    {"fpu_compare",fpu_compare,REQ_FPU}, {"fpu_round",fpu_round,REQ_FPU},
    {"fpu_trans",fpu_trans,REQ_FPU}, {"fpu_exceptions",fpu_exceptions,REQ_FPU},
    {"fpu_stack",fpu_stack,REQ_FPU}, {"fpu_p6",fpu_p6,REQ_FPU|REQ_CMOV}, {"fpu_fxsr",fpu_fxsr,REQ_FPU|REQ_FXSR},
    {"fpu_burn",fpu_burn,REQ_FPU}, {0,0,0}
};
