#ifndef DAYTONA_NATIVE_DRIVE_CHECK_H
#define DAYTONA_NATIVE_DRIVE_CHECK_H
#include "../include/drive.h"
#include "../include/force_math.h"
#include "../include/forces.h"
#include "../include/yaw.h"
#include "../include/tires.h"
#include "../include/gears.h"
#include "../include/motion.h"

/* Inputs only, not recorded vehicle poses. First sweep covers every curve,
 * gear, RPM bin and pedal byte; second covers every unsigned16 RPM value. */
#define NM_DRIVE_CHECK_CASES (2u*4u*67u*256u + 65536u)
#define NM_DRIVE_CHECK_HASH 0xbc9541f7u /* Verified daytona93 drive parameters. */
static inline nm_drive_input_t nm_drive_test_case(unsigned i) {
    const int32_t compensation[] = {INT32_MIN,INT32_MAX,-2001,-2000,-1,0,1,1999,2000,2001};
    nm_drive_input_t v; memset(&v,0,sizeof(v));
    v.throttle = (uint8_t)i; v.brake = (uint8_t)(i*73);
    v.curve = (i/(4*67*256))%2 ? 255 : 0;
    v.gear = (i/(67*256))%4;
    v.rpm = i < 2*4*67*256 ? (uint16_t)(((i/256)%67)*128) : (uint16_t)(i-2*4*67*256);
    v.speed_index = i%512;
    v.pitch = (int16_t)((int)(i%65536)-32768);
    v.drag_state = (int8_t)((int)(i%3)-1);
    v.compensation = compensation[i%10];
    if (i >= 2*4*67*256) {
        const uint32_t bits = i*1664525u+1013904223u;
        v.compensation = bits <= INT32_MAX ? (int32_t)bits : -1-(int32_t)~bits;
    }
    for (unsigned n = 0; n < 4; ++n)
        v.contact[n] = (uint16_t)(0xfff3u | ((i & (1u<<n)) ? ((i&16) ? 4 : 8) : 0));
    return v;
}
static inline uint32_t nm_drive_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_DRIVE_CHECK_CASES; ++i) {
        const nm_drive_input_t v = nm_drive_test_case(i);
        nm_drive_terms_t result;
        if (!nm_drive_terms(p,&v,&result)) return 0;
        const uint8_t *bytes = (const uint8_t *)&result;
        for (unsigned b = 0; b < sizeof(result); ++b) hash = (hash ^ bytes[b]) * 16777619u;
    }
    return hash;
}

#define NM_AXLE_CHECK_CASES (65536u*3u)
#define NM_AXLE_CHECK_HASH 0x4511e383u
static inline void nm_axle_test_case(unsigned i, nm_axle_input_t *v, nm_axle_state_t *s) {
    const float forces[] = {-65536,-1000,-1,-0.0f,0.0f,0.125f,1,1000,65536};
    const int16_t speeds[] = {-32768,-1,0,9,10,11,511,32767};
    memset(v,0,sizeof(*v));
    v->pitch = (int16_t)((int)(i%65536)-32768);
    v->bank = (int16_t)((int)((i*73)%65536)-32768);
    if (i < 65536) v->speed_index = 10;
    else if (i < 2*65536) v->speed_index = 11;
    else v->speed_index = speeds[i%8];
    v->contact[0] = (uint16_t)i; v->contact[1] = 0;
    v->contact[2] = (uint16_t)(i*73); v->contact[3] = (uint16_t)(i/65536);
    for (unsigned a = 0; a < 2; ++a) {
        v->inertial[a] = forces[(i+3*a)%9];
        s->load[a] = forces[(i/9+a)%9];
    }
    v->longitudinal_delta = forces[(i/81)%9];
}
static inline uint32_t nm_axle_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_AXLE_CHECK_CASES; ++i) {
        nm_axle_input_t v; nm_axle_state_t s; nm_axle_test_case(i,&v,&s);
        if (!nm_axle_load_step(p,&v,&s)) return 0;
        const uint8_t *bytes = (const uint8_t *)&s;
        for (unsigned b = 0; b < sizeof(s); ++b) hash = (hash ^ bytes[b]) * 16777619u;
    }
    return hash;
}

#define NM_FORCE_CHECK_CASES 65536u
#define NM_FORCE_CHECK_HASH 0xa3d27a9du
#define NM_DIVIDE_CHECK_CASES 262144u
#define NM_DIVIDE_CHECK_HASH 0xef97a891u
static inline void nm_divide_test_case(unsigned i, float *numerator, float *denominator) {
    const unsigned exponents[] = {0,1,63,126,127,128,191,254};
    const uint32_t bits = ((i/65536)%2 ? 0x80000000u : 0) |
        (exponents[(i/8192)%8]<<23) | ((i%8192)<<10) | (i/131072 ? 1023 : 0);
    *denominator = nm_force_float(bits);
    *numerator = (float)((int)(i%65536)-32768)*0.03125f;
}
static inline uint32_t nm_divide_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_DIVIDE_CHECK_CASES; ++i) {
        float a,b; nm_divide_test_case(i,&a,&b);
        uint32_t word = nm_force_bits(nm_force_divide(p,a,b));
        for (unsigned byte = 0; byte < 4; ++byte) hash = (hash ^ ((word>>(8*byte))&255)) * 16777619u;
    }
    return hash;
}
static inline void nm_force_test_case(unsigned i, nm_force_polar_t *a, nm_force_polar_t *b) {
    const float magnitudes[] = {0.0f,-0.0f,1,-1,0.01f,2000,1e-10f,1e10f};
    a->angle = (int)i-32768; b->angle = (int)((i*73)%65536)-32768;
    a->magnitude = magnitudes[i%8]; b->magnitude = magnitudes[(i/8)%8];
}
static inline uint32_t nm_force_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_FORCE_CHECK_CASES; ++i) {
        nm_force_polar_t a,b,result; nm_force_test_case(i,&a,&b);
        if (!nm_force_combine(p,&a,&b,&result)) return 0;
        float cart[2]; nm_force_cartesian(p,(uint16_t)a.angle,a.magnitude,cart);
        const uint32_t words[] = {(uint32_t)result.angle,nm_force_bits(result.magnitude),
            nm_force_bits(cart[0]),nm_force_bits(cart[1]),nm_force_bits(nm_force_divide(p,a.magnitude,1+b.magnitude))};
        for (unsigned n = 0; n < 5; ++n) for (unsigned byte = 0; byte < 4; ++byte)
            hash = (hash ^ ((words[n]>>(8*byte))&255)) * 16777619u;
    }
    return hash;
}

#define NM_FORCES_CHECK_CASES (65536u*3u)
#define NM_FORCES_CHECK_HASH 0x2fd6285eu
static inline void nm_forces_test_case(unsigned i, nm_forces_input_t *v, nm_forces_state_t *s) {
    const float forces[] = {0.0f,-0.0f,1,-1,0.01f,2000,-2000,64,1000};
    const float loads[] = {1,10,100,1000,10000};
    const int32_t speeds[] = {INT32_MIN,-32768,-1,0,1,20,21,32767,32768,65536,65537,INT32_MAX};
    const int32_t rpms[] = {0,1,1000,8499,8500,8501,9000,-1000};
    const int8_t counters[] = {-128,-1,0,1,4,5,24,127};
    memset(v,0,sizeof(*v)); memset(s,0,sizeof(*s));
    v->bank = (int16_t)((int)(i%65536)-32768);
    v->steer = (int16_t)((int)((i*73)%65536)-32768);
    v->speed_index = speeds[i%12]; v->rpm = rpms[(i/12)%8]; v->gear = (i/96)%4;
    v->condition_99 = (i/384)%2; v->condition_ec = (i/768)%2;
    v->condition_15c = (int8_t)((int)((i/1536)%3)-1);
    v->condition_265 = (int8_t)((int)((i/4608)%3)-1);
    v->terms.drive = forces[(i/9)%9]; v->terms.slope = forces[(i/81)%9];
    v->terms.drag = forces[(i/729)%9]; v->resistance = forces[(i/6561)%9];
    s->slip_decay = forces[(i/19683)%9]; s->slip_ratio = -17;
    for (unsigned a = 0; a < 2; ++a) {
        v->terms.brake[a] = forces[(i+3*a)%9];
        v->axle.load[a] = loads[(i/5+a)%5];
        s->inertial[a] = forces[(i/7+2*a)%9];
        s->tire_angle[a] = (int16_t)((int)((i*(37+a))%65536)-32768);
        s->trigger_count[a] = counters[(i/11+a)%8];
    }
}
static inline uint32_t nm_forces_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_FORCES_CHECK_CASES; ++i) {
        nm_forces_input_t v; nm_forces_state_t s; nm_forces_test_case(i,&v,&s);
        if (!nm_forces_step(p,&v,&s)) return 0;
        const uint8_t *bytes = (const uint8_t *)&s;
        for (unsigned b = 0; b < sizeof(s); ++b) hash = (hash ^ bytes[b]) * 16777619u;
    }
    return hash;
}

#define NM_TRACTION_CHECK_CASES 131072u
#define NM_TRACTION_CHECK_HASH 0xb03c174eu
static inline void nm_traction_test_case(unsigned i, nm_traction_input_t *v, nm_traction_state_t *s) {
    const int16_t speeds[] = {-32768,-1,0,69,70,99,100,149,150,32767};
    const int16_t pitches[] = {-32768,0,1535,1536,32767};
    const uint16_t pedals[] = {0,199,200,220,221,249,250,255,65535};
    const uint16_t masks[] = {0,1,2,4,12,65535};
    const int8_t counts[] = {-128,-1,0,1,15,16,32,48,64,127};
    const float lateral[] = {-1,-0.0f,0.0f,0.19999999f,0.2f,0.20000002f,1,-0.2f};
    const float forces[] = {-100,-1,-0.0f,0.0f,1,10,100,1000};
    memset(v,0,sizeof(*v)); memset(s,0,sizeof(*s));
    v->gear = i%4; v->course_slot = (i/4)%4;
    v->speed_index = speeds[(i/16)%10]; v->pitch = pitches[(i/160)%5];
    v->accel = pedals[(i/800)%9]; v->brake = pedals[(i/7200)%9];
    v->steer_angle = (int16_t)((int)(i%65536)-32768);
    v->tire_angle = (int16_t)((int)((i*37)%65536)-32768);
    v->steer = (int16_t)((int)((i*73)%65536)-32768);
    v->phase[0] = 7; v->phase[1] = (i/11)%2 ? 7 : -7;
    v->lateral = lateral[(i/13)%8]; s->previous_lateral = lateral[(i/17)%8];
    v->inertial[0] = forces[(i/19)%8]; v->inertial[1] = forces[(i/23)%8];
    v->surface = (uint16_t)i; v->override_count = (i/29)%2;
    for (unsigned a = 0; a < 4; ++a) v->contact[a] = masks[(i/(31+a))%6];
    s->drive = forces[(i/41)%8]; s->rear_load = (float)(1+i%100); s->slope = forces[(i/43)%8];
    s->condition_26c = -123;
    for (unsigned a = 0; a < 6; ++a) s->countdown[a] = counts[(i/(47+a))%10];
}
static inline uint32_t nm_traction_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_TRACTION_CHECK_CASES; ++i) {
        nm_traction_input_t v; nm_traction_state_t s; nm_traction_test_case(i,&v,&s);
        const unsigned accepted = nm_traction_step(p,&v,&s);
        hash = (hash ^ accepted) * 16777619u;
        const uint8_t *bytes = (const uint8_t *)&s;
        for (unsigned b = 0; b < sizeof(s); ++b) hash = (hash ^ bytes[b]) * 16777619u;
    }
    return hash;
}

#define NM_SPEED_CHECK_CASES 131072u
#define NM_SPEED_CHECK_HASH 0x0091ea86u
static inline void nm_speed_test_case(unsigned i, uint32_t *flags, unsigned *gear, nm_speed_state_t *s) {
    const float deltas[] = {-100,-1,-0.0f,0.0f,0.0001f,0.25f,1,100};
    *flags = i%2 ? 1u<<((i/2)%32) : 0xffffffffu;
    *gear = (i/64)%4;
    s->speed = (float)((int)((i/256)%512)-256)*0.25f;
    s->delta = deltas[(i/8)%8];
    s->rpm = -17; s->compensation = 23;
}
static inline uint32_t nm_speed_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_SPEED_CHECK_CASES; ++i) {
        nm_speed_state_t s; uint32_t flags; unsigned gear; nm_speed_test_case(i,&flags,&gear,&s);
        if (!nm_speed_step(p,flags,gear,&s)) return 0;
        const uint8_t *bytes = (const uint8_t *)&s;
        for (unsigned b = 0; b < sizeof(s); ++b) hash = (hash ^ bytes[b]) * 16777619u;
    }
    return hash;
}
#define NM_YAW_CHECK_CASES 131072u
#define NM_YAW_CHECK_HASH 0x2fefd38cu
static inline void nm_yaw_test_case(unsigned i, nm_yaw_input_t *v, nm_yaw_state_t *s) {
    const float speeds[] = {0.0f,-0.0f,0.001f,-0.001f,1,-1,10,100};
    const uint32_t counts[] = {0,1,2,3,4,5,0x7fffffffu,0xfffffffeu,0xffffffffu};
    const int offsets[] = {-911,-910,-909,-17,-16,-1,0,1,16,17,909,910,911};
    memset(v,0,sizeof(*v)); memset(s,0,sizeof(*s));
    v->speed = speeds[(i/11)%8]; v->speed_index = (int16_t)((int)(i%3)-1);
    v->course_slot = (i/3)%4; v->fixed_offset = (i/12)%3;
    v->steer_angle = nm_yaw_angle(i); v->steer = nm_yaw_angle(i*73);
    v->tire_angle[0] = nm_yaw_angle(i*37); v->tire_angle[1] = nm_yaw_angle(i*89);
    s->yaw = nm_yaw_angle(i*113); s->heading = nm_yaw_angle(i*193);
    s->body_yaw = 123; s->yaw_delta = -123; s->offset = nm_yaw_angle(i*47);
    s->inertial[0] = speeds[(i/17)%8]; s->inertial[1] = speeds[(i/23)%8];
    s->alignment_frames = counts[(i/19)%9];
    v->road_heading = nm_yaw_angle((uint32_t)((int)s->yaw+offsets[(i/29)%13]));
    /* Dedicated straight/wrap/alignment sweep, not just random angles. */
    if (i >= 65536) {
        v->speed = 0; v->steer_angle = 0;
        v->tire_angle[0] = v->tire_angle[1] = nm_yaw_angle(i);
    }
}
static inline uint32_t nm_yaw_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_YAW_CHECK_CASES; ++i) {
        nm_yaw_input_t v; nm_yaw_state_t s; nm_yaw_test_case(i,&v,&s);
        const unsigned accepted = nm_yaw_step(p,&v,&s);
        /* Canonical fields, never compiler-dependent structure padding. */
        const uint32_t words[] = {accepted,nm_force_bits(s.inertial[0]),nm_force_bits(s.inertial[1]),
            s.alignment_frames,(uint32_t)(int32_t)s.yaw,(uint32_t)(int32_t)s.heading,
            (uint32_t)(int32_t)s.body_yaw,(uint32_t)(int32_t)s.yaw_delta,(uint32_t)(int32_t)s.offset};
        for (unsigned w = 0; w < 9; ++w) for (unsigned b = 0; b < 4; ++b)
            hash = (hash ^ ((words[w]>>(8*b))&255)) * 16777619u;
    }
    return hash;
}
#define NM_TIRES_CHECK_CASES 131072u
#define NM_TIRES_CHECK_HASH 0x1a5bee04u
static inline void nm_tires_test_case(unsigned i, nm_tires_input_t *v, nm_tires_state_t *s) {
    const float speeds[] = {0,-0.0f,0.001f,-0.001f,1,-1,10,100};
    const int16_t indices[] = {-32768,-1,0,65,66,69,70,99,100,32767};
    const int16_t conditions[] = {-32768,-1,0,1,2,3,4,47,48,49,32767};
    const int8_t triggers[] = {-128,-1,0,1,4,5,127};
    const int16_t angles[] = {-13825,-13824,-13312,-769,-768,-1,0,1,768,769,13312,13824,13825};
    memset(v,0,sizeof(*v)); memset(s,0,sizeof(*s));
    v->speed = speeds[(i/11)%8]; v->drive = speeds[(i/13)%8];
    v->load[0] = (float)((int)(i%201)-100); v->load[1] = (float)((int)((i/7)%201)-100);
    v->front_grip = speeds[(i/17)%8]; s->rear_grip = speeds[(i/19)%8];
    v->yaw_delta = nm_yaw_angle(i); v->steer_angle = nm_yaw_angle(i*73);
    v->speed_index = indices[(i/23)%10]; s->condition = conditions[(i/29)%11];
    s->trigger_count = triggers[(i/31)%7]; s->filter = nm_yaw_angle(i*89);
    s->angle[0] = 123; s->angle[1] = nm_yaw_angle(i*37);
    s->flags = i*0x01010101u; s->override_count = (uint8_t)i; s->trigger_pair = 17;
    if (i >= 65536) {
        s->angle[1] = angles[i%13];
        v->yaw_delta = (int16_t)((int)((i/13)%531)-265);
        s->filter = (int16_t)((int)(i%513)-256);
    }
}
static inline uint32_t nm_tires_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_TIRES_CHECK_CASES; ++i) {
        nm_tires_input_t v; nm_tires_state_t s; nm_tires_test_case(i,&v,&s);
        const unsigned accepted = nm_tires_step(p,&v,&s);
        const uint32_t words[] = {accepted,nm_force_bits(s.rear_grip),s.flags,s.trigger_pair,
            (uint32_t)(int32_t)s.angle[0],(uint32_t)(int32_t)s.angle[1],
            (uint32_t)(int32_t)s.filter,(uint32_t)(int32_t)s.condition,
            (uint32_t)(int32_t)s.trigger_count,s.override_count};
        for (unsigned w = 0; w < 10; ++w) for (unsigned b = 0; b < 4; ++b)
            hash = (hash ^ ((words[w]>>(8*b))&255)) * 16777619u;
    }
    return hash;
}
#define NM_GEARS_CHECK_CASES 131072u
#define NM_GEARS_CHECK_HASH 0xeab110f6u
static inline void nm_gears_test_case(unsigned i, const nm_drive_params_t *p,
                                      nm_gears_input_t *v, nm_gears_state_t *s) {
    const int32_t limits[] = {INT32_MIN,INT32_MAX,-10000,-1,0,1,8499,8500,8501,10000};
    memset(v,0,sizeof(*v)); memset(s,0,sizeof(*s));
    v->manual = i%2; v->selector = (i/2)%8; s->gear = (i/16)%4;
    v->speed_index = i%3 ? (int32_t)(i+1) : 0;
    v->rpm_bias = limits[(i/67)%10]; s->filtered_rpm = limits[(i/71)%10];
    s->downshift_rpm = (int32_t)((i*13)%12000); s->brake = i*0x01010101u;
    s->shift_ticks = (int8_t)((int)((i/64)%256)-128);
    s->rpm = (int32_t)((i*37)%12000);
    if (i >= 65536 && !v->manual) {
        const int32_t threshold = (i/64)%2 ? p->gear_up[s->gear] : p->gear_down[s->gear];
        const int32_t candidate = nm_gears_signed((uint32_t)threshold+(i/128)%3-1u);
        /* Adjacent gear must exist. Out-of-range transitions are rejection tests. */
        if ((s->gear || candidate >= p->gear_down[0]) &&
            (s->gear < 3 || !v->speed_index || candidate <= p->gear_up[3])) s->rpm = candidate;
    }
}
static inline uint32_t nm_gears_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_GEARS_CHECK_CASES; ++i) {
        nm_gears_input_t v; nm_gears_state_t s; nm_gears_test_case(i,p,&v,&s);
        const unsigned accepted = nm_gears_step(p,&v,&s);
        const uint32_t words[] = {accepted,s.gear,s.brake,(uint32_t)s.rpm,
            (uint32_t)s.filtered_rpm,(uint32_t)s.downshift_rpm,(uint32_t)(int32_t)s.shift_ticks};
        for (unsigned w = 0; w < 7; ++w) for (unsigned b = 0; b < 4; ++b)
            hash = (hash ^ ((words[w]>>(8*b))&255)) * 16777619u;
    }
    return hash;
}
#define NM_RESISTANCE_CHECK_HASH 0x707d6da1u
static inline uint32_t nm_resistance_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 256; ++i) {
        nm_resistance_state_t s = {123,(uint8_t)i};
        if (!nm_resistance_step(p,&s)) return 0;
        const uint32_t words[] = {nm_force_bits(s.resistance),s.override_count};
        for (unsigned w = 0; w < 2; ++w) for (unsigned b = 0; b < 4; ++b)
            hash = (hash ^ ((words[w]>>(8*b))&255)) * 16777619u;
    }
    return hash;
}
#define NM_PROXIMITY_CHECK_CASES 65536u
#define NM_PROXIMITY_CHECK_HASH 0x99b6b08du
static inline void nm_proximity_test_case(unsigned i, const nm_drive_params_t *p,
                                          nm_proximity_input_t *v, int8_t *count) {
    const float distances[] = {0,0.001f,10,29.5f,29.99999f,30,30.00001f,100};
    const int offsets[] = {-911,-910,-909,-1,0,1,909,910,911,32767};
    memset(v,0,sizeof(*v));
    v->position[0] = (float)((int)(i%4096)-2048)*0.5f;
    v->position[1] = (float)((int)((i*73)%4096)-2048)*0.5f;
    v->heading = nm_yaw_angle(i); v->disabled = i%31 == 0;
    *count = (int8_t)((int)(i%256)-128);
    for (unsigned n = 0; n < 3; ++n) {
        float cart[2];
        nm_force_cartesian(p,(uint16_t)(i+offsets[(i/(13+n))%10]),distances[(i/(17+n))%8],cart);
        v->neighbor[n][0] = v->position[0]-cart[0];
        v->neighbor[n][1] = v->position[1]+cart[1];
    }
}
static inline uint32_t nm_proximity_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_PROXIMITY_CHECK_CASES; ++i) {
        nm_proximity_input_t v; int8_t count; nm_proximity_test_case(i,p,&v,&count);
        const unsigned accepted = nm_proximity_step(p,&v,&count);
        hash = (hash ^ accepted) * 16777619u;
        hash = (hash ^ (uint8_t)count) * 16777619u;
    }
    return hash;
}
#define NM_MOTION_CHECK_CASES (3u*65536u)
#define NM_MOTION_CHECK_HASH 0x028f30e6u
static inline void nm_motion_test_case(unsigned i, nm_motion_input_t *v, nm_motion_state_t *s) {
    const float speeds[] = {-1000,-1,-0.0f,0.0f,0.001f,1,30,1000};
    const float scales[] = {-1,-0.0f,0,0.25f,1,2};
    const int32_t indices[] = {INT32_MIN,-1,0,29,30,31,32,INT32_MAX};
    memset(v,0,sizeof(*v)); memset(s,0,sizeof(*s));
    s->heading = nm_yaw_angle(i); s->yaw = nm_yaw_angle(i*73);
    s->speed = speeds[(i/8)%8];
    for (unsigned a = 0; a < 3; ++a) {
        s->position[a] = (float)((int)((i*(13+a))%65536)-32768)*0.125f;
        v->displacement[a] = speeds[(i+a)%8];
    }
    s->delta[0] = 123; s->delta[1] = -321;
    s->lookahead[0] = 456; s->lookahead[1] = -654;
    v->scale = scales[(i/17)%6]; v->flags = i*0x01010101u;
    v->speed_index = indices[i%8]; v->spin_heading = nm_yaw_angle(i);
    v->spin_rotation = nm_yaw_angle(i*37); v->hold_rotation = nm_yaw_angle(i*89);
    if (i >= 65536 && i < 131072) { v->spin_ticks = (uint8_t)(1+i%32); v->guided_ticks = 3; }
    v->held = i >= 131072;
}
static inline uint32_t nm_motion_test_hash(const nm_drive_params_t *p) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_MOTION_CHECK_CASES; ++i) {
        nm_motion_input_t v; nm_motion_state_t s; nm_motion_test_case(i,&v,&s);
        if (!nm_motion_step(p,&v,&s)) return 0;
        const uint8_t *bytes = (const uint8_t *)&s;
        for (unsigned b = 0; b < sizeof(s); ++b) hash = (hash ^ bytes[b])*16777619u;
    }
    return hash;
}
#endif
