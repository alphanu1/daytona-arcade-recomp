/* Projection conventions follow the BSD-3-Clause reference raster.cpp;
 * see THIRD_PARTY.md. No rasterizer or board state is linked here.
 * A positive near plane is a native safety bound, not arcade state. */
#include "geometry.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "nm_mem.h"

_Static_assert(sizeof(float) == 4 && sizeof(double) == 8 && FLT_RADIX == 2 &&
               FLT_MANT_DIG == 24 && DBL_MANT_DIG == 53, "IEEE geometry checks");
_Static_assert(sizeof(nm_cached_vertex_t) == 32, "one cache line per projected vertex");

#if defined(__DREAMCAST__) || defined(NATIVE_GEOMETRY_FLOAT)
typedef float geometry_real_t;
#ifdef __DREAMCAST__
#define geometry_abs shz_fabsf
#else
#define geometry_abs fabsf
#endif
static int geometry_finite(float value) { return nm_finite_float(value); }
#else
/* Host reference retains double; target geometry is single precision only. */
typedef double geometry_real_t;
#define geometry_abs fabs
static int geometry_finite(double value) {
    uint64_t bits;
    nm_memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}
#endif

typedef struct { geometry_real_t x, y, z, u, v; } clip_point_t;

static geometry_real_t clip_distance(const nm_view_t *view, clip_point_t p, unsigned plane) {
    switch (plane) {
    case 0: return p.z - view->near_z;
    case 1: return p.x + (view->center[0] - (geometry_real_t)view->bounds[0]) * p.z;
    case 2: return (view->bounds[1] - (geometry_real_t)view->center[0]) * p.z - p.x;
    case 3: return (view->center[1] - (geometry_real_t)view->bounds[2]) * p.z - p.y;
    default: return p.y + (view->bounds[3] - (geometry_real_t)view->center[1]) * p.z;
    }
}

int nm_view_valid(const nm_view_t *view) {
    if (!nm_finite_float(view->near_z) || view->near_z <= 0 ||
        !nm_finite_float(view->center[0]) || !nm_finite_float(view->center[1])) return 0;
    for (unsigned i = 0; i < 4; ++i) if (!nm_finite_float(view->bounds[i])) return 0;
    return view->bounds[0] < view->bounds[1] && view->bounds[2] < view->bounds[3];
}

static int bounds_test(const nm_view_t *view, const float bounds[6],
                      const float matrix[12], const float focus[2], int classify) {
    if (!nm_view_valid(view)) return 1;
    for (unsigned i = 0; i < 6; ++i) if (!nm_finite_float(bounds[i])) return 1;
    for (unsigned i = 0; i < 12; ++i) if (!nm_finite_float(matrix[i])) return 1;
    for (unsigned i = 0; i < 2; ++i) if (!nm_finite_float(focus[i])) return 1;
    for (unsigned i = 0; i < 3; ++i) if (bounds[i] > bounds[i+3]) return 1;
    geometry_real_t low[3], high[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        low[axis] = high[axis] = matrix[9+axis];
        geometry_real_t magnitude = geometry_abs(low[axis]);
        for (unsigned c = 0; c < 3; ++c) {
            const geometry_real_t a = (geometry_real_t)matrix[c*3+axis] * bounds[c];
            const geometry_real_t b = (geometry_real_t)matrix[c*3+axis] * bounds[c+3];
            low[axis] += a < b ? a : b;
            high[axis] += a > b ? a : b;
            magnitude += geometry_abs(a) > geometry_abs(b) ? geometry_abs(a) : geometry_abs(b);
        }
        if (!geometry_finite(magnitude) || magnitude > FLT_MAX) return 1;
        if (axis < 2) {
            const geometry_real_t a = low[axis] * focus[axis], b = high[axis] * focus[axis];
            low[axis] = a < b ? a : b;
            high[axis] = a > b ? a : b;
            magnitude *= geometry_abs(focus[axis]);
        }
        if (!geometry_finite(magnitude) || magnitude > FLT_MAX) return 1;
        /* Exceeds the validated 8*epsilon magnitude FTRV error bound and
         * allows interval accumulation and focus rounding. */
        const geometry_real_t slack = 32 * FLT_EPSILON * magnitude + FLT_MIN;
        low[axis] -= slack; high[axis] += slack;
    }
    const geometry_real_t planes[5][4] = {
        {0,0,1,-view->near_z},
        {1,0,(geometry_real_t)view->center[0]-view->bounds[0],0},
        {-1,0,(geometry_real_t)view->bounds[1]-view->center[0],0},
        {0,-1,(geometry_real_t)view->center[1]-view->bounds[2],0},
        {0,1,(geometry_real_t)view->bounds[3]-view->center[1],0},
    };
    int inside=classify;
    for (unsigned p = 0; p < 5; ++p) {
        geometry_real_t maximum = planes[p][3], magnitude = geometry_abs(maximum);
        for (unsigned a = 0; a < 3; ++a) {
            const geometry_real_t term = planes[p][a] * (planes[p][a] < 0 ? low[a] : high[a]);
            maximum += term; magnitude += geometry_abs(term);
        }
        /* Also retain subnormal/borderline cases where float plane arithmetic
         * could round toward the inside. False positives only cost work. */
        if (!geometry_finite(maximum) || !geometry_finite(magnitude)) return 1;
        if (maximum < -(32 * FLT_EPSILON * magnitude + FLT_MIN)) return 0;
        if(inside) {
            geometry_real_t minimum=planes[p][3], min_magnitude=geometry_abs(minimum);
            for(unsigned a=0;a<3;++a) {
                const geometry_real_t term=planes[p][a]*(planes[p][a]<0 ? high[a] : low[a]);
                minimum+=term; min_magnitude+=geometry_abs(term);
            }
            if(!geometry_finite(minimum) || !geometry_finite(min_magnitude) ||
               minimum<=32*FLT_EPSILON*min_magnitude+FLT_MIN) inside=0;
        }
    }
    return inside ? 2 : 1;
}

int nm_bounds_visible(const nm_view_t *view, const float bounds[6],
                      const float matrix[12], const float focus[2]) {
    return bounds_test(view,bounds,matrix,focus,0);
}

int nm_bounds_classify(const nm_view_t *view, const float bounds[6],
                      const float matrix[12], const float focus[2]) {
    return bounds_test(view,bounds,matrix,focus,1);
}

static unsigned project_classified_triangle(const nm_view_t *view, const nm_view_vertex_t input[3],
    unsigned outside_any, nm_screen_vertex_t output[NM_CLIP_VERTICES]);

static float bounds_dot(nm_bounds_vector_t a, nm_bounds_vector_t b) {
#ifdef __DREAMCAST__
    return shz_vec4_dot(a,b);
#else
    return a.e[0]*b.e[0]+a.e[1]*b.e[1]+a.e[2]*b.e[2]+a.e[3]*b.e[3];
#endif
}

/* The bits of a float in memory as one integer load (a memcpy from a pointer
 * of unknown alignment is a library call on the SH4). */
typedef uint32_t __attribute__((may_alias)) bounds_word_t;
static inline uint32_t bounds_bits(const float *value) { return *(const bounds_word_t *)(const void *)value; }
/* |value| < 2^40, hence finite: one integer compare on the bits. */
static inline int bounds_small(const float *value) {
    return (bounds_bits(value) & 0x7fffffffu) < 0x53800000u;
}
/* bounds_dot of two vectors given as scalars (the same FIPR on the SH4, the
 * same sum on the host), without passing them through memory. */
static inline float bounds_dot4(float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3) {
#ifdef __DREAMCAST__
    return shz_dot8f(a0,a1,a2,a3,b0,b1,b2,b3);
#else
    return a0*b0+a1*b1+a2*b2+a3*b3;
#endif
}
/* nm_bounds_planes_prepare when the matrix, the focus and the view are all
 * below 2^40 in magnitude (every instance of a frame): plane entries are then
 * below 2^41 and every product and four-term sum below 2^84, so none of the
 * general routine's finite tests can fail and they are left out. The plane
 * values are the same dot products of the same vectors (FIPR on the SH4: bit
 * for bit the general routine's). The error term's magnitude sum
 * |p0 c0| + .. + |p3 c3| and the floor 1 + |p0| + .. + |p3| skip the plane's
 * zero entries (an exact zero term: the host's sums are bit for bit the
 * general routine's; on the SH4 the general routine's fast-math order may
 * round them differently, within 2 epsilon of a term the 128 epsilon of
 * slack covers many times, so the bound stays conservative). valid=2: every
 * value below 2^84 (classify's fast form).
 * What depends on the view and the focus alone (their validity, the planes,
 * the floors) is kept for the next call with the same nine words: the
 * instances of a frame share them. About 1,360 SH4 instructions a call before,
 * 660 now (GDB SH simulator, crew-like matrices). */
static struct {
    uint32_t key[9];
    int ready;
#ifdef __DREAMCAST__
    /* The planes in shz_dot8f's register order (fr4..fr7 = y, x, w, z of its first vector). */
    alignas(32) float planes[5][4];
#endif
    float near_z, fx, fy, left, right, top, bottom;
    float ax, ay, al, ar, at, ab, an, floor[5];
} bounds_view;
/* 1: bounds_view holds this view and focus (both valid and below 2^40); 0: the general routine. */
static int bounds_view_select(const nm_view_t *view, const float focus[2]) {
    const uint32_t key[9]={bounds_bits(view->center),bounds_bits(view->center+1),bounds_bits(view->bounds),
        bounds_bits(view->bounds+1),bounds_bits(view->bounds+2),bounds_bits(view->bounds+3),bounds_bits(&view->near_z),
        bounds_bits(focus),bounds_bits(focus+1)};
    unsigned differ=!bounds_view.ready;
    for(unsigned i=0;i<9;++i) differ|=key[i]^bounds_view.key[i];
    if(!differ) return 1;
    bounds_view.ready=0;
    if(!nm_view_valid(view)) return 0;
    for(unsigned i=0;i<9;++i) if((key[i] & 0x7fffffffu) >= 0x53800000u) return 0;
    nm_memcpy(bounds_view.key, key, sizeof(key));
    const float near_z=view->near_z, fx=focus[0], fy=focus[1];
    const float left=view->center[0]-view->bounds[0], right=view->bounds[1]-view->center[0];
    const float top=view->center[1]-view->bounds[2], bottom=view->bounds[3]-view->center[1];
    bounds_view.near_z=near_z; bounds_view.fx=fx; bounds_view.fy=fy;
    bounds_view.left=left; bounds_view.right=right; bounds_view.top=top; bounds_view.bottom=bottom;
    const float ax=(float)geometry_abs(fx), ay=(float)geometry_abs(fy);
    const float al=(float)geometry_abs(left), ar=(float)geometry_abs(right);
    const float at=(float)geometry_abs(top), ab=(float)geometry_abs(bottom), an=(float)geometry_abs(-near_z);
    bounds_view.ax=ax; bounds_view.ay=ay; bounds_view.al=al; bounds_view.ar=ar; bounds_view.at=at; bounds_view.ab=ab;
    bounds_view.an=an;
    bounds_view.floor[0]=((1.0f+1.0f)+an)*(128*FLT_MIN); bounds_view.floor[1]=((1.0f+ax)+al)*(128*FLT_MIN);
    bounds_view.floor[2]=((1.0f+ax)+ar)*(128*FLT_MIN); bounds_view.floor[3]=((1.0f+ay)+at)*(128*FLT_MIN);
    bounds_view.floor[4]=((1.0f+ay)+ab)*(128*FLT_MIN);
#ifdef __DREAMCAST__
    const float planes[5][4]={{0,0,-near_z,1},{0,fx,0,left},{0,-fx,0,right},{-fy,0,0,top},{fy,0,0,bottom}};
    nm_memcpy(bounds_view.planes, planes, sizeof(planes));
#endif
    bounds_view.ready=1;
    return 1;
}
static void bounds_planes_prepare_small(nm_bounds_planes_t *out, const float matrix[12]) {
#ifdef __DREAMCAST__
    /* shz_dot8f(plane, column) five times a column with the column kept in fv8: the same FIPR fv4,fv8 on
     * the same register contents; the product lands in fr11 (the column's z), put back after each. Written
     * out five times (not .rept: GCC sizes an asm by its lines, for the constant pools). The errors follow
     * in a loop of their own, with every other FPU register free. */
#define BOUNDS_PLANE_FIPR \
        "fmov.s @%[source]+,fr4\n\t" \
        "fmov.s @%[source]+,fr5\n\t" \
        "fmov.s @%[source]+,fr6\n\t" \
        "fmov.s @%[source]+,fr7\n\t" \
        "fipr fv4,fv8\n\t" \
        "fmov.s fr11,@%[target]\n\t" \
        "add #16,%[target]\n\t" \
        "fmov %[keep],fr11\n\t"
    for(unsigned c=0;c<4;++c) {
        register float cy asm("fr8")=matrix[c*3+1], cx asm("fr9")=matrix[c*3], cw asm("fr10")=c==3 ? 1.0f : 0.0f;
        register float cz asm("fr11")=matrix[c*3+2], kz asm("fr12")=matrix[c*3+2];
        const float *source=bounds_view.planes[0];
        float *target=out->plane[0].e+c;
        asm volatile(BOUNDS_PLANE_FIPR BOUNDS_PLANE_FIPR BOUNDS_PLANE_FIPR BOUNDS_PLANE_FIPR BOUNDS_PLANE_FIPR
            : [source] "+r" (source), [target] "+r" (target), "+f" (cz)
            : "f" (cy), "f" (cx), "f" (cw), [keep] "f" (kz)
            : "fr4", "fr5", "fr6", "fr7", "memory");
    }
#undef BOUNDS_PLANE_FIPR
#endif
    const float ax=bounds_view.ax, ay=bounds_view.ay, al=bounds_view.al, ar=bounds_view.ar, at=bounds_view.at,
        ab=bounds_view.ab;
    const float f0=bounds_view.floor[0], f1=bounds_view.floor[1], f2=bounds_view.floor[2], f3=bounds_view.floor[3],
        f4=bounds_view.floor[4];
    for(unsigned c=0;c<4;++c) {
        const float x=matrix[c*3], y=matrix[c*3+1], z=matrix[c*3+2];
        const float mx=(float)geometry_abs(x), my=(float)geometry_abs(y), mz=(float)geometry_abs(z);
#ifndef __DREAMCAST__
        const float w=c==3 ? 1.0f : 0.0f, near_z=bounds_view.near_z, fx=bounds_view.fx, fy=bounds_view.fy;
        out->plane[0].e[c]=bounds_dot4(0,0,1,-near_z,x,y,z,w);
        out->plane[1].e[c]=bounds_dot4(fx,0,bounds_view.left,0,x,y,z,w);
        out->plane[2].e[c]=bounds_dot4(-fx,0,bounds_view.right,0,x,y,z,w);
        out->plane[3].e[c]=bounds_dot4(0,-fy,bounds_view.top,0,x,y,z,w);
        out->plane[4].e[c]=bounds_dot4(0,fy,bounds_view.bottom,0,x,y,z,w);
#endif
        /* Products first, then their sum: no contraction into one rounding (the host's sums as before). */
        const float px=ax*mx, py=ay*my, pl=al*mz, pr=ar*mz, pt=at*mz, pb=ab*mz;
        const float m0=c==3 ? mz+bounds_view.an : mz, m1=px+pl, m2=px+pr, m3=py+pt, m4=py+pb;
        out->error[0].e[c]=128*FLT_EPSILON*m0+f0;
        out->error[1].e[c]=128*FLT_EPSILON*m1+f1;
        out->error[2].e[c]=128*FLT_EPSILON*m2+f2;
        out->error[3].e[c]=128*FLT_EPSILON*m3+f3;
        out->error[4].e[c]=128*FLT_EPSILON*m4+f4;
    }
    out->valid=2;
}

void nm_bounds_planes_prepare(nm_bounds_planes_t *out, const nm_view_t *view,
    const float matrix[12], const float focus[2]) {
    out->valid=0;
    if(bounds_view_select(view,focus)) {
        int small=1;
        for(unsigned a=0;a<12;++a) small&=bounds_small(matrix+a);
        if(small) { bounds_planes_prepare_small(out,matrix); return; }
    }
    if(!nm_view_valid(view)) return;
    for(unsigned a=0;a<12;++a) if(!nm_finite_float(matrix[a])) return;
    for(unsigned a=0;a<2;++a) if(!nm_finite_float(focus[a])) return;
    const float planes[5][4]={
        {0,0,1,-view->near_z},
        {focus[0],0,view->center[0]-view->bounds[0],0},
        {-focus[0],0,view->bounds[1]-view->center[0],0},
        {0,-focus[1],view->center[1]-view->bounds[2],0},
        {0,focus[1],view->bounds[3]-view->center[1],0}};
    for(unsigned p=0;p<5;++p) {
        nm_bounds_vector_t plane;
        float floor=1;
        for(unsigned a=0;a<4;++a) {
            plane.e[a]=planes[p][a];
            if(!nm_finite_float(plane.e[a])) return;
            floor+=(float)geometry_abs(plane.e[a]);
        }
        floor*=128*FLT_MIN;
        for(unsigned c=0;c<4;++c) {
            nm_bounds_vector_t column;
            float magnitude=0;
            for(unsigned a=0;a<4;++a) {
                column.e[a]=a<3 ? matrix[c*3+a] : (c==3 ? 1 : 0);
                magnitude+=(float)geometry_abs(plane.e[a]*column.e[a]);
            }
            out->plane[p].e[c]=bounds_dot(plane,column);
            /* Covers FTRV input perturbation, focus/plane composition and
             * both four-term dot products. Absolute slack covers underflow. */
            out->error[p].e[c]=128*FLT_EPSILON*magnitude+floor;
            if(!nm_finite_float(out->plane[p].e[c]) || !nm_finite_float(out->error[p].e[c])) return;
        }
    }
    out->valid=1;
}

int nm_bounds_planes_classify(const nm_bounds_planes_t *planes, const float bounds[6]) {
    if(!planes->valid) return 1;
    /* Planes of the bounded form (valid 2, every value below 2^84) and bounds
     * below 2^40: every dot product below 2^126, so the general form's finite
     * tests cannot fail; the same decisions without them. */
    if(planes->valid==2 && bounds_small(bounds) && bounds_small(bounds+1) && bounds_small(bounds+2) &&
       bounds_small(bounds+3) && bounds_small(bounds+4) && bounds_small(bounds+5)) {
        if(bounds[0]>bounds[3] || bounds[1]>bounds[4] || bounds[2]>bounds[5]) return 1;
        const float ex=(float)geometry_abs(bounds[0])>(float)geometry_abs(bounds[3]) ? (float)geometry_abs(bounds[0]) :
            (float)geometry_abs(bounds[3]);
        const float ey=(float)geometry_abs(bounds[1])>(float)geometry_abs(bounds[4]) ? (float)geometry_abs(bounds[1]) :
            (float)geometry_abs(bounds[4]);
        const float ez=(float)geometry_abs(bounds[2])>(float)geometry_abs(bounds[5]) ? (float)geometry_abs(bounds[2]) :
            (float)geometry_abs(bounds[5]);
        int inside=1;
        for(unsigned p=0;p<5;++p) {
            const float *const plane=planes->plane[p].e, *const error_row=planes->error[p].e;
            /* plane < 0 on the bits: finite values, -0 is not below 0 (the general form's compare). */
            const unsigned sx=bounds_bits(plane)>0x80000000u ? 3u : 0u, sy=bounds_bits(plane+1)>0x80000000u ? 3u : 0u,
                sz=bounds_bits(plane+2)>0x80000000u ? 3u : 0u;
            const float px=plane[0], py=plane[1], pz=plane[2], pw=plane[3];
            const float error=bounds_dot4(error_row[0],error_row[1],error_row[2],error_row[3],ex,ey,ez,1);
            if(bounds_dot4(px,py,pz,pw,bounds[3-sx],bounds[4-sy],bounds[5-sz],1) < -error) return 0;
            if(inside && bounds_dot4(px,py,pz,pw,bounds[sx],bounds[1+sy],bounds[2+sz],1)<=error) inside=0;
        }
        return inside ? 2 : 1;
    }
    nm_bounds_vector_t extent;
    extent.e[3]=1;
    for(unsigned a=0;a<3;++a) {
        if(!nm_finite_float(bounds[a]) || !nm_finite_float(bounds[a+3]) || bounds[a]>bounds[a+3]) return 1;
        const float lo=(float)geometry_abs(bounds[a]), hi=(float)geometry_abs(bounds[a+3]);
        extent.e[a]=lo>hi ? lo : hi;
    }
    int inside=1;
    for(unsigned p=0;p<5;++p) {
        nm_bounds_vector_t low,high;
        low.e[3]=high.e[3]=1;
        for(unsigned a=0;a<3;++a) {
            const unsigned side=planes->plane[p].e[a]<0 ? 3 : 0;
            low.e[a]=bounds[a+side]; high.e[a]=bounds[a+3-side];
        }
        const float error=bounds_dot(planes->error[p],extent);
        const float maximum=bounds_dot(planes->plane[p],high);
        if(!nm_finite_float(error) || !nm_finite_float(maximum)) return 1;
        if(maximum < -error) return 0;
        if(inside) {
            const float minimum=bounds_dot(planes->plane[p],low);
            if(!nm_finite_float(minimum) || minimum<=error) inside=0;
        }
    }
    return inside ? 2 : 1;
}

/* nm_bounds_quick_classify: each plane's value at the transformed box centre
 * and its spread over the box (the plane through the matrix, |P M| h: the
 * box's own extremes are value - spread and value + spread) against a margin
 * of 2^-12 of the terms' magnitude (plus 2^-60). Let mag be, for a plane, the
 * sum of |plane entry| times the bound u of that coordinate's magnitude over
 * the box (every product of the transform taken in absolute value, plus |t|,
 * plus |M| h) and |near| for the near plane; every term of the value and the
 * spread is below it. Each value here is a few products and sums, within 32
 * epsilon of mag of its exact value in any order (fast-math may reassociate). nm_bounds_planes_classify decides with
 * its own error term, at most 128 epsilon of the same magnitude plus a floor
 * below 2^-60 for boxes and magnitudes in range, and its FIPRs are within 16
 * epsilon. So, by more than 2048 epsilon of mag:
 *   a box beyond one plane is beyond it for the planes too: 0;
 *   a box inside every plane is inside for them too: 2;
 *   a box not beyond any plane and across one (part of it beyond that plane by
 *   the margin) is neither for them: 1.
 * Whenever this returns 0, 1 or 2 the planes return the same. Everything else
 * is 3 (the caller takes the planes): a box within 2048 epsilon of a plane, a
 * magnitude of 2^100 or more, a box coordinate of 2^40 or more, an inverted
 * box, anything not finite (a NaN or an infinity never passes the sign tests).
 * About 600 SH4 instructions (GDB SH simulator), against 1,250 for the planes
 * and their classification (2,050 before the planes' bounded form). */
int nm_bounds_quick_classify(const nm_view_t *view, const float bounds[6], const float matrix[12], const float focus[2]) {
    /* Every test is a sign test of a difference on its bits (finite.h: false for a NaN; a float compare the
     * compiler must invert costs a call to __unordsf2 on the SH4). */
    const float *const m=matrix;
    const float cx=(bounds[0]+bounds[3])*0.5f, cy=(bounds[1]+bounds[4])*0.5f, cz=(bounds[2]+bounds[5])*0.5f;
    const float hx=(bounds[3]-bounds[0])*0.5f, hy=(bounds[4]-bounds[1])*0.5f, hz=(bounds[5]-bounds[2])*0.5f;
    if(nm_float_negative(hx) || nm_float_negative(hy) || nm_float_negative(hz)) return 3;
    const float big=0x1p40f;
    if(!nm_float_positive(big-((float)geometry_abs(cx)+hx)) || !nm_float_positive(big-((float)geometry_abs(cy)+hy)) ||
       !nm_float_positive(big-((float)geometry_abs(cz)+hz))) return 3;
    const float fx=focus[0], fy=focus[1], near_z=view->near_z;
    int outside=0, inside=1, across=0, kept=1;
#define QUICK_PLANE(value,spread,mag) do { \
        const float plane_value=(value), plane_spread=(spread), plane_mag=(mag); \
        if(!nm_float_positive(0x1p100f-plane_mag)) return 3; \
        const float margin=plane_mag*0x1p-12f+0x1p-60f; \
        outside|=nm_float_negative(plane_value+plane_spread+margin); \
        kept&=nm_float_positive(plane_value+plane_spread-margin); \
        inside&=nm_float_positive(plane_value-plane_spread-margin); \
        across|=nm_float_negative(plane_value-plane_spread+margin); \
    } while(0)
    /* One view axis at a time (its centre v, extent e, magnitude bound u), the planes that need it as soon as
     * it is known: few values live at once, none spilled. */
#define QUICK_AXIS(i,v,e,u) \
    float v, e, u; \
    { \
        const float t0=m[i]*cx, t1=m[3+i]*cy, t2=m[6+i]*cz; \
        v=t0+t1+t2+m[9+i]; \
        e=(float)geometry_abs(m[i])*hx+(float)geometry_abs(m[3+i])*hy+(float)geometry_abs(m[6+i])*hz; \
        u=(float)geometry_abs(t0)+(float)geometry_abs(t1)+(float)geometry_abs(t2)+(float)geometry_abs(m[9+i])+e; \
    }
    QUICK_AXIS(2,vz,ez,uz)
    QUICK_PLANE(vz-near_z, ez, uz+(float)geometry_abs(near_z));
    /* The spread of plane a x + b z over the box: |a M_xj + b M_zj| h_j summed over the box axes j (the box's own
     * extremes, not its view-space bounding box's: those bound the planes' values from one side only). */
#define QUICK_SPREAD(a,u,b) ((float)geometry_abs((a)*m[u]+(b)*m[2])*hx+(float)geometry_abs((a)*m[3+(u)]+(b)*m[5])*hy+ \
        (float)geometry_abs((a)*m[6+(u)]+(b)*m[8])*hz)
    {
        QUICK_AXIS(0,vx,ex,ux)
        const float ax=(float)geometry_abs(fx), left=view->center[0]-view->bounds[0], right=view->bounds[1]-view->center[0];
        const float al=(float)geometry_abs(left), ar=(float)geometry_abs(right);
        QUICK_PLANE(fx*vx+left*vz, QUICK_SPREAD(fx,0,left), ax*ux+al*uz);
        QUICK_PLANE(right*vz-fx*vx, QUICK_SPREAD(-fx,0,right), ax*ux+ar*uz);
        (void)ex;
    }
    {
        QUICK_AXIS(1,vy,ey,uy)
        const float ay=(float)geometry_abs(fy), top=view->center[1]-view->bounds[2], bottom=view->bounds[3]-view->center[1];
        const float at=(float)geometry_abs(top), ab=(float)geometry_abs(bottom);
        QUICK_PLANE(top*vz-fy*vy, QUICK_SPREAD(-fy,1,top), ay*uy+at*uz);
        QUICK_PLANE(fy*vy+bottom*vz, QUICK_SPREAD(fy,1,bottom), ay*uy+ab*uz);
        (void)ey;
    }
#undef QUICK_SPREAD
#undef QUICK_AXIS
#undef QUICK_PLANE
    return outside ? 0 : inside ? 2 : kept && across ? 1 : 3;
}

unsigned nm_project_cached_clipped(const nm_view_t *view, const nm_cached_vertex_t *const points[3],
                                    unsigned any, nm_screen_vertex_t output[NM_CLIP_VERTICES]) {
    const nm_view_vertex_t input[3] = {points[0]->view, points[1]->view, points[2]->view};
    /* The inline dispatcher already checked the cached plane classifications. */
    return project_classified_triangle(view, input, any, output);
}

unsigned nm_project_triangle(const nm_view_t *view, const nm_view_vertex_t input[3],
                             nm_screen_vertex_t output[NM_CLIP_VERTICES]) {
    if (!nm_view_valid(view)) return 0;

    unsigned outside_any = 0, outside_all = 31;
    for (unsigned i = 0; i < 3; ++i) {
        if (!nm_geometry_finite_vertex(input[i])) return 0;
        const unsigned outside = nm_geometry_classify(view, &input[i]);
        if (outside & 32) return 0;
        outside_any |= outside; outside_all &= outside;
    }
    if (outside_all) return 0;
    return project_classified_triangle(view, input, outside_any, output);
}

static unsigned project_classified_triangle(const nm_view_t *view, const nm_view_vertex_t input[3],
    unsigned outside_any, nm_screen_vertex_t output[NM_CLIP_VERTICES]) {
    unsigned count = 3;
    /* Target uses float throughout; double intermediates are host-reference only. */
    clip_point_t buffers[2][NM_CLIP_VERTICES];
    clip_point_t *in = buffers[0], *out = buffers[1];
    if (outside_any) for (unsigned i = 0; i < 3; ++i) {
        const nm_view_vertex_t p = input[i];
        in[i] = (clip_point_t){p.x, p.y, p.z, p.u, p.v};
    }
    /* Clip before division: eye-plane crossings can otherwise produce huge
     * screen coordinates and lose subpixel precision during interpolation. */
    for (unsigned step = 0; step < 5 && count >= 3; ++step) {
        /* Float target clips near first, so subsequent interpolation combines
         * positive depths. Host reference retains its original side-first order. */
#if defined(__DREAMCAST__) || defined(NATIVE_GEOMETRY_FLOAT)
        const unsigned plane = step;
#else
        const unsigned plane = step == 4 ? 0 : step + 1;
#endif
        if (!(outside_any & (1u << plane))) continue;
        unsigned used = 0;
        const geometry_real_t first_distance = clip_distance(view, in[0], plane);
        geometry_real_t next_distance = first_distance;
        for (unsigned i = 0; i < count; ++i) {
            clip_point_t a = in[i], b = in[i + 1 == count ? 0 : i + 1];
            geometry_real_t da = next_distance;
            geometry_real_t db = i + 1 == count ? first_distance : clip_distance(view, b, plane);
            /* An endpoint is shared by adjacent edges. Save its distance
             * before the numerically stable interpolation swaps endpoints. */
            next_distance = db;
            if (!geometry_finite(da) || !geometry_finite(db)) return 0;
            if (da >= 0) {
                if (used == NM_CLIP_VERTICES) return 0;
                out[used++] = a;
            }
            if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                /* Interpolate from the nearer endpoint to avoid subtracting
                 * almost the entire far endpoint when depths differ greatly. */
                if (geometry_abs(da) > geometry_abs(db)) {
                    clip_point_t swap = a; a = b; b = swap;
                    const geometry_real_t d = da; da = db; db = d;
                }
#ifdef __DREAMCAST__
                const float denominator = da - db;
                const float magnitude = shz_fabsf(denominator);
                const float t = magnitude >= 0x1p-63f && magnitude < 0x1p64f ?
                    shz_divf(da,denominator) : da/denominator;
#else
                const geometry_real_t t = da / (da - db);
#endif
#ifdef __DREAMCAST__
                clip_point_t p = {shz_lerpf(a.x,b.x,t),shz_lerpf(a.y,b.y,t),
                    shz_lerpf(a.z,b.z,t),shz_lerpf(a.u,b.u,t),shz_lerpf(a.v,b.v,t)};
#else
                clip_point_t p = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
#endif
                if (plane == 0) p.z = view->near_z;
                else if (plane < 3) p.x = (view->bounds[plane - 1] - view->center[0]) * p.z;
                else p.y = (view->center[1] - view->bounds[plane - 1]) * p.z;
                if (used == NM_CLIP_VERTICES) return 0;
                out[used++] = p;
            }
        }
        clip_point_t *swap = in; in = out; out = swap;
        count = used;
    }
    if (count < 3) return 0;
    for (unsigned i = 0; i < count; ++i) {
        const nm_view_vertex_t p = outside_any ? (nm_view_vertex_t){
            in[i].x, in[i].y, in[i].z, in[i].u, in[i].v} : input[i];
        if (!nm_geometry_project_point(view, &p, &output[i])) return 0;
    }
    return count;
}
