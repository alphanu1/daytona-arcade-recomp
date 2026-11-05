#define _POSIX_C_SOURCE 200809L
#include "drive_check.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <float.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 2) {
        nm_drive_params_t *actual = nm_drive_load(argv[1]);
        if (!actual) return 1;
        printf("drive cases=%u hash=%08x payload=%zu\n",NM_DRIVE_CHECK_CASES,nm_drive_test_hash(actual),sizeof(*actual));
        printf("axle cases=%u hash=%08x\n",NM_AXLE_CHECK_CASES,nm_axle_test_hash(actual));
        printf("force math cases=%u hash=%08x; divide cases=%u hash=%08x\n",NM_FORCE_CHECK_CASES,
               nm_force_test_hash(actual),NM_DIVIDE_CHECK_CASES,nm_divide_test_hash(actual));
        printf("forces cases=%u hash=%08x\n",NM_FORCES_CHECK_CASES,nm_forces_test_hash(actual));
        printf("traction cases=%u hash=%08x; speed cases=%u hash=%08x\n",NM_TRACTION_CHECK_CASES,
               nm_traction_test_hash(actual),NM_SPEED_CHECK_CASES,nm_speed_test_hash(actual));
        printf("yaw cases=%u hash=%08x\n",NM_YAW_CHECK_CASES,nm_yaw_test_hash(actual));
        printf("tires cases=%u hash=%08x\n",NM_TIRES_CHECK_CASES,nm_tires_test_hash(actual));
        printf("gears cases=%u hash=%08x\n",NM_GEARS_CHECK_CASES,nm_gears_test_hash(actual));
        printf("resistance cases=256 hash=%08x; proximity cases=%u hash=%08x\n",
               nm_resistance_test_hash(actual),NM_PROXIMITY_CHECK_CASES,nm_proximity_test_hash(actual));
        printf("motion cases=%u hash=%08x\n",NM_MOTION_CHECK_CASES,nm_motion_test_hash(actual));
        free(actual); return 0;
    }
    nm_drive_params_t *p = calloc(1,sizeof(*p)); assert(p);
    for (unsigned row = 0; row < 4; ++row) for (unsigned i = 0; i < 14; ++i) p->collision_schedule[row][i] = (uint8_t)i;
    for (unsigned body = 0; body < 2; ++body) for (unsigned side = 0; side < 4; ++side) p->collision_flags[body][side] = 1u<<(31-4*body-side);
    p->throttle_scale = 2; p->throttle_offset = 0.5f; p->gravity = 4;
    p->compensation_scale = 0.0005f;
    p->brake[0] = 2; p->brake[1] = 3;
    for (unsigned g = 0; g < 4; ++g) p->gear[g] = (float)(g+1);
    for (unsigned c = 0; c < 2; ++c) for (unsigned r = 0; r < 67; ++r) p->curve[c][r] = (float)(r+c+1);
    for (unsigned c = 0; c < 3; ++c) for (unsigned s = 0; s < 512; ++s) p->drag[c][s] = (float)(2*(c+1));
    const float sine = 0.25f; memcpy(&p->sine[1],&sine,4);
    for (unsigned i = 0; i < 4; ++i) p->collision_corner[i] = (uint8_t)i;
    assert(nm_drive_params_valid(p));
    nm_drive_input_t v = {0}; v.throttle = v.brake = 255; v.pitch = 2;
    nm_drive_terms_t out;
    assert(nm_drive_terms(p,&v,&out));
    assert(out.drive == 1.5f && out.brake[0] == 2 && out.brake[1] == 3 && out.slope == 0.5f && out.drag == 1);
    v.rpm = 65535; v.curve = 255; v.gear = 3; v.compensation = 2000;
    v.pitch = -1; v.contact[0] = 4;
    assert(nm_drive_terms(p,&v,&out));
    assert(out.drive == 400 && out.slope == -0.5f && out.drag == 4);
    v.contact[1] = v.contact[2] = 8;
    assert(nm_drive_terms(p,&v,&out) && out.drag == 3);
    const nm_drive_terms_t before = out;
    v.gear = 4; assert(!nm_drive_terms(p,&v,&out)); v.gear = 0;
    v.speed_index = 512; assert(!nm_drive_terms(p,&v,&out)); v.speed_index = 0;
    /* Negative speed index (nm_drive_drag_bits): the ROM words before the base table (the 10dd8 table, its
     * size word 0x200, three erased words) and at the surface tables' 16-bit wrap (erased). */
    for (unsigned i = 0; i < 127; ++i) p->guided_targets[i] = (float)i;
    uint32_t word; float value;
    assert(nm_drive_drag_bits(p,-1,0,&word) && word == 0xffffffffu && nm_drive_drag_bits(p,-3,0,&word) && word == 0xffffffffu);
    assert(nm_drive_drag_bits(p,-4,0,&word) && word == 0x200u);
    assert(nm_drive_drag_bits(p,-5,0,&word)); memcpy(&value,&word,4); assert(value == 126.0f);
    assert(nm_drive_drag_bits(p,-131,0,&word)); memcpy(&value,&word,4); assert(value == 0.0f);
    assert(!nm_drive_drag_bits(p,-132,0,&word) && !nm_drive_drag_bits(p,512,0,&word));
    assert(nm_drive_drag_bits(p,-7,1,&word) && word == 0xffffffffu && nm_drive_drag_bits(p,-7,2,&word) && word == 0xffffffffu);
    assert(nm_drive_drag_bits(p,511,1,&word)); memcpy(&value,&word,4); assert(value == 4.0f);
    v.speed_index = (uint32_t)-7; /* Three loose wheels: the surface word is the NaN. */
    assert(nm_drive_terms(p,&v,&out)); memcpy(&word,&out.drag,4); assert(word == 0xffffffffu);
    v.contact[0] = v.contact[1] = v.contact[2] = 0; /* On the road: the base word alone. */
    assert(nm_drive_terms(p,&v,&out) && out.drag == 62.0f);
    v.speed_index = (uint32_t)-1; assert(nm_drive_terms(p,&v,&out)); memcpy(&word,&out.drag,4); assert(word == 0xffffffffu);
    v.speed_index = (uint32_t)-132; assert(!nm_drive_terms(p,&v,&out));
    v.contact[0] = 4; v.contact[1] = v.contact[2] = 8; v.speed_index = 0; out = before;
    for (unsigned i = 0; i < 127; ++i) p->guided_targets[i] = 0;
    p->gravity = NAN; assert(!nm_drive_params_valid(p) && !nm_drive_terms(p,&v,&out)); p->gravity = 4;
    assert(!nm_drive_terms(NULL,&v,&out) && !nm_drive_terms(p,NULL,&out) && !nm_drive_terms(p,&v,NULL));
    assert(!memcmp(&before,&out,sizeof(out)));
    p->axle.bank[0] = 16; p->axle.bank[1] = 32;
    p->axle.pitch = 4; p->axle.transfer = 2;
    p->axle.scale[0] = p->axle.scale[1] = 1;
    p->axle.maximum[0] = p->axle.maximum[1] = 1000000;
    const float one = 1; memcpy(&p->sine[16383],&one,4);
    nm_axle_input_t axle = {0}; axle.pitch = -1; axle.speed_index = 10;
    nm_axle_state_t state = {{0,0}};
    assert(nm_axle_load_step(p,&axle,&state));
    assert(state.load[0] == 1.125f && state.load[1] == 1.875f);
    axle.speed_index = 11;
    assert(nm_axle_load_step(p,&axle,&state));
    assert(state.load[0] == 1.0546875f && state.load[1] == 1.7578125f);
    assert(nm_axle_surface(0,10) == 1 && nm_axle_surface(0,11) == 0);
    assert(nm_axle_surface(0x0402,11) == 230.0f/255 && nm_axle_surface(0xc,11) == 128.0f/255);
    assert(nm_axle_surface(0xffff,11) == 173.0f/255 && nm_axle_surface(0x8001,11) == 150.0f/255);
    assert(nm_axle_clamp(-100,-1,100) == -100); /* Not a numerical float clamp. */
    assert(!signbit(nm_axle_clamp(-0.0f,0.0f,100)));
    const nm_axle_state_t before_axle = state;
    assert(!nm_axle_load_step(NULL,&axle,&state) && !nm_axle_load_step(p,NULL,&state) && !nm_axle_load_step(p,&axle,NULL));
    p->axle.bank[1] = NAN;
    assert(!nm_drive_params_valid(p) && !nm_axle_load_step(p,&axle,&state)); p->axle.bank[1] = 32;
    axle.inertial[1] = INFINITY;
    assert(!nm_axle_load_step(p,&axle,&state)); axle.inertial[1] = 0;
    /* A NaN delta (finish phases after a NaN drag term) reaches the clamp, whose integer compare of the bits
     * settles it: a positive NaN is above the maximum, the drag word's 0xffffffff below the minimum. */
    nm_axle_state_t nan_state = state;
    axle.longitudinal_delta = nm_nan_float(0x7fc00000u);
    assert(nm_axle_load_step(p,&axle,&nan_state));
    for (unsigned a = 0; a < 2; ++a)
        assert(nan_state.load[a] == (1000000.0f-state.load[a])*0.0625f+state.load[a]);
    nan_state = state; axle.longitudinal_delta = nm_nan_float(0xffffffffu);
    assert(nm_axle_load_step(p,&axle,&nan_state));
    for (unsigned a = 0; a < 2; ++a) assert(nan_state.load[a] == (0.0f-state.load[a])*0.0625f+state.load[a]);
    axle.longitudinal_delta = INFINITY;
    assert(!nm_axle_load_step(p,&axle,&state)); axle.longitudinal_delta = 0;
    assert(!memcmp(&before_axle,&state,sizeof(state)));
    p->reciprocal[0] = nm_force_bits(1); p->reciprocal[1] = nm_force_bits(4);
    assert(nm_force_divide(p,2,2) == 3 && nm_force_divide(p,2,-2) == -3);
    float cart[2]; nm_force_cartesian(p,1,4,cart);
    assert(cart[0] == 1 && cart[1] == 4);
    const unsigned sqrt_index = 0x2000 ^ ((nm_force_bits(25)>>10)&0x3ffe);
    p->inverse_sqrt[sqrt_index] = nm_force_bits(2);
    p->inverse_sqrt[sqrt_index+1] = nm_force_bits(6);
    assert(nm_force_length(p,3,4) == 17.96875f); /* Authored synthetic lookup entries. */
    nm_force_polar_t a = {0,1}, b = {0,INFINITY}, combined = {7,8}, combined_before = combined;
    assert(!nm_force_combine(p,&a,&b,&combined));
    assert(!nm_force_combine(NULL,&a,&b,&combined) && !nm_force_combine(p,NULL,&b,&combined));
    assert(!nm_force_combine(p,&a,NULL,&combined) && !nm_force_combine(p,&a,&b,NULL));
    a.magnitude = b.magnitude = FLT_MAX;
    assert(!nm_force_combine(p,&a,&b,&combined));
    assert(!memcmp(&combined,&combined_before,sizeof(combined)));
    nm_forces_input_t force_input = {0};
    nm_forces_state_t force_state = {0};
    force_state.slip_decay = -1;
    force_state.tire_angle[0] = -32768; force_state.tire_angle[1] = 32767;
    force_input.steer = -32768; force_input.condition_99 = 1;
    assert(nm_forces_step(p,&force_input,&force_state));
    assert(force_state.steer_angle == 4096 && force_state.tire_angle[0] == 24576 && force_state.tire_angle[1] == 24575);
    assert(force_state.grip[0] == 1 && force_state.grip[1] == 1 && force_state.longitudinal_delta == 0);
    assert(force_state.slip_decay == -1 && !force_state.trigger_pair && !force_state.rpm_boost);
    force_input.condition_ec = 1;
    assert(nm_forces_step(p,&force_input,&force_state));
    assert(!force_state.steer_angle && !force_state.tire_angle[0] && !force_state.tire_angle[1]);
    const nm_forces_state_t before_forces = force_state;
    force_input.gear = 4; assert(!nm_forces_step(p,&force_input,&force_state)); force_input.gear = 0;
    force_input.terms.drive = NAN; assert(!nm_forces_step(p,&force_input,&force_state)); force_input.terms.drive = 0;
    force_input.resistance = INFINITY; assert(!nm_forces_step(p,&force_input,&force_state)); force_input.resistance = 0;
    force_input.axle.load[1] = NAN; assert(!nm_forces_step(p,&force_input,&force_state)); force_input.axle.load[1] = 0;
    force_input.terms.drive = FLT_MAX; assert(!nm_forces_step(p,&force_input,&force_state)); force_input.terms.drive = 0;
    assert(!nm_forces_step(NULL,&force_input,&force_state) && !nm_forces_step(p,NULL,&force_state) && !nm_forces_step(p,&force_input,NULL));
    assert(!memcmp(&before_forces,&force_state,sizeof(force_state)));
    nm_traction_input_t traction_input = {0}; traction_input.gear = 1;
    nm_traction_state_t traction = {0}; traction.drive = 2; traction.rear_load = 16; traction.slope = 3;
    traction.condition_26c = 77;
    for (unsigned i = 1; i < 6; ++i) traction.countdown[i] = 1;
    assert(nm_traction_step(p,&traction_input,&traction));
    assert(traction.drive == 2 && traction.rear_load == 500000 && traction.slope == -2 && !traction.condition_26c);
    for (unsigned i = 0; i < 6; ++i) assert(!traction.countdown[i]);
    traction.drive = 1; traction.rear_load = 0.5f; traction.countdown[0] = 1;
    traction_input.inertial[1] = 1;
    assert(nm_traction_step(p,&traction_input,&traction) && traction.drive == 0.375f && !traction.countdown[0]);
    const nm_traction_state_t before_traction = traction;
    traction_input.lateral = NAN; traction_input.course_slot = 3;
    assert(nm_traction_step(p,&traction_input,&traction));
    traction_input.course_slot = 0; assert(!nm_traction_step(p,&traction_input,&traction)); traction_input.lateral = 0;
    traction_input.inertial[1] = INFINITY; assert(!nm_traction_step(p,&traction_input,&traction)); traction_input.inertial[1] = 0;
    assert(!nm_traction_step(NULL,&traction_input,&traction) && !nm_traction_step(p,NULL,&traction) && !nm_traction_step(p,&traction_input,NULL));
    assert(!memcmp(&before_traction,&traction,sizeof(traction)));
    traction.previous_lateral = -FLT_MAX;
    traction_input.lateral = FLT_MAX; traction_input.tire_angle = -4096;
    traction_input.speed_index = 150; traction_input.surface = 2;
    assert(nm_traction_step(p,&traction_input,&traction));
    assert(traction.previous_lateral == FLT_MAX && traction.rear_load == 1.03f*0.5f);
    p->rpm_scale = 1;
    nm_speed_state_t speed = {2.25f,0.25f,-1,-1};
    assert(nm_speed_step(p,8,0,&speed) && speed.speed == 2.5f && speed.rpm == 3 && !speed.compensation);
    speed = (nm_speed_state_t){9000,0.5f,0,0};
    assert(nm_speed_step(p,8,0,&speed) && speed.rpm == 8500 && speed.compensation == 501);
    speed = (nm_speed_state_t){-0.0f,-0.0f,1,1};
    assert(nm_speed_step(p,8,0,&speed) && !signbit(speed.speed) && !signbit(speed.delta) && !speed.rpm);
    speed = (nm_speed_state_t){1,-2,1,1};
    assert(nm_speed_step(p,8,0,&speed) && speed.speed == 0 && speed.delta == 0 && !speed.compensation);
    const nm_speed_state_t before_speed = speed;
    assert(nm_speed_step(p,1,4,&speed) && !nm_speed_step(p,8,4,&speed));
    p->rpm_scale = NAN; assert(!nm_drive_params_valid(p) && !nm_speed_step(p,8,0,&speed)); p->rpm_scale = 1;
    assert(!nm_speed_step(NULL,8,0,&speed) && !nm_speed_step(p,8,0,NULL));
    assert(!memcmp(&before_speed,&speed,sizeof(speed)));
    speed.speed = FLT_MAX; speed.delta = FLT_MAX;
    const nm_speed_state_t overflow_speed = speed;
    assert(!nm_speed_step(p,8,0,&speed) && !memcmp(&overflow_speed,&speed,sizeof(speed)));
    /* cvtri overflow is masked (AC 3f001000) and saturates (80960KB PRM 11-50). */
    speed.speed = 2147483648.0f; speed.delta = 0;
    assert(nm_speed_step(p,8,0,&speed) && speed.rpm == 8500 && speed.compensation == INT32_MAX-8500);
    int32_t rounded = 17;
    assert(nm_drive_round_i32(-2.5f,&rounded) && rounded == -3);
    assert(nm_drive_round_i32(-2147483648.0f,&rounded) && rounded == INT32_MIN);
    assert(nm_drive_round_i32(INFINITY,&rounded) && rounded == INT32_MAX);
    assert(nm_drive_round_i32(-INFINITY,&rounded) && rounded == INT32_MIN);
    assert(nm_drive_round_i32(3e9f,&rounded) && rounded == INT32_MAX);
    assert(nm_drive_round_i32(-3e9f,&rounded) && rounded == INT32_MIN);
    assert(nm_drive_round_i32(2147483520.0f,&rounded) && rounded == 2147483520);
    assert(nm_drive_round_i32(NAN,&rounded) && rounded == INT32_MIN);
    /* The host's NaN operations (force_math.h, the reference's TGP model): the first NaN operand, a signalling
     * one first and quietened; an invalid operation of other operands gives 0x7fc00000. */
    const float quiet = nm_nan_float(0xffffffffu), signalling = nm_nan_float(0x7f800001u), payload = nm_nan_float(0x7fc00005u);
    assert(nm_nan_word(nm_nan_mul(quiet,2)) == 0xffffffffu && nm_nan_word(nm_nan_add(2,quiet)) == 0xffffffffu);
    assert(nm_nan_word(nm_nan_mul(2,signalling)) == 0x7fc00001u && nm_nan_word(nm_nan_sub(payload,quiet)) == 0x7fc00005u);
    assert(nm_nan_word(nm_nan_mul(quiet,signalling)) == 0x7fc00001u);
    assert(nm_nan_word(nm_nan_sub(INFINITY,INFINITY)) == 0x7fc00000u && nm_nan_word(nm_nan_mul(0,INFINITY)) == 0x7fc00000u);
    assert(nm_nan_mul(3,0.5f) == 1.5f && nm_nan_add(1,2) == 3 && nm_nan_sub(1,2) == -1 && nm_nan_mul(2,INFINITY) == INFINITY);
    /* 10338 after a NaN drag term: the NaN delta's sign bit zeroes speed and delta. */
    speed = (nm_speed_state_t){0.25f,quiet,100,0};
    assert(nm_speed_step(p,8,0,&speed) && speed.speed == 0 && speed.delta == 0 && !speed.rpm && !speed.compensation);
    speed.delta = nm_nan_float(0x7fc00000u); assert(!nm_speed_step(p,8,0,&speed));
    assert(nm_drive_round_i32(-NAN,&rounded) && rounded == INT32_MIN);
    assert(rounded == INT32_MIN);
    nm_yaw_input_t yaw_input = {0};
    nm_yaw_state_t yaw = {0};
    yaw_input.fixed_offset = 1; yaw_input.speed_index = 1; yaw_input.road_heading = 130; yaw_input.steer = -1;
    yaw.yaw = 100; yaw.offset = 20; yaw.alignment_frames = 3; yaw.inertial[0] = 16; yaw.inertial[1] = -16;
    assert(nm_yaw_step(p,&yaw_input,&yaw));
    assert(yaw.yaw == 110 && yaw.heading == 120 && yaw.body_yaw == 114 && !yaw.yaw_delta);
    assert(yaw.alignment_frames == 4 && yaw.offset == 20 && yaw.inertial[0] == 15 && yaw.inertial[1] == -15);
    yaw.alignment_frames = UINT32_MAX;
    assert(nm_yaw_step(p,&yaw_input,&yaw) && !yaw.alignment_frames && yaw.yaw == 110);
    yaw_input.road_heading = -1000;
    assert(nm_yaw_step(p,&yaw_input,&yaw) && !yaw.alignment_frames);
    const nm_yaw_state_t before_yaw = yaw;
    yaw_input.speed = NAN;
    assert(!nm_yaw_step(p,&yaw_input,&yaw)); yaw_input.speed = 0;
    assert(!nm_yaw_step(NULL,&yaw_input,&yaw) && !nm_yaw_step(p,NULL,&yaw) && !nm_yaw_step(p,&yaw_input,NULL));
    p->turn_scale = INFINITY;
    assert(!nm_yaw_step(p,&yaw_input,&yaw)); p->turn_scale = 0;
    assert(!memcmp(&before_yaw,&yaw,sizeof(yaw)));
    assert(nm_yaw_angle(32768) == -32768 && nm_yaw_angle(65536) == 0 && nm_yaw_angle(UINT32_MAX) == -1);
    memset(p,0,sizeof(*p));
    for (unsigned row = 0; row < 4; ++row) for (unsigned i = 0; i < 14; ++i) p->collision_schedule[row][i] = (uint8_t)i;
    for (unsigned i = 0; i < 4; ++i) p->collision_corner[i] = (uint8_t)i;
    for (unsigned body = 0; body < 2; ++body) for (unsigned side = 0; side < 4; ++side) p->collision_flags[body][side] = 1u<<(31-4*body-side);
    p->sine[16383] = nm_force_bits(1);
    nm_tires_input_t tire_input = {0};
    nm_tires_state_t tires = {0};
    tire_input.yaw_delta = 1;
    assert(nm_tires_step(p,&tire_input,&tires));
    assert(tires.filter == -1 && tires.angle[1] == -1 && tires.angle[0] == -1); /* SHRI floors. */
    memset(&tires,0,sizeof(tires)); tire_input.yaw_delta = 0; tire_input.speed_index = 66;
    tires.angle[1] = -13824;
    assert(nm_tires_step(p,&tire_input,&tires));
    assert(tires.angle[1] == -13824 && tires.override_count == 1 && tires.flags == 0x10002);
    memset(&tires,0,sizeof(tires)); tires.angle[1] = 13824;
    assert(nm_tires_step(p,&tire_input,&tires));
    assert(tires.angle[1] == 13824 && !tires.override_count && tires.flags == 0x8002);
    memset(&tires,0,sizeof(tires)); tire_input.speed_index = 100; tire_input.yaw_delta = 217;
    assert(nm_tires_step(p,&tire_input,&tires));
    assert(tires.condition == 3 && tires.trigger_count == 32 && tires.trigger_pair);
    tire_input.drive = -0.0f; tire_input.yaw_delta = 0;
    assert(nm_tires_step(p,&tire_input,&tires) && !tires.condition && !tires.trigger_pair);
    tires.condition = 1; tires.trigger_count = -1;
    assert(nm_tires_step(p,&tire_input,&tires));
    assert(tires.condition == -2 && tires.trigger_count == 24 && tires.trigger_pair);
    const nm_tires_state_t before_tires = tires;
    assert(!nm_tires_step(NULL,&tire_input,&tires) && !nm_tires_step(p,NULL,&tires) && !nm_tires_step(p,&tire_input,NULL));
    tire_input.speed = NAN; assert(!nm_tires_step(p,&tire_input,&tires)); tire_input.speed = 0;
    tire_input.front_grip = INFINITY; assert(!nm_tires_step(p,&tire_input,&tires)); tire_input.front_grip = 0;
    p->tire_response[1] = NAN; assert(!nm_tires_step(p,&tire_input,&tires)); p->tire_response[1] = 0;
    assert(!memcmp(&before_tires,&tires,sizeof(tires)));
    tires.condition = 1; tires.rear_grip = FLT_MAX; tire_input.drive = 0;
    const nm_tires_state_t overflow_tires = tires;
    assert(!nm_tires_step(p,&tire_input,&tires) && !memcmp(&overflow_tires,&tires,sizeof(tires)));
    p->reciprocal[0] = nm_force_bits(1); p->reciprocal[1] = nm_force_bits(4);
    for (unsigned g = 0; g < 4; ++g) {
        p->gear[g] = (float)(g+1); p->gear_shift_scale[g] = 2;
        p->gear_down[g] = g ? 100 : -5000; p->gear_up[g] = g == 3 ? 50000 : 10000;
        p->gear_rpm_delta[g][0] = -10; p->gear_rpm_delta[g][1] = 10;
    }
    p->gear_selector[0] = -1; p->gear_selector[1] = 0; p->gear_selector[2] = 1;
    nm_gears_input_t gear_input = {0}; nm_gears_state_t gear_state = {0};
    gear_input.speed_index = 1; gear_state.rpm = 10001; gear_state.shift_ticks = -1;
    assert(nm_gears_step(p,&gear_input,&gear_state));
    assert(gear_state.gear == 1 && gear_state.rpm == 20002 && gear_state.shift_ticks == 16 && gear_state.filtered_rpm == 10);
    memset(&gear_state,0,sizeof(gear_state)); gear_state.rpm = 10001; gear_input.speed_index = 0;
    assert(nm_gears_step(p,&gear_input,&gear_state) && !gear_state.gear && gear_state.rpm == 10001 && !gear_state.shift_ticks);
    gear_state.gear = 1; gear_state.rpm = 99; gear_state.filtered_rpm = 0;
    assert(nm_gears_step(p,&gear_input,&gear_state));
    assert(!gear_state.gear && gear_state.rpm == 149 && gear_state.shift_ticks == -16); /* Refined reciprocal + half tie. */
    gear_state.gear = 1; gear_state.rpm = 100; gear_state.shift_ticks = 0;
    assert(nm_gears_step(p,&gear_input,&gear_state) && gear_state.gear == 1 && !gear_state.shift_ticks);
    gear_input.manual = 1; gear_input.selector = 1; gear_state.filtered_rpm = 0;
    assert(nm_gears_step(p,&gear_input,&gear_state));
    assert(!gear_state.gear && gear_state.rpm == 600 && gear_state.downshift_rpm == 4600 && gear_state.shift_ticks == -16);
    assert(nm_gears_step(p,&gear_input,&gear_state));
    assert(gear_state.rpm == 4600 && gear_state.downshift_rpm == 4350 && gear_state.shift_ticks == -15);
    gear_state.downshift_rpm = 9001; gear_state.shift_ticks = -1; gear_state.filtered_rpm = 9000;
    assert(nm_gears_step(p,&gear_input,&gear_state));
    assert(gear_state.rpm == 8500 && gear_state.brake == 255 && gear_state.downshift_rpm == 8751 &&
           !gear_state.shift_ticks && gear_state.filtered_rpm == 8500);
    gear_state.gear = 3; gear_state.rpm = 100; gear_state.filtered_rpm = INT32_MAX;
    gear_input.manual = 0; gear_input.rpm_bias = INT32_MAX;
    assert(nm_gears_step(p,&gear_input,&gear_state) && gear_state.filtered_rpm == INT32_MIN+9);
    gear_input.manual = 1; gear_input.selector = 0; gear_state.filtered_rpm = INT32_MAX;
    assert(nm_gears_step(p,&gear_input,&gear_state) && !gear_state.filtered_rpm);
    const nm_gears_state_t before_gears = gear_state;
    gear_input.selector = 8; assert(!nm_gears_step(p,&gear_input,&gear_state)); gear_input.selector = 0;
    assert(!nm_gears_step(NULL,&gear_input,&gear_state) && !nm_gears_step(p,NULL,&gear_state) && !nm_gears_step(p,&gear_input,NULL));
    assert(!memcmp(&before_gears,&gear_state,sizeof(gear_state)));
    gear_state.gear = 4; const nm_gears_state_t invalid_gears = gear_state;
    assert(!nm_gears_step(p,&gear_input,&gear_state) && !memcmp(&invalid_gears,&gear_state,sizeof(gear_state)));
    gear_state.gear = 0; gear_state.rpm = -5001; gear_input.manual = 0;
    const nm_gears_state_t underflow_gears = gear_state;
    assert(!nm_gears_step(p,&gear_input,&gear_state) && !memcmp(&underflow_gears,&gear_state,sizeof(gear_state)));
    gear_state.gear = 3; gear_state.rpm = 50001; gear_input.speed_index = 1;
    assert(!nm_gears_step(p,&gear_input,&gear_state));
    gear_state.gear = 0; gear_state.rpm = 10001; p->gear_shift_scale[0] = INFINITY;
    assert(!nm_drive_params_valid(p) && !nm_gears_step(p,&gear_input,&gear_state)); p->gear_shift_scale[0] = 2;
    p->gear_shift_scale[0] = FLT_MAX; /* Upshift rpm overflows: the masked cvtri saturates (PRM 11-50). */
    assert(nm_gears_step(p,&gear_input,&gear_state) && gear_state.gear == 1 && gear_state.rpm == INT32_MAX);
    p->gear_shift_scale[0] = 2;
    assert(nm_gears_signed(UINT32_MAX) == -1 && nm_gears_signed(0x80000000u) == INT32_MIN);
    for (unsigned i = 0; i < 5; ++i) p->resistance[i] = (float)(10+i);
    nm_resistance_state_t resistance = {NAN,128};
    assert(nm_resistance_step(p,&resistance) && resistance.override_count == 127 && resistance.resistance == 14);
    resistance.override_count = 1;
    assert(nm_resistance_step(p,&resistance) && !resistance.override_count && resistance.resistance == 11);
    assert(nm_resistance_step(p,&resistance) && resistance.resistance == 10);
    resistance.override_count = 2;
    assert(nm_resistance_step(p,&resistance) && resistance.override_count == 1 && resistance.resistance == 12);
    const nm_resistance_state_t before_resistance = resistance;
    p->resistance[1] = NAN;
    assert(!nm_drive_params_valid(p) && !nm_resistance_step(p,&resistance)); p->resistance[1] = 11;
    assert(!nm_resistance_step(NULL,&resistance) && !nm_resistance_step(p,NULL));
    assert(!memcmp(&before_resistance,&resistance,sizeof(resistance)));
    assert(nm_resistance_marked_step(p,1,0,&resistance) && resistance.resistance==11 && resistance.override_count==1);
    assert(nm_resistance_marked_step(p,0,4,&resistance) && resistance.resistance==14 && resistance.override_count==1);
    const nm_resistance_state_t before_marked=resistance;
    for(int mark=-128;mark<128;++mark) if(mark<0 || mark>4) {
        assert(!nm_resistance_marked_step(p,1,(int8_t)mark,&resistance));
        assert(!memcmp(&before_marked,&resistance,sizeof(resistance)));
    }
    assert(nm_resistance_marked_step(p,0,0,&resistance) && resistance.resistance==11 && !resistance.override_count);
    nm_proximity_input_t nearby = {0};
    nearby.neighbor[0][1] = nearby.neighbor[1][1] = -10; nearby.neighbor[2][1] = 10;
    int8_t drag_count = 32;
    assert(nm_proximity_step(p,&nearby,&drag_count) && drag_count == 64); /* Third neighbor. */
    nearby.heading = 910;
    assert(nm_proximity_step(p,&nearby,&drag_count) && drag_count == 63); /* Strict threshold. */
    nearby.heading = 909;
    assert(nm_proximity_step(p,&nearby,&drag_count) && drag_count == 64);
    nearby.heading = 910; drag_count = -128;
    assert(nm_proximity_step(p,&nearby,&drag_count) && drag_count == -128);
    nearby.position[0] = NAN; nearby.disabled = 1;
    assert(nm_proximity_step(p,&nearby,&drag_count) && drag_count == -128);
    nearby.disabled = 0; assert(!nm_proximity_step(p,&nearby,&drag_count)); nearby.position[0] = 0;
    nearby.neighbor[0][0] = INFINITY;
    assert(!nm_proximity_step(p,&nearby,&drag_count) && drag_count == -128);
    assert(!nm_proximity_step(NULL,&nearby,&drag_count) && !nm_proximity_step(p,NULL,&drag_count) && !nm_proximity_step(p,&nearby,NULL));
    nearby.neighbor[0][0] = 0; nearby.neighbor[0][1] = 10; nearby.heading = 0;
    nearby.neighbor[1][0] = NAN;
    assert(nm_proximity_step(p,&nearby,&drag_count) && drag_count == 64); /* Later neighbor is never consumed. */
    nm_motion_input_t motion = {0}; nm_motion_state_t pose = {0};
    pose.position[0] = 10; pose.position[1] = 20; pose.position[2] = 30;
    pose.speed = 4; motion.scale = 0.5f;
    assert(nm_motion_step(p,&motion,&pose));
    assert(pose.position[0] == 10 && pose.position[1] == 20 && pose.position[2] == 32);
    assert(pose.delta[0] == 0 && pose.delta[1] == 2 && pose.lookahead[1] == 62);
    pose.yaw = 32767; motion.spin_ticks = 32; motion.spin_rotation = 96;
    motion.spin_heading = 0; motion.flags = 0x400000; motion.speed_index = 30;
    p->spin_scale[31] = 0.5f; p->spin_minimum = 3;
    assert(nm_motion_step(p,&motion,&pose));
    assert(pose.speed == 2 && pose.yaw == -32767 && pose.position[2] == 35 && pose.lookahead[1] == 62);
    motion.spin_rotation = -47; motion.flags = 0; motion.speed_index = 31;
    assert(nm_motion_step(p,&motion,&pose));
    assert(pose.speed == 1 && pose.yaw == -32767 && pose.position[2] == 36);
    const nm_motion_state_t pose_before = pose;
    motion.spin_ticks = 33; assert(!nm_motion_step(p,&motion,&pose));
    motion.spin_ticks = 0; motion.guided_ticks = 1; assert(!nm_motion_step(p,&motion,&pose));
    motion.guided_ticks = 0; motion.scale = INFINITY; assert(!nm_motion_step(p,&motion,&pose));
    assert(!memcmp(&pose_before,&pose,sizeof(pose)));
    motion.scale = FLT_MAX;
    assert(!nm_motion_step(p,&motion,&pose)); /* Lookahead overflows even if the first step fits. */
    assert(!memcmp(&pose_before,&pose,sizeof(pose)));
    motion.spin_ticks = 1; p->spin_scale[0] = NAN;
    assert(!nm_motion_step(p,&motion,&pose) && !nm_drive_params_valid(p));
    assert(!memcmp(&pose_before,&pose,sizeof(pose))); p->spin_scale[0] = 0;
    motion.held = 1; motion.spin_ticks = 255; motion.hold_rotation = -2;
    motion.displacement[0] = -1; motion.displacement[1] = 2; motion.displacement[2] = -3;
    assert(nm_motion_step(p,&motion,&pose)); /* Unused scale/spin data are not consumed. */
    assert(pose.yaw == 32767 && pose.heading == 32767 && pose.position[0] == 9 && pose.position[1] == 22 && pose.position[2] == 33);
    const nm_motion_state_t held_before = pose;
    motion.displacement[2] = NAN; assert(!nm_motion_step(p,&motion,&pose));
    assert(!memcmp(&held_before,&pose,sizeof(pose)));
    assert(!nm_motion_step(NULL,&motion,&pose) && !nm_motion_step(p,NULL,&pose) && !nm_motion_step(p,&motion,NULL));
    assert(!nm_drive_load(NULL));
    p->guided_reserved=1;assert(!nm_drive_params_valid(p));p->guided_reserved=0;
    p->guided_targets[126]=NAN;assert(!nm_drive_params_valid(p));p->guided_targets[126]=0;
    const size_t size = 16+sizeof(*p);
    uint8_t *bytes = malloc(size+1); assert(bytes);
    const uint32_t header[] = {0x52444e44,(uint32_t)size,0,0};
    memcpy(bytes,header,16); memcpy(bytes+16,p,sizeof(*p)); bytes[size] = 0;
    /* Same fixed-size reader as the target: exact length, reserved fields,
     * finite coefficients and bounded sine entries. No external fixtures. */
    for (unsigned test = 0; test < 505; ++test) {
        char path[] = "/tmp/daytona-drive-XXXXXX";
        int fd = mkstemp(path); assert(fd >= 0);
        FILE *f = fdopen(fd,"wb"); assert(f);
        size_t length = size;
        memcpy(bytes,header,16); memcpy(bytes+16,p,sizeof(*p));
        if (test == 1) length = 0;
        if (test == 2) length = 15;
        if (test == 3) --length;
        if (test == 4) ++length;
        if (test >= 5 && test <= 8) bytes[(test-5)*4] ^= 1;
        const uint32_t bad = test == 9 ? 0x7fc00000 : test == 10 ? 0x40000000 : 0xbf800000;
        if (test >= 9 && test <= 11) memcpy(bytes+16+(test == 9 ? 0 : offsetof(nm_drive_params_t,sine)+65532),&bad,4);
        if (test == 12) { const uint32_t nan = 0x7fc00000; memcpy(bytes+16+offsetof(nm_drive_params_t,axle),&nan,4); }
        if (test == 13) bytes[3] = '1'; /* Prior DND1 layout is rejected. */
        if (test == 14) bytes[3] = '2';
        if (test == 17) bytes[3] = '3';
        if (test == 20) bytes[3] = '4';
        if (test == 22) bytes[3] = '5';
        if (test >= 23 && test <= 27) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+offsetof(nm_drive_params_t,turn_geometry)+4*(test-23),&nan,4);
        }
        if (test == 28) bytes[3] = '6';
        if (test >= 29 && test <= 31) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+offsetof(nm_drive_params_t,tire_response)+4*(test-29),&nan,4);
        }
        if (test == 32) bytes[3] = '7';
        if (test >= 33 && test <= 36) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+offsetof(nm_drive_params_t,gear_shift_scale)+4*(test-33),&nan,4);
        }
        if (test == 37) { const int32_t bad = INT32_MAX; memcpy(bytes+16+offsetof(nm_drive_params_t,gear_down),&bad,4); }
        if (test == 38) { const int32_t bad = INT32_MAX; memcpy(bytes+16+offsetof(nm_drive_params_t,gear_rpm_delta),&bad,4); }
        if (test == 39 || test == 40) bytes[16+offsetof(nm_drive_params_t,gear_selector)] = test == 39 ? 4 : 254;
        if (test == 41) bytes[3] = '8';
        if (test >= 42 && test <= 46) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+offsetof(nm_drive_params_t,resistance)+4*(test-42),&nan,4);
        }
        if (test == 47) bytes[3] = '9';
        if (test >= 48 && test <= 80) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+offsetof(nm_drive_params_t,spin_scale)+4*(test-48),&nan,4);
        }
        if (test == 81) bytes[3] = 'A';
        if (test >= 82 && test <= 93) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+offsetof(nm_drive_params_t,wheel_local)+4*(test-82),&nan,4);
        }
        if (test == 94) bytes[3] = 'B';
        if (test == 352) bytes[3] = 'C';
        if (test == 353) bytes[3] = 'D';
        if (test == 354) bytes[16+offsetof(nm_drive_params_t,camera_reset_reserved)] = 1;
        if (test == 355) bytes[3] = 'E';
        if (test >= 356 && test < 360) bytes[16+offsetof(nm_drive_params_t,pivot_sides)+test-356] = 2;
        if (test == 360) bytes[3] = 'F';
        if (test == 363) bytes[3] = 'G';
        if (test == 364) bytes[3] = 'H';
        if (test == 381) bytes[3] = 'I';
        if (test == 384) bytes[3] = 'J';
        if (test >= 385 && test < 389) bytes[16+offsetof(nm_drive_params_t,collision_corner)+test-385] = 4;
        if (test == 389) bytes[3] = 'K';
        if (test == 390) bytes[16+offsetof(nm_drive_params_t,collision_corner)] = 1;
        if (test == 391) bytes[3] = 'L';
        if (test == 504) bytes[3] = 'M'; /* DND22 cannot supply the tilt tail. */
        if (test >= 392 && test < 504) {
            const unsigned at = (test-392)%56;
            bytes[16+offsetof(nm_drive_params_t,collision_schedule)+at] = test < 448 ? 14 : (uint8_t)((at+1)%14);
        }
        if (test >= 382 && test < 384) {
            const uint32_t bad_step = test == 382 ? 0x7fc00000u : 0x7f800000u;
            memcpy(bytes+16+offsetof(nm_drive_params_t,collision_speed_step),&bad_step,4);
        }
        if (test >= 365 && test < 381) {
            const uint32_t mask = test < 373 ? 0 : 3;
            memcpy(bytes+16+offsetof(nm_drive_params_t,collision_flags)+4*((test-365)%8),&mask,4);
        }
        if (test >= 361 && test < 363) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+offsetof(nm_drive_params_t,speed_index_scale)+4*(test-361),&nan,4);
        }
        if (test >= 95 && test < 352) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+offsetof(nm_drive_params_t,guided_adjust_limit)+4*(test-95),&nan,4);
        }
        if (test == 21) { const uint32_t nan = 0x7fc00000; memcpy(bytes+16+offsetof(nm_drive_params_t,rpm_scale),&nan,4); }
        if (test == 15 || test == 16) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+(test == 15 ? offsetof(nm_drive_params_t,reciprocal) : offsetof(nm_drive_params_t,inverse_sqrt)),&nan,4);
        }
        if (test == 18 || test == 19) {
            const uint32_t nan = 0x7fc00000;
            memcpy(bytes+16+(test == 18 ? offsetof(nm_drive_params_t,acceleration_scale) : offsetof(nm_drive_params_t,slip_gear)),&nan,4);
        }
        assert(fwrite(bytes,1,length,f) == length && !fclose(f));
        nm_drive_params_t *loaded = nm_drive_load(path);
        if ((loaded != NULL) != (test == 0)) fprintf(stderr,"drive pack validation case=%u loaded=%d\n",test,loaded != NULL);
        assert((loaded != NULL) == (test == 0));
        if (loaded) assert(!memcmp(loaded,p,sizeof(*p)));
        free(loaded); assert(!unlink(path));
    }
    free(bytes); free(p);
    puts("PASS: native vehicle analytical cases, invalid inputs, DND27 roundtrip and malformed files");
    return 0;
}
