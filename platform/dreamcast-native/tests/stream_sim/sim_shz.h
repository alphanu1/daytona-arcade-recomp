#ifndef SIM_SHZ_H
#define SIM_SHZ_H
/* SH4ZAM for the host submission simulator: the operations the renderer
 * (include/render/models/model_submission.h) takes from it, with the SH4's
 * approximate instructions modelled. FTRV (shz_xmtrx_transform_vec4,
 * sim_main.c) is exact float arithmetic in every mode.
 *
 * SIM_SH4 (environment) chooses the model:
 *   sw     (default) SH4ZAM's own host back-end (SHZ_BACKEND SHZ_SW,
 *          sh4zam/inline/sw/shz_scalar_sw.inl.h and inline/shz_scalar.inl.h):
 *          FIPR = (x1*x2)+(y1*y2)+(z1*z2) in float, FSRRA = 1/sqrtf(fabsf(x)),
 *          shz_invf_fsrra(x) = FSRRA(x*x), shz_divf_fsrra(a,b) =
 *          a*shz_invf_fsrra(b), shz_lerpf(a,b,t) = t*(b-a)+a.
 *          check_shz_stubs.c compares these with the real SH4ZAM headers.
 *          SH4ZAM's back-end is correctly rounded IEEE arithmetic: it does
 *          NOT reproduce FIPR's or FSRRA's results on the SH4, whose
 *          internal rounding (FIPR truncates its products to 28 bits and
 *          aligns them to 30; FSRRA is a table and an iteration) is not
 *          documented to the bit. Hence:
 *   bound  every FIPR and FSRRA result of the SH4ZAM forms NATIVE_SH4ZAM_GFX
 *          adds (the car light classes' FIPRs, SQ_FIPR6; the clipper's FSRRA
 *          division and 1/w) moved to the edge of its error
 *          envelope: FIPR by 8 FLT_EPSILON of the sum of its term
 *          magnitudes (the bound the console validates for FTRV rows and
 *          FIPR, code/render/models/model_validation.c), FSRRA by 2^-20 of
 *          the result (meshpack.h; the SH4 manual's FSRRA error is within
 *          that), in a direction that is a fixed hash of the operands (the
 *          same operands always give the same result, as on the SH4). A
 *          field whose tolerance holds here holds for any SH4 whose results
 *          lie within those bounds.
 *   bound-all  the same for every FIPR of the renderer, also those it had
 *          before (the face side tests' plane constants and batch terms,
 *          the facing dot of a face whose side those leave undecided).
 *   ideal  HEAD 744dd29's stubs: FSRRA divisions are exact quotients
 *          (shz_divf_fsrra(a,b) = a/b). With -DNM_SH4ZAM_GFX=0 the stream is
 *          HEAD's simulator's word for word. */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
typedef struct { float x,y,z,w; } shz_vec4_t;
static inline shz_vec4_t shz_vec4_init(float x,float y,float z,float w) { return (shz_vec4_t){x,y,z,w}; }
static inline float shz_vec4_dot(shz_vec4_t a,shz_vec4_t b) { return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w; }
static inline float shz_fabsf(float a) { return fabsf(a); }
static inline float shz_inv_sqrtf(float a) { return 1.0f/sqrtf(a); }
enum { SIM_SH4_SW, SIM_SH4_IDEAL, SIM_SH4_BOUND, SIM_SH4_BOUND_ALL };
static int sim_sh4_mode=-1;
static unsigned long long sim_sh4_fipr,sim_sh4_fsrra; /* operations modelled (statistics) */
static inline int sim_sh4(void) {
    if(sim_sh4_mode<0) {
        const char *m=getenv("SIM_SH4");
        sim_sh4_mode=!m || !strcmp(m,"sw") ? SIM_SH4_SW : !strcmp(m,"ideal") ? SIM_SH4_IDEAL : !strcmp(m,"bound") ? SIM_SH4_BOUND :
            !strcmp(m,"bound-all") ? SIM_SH4_BOUND_ALL : -2;
        if(sim_sh4_mode==-2) { fprintf(stderr,"SIM_SH4 must be sw, ideal, bound or bound-all\n"); exit(2); }
    }
    return sim_sh4_mode;
}
static inline const char *sim_sh4_name(void) { static const char *n[]={"sw","ideal","bound","bound-all"}; return n[sim_sh4()]; }
static inline int sim_sh4_bound(void) { return sim_sh4()>=SIM_SH4_BOUND; }
/* +1 or -1 from the operands' bits (splitmix64 of them). */
static inline double sim_sh4_sign(const float *v,unsigned n,uint64_t salt) {
    uint64_t h=salt;
    for(unsigned i=0;i<n;++i) {
        uint32_t b; memcpy(&b,v+i,4);
        h+=b+UINT64_C(0x9e3779b97f4a7c15); h^=h>>30; h*=UINT64_C(0xbf58476d1ce4e5b9); h^=h>>27; h*=UINT64_C(0x94d049bb133111eb); h^=h>>31;
    }
    return h>>63 ? -1.0 : 1.0;
}
/* FIPR at its error bound (any mode). */
static inline float sim_fipr_bound(float x1,float y1,float z1,float w1,float x2,float y2,float z2,float w2) {
    ++sim_sh4_fipr;
    const float v[8]={x1,y1,z1,w1,x2,y2,z2,w2};
    const double p[4]={(double)x1*x2,(double)y1*y2,(double)z1*z2,(double)w1*w2};
    const double exact=p[0]+p[1]+p[2]+p[3],magnitude=fabs(p[0])+fabs(p[1])+fabs(p[2])+fabs(p[3]);
    return (float)(exact+sim_sh4_sign(v,8,1)*8.0*FLT_EPSILON*magnitude);
}
static inline float shz_dot8f(float x1,float y1,float z1,float w1,float x2,float y2,float z2,float w2) {
    if(sim_sh4()!=SIM_SH4_BOUND_ALL) return (x1*x2)+(y1*y2)+(z1*z2)+(w1*w2);
    return sim_fipr_bound(x1,y1,z1,w1,x2,y2,z2,w2);
}
static inline float shz_dot6f(float x1,float y1,float z1,float x2,float y2,float z2) {
    if(sim_sh4()!=SIM_SH4_BOUND_ALL) return (x1*x2)+(y1*y2)+(z1*z2);
    return sim_fipr_bound(x1,y1,z1,0.0f,x2,y2,z2,0.0f);
}
/* The FIPRs NATIVE_SH4ZAM_GFX adds (model_submission.h, SQ_FIPR6). */
static inline float sim_fipr6(float x1,float y1,float z1,float x2,float y2,float z2) {
    if(!sim_sh4_bound()) return (x1*x2)+(y1*y2)+(z1*z2);
    return sim_fipr_bound(x1,y1,z1,0.0f,x2,y2,z2,0.0f);
}
#define SQ_FIPR6 sim_fipr6
/* FSRRA of x (x > 0). */
static inline float sim_fsrra(float x) {
    if(!sim_sh4_bound()) return 1.0f/sqrtf(fabsf(x));
    ++sim_sh4_fsrra;
    return (float)(1.0/sqrt(fabs((double)x))*(1.0+sim_sh4_sign(&x,1,2)*0x1p-20));
}
/* SH4ZAM's root: x*FSRRA(x), 0 at 0 (the renderer takes it for bounds at load). */
static inline float shz_sqrtf(float x) {
    if(sim_sh4()==SIM_SH4_IDEAL) return sqrtf(x);
    return x==0.0f ? 0.0f : sim_fsrra(x)*x;
}
static inline float shz_invf_fsrra(float x) {
    if(sim_sh4()==SIM_SH4_IDEAL) return 1.0f/fabsf(x);
    return sim_fsrra(x*x);
}
static inline float shz_divf_fsrra(float a,float b) {
    if(sim_sh4()==SIM_SH4_IDEAL) return a/b;
    return a*shz_invf_fsrra(b);
}
static inline float shz_lerpf(float a,float b,float t) { return t*(b-a)+a; }
#endif
