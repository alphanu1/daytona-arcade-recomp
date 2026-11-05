#include "contact_check.h"
#include "boundary_check.h"
#include "../include/player.h"
#include "../include/collision.h"
#include "../include/damage.h"
#include "../include/race_intents.h"
#include "../include/race_field.h"
#undef NDEBUG
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(sizeof(nm_wheels_state_t) == 144 && offsetof(nm_wheels_state_t,road_heading) == 140,
               "ground digest has no interior padding");
_Static_assert(sizeof(nm_boundary_state_t) == 240 && sizeof(nm_boundary_input_t) == 12,
               "bounded boundary state/input budget");
_Static_assert(sizeof(nm_player_t) == 608 && sizeof(nm_player_dynamics_input_t) == 24 && sizeof(nm_player_active_input_t) == 68,
               "single-copy player state/input budget");
_Static_assert(sizeof(nm_player_tick_input_t) == 88,"bounded player entry input");
_Static_assert(sizeof(nm_collision_body_t) == 12,"bounded pair body view");
_Static_assert(sizeof(nm_collision_shape_t) == 72 && sizeof(nm_collision_contact_t) == 44,"bounded contact state");
_Static_assert(sizeof(nm_collision_reaction_t) == 16,"bounded collision reaction view");
_Static_assert(sizeof(nm_collision_damage_body_t) == 64 && sizeof(nm_collision_damage_input_t) == 12,
               "bounded collision damage view/input");

static void check_collision_detection(nm_drive_params_t *p) {
    const uint32_t old_sine = p->sine[16383]; p->sine[16383] = nm_force_bits(1);
    uint8_t old_corners[4]; memcpy(old_corners,p->collision_corner,4);
    const uint8_t corners[4] = {0,2,3,1}; memcpy(p->collision_corner,corners,4);
    uint16_t old_signals[4]; memcpy(old_signals,p->collision_signal,8);
    for (unsigned i = 0; i < 4; ++i) p->collision_signal[i] = (uint16_t)(17+i);
    const float perimeter[4][3] = {{-1,NAN,1},{1,NAN,1},{1,NAN,-1},{-1,NAN,-1}};
    assert(nm_contact_contains(perimeter,0,0) == 1);
    assert(nm_contact_contains(perimeter,1,-1) == 1);
    assert(nm_contact_contains(perimeter,nextafterf(1,INFINITY),0) == 0);
    assert(nm_contact_contains(perimeter,-0.0f,0) == 1);
    assert(nm_contact_contains(NULL,0,0) == -1 && nm_contact_contains(perimeter,NAN,0) == -1);
    nm_collision_shape_t a = {.flags=32,.heading=0}, b;
    for (unsigned v = 0; v < 4; ++v) memcpy(a.world[v],perimeter[v^(v>>1)],12);
    a.position[0] = NAN; // Cached corners do not consume the current position.
    b = a; nm_collision_contact_t out; memset(&out,0x5a,sizeof(out)); uint16_t signal = 99;
    assert(nm_collision_detect(p,&a,&b,1,&out,&signal));
    assert(out.flags == 3 && out.corner[0] == 0 && out.corner[1] == 0 && out.countdown == 4 && b.side == 0);
    assert(out.quarter == 0 && out.side[0] == 2 && out.side[1] == 1 && signal == 17);
    for (unsigned primary = 0; primary < 2; ++primary) for (unsigned inhibited = 0; inhibited < 2; ++inhibited) {
        b.inhibit_ticks = inhibited ? -32768 : 0; signal = 99;
        assert(nm_collision_detect(p,&a,&b,primary,&out,&signal));
        assert(signal == (primary && !inhibited ? 17 : 99));
    }
    // Both passes choose only the FIRST vertex, not the deepest/last point.
    assert(nm_collision_detect(p,&a,&a,1,&out,&signal) && a.side == 0);
    for (unsigned v = 0; v < 4; ++v) b.world[v][0] += 8;
    const nm_collision_contact_t before = out; b.side = 77; signal = 99;
    assert(nm_collision_detect(p,&a,&b,1,&out,&signal) && out.flags == 0 && b.side == 77 && signal == 99);
    nm_collision_contact_t preserved = out; preserved.flags = before.flags;
    assert(!memcmp(&preserved,&before,sizeof(out)));
    a.flags &= ~32u; // Same NaN position is consumed without the cache flag.
    nm_collision_shape_t old_a = a, old_b = b; const nm_collision_contact_t old_out = out;
    assert(!nm_collision_detect(p,&a,&b,1,&out,&signal));
    assert(!memcmp(&a,&old_a,sizeof(a)) && !memcmp(&b,&old_b,sizeof(b)) && !memcmp(&out,&old_out,sizeof(out)) && signal == 99);
    memset(a.position,0,sizeof(a.position)); b.flags &= ~32u;
    old_a = a; old_b = b; // First transform succeeds; second's NaN rolls both back.
    assert(!nm_collision_detect(p,&a,&b,1,&out,&signal));
    assert(!memcmp(&a,&old_a,sizeof(a)) && !memcmp(&b,&old_b,sizeof(b)) && !memcmp(&out,&old_out,sizeof(out)) && signal == 99);
    assert(!nm_collision_detect(NULL,&a,&b,1,&out,&signal) && !nm_collision_detect(p,NULL,&b,1,&out,&signal));
    assert(!nm_collision_detect(p,&a,NULL,1,&out,&signal) && !nm_collision_detect(p,&a,&b,1,NULL,&signal));
    assert(!nm_collision_detect(p,&a,&b,1,&out,NULL));
    p->collision_corner[0] = 4; assert(!nm_collision_detect(p,&a,&b,1,&out,&signal));
    p->collision_corner[0] = 2; assert(!nm_collision_detect(p,&a,&b,1,&out,&signal));
    p->sine[16383] = old_sine; memcpy(p->collision_corner,old_corners,4); memcpy(p->collision_signal,old_signals,8);
    puts("PASS: ordered quad contact, inclusive edges, ignored Y, corner caching, alias and atomic invalid-input failure");
}

static void check_road_tail(nm_drive_params_t *p) {
    assert(nm_road_marks_test_hash() == NM_ROAD_MARKS_HASH);
    uint8_t values[18]; memset(values,0xa5,sizeof(values));
    nm_road_marks_t marks = {values,17,1};
    assert(nm_road_marks_step(&marks,0,128,0,0));
    assert(!marks.timer && values[1] == 0 && values[0] == 0xa5 && values[17] == 0xa5);
    assert(nm_road_marks_step(&marks,0,127,-128,0) && marks.timer == 160);
    for (unsigned behind = 0; behind < 16; ++behind) assert(values[(17-behind)%17] == behind/4);
    assert(values[1] == 0 && values[17] == 0xa5);
    memset(values,0xa5,sizeof(values)); marks.count = 15;
    assert(nm_road_marks_step(&marks,0,-1,0,0));
    assert(values[UINT32_MAX%15] == 0 && values[14] == 0xa5); // Wrap32 before remainder.
    marks.count = 0; const nm_road_marks_t invalid = marks; uint8_t before[18]; memcpy(before,values,sizeof(before));
    assert(!nm_road_marks_step(&marks,0,0,0,0) && !memcmp(&marks,&invalid,sizeof(marks)) && !memcmp(values,before,sizeof(values)));
    assert(!nm_road_marks_step(NULL,0,0,0,0));
    marks.count = 4097; assert(!nm_road_marks_step(&marks,0,0,0,0));
    marks.count = 1; marks.values = NULL; assert(!nm_road_marks_step(&marks,0,0,0,0)); marks.values = values;
    nm_road_profile_t profiles[2] = {{0}};
    nm_road_pack_t road = {.profiles=profiles,.profile_count=2};
    nm_player_t s = {0}; s.boundary.ground.position[0] = NAN;
    const nm_player_t saved = s; const uint32_t timer = marks.timer;
    assert(!nm_player_road_tail(p,&road,&marks,&s));
    assert(!memcmp(&s,&saved,sizeof(s)) && marks.timer == timer && !memcmp(values,before,sizeof(values)));
    s.boundary.ground.position[0] = 0; s.phase[0] = 2;
    assert(!nm_player_road_tail(p,&road,&marks,&s));
    assert(!nm_player_road_tail(NULL,&road,&marks,&s) && !nm_player_road_tail(p,NULL,&marks,&s));
    assert(!nm_player_road_tail(p,&road,NULL,&s) && !nm_player_road_tail(p,&road,&marks,NULL));
    puts("PASS: road marks speed boundary, signed inhibits, unsigned remainder, aliases and atomic tail rejection");
}

static void check_tilt(nm_drive_params_t *p) {
    int16_t old_wave[256]; memcpy(old_wave,p->shake_angles[1],sizeof(old_wave));
    const int16_t old_tail = p->tilt_tail[1], old_boost = p->boost_tilt[0];
    for (unsigned i = 0; i < 256; ++i) p->shake_angles[1][i] = (int16_t)(1000+i);
    p->tilt_tail[1] = 32767; p->boost_tilt[0] = -32768;
    nm_player_t s = {0}; s.surface_phase = 255; s.boundary.ground.combined_mask = 4;
    s.boundary.damage.override_count = 231;
    assert(nm_player_tilt_step(p,0x200000,&s));
    assert(s.tilt[0] == 1033 && s.tilt[1] == 1065 && s.surface_phase == 255 && s.boundary.damage.override_count == 231);
    s.speed_index = -1;
    assert(nm_player_tilt_step(p,0x200000,&s));
    assert(s.surface_phase == 62 && s.boundary.damage.override_count == 255);
    s.boundary.ground.combined_mask = 2; s.speed_index = 1; s.tilt_phase = 30; s.boundary.damage.damage = 0x10;
    assert(nm_player_tilt_step(p,0x200000,&s));
    assert(s.tilt_phase == 31 && s.tilt[0] == -2 && s.tilt[1] == 1065);
    s.force.rpm_boost = UINT32_MAX; s.speed_index = 0; s.tilt_phase = -1;
    assert(nm_player_tilt_step(p,0,&s));
    assert(s.tilt_phase == 0 && s.tilt[0] == -32768 && s.tilt[1] == 1065);
    const nm_player_t before = s;
    assert(!nm_player_tilt_step(NULL,0,&s) && !memcmp(&s,&before,sizeof(s)));
    assert(!nm_player_tilt_step(p,0,NULL));
    memcpy(p->shake_angles[1],old_wave,sizeof(old_wave)); p->tilt_tail[1] = old_tail; p->boost_tilt[0] = old_boost;
    puts("PASS: signed surface tilt, zero-speed writes, boost, narrow overflow and null-input rejection");
}

static void check_body_angles(nm_drive_params_t *p) {
    const float old = p->inertial_scale[0]; p->inertial_scale[0] = 1.0f;
    nm_player_t s = {0}; s.force.rear_bank_force = 0.5f;
    s.force.longitudinal_delta = 1.0f; s.filtered_pitch = 0;
    s.boundary.ground.pitch = 32767; s.boundary.ground.yaw = 32767;
    s.boundary.ground.roll = -32768; s.render_yaw_offset = 1; s.render_roll_offset = -1;
    assert(nm_player_body_angles_step(p,7,&s));
    assert(s.bank == -1 && s.filtered_pitch == 71 && s.render_angles[0] == -32698 &&
           s.render_angles[1] == -32768 && s.render_angles[2] == 32766);
    s.force.rear_bank_force = NAN; s.force.longitudinal_delta = NAN;
    assert(nm_player_body_angles_step(p,0,&s) && s.bank == -1280);
    /* Masked cvtri overflow saturates (80960KB PRM 11-50): +inf -> INT32_MAX. */
    s.force.rear_bank_force = INFINITY; s.force.longitudinal_delta = 0;
    nm_player_t before = s;
    assert(nm_player_body_angles_step(p,0,&s) && s.bank == -1280);
    s.force.rear_bank_force = 1; s.force.longitudinal_delta = INFINITY; s.filtered_pitch = 10; before = s;
    assert(nm_player_body_angles_step(p,0,&s) && s.filtered_pitch == 74);
    assert(!nm_player_body_angles_step(NULL,0,&s) && !nm_player_body_angles_step(p,0,NULL));
    for (unsigned i = 0; i < NM_BODY_ANGLES_CASES; ++i) {
        int16_t offset; nm_body_angles_test_input(i,&s,&offset); before = s;
        if (!nm_player_body_angles_step(p,offset,&s)) assert(!memcmp(&s,&before,sizeof(s)));
    }
    p->inertial_scale[0] = old;
    puts("PASS: body angles rounding/clamps/widths and atomic overflow rejection");
}

static void check_announcements(void) {
    nm_race_car_t cars[3]={0};
    cars[0].car.rank_key=-1;cars[0].car.rank_key_high=0x1234;
    cars[1].car.rank_key=-32768;cars[1].car.rank_key_high=0xffff;
    cars[2].car.rank_key=1;cars[2].car.rank_key_high=0x8000;
    cars[0].car.speed_index=260;cars[0].car.rpm=7800;
    cars[0].car.force.trigger_count[1]=-128;cars[0].car.drag_state=-1;
    for(unsigned rank=0;rank<256;++rank) {
        cars[0].rank=(uint8_t)rank;
        const nm_race_announcement_input_t view=nm_race_announcement_view(cars,cars+1,cars+2,0x500000,2,3,44);
        assert(view.progress==0x1234ffff && view.next_progress==0xffff8000 && view.next_next_progress==0x80000001);
        assert(view.flags==0x500000 && view.gate==2 && view.inhibit==3 && view.cars==44);
        assert(view.speed==260 && view.rpm==7800 && view.condition==-128 && view.passing==-1);
        assert(view.rank==(rank<128 ? (int)rank : (int)rank-256));
    }
    nm_road_pack_t progress_road={0};progress_road.profile_count=100;
    cars[0].car.lap.lap=1;cars[0].car.phase[0]=5;
    nm_player_road_progress(&progress_road,&cars[0].car);
    assert(cars[0].car.rank_key==104 && cars[0].car.rank_key_high==0x1234);
    const uint32_t ranks[9]={48,49,50,51,52,53,54,55,56};
    for(unsigned predicate=0;predicate<9;++predicate) {
        nm_race_announcement_row_t row={0,0x12345678,predicate};
        nm_race_announcement_input_t in={.flags=0x100000,.speed=260,.rpm=7800,
            .condition=-1,.passing=-1,.cars=1};
        if(predicate==2) in.speed=100;
        nm_race_announcement_state_t s={0,27,0xaabbccdd};
        assert(nm_race_announcement_step(&row,1,ranks,&in,&s));
        assert(s.cursor==1 && s.repeats==27 && s.pending==(predicate==8 ? 48u : 0x12345678u));
        if(!predicate) continue;
        switch(predicate) {
            case 1: in.speed=259;break;
            case 2: in.speed=101;break;
            case 3: in.condition=0;break;
            case 4: in.next_next_progress=3;break;
            case 5: in.next_progress=2;break;
            case 6: in.rpm=7799;break;
            case 7: in.rank=1;break;
            case 8: in.rank=9;break;
        }
        s=(nm_race_announcement_state_t){0,27,0xaabbccdd};
        assert(nm_race_announcement_step(&row,1,ranks,&in,&s));
        assert(s.cursor==1 && s.repeats==27 && s.pending==0xaabbccdd);
    }
    nm_race_announcement_row_t row={0,0x12345678,4};
    nm_race_announcement_input_t in={.flags=0x400000,.progress=0x80000000};
    nm_race_announcement_state_t s={0,27,0xaabbccdd};
    assert(nm_race_announcement_step(&row,1,ranks,&in,&s) && s.pending==0x12345678);
    s=(nm_race_announcement_state_t){0,27,0xaabbccdd};in.progress=0x8000;
    assert(nm_race_announcement_step(&row,1,ranks,&in,&s) && !s.cursor && s.pending==0xaabbccdd);
    row.predicate=8;in.progress=0;in.rank=-1;
    const nm_race_announcement_state_t before=s;
    assert(!nm_race_announcement_step(&row,1,ranks,&in,&s) && !memcmp(&s,&before,sizeof(s)));
    row.predicate=9;
    assert(!nm_race_announcement_step(&row,1,ranks,&in,&s) && !memcmp(&s,&before,sizeof(s)));
    in.gate=1;s.cursor=UINT32_MAX;
    assert(nm_race_announcement_step(&row,1,ranks,&in,&s) && !s.cursor && !s.repeats && s.pending==0xaabbccdd);
    in.gate=0;in.flags=0;s.cursor=UINT32_MAX;s.repeats=27;
    assert(nm_race_announcement_step(&row,1,ranks,&in,&s) && !s.cursor && !s.repeats && s.pending==0xaabbccdd);
}

static void check_race_signals(nm_drive_params_t *p) {
    check_announcements();
    assert(nm_race_intents_test());
    {
    nm_race_car_t player={0};
    nm_race_intents_t state={{{-1,0,0},{-1,0,0}},0};
    const nm_race_intent_row_t rows[2]={{{0,0},0,0,0},{{111,222},5,4,0}};
    const uint32_t special[2]={333,444};
    nm_race_intent_output_t output;
    player.car.boundary.pivot_signal=player.car.boundary.damage.signal=1;
    player.car.lap.completed=255;player.car.lap.lap=0;
    assert(nm_race_consume_intents(&player,&state,rows,2,1,special,256,&output));
    assert(!player.car.boundary.pivot_signal && !player.car.boundary.damage.signal &&
        !state.lane[0].active && state.lane[1].active==1 && state.lane[1].timer==5 && !output.count);
    assert(nm_race_consume_intents(&player,&state,rows,2,1,special,256,&output));
    assert(output.count==1 && output.command[0]==222 && state.lane[1].timer==4 && !state.lane[1].active);
    state.lane[1].timer=0;player.car.boundary.damage.signal=-1;
    const nm_race_car_t saved_player=player;const nm_race_intents_t saved_state=state;
    memset(&output,0x5a,sizeof(output));const nm_race_intent_output_t saved_output=output;
    assert(!nm_race_consume_intents(&player,&state,rows,2,1,special,256,&output));
    assert(!memcmp(&player,&saved_player,sizeof(player)) && !memcmp(&state,&saved_state,sizeof(state)) &&
        !memcmp(&output,&saved_output,sizeof(output)));
    nm_race_announcement_row_t create={0,0x00010001,0};
    const uint32_t ranks[9]={0};
    nm_race_announcement_input_t input={.flags=0x100000};
    uint32_t cursor=0;
    state.repeats=27;
    assert(nm_race_create_intent(&player,&state,&cursor,&create,1,ranks,&input));
    assert(cursor==1 && state.repeats==27 && player.car.boundary.pivot_signal==1 &&
        player.car.boundary.damage.signal==1);
    /* The next consumption must see both halves written by creation. */
    player.car.lap.completed=0;state.lane[0].timer=state.lane[1].timer=0;
    assert(nm_race_consume_intents(&player,&state,rows,2,0,special,256,&output));
    assert(!output.count && state.lane[0].active==1 && state.lane[1].active==1);
    assert(nm_race_consume_intents(&player,&state,rows,2,0,special,256,&output));
    assert(output.count==2 && output.command[0]==111 && output.command[1]==111);
    input.gate=1;
    assert(nm_race_create_intent(&player,&state,&cursor,&create,1,ranks,&input));
    assert(!cursor && !state.repeats);
    input.gate=0;create.predicate=9;
    const nm_race_car_t before_player=player;const nm_race_intents_t before_state=state;
    assert(!nm_race_create_intent(&player,&state,&cursor,&create,1,ranks,&input));
    assert(!cursor && !memcmp(&player,&before_player,sizeof(player)) &&
        !memcmp(&state,&before_state,sizeof(state)));
    memset(&player,0,sizeof(player));memset(&state,0,sizeof(state));
    create.predicate=0;input.flags=0x100000;input.inhibit=1;cursor=0;
    assert(nm_race_event_tail(&player,&state,&cursor,rows,2,0,special,256,&create,1,ranks,&input,&output));
    assert(!output.count && cursor==1 && player.car.boundary.pivot_signal==1 &&
        player.car.boundary.damage.signal==1 && !state.lane[0].active && !state.lane[1].active);
    /* Disable creation on the next frame: consumption must still run. */
    input.gate=1;
    assert(nm_race_event_tail(&player,&state,&cursor,rows,2,0,special,256,&create,1,ranks,&input,&output));
    assert(!cursor && !player.car.boundary.pivot_signal && !player.car.boundary.damage.signal &&
        state.lane[0].active==1 && state.lane[1].active==1);
    input.flags=0x10100000;
    const nm_race_car_t attract_player=player;const nm_race_intents_t attract_state=state;
    output.count=4;
    assert(nm_race_event_tail(&player,&state,&cursor,NULL,0,0,NULL,0,NULL,0,NULL,&input,&output));
    assert(!output.count && !cursor && !memcmp(&player,&attract_player,sizeof(player)) &&
        !memcmp(&state,&attract_state,sizeof(state)));
    }
    _Static_assert(sizeof(nm_race_intent_row_t)==16 && sizeof(nm_race_intents_t)==16,"bounded intent rows/state");
    _Static_assert(sizeof(nm_race_timers_t) == 16 && offsetof(nm_race_timers_t,short_ticks) == 12,"race timer digest layout");
    _Static_assert(sizeof(nm_traffic_pass_body_t) == 20 && sizeof(nm_traffic_pass_t) == 6,"bounded pass view/state");
    assert(nm_race_timers_test_hash() == NM_RACE_TIMERS_HASH);
    assert(!nm_race_timers_step(NULL));
    nm_traffic_pass_body_t body[3] = {0}; body[0].next = 1; body[0].previous = 2;
    nm_traffic_pass_t state = {UINT16_MAX,UINT16_MAX,99}; body[0].position[0] = NAN;
    assert(nm_traffic_pass(p,body,3,0,&state) && state.next == 1 && state.previous == 2 && state.signal == 99);
    state.next = 2; const nm_traffic_pass_t before = state;
    assert(!nm_traffic_pass(p,body,3,0,&state) && !memcmp(&before,&state,sizeof(state)));
    body[0].position[0] = 0; body[2].progress = 0x80000000u;
    state.previous = 1; // Both identities match: first branch wins; wrapped INT_MIN <=40.
    assert(nm_traffic_pass(p,body,3,0,&state) && state.signal == 4);
    state.next = UINT16_MAX; state.previous = 1; body[1].progress = 81;
    assert(nm_traffic_pass(p,body,3,0,&state) && state.signal == 11);
    body[0].next = 3; const nm_traffic_pass_t saved = state;
    assert(!nm_traffic_pass(p,body,3,0,&state) && !memcmp(&state,&saved,sizeof(state)));
    assert(!nm_traffic_pass(NULL,body,3,0,&state) && !nm_traffic_pass(p,NULL,3,0,&state));
    assert(!nm_traffic_pass(p,body,0,0,&state) && !nm_traffic_pass(p,body,3,3,&state));
    assert(!nm_traffic_pass(p,body,3,0,NULL));
    puts("PASS: signed race timers, pass precedence, wrapped gap and atomic validation");
}

static void check_traffic_near(nm_drive_params_t *p) {
    _Static_assert(sizeof(nm_traffic_near_body_t) == 28,"bounded traffic proximity view");
    nm_traffic_near_body_t body[21] = {0}; uint8_t updates = 99;
    for (unsigned i = 0; i < 21; ++i) {
        body[i].next = (uint16_t)((i+1)%21); body[i].previous = (uint16_t)((i+20)%21);
        body[i].flags = 0x80406400; body[i].marker146 = 0x1234abcd; body[i].hold = 77;
    }
    body[19].progress = 1000; body[20].progress = 872; body[0].progress = 871;
    uint32_t ahead[2] = {872,871};
    body[8].position[0] = NAN; // Unvisited positions are irrelevant.
    assert(nm_traffic_near(p,body,21,20,20,19,ahead,&updates) && !updates);
    assert(body[0].flags == 0x80404400 && body[19].flags == 0x80402400 && body[18].flags == 0x80402400);
    assert(body[17].flags == 0x80400400 && body[8].flags == 0x80400000 && body[20].flags == 0x80406400);
    assert(body[0].marker146 == 8 && body[7].marker146 == 1 && body[19].marker146 == 8 && body[12].marker146 == 1);
    assert(body[8].marker146 == 0x12340000 && body[20].marker146 == 0x1234abcd);
    assert(body[19].hold == 4 && body[20].hold == 77); // Subject, not player.
    nm_traffic_near_body_t alias = {0}; alias.flags = UINT32_MAX; alias.marker146 = UINT32_MAX; alias.hold = 99;
    const uint32_t zero[2] = {0,0};
    assert(nm_traffic_near(p,&alias,1,1,0,0,zero,&updates) && alias.flags == UINT32_MAX && alias.marker146 == 1 && alias.hold == 99);
    body[19].hold = 77; body[0].progress = 872;
    ahead[1] = 872;
    assert(nm_traffic_near(p,body,21,20,20,19,ahead,&updates) && body[19].hold == 77); // Strict >128, not >=.
    for (unsigned bad = 0; bad < 5; ++bad) {
        nm_traffic_near_body_t trial[21]; memcpy(trial,body,sizeof(body));
        unsigned reset = 20, player = 20, subject = 19; updates = 99;
        if (bad == 0) reset = 22;
        if (bad == 1) player = 21;
        if (bad == 2) subject = 21;
        if (bad == 3) trial[20].next = 21;
        if (bad == 4) trial[7].position[0] = NAN;
        nm_traffic_near_body_t before[21]; memcpy(before,trial,sizeof(trial));
        assert(!nm_traffic_near(p,trial,21,reset,player,subject,ahead,&updates));
        assert(!memcmp(before,trial,sizeof(trial)) && updates == 99);
    }
    assert(!nm_traffic_near(NULL,body,21,20,20,19,ahead,&updates));
    assert(!nm_traffic_near(p,body,21,0,20,19,ahead,&updates));
    assert(!nm_traffic_near(p,body,21,20,20,19,ahead,NULL));
    assert(!nm_traffic_near(p,body,21,20,20,19,NULL,&updates));
    // A caller outside the car ring consumes explicit immutable progress,
    // never dereferences its absent car links. Its hold still changes.
    nm_traffic_near_body_t worker[2] = {{0},{0}};
    worker[1].next = worker[1].previous = UINT16_MAX; worker[1].progress = 129;
    assert(nm_traffic_near(p,worker,2,1,0,1,zero,&updates) && worker[1].hold == 4);
    puts("PASS: traffic proximity reset widths, ordered neighbor marks/flags, caller subject and atomic invalid failure");
}

static void check_traffic_order(void) {
    _Static_assert(sizeof(nm_traffic_order_body_t) == 10,"bounded traffic ordering view");
    nm_traffic_order_body_t body[44] = {0}; uint16_t ranks[44], track[44];
    for (unsigned count = 0; count <= 45; ++count) {
        for (unsigned i = 0; i < 44; ++i) { ranks[i] = 91; track[i] = 92; }
        const int valid = count > 0 && count <= 44;
        assert(nm_traffic_order_init(ranks,track,count) == valid);
        for (unsigned i = 0; i < 44; ++i) {
            assert(ranks[i] == (valid && i < count ? count-1-i : 91));
            assert(track[i] == (valid && i < count ? count-1-i : 92));
        }
    }
    assert(!nm_traffic_order_init(NULL,track,20));
    assert(!nm_traffic_order_init(ranks,NULL,20));
    for (unsigned i = 0; i < 44; ++i) {
        body[i].rank_key = (int16_t)i; body[i].profile = (uint16_t)i;
        body[i].next = body[i].previous = 43; body[i].rank = 99;
        ranks[i] = track[i] = (uint16_t)i;
    }
    int8_t lap = 0;
    assert(nm_traffic_order(body,44,ranks,track,5,5,0x100000,&lap));
    const uint16_t one_pass[] = {1,2,3,4,0};
    assert(!memcmp(ranks,one_pass,sizeof(one_pass)) && !memcmp(track,one_pass,sizeof(one_pass)));
    assert(body[1].next == 0 && body[1].previous == 2 && body[0].next == 4 && body[0].previous == 1);
    for (unsigned i = 0; i < 5; ++i) assert(body[one_pass[i]].rank == i);
    for (unsigned i = 5; i < 44; ++i) assert(ranks[i] == i && track[i] == i && body[i].next == 43 && body[i].rank == 99);
    body[1].lap = body[2].lap = 1; body[3].lap = 0;
    memset(ranks,0xff,sizeof(ranks)); // Disabled rank array is not consumed.
    assert(nm_traffic_order(body,44,ranks,track,5,4,0,&lap));
    const uint16_t rotated[] = {3,4,0,1,2};
    assert(!memcmp(track,rotated,sizeof(rotated)) && ranks[0] == UINT16_MAX && lap == 0);
    assert(body[3].next == 1 && body[3].previous == 4 && body[2].next == 1); // Unselected links retained.
    for (unsigned bad = 0; bad < 7; ++bad) {
        unsigned count = 44, sort = 5, ring = 4; uint32_t flags = 0;
        if (bad == 0) count = 0;
        if (bad == 1) sort = 45;
        if (bad == 2) ring = 2;
        if (bad == 3) ring = 6;
        if (bad == 4) flags = 0x400000;
        if (bad == 5) track[0] = 44;
        if (bad == 6) {
            sort = ring = 3; lap = -1;
            for (unsigned i = 0; i < 3; ++i) { track[i] = (uint16_t)i; body[i].lap = 0; body[i].profile = 0; }
        }
        nm_traffic_order_body_t before[44]; uint16_t old_track[44], old_ranks[44];
        memcpy(before,body,sizeof(body)); memcpy(old_track,track,sizeof(track)); memcpy(old_ranks,ranks,sizeof(ranks));
        const int8_t old_lap = lap;
        assert(!nm_traffic_order(body,count,ranks,track,sort,ring,flags,&lap));
        assert(!memcmp(before,body,sizeof(body)) && !memcmp(old_track,track,sizeof(track)) && !memcmp(old_ranks,ranks,sizeof(ranks)) && lap == old_lap);
        track[0] = 3;
    }
    const uint32_t hash = nm_traffic_order_test_hash(); assert(hash == NM_TRAFFIC_ORDER_HASH);
    printf("PASS: traffic single-pass ordering, wrap groups, ring direction, disabled rank and atomic failure; cases=%u hash=%08x\n",NM_TRAFFIC_ORDER_CASES,(unsigned)hash);
}

static void check_collision_preparation(void) {
    _Static_assert(sizeof(nm_collision_rank_t) == 44,"bounded collision preparation view");
    nm_collision_rank_t body[64]; unsigned count, reset, primary; uint32_t wrap; uint16_t order[14];
    nm_collision_prepare_test_input(12,body,&count,&reset,&primary,&wrap);
    assert(count == 40 && reset == 39 && primary == 39);
    for (unsigned j = 0; j < count; ++j) body[j].profile = 20;
    nm_collision_rank_t before[64]; memcpy(before,body,sizeof(body));
    assert(nm_collision_prepare(body,count,reset,primary,wrap,order));
    for (unsigned i = 0; i < 14; ++i) {
        assert(order[i] == (primary+count-5+i)%count);
        assert(body[order[i]].relative == i-5u && body[order[i]].adjusted == 20 && body[order[i]].candidates == 13-i);
    }
    assert(body[primary].flags == before[primary].flags && body[primary].contact_side == 255);
    assert(body[20].relative == ((before[20].relative&0xffffff00u)|127u));
    assert(body[20].candidates == before[20].candidates && !memcmp(&body[20].motion,&before[20].motion,sizeof(body[20].motion)));
    for (unsigned failure = 0; failure < 5; ++failure) {
        memcpy(body,before,sizeof(body)); memset(order,0xa5,sizeof(order));
        if (failure == 0) body[primary].previous = UINT16_MAX;
        if (failure == 1) body[6].next = UINT16_MAX; // Late consumed forward link.
        if (failure == 2) body[7].position[1] = NAN;
        if (failure == 3) { body[7].position[1] = FLT_MAX; body[7].motion.previous[1] = -FLT_MAX; }
        if (failure == 4) body[6].previous = UINT16_MAX; // Backward walk only.
        nm_collision_rank_t saved[64]; memcpy(saved,body,sizeof(body));
        assert(!nm_collision_prepare(body,count,reset,primary,wrap,order));
        assert(!memcmp(body,saved,sizeof(body)) && order[0] == 0xa5a5 && order[13] == 0xa5a5);
    }
    memcpy(body,before,sizeof(body));
    body[7].next = body[34].previous = UINT16_MAX; body[20].position[0] = NAN;
    assert(nm_collision_prepare(body,count,reset,primary,wrap,order)); // Unused final links/positions need not be valid.
    assert(!nm_collision_prepare(NULL,count,reset,primary,wrap,order));
    assert(!nm_collision_prepare(body,count,reset,primary,wrap,NULL));
    assert(!nm_collision_prepare(body,0,reset,primary,wrap,order));
    assert(!nm_collision_prepare(body,65536,reset,primary,wrap,order));
    assert(!nm_collision_prepare(body,count,0,primary,wrap,order));
    assert(!nm_collision_prepare(body,count,count+1,primary,wrap,order));
    assert(!nm_collision_prepare(body,count,reset,count,wrap,order));
    const uint32_t hash = nm_collision_prepare_test_hash();
    assert(hash == NM_COLLISION_PREPARE_HASH);
    printf("PASS: collision preparation cases=%u hash=%08x, linked order, outside-primary and atomic invalid failure\n",
        NM_COLLISION_PREPARE_CASES,(unsigned)hash);
}

static void check_collision_damage(nm_drive_params_t *p) {
    const uint32_t old_inverse = p->inverse_sqrt[1]; const float old_scale = p->speed_index_scale;
    // Analytic distance(1,0)=1 with this ROM-free inverse-square-root entry.
    p->inverse_sqrt[1] = 0x3f800000;
    for (unsigned selector = 0; selector < 4; ++selector) {
        for (unsigned t = 0; t < 3; ++t) p->collision_threshold[t][selector] = (uint16_t)(10*(t+1));
        p->collision_flags[0][selector] = 1u<<(22+selector);
        p->collision_flags[1][selector] = 1u<<(26+selector);
    }
    for (unsigned selector = 0; selector < 4; ++selector) for (unsigned inhibit = 0; inhibit < 2; ++inhibit)
        for (unsigned value = 0; value <= 40; ++value) {
            p->speed_index_scale = (float)value;
            nm_collision_damage_input_t in = {.player_mode=(uint8_t)(selector>>1)};
            nm_collision_damage_body_t a = {.delta={1,0},.input_inhibit=inhibit ? -128 : 0};
            nm_collision_damage_body_t b = {.entity_id=(uint8_t)(selector&1),.contact_side=3};
            b.damage.side = 2; b.damage.sequence_counter = 255; b.damage.crash_count = 255;
            int32_t strength = -1;
            assert(nm_collision_damage(p,&in,&a,&b,&strength) && strength == (int32_t)value);
            const int directional = value >= 10 && value < 30 && (value > 20 || inhibit);
            assert(a.damage.flags == (value >= 30 ? 64u : directional ? 1u<<24 : 0));
            assert((b.damage.flags & ~(1u<<10)) == (value >= 30 && b.entity_id ? 64u : directional ? 1u<<28 : 0));
            assert(a.damage.override_count == (value > 20 ? 32 : 0));
            assert(b.damage.shake_ticks == (b.entity_id ? 0 : value > 20 ? 30 : directional ? 16 : 0));
            assert(b.damage.remaining == (!b.entity_id && value >= 30 ? 256 : 0));
            if (!b.entity_id && value >= 30) {
                assert(b.damage.row == 1 && b.damage.inhibit_ticks == 128 && b.damage.initial_inhibit == 129);
                assert(b.damage.sequence_counter == 0 && b.damage.crash_count == 0 && b.damage.orientation == 3);
                assert(b.damage.auxiliary_request && b.damage.sound_requests == NM_CRASH_SOUND_START);
            }
        }
    p->speed_index_scale = 31;
    nm_collision_damage_input_t in = {0};
    for (unsigned gate = 0; gate < 4; ++gate) {
        nm_collision_damage_body_t a = {.delta={1,0}}, b = {.contact_side=3};
        a.damage.flags = 1u<<26;
        if (gate == 0) b.contact_side = 255;
        if (gate == 1) a.damage.inhibit_ticks = -1;
        if (gate == 2) in.control = UINT32_MAX;
        if (gate == 3) b.damage.inhibit_ticks = -1;
        int32_t strength;
        assert(nm_collision_damage(p,&in,&a,&b,&strength) && strength == 31 && !b.damage.remaining);
        assert(a.damage.flags == (gate == 3 ? 64u : 0));
        in.control = 0;
    }
    nm_collision_damage_body_t a = {.delta={1,0}}, b = {.contact_side=3};
    in.curve = 2; int32_t strength = 123;
    const nm_collision_damage_body_t old_a = a, old_b = b;
    assert(!nm_collision_damage(p,&in,&a,&b,&strength)); // Late crash validation rolls back first-body flags too.
    assert(!memcmp(&a,&old_a,sizeof(a)) && !memcmp(&b,&old_b,sizeof(b)) && strength == 123);
    in.curve = 0; p->speed_index_scale = FLT_MAX; /* Severity cvtri overflow: masked, saturates (PRM 11-50). */
    assert(nm_collision_damage(p,&in,&a,&b,&strength) && strength == INT32_MAX);
    a = old_a; b = old_b; strength = 123;
    assert(!nm_collision_damage(NULL,&in,&a,&b,&strength) && !nm_collision_damage(p,NULL,&a,&b,&strength));
    assert(!nm_collision_damage(p,&in,NULL,&b,&strength) && !nm_collision_damage(p,&in,&a,NULL,&strength));
    assert(!nm_collision_damage(p,&in,&a,&b,NULL));
    p->speed_index_scale = 1; a.delta[0] = NAN;
    assert(nm_collision_damage(p,&in,&a,&b,&strength) && strength == INT32_MIN);
    a = (nm_collision_damage_body_t){.input_inhibit=-1,.entity_id=1};
    a.damage.flags = 2u|(1u<<26); a.damage.side = 2;
    p->collision_threshold[0][1] = 0;
    assert(nm_collision_damage(p,&in,&a,&a,&strength) && strength == 0);
    assert(a.damage.flags == ((1u<<24)|(1u<<28)));
    nm_damage_state_t damage = {0}, before = damage;
    assert(!nm_crash_begin(NULL,0,0,0,&damage) && !nm_crash_begin(p,0,0,0,NULL));
    assert(!nm_crash_begin(p,2,0,0,&damage) && !memcmp(&damage,&before,sizeof(damage)));
    p->inverse_sqrt[1] = old_inverse; p->speed_index_scale = old_scale;
    memset(p->collision_threshold,0,sizeof(p->collision_threshold));
    memset(p->collision_flags,0,sizeof(p->collision_flags));
    puts("PASS: collision damage thresholds/gates, shared crash entry, alias ordering and atomic failure");
}

static void check_collision_route(nm_drive_params_t *p) {
    _Static_assert(sizeof(nm_collision_travel_t) == 16 && sizeof(nm_collision_route_input_t) == 28 &&
                   sizeof(nm_collision_route_t) == 4,"bounded travel and dispatch state");
    nm_collision_travel_t travel = {.previous={10,-10}};
    float position[2] = {12,-14};
    assert(nm_collision_track(position,&travel) && travel.travel[0] == 2 && travel.travel[1] == -4);
    assert(travel.previous[0] == 12 && travel.previous[1] == -14);
    assert(nm_collision_track(travel.previous,&travel) && travel.travel[0] == 0 && travel.travel[1] == 0);
    position[0] = -0.0f; position[1] = 0; travel.previous[0] = 0;
    assert(nm_collision_track(position,&travel) && nm_force_bits(travel.travel[0]) == 0x80000000u);
    position[0] = 5; position[1] = FLT_MAX; travel.previous[1] = -FLT_MAX;
    const nm_collision_travel_t before = travel;
    assert(!nm_collision_track(position,&travel) && !memcmp(&travel,&before,sizeof(travel)));
    position[1] = NAN; assert(!nm_collision_track(position,&travel));
    assert(!nm_collision_track(NULL,&travel) && !nm_collision_track(position,NULL));
    nm_collision_route_input_t in = {0}; nm_collision_route_t out = {0};
    const uint32_t flags[] = {0,1,2,4,0x80000000u,UINT32_MAX};
    for (unsigned angle = 0; angle < 65536; ++angle) for (unsigned i = 0; i < 6; ++i) {
        in.flags = flags[i]; in.heading[0] = nm_yaw_angle(angle+32767); in.heading[1] = 32767;
        const int difference = nm_yaw_angle(angle), skip = !in.flags || (difference >= -64 && difference <= 64);
        assert(nm_collision_route(p,&in,NULL,NULL,&out));
        assert(out.branch == (skip ? NM_COLLISION_NONE : in.flags == 2 ? NM_COLLISION_SECOND : NM_COLLISION_FIRST));
        assert(out.caller_result == (skip ? 150 : 300));
    }
    in.flags = 3; in.heading[0] = in.heading[1] = 0;
    nm_collision_choice_body_t body = {0}; unsigned chosen;
    assert(nm_collision_choose(p,&in.contact,&body,&body,&chosen));
    assert(nm_collision_route(p,&in,&body,&body,&out) && out.branch == chosen && out.caller_result == 300);
    const nm_collision_route_t old = out; in.contact.corner[0] = UINT32_MAX;
    assert(!nm_collision_route(p,&in,&body,&body,&out) && !memcmp(&out,&old,sizeof(out)));
    assert(!nm_collision_route(NULL,&in,&body,&body,&out) && !nm_collision_route(p,NULL,&body,&body,&out));
    assert(!nm_collision_route(p,&in,NULL,&body,&out) && !nm_collision_route(p,&in,&body,NULL,&out));
    assert(!nm_collision_route(p,&in,&body,&body,NULL));
    puts("PASS: collision travel history/alias/atomic failure and all wrapped heading dispatch boundaries");
}

static void check_collision_candidate(nm_drive_params_t *p) {
    _Static_assert(sizeof(nm_collision_probe_t) == 12,"bounded collision broad-phase view");
    const uint32_t old[2] = {p->inverse_sqrt[0],p->inverse_sqrt[1]};
    // Refined inverse=2-1, with matching exponent adjustments for lengths1/8.
    p->inverse_sqrt[0] = 0x3f800000; p->inverse_sqrt[1] = 0x40000000;
    nm_collision_probe_t a = {{1,0},0}, b = {{0,0},0}; unsigned hit = 99;
    assert(nm_collision_candidate(p,&a,&b,&hit) && hit);
    const float limit = nm_force_float(0x3fd9999a);
    for (unsigned sign = 0; sign < 2; ++sign) for (unsigned step = 0; step < 3; ++step) {
        const float value = step == 0 ? nextafterf(limit,0) : step == 1 ? limit : nextafterf(limit,INFINITY);
        a.word5c = nm_force_bits(value) | (sign<<31);
        assert(nm_collision_candidate(p,&a,&b,&hit) && hit == (step < 2));
    }
    const uint32_t exceptional[] = {0x7f800000,0xff800000,0x7fc12345,0xffc12345};
    for (unsigned i = 0; i < 4; ++i) {
        a.word5c = b.word5c = exceptional[i];
        assert(nm_collision_candidate(p,&a,&b,&hit) && !hit); // NaN bits fail the source unsigned test.
    }
    a.position[0] = 8;
    assert(nm_collision_candidate(p,&a,&b,&hit) && !hit); // Unused5c NaNs do not reject a distance miss.
    a.word5c = 0;
    assert(nm_collision_candidate(p,&a,&a,&hit) && hit);
    a.position[0] = 0; b = a;
    const nm_collision_probe_t circle_snapshot = b;
    b.position[0] = 8; // Prior response moved the centre body, not command40's snapshot.
    assert(nm_collision_candidate(p,&a,&circle_snapshot,&hit) && hit);
    assert(nm_collision_candidate(p,&a,&b,&hit) && !hit);
    hit = 99; a.position[0] = FLT_MAX; b.position[0] = -FLT_MAX;
    assert(!nm_collision_candidate(p,&a,&b,&hit) && hit == 99);
    a.position[0] = NAN;
    assert(!nm_collision_candidate(p,&a,&b,&hit) && hit == 99);
    assert(!nm_collision_candidate(NULL,&a,&b,&hit) && !nm_collision_candidate(p,NULL,&b,&hit));
    assert(!nm_collision_candidate(p,&a,NULL,&hit) && !nm_collision_candidate(p,&a,&b,NULL));
    memcpy(p->inverse_sqrt,old,sizeof(old));
    puts("PASS: collision broad-phase distance, inclusive raw-word limit, NaN/alias and atomic failure");
}

static void check_collision_walk(nm_drive_params_t *p) {
    _Static_assert(sizeof(nm_collision_walk_t) == 24 && sizeof(nm_collision_walk_body_t) == 16,"bounded walk state");
    uint8_t saved[4][14]; memcpy(saved,p->collision_schedule,sizeof(saved));
    for (unsigned row = 0; row < 4; ++row) for (unsigned i = 0; i < 14; ++i) p->collision_schedule[row][i] = (uint8_t)i;
    nm_collision_walk_body_t body[14] = {0}; uint16_t order[14], pair[2] = {99,98};
    for (unsigned i = 0; i < 14; ++i) { body[i].next = (uint16_t)((i+1)%14); order[i] = (uint16_t)i; }
    body[13].probe.position[0]=99; /* Last empty group is NOT the last circle owner. */
    nm_collision_walk_t s = {.centre={3,7},.circle_radius=8,.budget=1000};
    assert(!nm_collision_walk_next(p,body,14,order,1,0,0,&s,pair) && s.budget == 1000 && pair[0] == 99);
    assert(s.centre[0]==3 && s.centre[1]==7 && s.circle_radius==8);
    body[0].candidates = 3; s = (nm_collision_walk_t){.budget=1000};
    assert(nm_collision_walk_next(p,body,14,order,1,0,0,&s,pair) == 1);
    assert(pair[0] == 0 && pair[1] == 1 && s.budget == 995 && s.remaining == 2);
    body[0].probe.position[0] = NAN; // Centre was cached; the current position is not read again.
    assert(nm_collision_walk_next(p,body,14,order,1,0,150,&s,pair) == 1);
    assert(pair[0] == 2 && pair[1] == 0 && s.budget == 835 && s.remaining == 1);
    body[0].probe.word5c = nm_force_bits(2); // Raw word is live; third candidate now misses.
    assert(!nm_collision_walk_next(p,body,14,order,1,0,300,&s,pair) && s.budget == 515);
    assert(!s.centre[0] && !s.centre[1] && nm_force_bits(s.circle_radius)==0x40a54413u);
    const nm_collision_walk_t finished = s;
    assert(!nm_collision_walk_next(p,body,14,order,1,0,0,&s,pair) && !memcmp(&s,&finished,sizeof(s)));
    body[0].probe = (nm_collision_probe_t){{0,0},0}; body[0].candidates = 1; body[1].candidates = 1;
    s = (nm_collision_walk_t){.budget=10};
    assert(nm_collision_walk_next(p,body,14,order,1,0,0,&s,pair) == 1 && s.budget == 5);
    body[1].probe.position[0]=9;body[1].probe.position[1]=11;
    assert(!nm_collision_walk_next(p,body,14,order,1,0,300,&s,pair) && s.budget == -310);
    assert(s.centre[0]==9 && s.centre[1]==11 && nm_force_bits(s.circle_radius)==0x40a54413u);
    body[1].probe.position[0]=body[1].probe.position[1]=0;
    // Even an exhausted last inner group pays the next nonempty group's five units.
    s = (nm_collision_walk_t){.budget=5};
    assert(!nm_collision_walk_next(p,body,14,order,1,0,0,&s,pair) && s.budget == 0);
    assert(nm_force_bits(s.circle_radius)==0x40a54413u);
    for (unsigned bad = 0; bad < 15; ++bad) {
        s = (nm_collision_walk_t){.budget=1000}; body[0].next = 1; body[1].probe.position[0] = 0;
        p->collision_schedule[0][0] = 0; order[0] = 0;
        unsigned phase = 0, cost = 0;
        if (bad == 0) s.outer = 15;
        if (bad == 1) s.budget = 1;
        if (bad == 2) s.pending = 2;
        if (bad == 3) { s.pending = 1; s.outer = 1; s.budget = 0; cost = 150; }
        if (bad == 4) cost = 150;
        if (bad == 5) phase = 4;
        if (bad == 6) body[0].next = 14;
        if (bad == 7) body[1].probe.position[0] = NAN;
        if (bad == 8) p->collision_schedule[0][0] = 14;
        if (bad == 9) order[0] = 14;
        if (bad == 10) { s.pending = 1; s.outer = 1; }
        if (bad == 11) { s.pending = 1; s.outer = 1; cost = 149; }
        if (bad == 12) s.budget = -315;
        if (bad == 13) s.budget = 1005;
        if (bad == 14) { s.pending = 1; cost = 150; }
        const nm_collision_walk_t before = s; const uint16_t old_pair[2] = {pair[0],pair[1]};
        assert(nm_collision_walk_next(p,body,14,order,1,phase,cost,&s,pair) == -1);
        assert(!memcmp(&s,&before,sizeof(s)) && !memcmp(pair,old_pair,sizeof(pair)));
    }
    assert(nm_collision_walk_next(NULL,body,14,order,1,0,0,&s,pair) == -1);
    assert(nm_collision_walk_next(p,body,0,order,1,0,0,&s,pair) == -1);
    memcpy(p->collision_schedule,saved,sizeof(saved));
    puts("PASS: collision walk primary swap, cached centre/live word, work charges, completion and atomic invalid failure");
}

static void check_collision_choice(nm_drive_params_t *p) {
    _Static_assert(sizeof(nm_collision_choice_body_t) == 56 && sizeof(nm_collision_choice_input_t) == 20,
                   "bounded collision choice inputs");
    const uint32_t saved_inverse = p->inverse_sqrt[1];
    uint8_t saved_corners[4]; memcpy(saved_corners,p->collision_corner,4);
    p->inverse_sqrt[1] = 0x3f800000;
    for (unsigned i = 0; i < 4; ++i) p->collision_corner[i] = (uint8_t)i;
    nm_collision_choice_body_t a = {0}, b = {.travel={1,0}};
    for (unsigned i = 0; i < 4; ++i) a.world[i][2] = 1;
    nm_collision_choice_input_t in = {.depth={1,-1}};
    unsigned branch = 77;
    for (unsigned angle = 0; angle < 65536; ++angle) {
        in.angle = 0xa5a50000u|angle; in.corner[0] = angle&3; in.corner[1] = (angle>>2)&3;
        assert(nm_collision_choose(p,&in,&a,&b,&branch) && branch == ((angle>>15)^1));
    }
    in.corner[0] = UINT32_MAX; in.corner[1] = UINT32_MAX;
    in.depth[0] = -2; assert(nm_collision_choose(p,&in,&a,&b,&branch) && branch == 1);
    in.depth[1] = 2; assert(nm_collision_choose(p,&in,&a,&b,&branch) && branch == 0); // Equal-depth early tie favours first.
    in.depth[0] = NAN; assert(nm_collision_choose(p,&in,&a,&b,&branch) && branch == 1); // Depth uses bits only.
    in.depth[0] = in.depth[1] = 1; branch = 77;
    assert(!nm_collision_choose(p,&in,&a,&b,&branch) && branch == 77); // At distance equality corners ARE consumed.
    in.corner[0] = in.corner[1] = 0; p->collision_corner[0] = 4;
    assert(!nm_collision_choose(p,&in,&a,&b,&branch) && branch == 77);
    p->collision_corner[0] = 0; a.world[0][2] = INFINITY;
    assert(!nm_collision_choose(p,&in,&a,&b,&branch) && branch == 77);
    in.depth[0] = 2; assert(nm_collision_choose(p,&in,&a,&b,&branch) && branch == 1); // Unused nonfinite corner.
    a.travel[0] = -FLT_MAX; b.travel[0] = FLT_MAX; branch = 77;
    assert(!nm_collision_choose(p,&in,&a,&b,&branch) && branch == 77);
    assert(!nm_collision_choose(NULL,&in,&a,&b,&branch) && !nm_collision_choose(p,NULL,&a,&b,&branch));
    assert(!nm_collision_choose(p,&in,NULL,&b,&branch) && !nm_collision_choose(p,&in,&a,NULL,&branch));
    assert(!nm_collision_choose(p,&in,&a,&b,NULL));
    p->inverse_sqrt[1] = saved_inverse; memcpy(p->collision_corner,saved_corners,4);
    puts("PASS: collision choice all angle bits, strict distance/depth ties, unused inputs and atomic failure");
}

static void check_collision_impulse(nm_drive_params_t *p) {
    _Static_assert(sizeof(nm_collision_impulse_body_t) == 32 && sizeof(nm_collision_impulse_shared_t) == 4 &&
                   sizeof(nm_collision_impulse_input_t) == 12,"bounded collision impulse views");
    const float saved = p->collision_speed_step; p->collision_speed_step = 2;
    const int severity[] = {INT32_MIN,-1,0,24,25,29,30,INT32_MAX};
    for (unsigned side = 0; side < 256; ++side) for (unsigned k = 0; k < 8; ++k) {
        nm_collision_impulse_body_t a = {.speed=20,.delta={2,-4},.yaw=-32768};
        nm_collision_impulse_body_t b = {.speed=10,.delta={-2,4},.yaw=32767,.contact_side=(uint8_t)side};
        nm_collision_impulse_shared_t shared = {.signal=123};
        const nm_collision_impulse_input_t in = {.contact_flags=2,.severity=severity[k]};
        assert(nm_collision_impulse(p,&in,&a,&b,&shared));
        const int lateral = side == 1 || side == 3;
        const float transfer = lateral ? 8 : (uint32_t)severity[k] >= 30 ? 2 : 0;
        assert(a.speed == 20-transfer && b.speed == 10+transfer);
        assert(nm_force_bits(a.correction[0]) == nm_force_bits(lateral ? -4.0f : -0.0f));
        assert(nm_force_bits(b.correction[1]) == nm_force_bits(lateral ? -8.0f : 0.0f));
        assert(a.own_yaw == -32768 && b.other_yaw == -32768 && a.other_yaw == 32767 && b.own_yaw == 32767);
        assert(shared.signal == (severity[k] >= 25 ? 1 : 123));
        assert(shared.shake_ticks == (severity[k] >= 25 ? 8 : 0));
        assert(b.response_ticks == (severity[k] >= 25 ? 16 : 0));
    }
    nm_collision_impulse_input_t in = {.contact_flags=2,.severity=30};
    for (int inhibit = -128; inhibit < 128; ++inhibit) {
        nm_collision_impulse_body_t a = {.speed=20,.input_inhibit=(int8_t)inhibit}, b = {.speed=10};
        nm_collision_impulse_shared_t shared = {0};
        assert(nm_collision_impulse(p,&in,&a,&b,&shared));
        assert(a.speed == (inhibit < -48 || inhibit > 48 ? 20 : 18));
    }
    for (unsigned bit = 15; bit <= 16; ++bit) {
        nm_collision_impulse_body_t a = {.speed=NAN}, b = {.speed=NAN};
        const nm_collision_impulse_body_t old_a = a, old_b = b;
        nm_collision_impulse_shared_t shared = {0}; in.flags = 1u<<bit;
        assert(nm_collision_impulse(p,&in,&a,&b,&shared));
        assert(!memcmp(&a,&old_a,sizeof(a)) && !memcmp(&b,&old_b,sizeof(b)) && !shared.signal);
    }
    in.flags = 0;
    nm_collision_impulse_body_t a = {.speed=20}, b = {.speed=10,.contact_side=1,.inhibit_ticks=-1,.response_ticks=-1};
    nm_collision_impulse_shared_t shared = {.shake_ticks=-1};
    assert(nm_collision_impulse(p,&in,&a,&b,&shared) && a.speed == 14.5f && b.speed == 15.5f);
    assert(shared.shake_ticks == -1 && b.response_ticks == -1);
    a = (nm_collision_impulse_body_t){.speed=-20}; b = (nm_collision_impulse_body_t){.speed=-10};
    assert(nm_collision_impulse(p,&in,&a,&b,&shared) && a.speed == -22 && b.speed == -8); // BIT ordering, not float ordering.
    a = (nm_collision_impulse_body_t){.speed=10,.yaw=-42};
    assert(nm_collision_impulse(p,&in,&a,&a,&shared) && a.speed == 12 && nm_force_bits(a.correction[0]) == 0x80000000u);
    assert(a.own_yaw == -42 && a.other_yaw == -42);
    a = (nm_collision_impulse_body_t){.speed=FLT_MAX}; b = (nm_collision_impulse_body_t){.speed=-FLT_MAX,.contact_side=1};
    const nm_collision_impulse_body_t old_a = a, old_b = b;
    const nm_collision_impulse_shared_t old_shared = shared;
    assert(!nm_collision_impulse(p,&in,&a,&b,&shared));
    assert(!memcmp(&a,&old_a,sizeof(a)) && !memcmp(&b,&old_b,sizeof(b)) && !memcmp(&shared,&old_shared,sizeof(shared)));
    assert(!nm_collision_impulse(NULL,&in,&a,&b,&shared) && !nm_collision_impulse(p,NULL,&a,&b,&shared));
    assert(!nm_collision_impulse(p,&in,NULL,&b,&shared) && !nm_collision_impulse(p,&in,&a,NULL,&shared));
    assert(!nm_collision_impulse(p,&in,&a,&b,NULL));
    p->collision_speed_step = saved;
    puts("PASS: collision impulse signed/unsigned thresholds, bit ordering, side/gates, aliases and atomic failure");
}

static void check_collision(nm_drive_params_t *p) {
    const uint32_t saved = p->sine[16383]; p->sine[16383] = 0x3f800000;
    nm_collision_body_t a = {{2,4},77}, b = {{-2,-4},88};
    assert(nm_collision_separate(p,0,2,0x123456ff,&a,&b));
    assert(a.position[0] == 2 && a.position[1] == 5 && a.side == 1);
    assert(b.position[0] == -2 && b.position[1] == -5 && b.side == 255);
    assert(nm_collision_separate(p,16384,2,0,&a,&b));
    assert(a.position[0] == 1 && b.position[0] == -1 && a.side == 2 && b.side == 0);
    nm_collision_body_t same = {{1,2},0};
    assert(nm_collision_separate(p,0,2,1,&same,&same));
    assert(same.position[0] == 1 && same.position[1] == 2 && same.side == 3);
    // Reject atomically even when only the second body's last axis overflows.
    b.position[1] = -FLT_MAX;
    const nm_collision_body_t before_a = a, before_b = b;
    assert(!nm_collision_separate(p,0,FLT_MAX,0,&a,&b));
    assert(!memcmp(&a,&before_a,sizeof(a)) && !memcmp(&b,&before_b,sizeof(b)));
    assert(!nm_collision_separate(p,0,NAN,0,&a,&b));
    assert(!nm_collision_separate(NULL,0,0,0,&a,&b));
    assert(!nm_collision_separate(p,0,0,0,NULL,&b));
    assert(!nm_collision_separate(p,0,0,0,&a,NULL));
    nm_road_profile_t profiles[2] = {0};
    const nm_road_pack_t road = {.profiles=profiles,.profile_count=2};
    int16_t lateral = -1234, angle = 999;
    assert(nm_road_measure_lateral(p,&profiles[0],1,0,&lateral) && lateral == 939);
    assert(nm_road_measure_lateral(p,&profiles[0],-1,0,&lateral) && lateral == 1109);
    assert(nm_road_measure_lateral(p,&profiles[0],100,0,&lateral) && lateral == 0);
    assert(nm_road_measure_lateral(p,&profiles[0],-100,0,&lateral) && lateral == 1792);
    profiles[0].lanes[4].angles[1] = 4096;
    assert(nm_road_measure_lateral(p,&profiles[0],0,1,&lateral) && lateral == 939);
    profiles[0].lanes[4].angles[1] = 0;
    assert(!nm_road_measure_lateral(p,&profiles[0],FLT_MAX,0,&lateral) && lateral == 939);
    assert(!nm_road_measure_lateral(NULL,&profiles[0],0,0,&lateral));
    assert(!nm_road_measure_lateral(p,NULL,0,0,&lateral));
    assert(!nm_road_measure_lateral(p,&profiles[0],0,0,NULL));
    for (unsigned table = 0; table < 2; ++table) for (unsigned index = 0; index < 5; ++index)
        for (unsigned body = 0; body < 2; ++body) p->collision_ticks[table][index][body] = (uint8_t)(128+table*20+index*2+body);
    for (int control = -1; control <= 1; ++control) for (unsigned swap = 0; swap < 2; ++swap) {
        profiles[1].profile_control = (int16_t)control;
        nm_collision_reaction_t first = {{swap ? -1 : 1,0},0,-1234,0,0,0};
        nm_collision_reaction_t second = {{swap ? 1 : -1,0},0,-5678,1,0,0};
        const unsigned index = control ? (control < 0 ? 1u : 2u)+(swap ? 2u : 0u) : 0;
        assert(nm_collision_react(p,&road,&first,&second,&angle));
        assert((uint8_t)first.correction_ticks == 129+index*2 && (uint8_t)second.correction_ticks == 128+index*2);
        assert(first.initial_inhibit == 149+index*2 && second.initial_inhibit == 148+index*2);
        assert(first.lateral == (control ? (swap ? 1109 : 939) : -1234));
        assert(second.lateral == (control ? (swap ? 939 : 1109) : -5678));
    }
    profiles[0].profile_control = -1;
    nm_collision_reaction_t first = {{0,0},INT16_MIN,-1234,0,0,0};
    assert(nm_collision_react(p,&road,&first,&first,&angle));
    assert(first.lateral == 1024 && (uint8_t)first.correction_ticks == 135 && first.initial_inhibit == 155);
    nm_collision_reaction_t second = {{0,0},0,-5678,1,0,0};
    profiles[1].lanes[4].xyz[2] = NAN; // Late second-body lateral failure, finite pair angle.
    const nm_collision_reaction_t old_first = first, old_second = second;
    const int16_t old_angle = angle;
    assert(!nm_collision_react(p,&road,&first,&second,&angle));
    assert(!memcmp(&first,&old_first,sizeof(first)) && !memcmp(&second,&old_second,sizeof(second)) && angle == old_angle);
    assert(!nm_collision_react(NULL,&road,&first,&second,&angle));
    assert(!nm_collision_react(p,NULL,&first,&second,&angle));
    assert(!nm_collision_react(p,&road,NULL,&second,&angle));
    assert(!nm_collision_react(p,&road,&first,NULL,&angle));
    assert(!nm_collision_react(p,&road,&first,&second,NULL));
    second.profile = 2; assert(!nm_collision_react(p,&road,&first,&second,&angle));
    second.profile = 1; profiles[1].profile_control = 0; first.profile = 2;
    assert(nm_collision_react(p,&road,&first,&second,&angle)); // First profile/laterals unused on zero-control branch.
    memset(p->collision_ticks,0,sizeof(p->collision_ticks));
    p->sine[16383] = saved;
}

static void check_player_offcourse(nm_drive_params_t *p) {
    nm_contact_quad_t q = {.heading=32767,.recovery_height=3,.recovery_center={64,131,-64}};
    const nm_contact_pack_t pack = {.quads=&q,.quad_count=1};
    const int32_t timers[] = {INT32_MIN,-1,0,511,512,513,INT32_MAX};
    const uint16_t masks[] = {0,1,2,4,8,0x200,0x400,0xffff};
    /* Analytic table entries for denominator64 only. Restore the ROM-free
     * fixture afterward; actual packs get independent i960/TGP checks. */
    const uint32_t saved[] = {p->reciprocal[0],p->reciprocal[1]};
    p->reciprocal[0] = 0x3f800000; p->reciprocal[1] = 0x40000000;
    for (unsigned t = 0; t < 7; ++t) for (unsigned m = 0; m < 8; ++m) for (int force = 0; force < 2; ++force) {
        nm_player_t s = {0}; s.offcourse_ticks = timers[t]; s.boundary.speed = -0.0f;
        s.boundary.ground.combined_mask = masks[m]; s.boundary.ground.yaw = INT16_MIN;
        s.boundary.ground.position[1] = NAN; // Reset overwrites old height before use.
        const int32_t next = nm_gears_signed((uint32_t)(force ? 0x18c0 : timers[t])+1u);
        assert(nm_player_offcourse_step(p,&pack,force,&s));
        if (masks[m]&0x602) assert(!s.offcourse_ticks && !s.boundary.recovery_ticks);
        else if (next > 512) {
            assert(!s.offcourse_ticks && s.boundary.hold_ticks == 64 && s.boundary.recovery_ticks == 64);
            assert(nm_force_bits(s.boundary.speed) == 0 && s.boundary.ground.position[1] == 3);
            assert(s.boundary.displacement[0] == 1 && s.boundary.displacement[1] == 2 && s.boundary.displacement[2] == -1);
            assert(s.boundary.ground.road_heading == 32767 && s.boundary.recovery_rotation == 1023);
            for (unsigned v = 0; v < 4; ++v) assert(s.boundary.ground.height[v] == 3);
        } else assert(s.offcourse_ticks == next && !s.boundary.recovery_ticks);
    }
    nm_player_t s = {0}; s.offcourse_ticks = 512; s.boundary.ground.position[2] = NAN;
    const nm_player_t before = s;
    assert(!nm_player_offcourse_step(p,&pack,1,&s));
    assert(!memcmp(&s,&before,sizeof(s))); // Late failure rolls back timer, speed and all heights.
    assert(!nm_player_offcourse_step(NULL,&pack,0,&s));
    assert(!nm_player_offcourse_step(p,NULL,0,&s));
    assert(!nm_player_offcourse_step(p,&pack,0,NULL));
    s.boundary.ground.position[2] = 0; s.boundary.ground.road = 1;
    assert(!nm_player_offcourse_step(p,&pack,0,&s));
    s.boundary.ground.combined_mask = 2;
    assert(nm_player_offcourse_step(p,&pack,1,&s)); // No invalid road read on cleared branch.
    p->reciprocal[0] = saved[0]; p->reciprocal[1] = saved[1];
}

static void check_player_early(nm_drive_params_t *p) {
    p->speed_index_scale = 1; p->inhibit_speed_drop = 0.25f;
    const int32_t speeds[] = {-1,0,1,42,43,170,171,16777216,33554432};
    const int phases[] = {0,0,64,2688,2730,2730,10920,10920,0};
    for (unsigned i = 0; i < sizeof(speeds)/sizeof(*speeds); ++i) for (unsigned boost = 0; boost < 2; ++boost) {
        nm_player_t s = {0}; s.boundary.speed = (float)speeds[i];
        s.wheel_phase[0] = INT16_MIN; s.wheel_phase[1] = INT16_MAX; s.force.rpm_boost = boost;
        assert(nm_player_early_step(p,&s) && s.speed_index == speeds[i]);
        assert(s.wheel_phase[0] == nm_yaw_angle((uint32_t)(INT16_MIN-phases[i])));
        assert(s.wheel_phase[1] == nm_yaw_angle((uint32_t)(INT16_MAX-(boost ? 10920 : phases[i]))));
    }
    for (unsigned timer = 0; timer < 256; ++timer) {
        p->shake_angles[0][timer] = (int16_t)((int)timer-128); p->shake_angles[1][timer] = (int16_t)(timer*73);
        for (unsigned direction = 0; direction < 2; ++direction) {
            nm_player_t s = {0}; s.boundary.speed = 4;
            s.correction_ticks = timer < 128 ? (int8_t)timer : (int8_t)((int)timer-256);
            s.boundary.damage.shake_ticks = s.input_inhibit = s.correction_ticks;
            s.boundary.ground.position[0] = 8; s.boundary.ground.position[2] = 12;
            s.correction[0] = 2; s.correction[1] = -4;
            s.inhibit_direction = direction ? 1 : -1; s.inhibit_angle = 17;
            s.inhibit_scale = 42; s.shake_angles[0] = 99; s.shake_angles[1] = -99;
            p->inhibit_angles[0][timer] = -1000;
            assert(nm_player_early_step(p,&s));
            if (timer) {
                assert((uint8_t)s.correction_ticks == (uint8_t)(timer-1));
                assert((uint8_t)s.input_inhibit == (uint8_t)(timer-1));
                assert((uint8_t)s.boundary.damage.shake_ticks == (uint8_t)(timer-1));
                assert(s.shake_angles[0] == p->shake_angles[0][timer] && s.shake_angles[1] == p->shake_angles[1][timer]);
                assert(s.boundary.ground.position[0] == (timer == 2 ? 8 : 10));
                assert(s.boundary.ground.position[2] == (timer == 2 ? 12 : 8));
                assert(s.correction[0] == (timer == 2 ? 0 : 1.5f) && s.correction[1] == (timer == 2 ? 0 : -3));
                assert(s.control == 224 && s.early_sound_request == (timer == 81));
                assert(s.inhibit_angle == (direction ? 1000 : -1000) && s.boundary.speed == 3.75f);
            } else {
                assert(s.boundary.ground.position[0] == 8 && s.correction[0] == 2);
                assert(s.inhibit_scale == 42 && s.inhibit_angle == 17 && s.shake_angles[0] == 99 && !s.early_sound_request);
            }
        }
    }
    for (unsigned flags = 1; flags < 4; ++flags) for (unsigned speed = 19; speed <= 20; ++speed) {
        nm_player_t s = {0}; s.boundary.speed = (float)speed;
        s.boundary.damage.flags = flags<<15; s.boundary.damage.inhibit_ticks = 256;
        assert(nm_player_early_step(p,&s) && !s.boundary.damage.flags);
        assert(s.early_sound_request == (speed == 20));
        if (speed == 20) assert(s.input_inhibit == -112 && s.inhibit_direction == (flags&2 ? -1 : 1));
    }
    nm_player_t s = {0}; s.boundary.speed = NAN;
    assert(nm_player_early_step(p,&s) && s.speed_index == INT32_MIN); // Masked conversion, no undefined C cast.
    s.boundary.speed = 2147483648.0f; /* e5f4's cvtri overflow is masked and saturates (PRM 11-50). */
    assert(nm_player_early_step(p,&s) && s.speed_index == INT32_MAX);
    s.boundary.speed = 1; s.correction_ticks = 2; s.correction[0] = NAN;
    assert(nm_player_early_step(p,&s) && s.correction[0] == 0); // Cleared before consumption.
    s.correction_ticks = 3; s.correction[0] = NAN; nm_player_t before = s;
    assert(!nm_player_early_step(p,&s) && !memcmp(&s,&before,sizeof(s)));
    assert(!nm_player_early_step(NULL,&s) && !nm_player_early_step(p,NULL));
    p->speed_index_scale = p->inhibit_speed_drop = 0;
    puts("PASS: early player speed phases, signed/unsigned timers, flag precedence and atomic failure");
}

static void check_damage(nm_drive_params_t *math) {
    nm_contact_quad_t quad = {0};
    const nm_contact_pack_t pack = {.quads=&quad,.quad_count=1};
    for (unsigned c = 0; c < 2; ++c) for (unsigned i = 0; i < 16; ++i) {
        math->crash_model[c][i] = 100+c*16+i;
        math->crash_threshold[c][0][i] = 100;
        math->crash_threshold[c][1][i] = 50;
    }
    for (unsigned i = 0; i < NM_CRASH_ROWS; ++i)
        math->crash_rows[i] = (nm_crash_row_t){1000+i,0,{INT16_MIN,3,-7}};
    nm_damage_input_t in = {.wheel_surface={0,0,0,0},.speed_index=49};
    nm_damage_state_t s;
    for (unsigned wheel = 0; wheel < 5; ++wheel) {
        for (unsigned v = 0; v < 4; ++v) in.wheel_surface[v] = v >= wheel ? 0 : 1;
        s = (nm_damage_state_t){.last_surface=UINT32_MAX,.flags=UINT32_MAX};
        assert(nm_boundary_spin(math,&pack,&in,&s));
        const unsigned selected = wheel == 4 ? 0 : wheel;
        assert(s.flags == (0xfc3fffffu|(1u<<(25-selected))));
        assert(s.damage == ((1u<<(3-selected))|(1u<<(7-selected))));
        assert(s.body_model == math->crash_model[0][s.damage&15]);
        assert(s.shake_ticks == 24 && s.side == 4 && s.signal == 1);
        assert(s.spin_ticks == 32 && s.override_count == 32 && !s.remaining && !s.sound_requests);
    }
    in.heading = 16384; s.last_surface = UINT32_MAX;
    assert(nm_boundary_spin(math,&pack,&in,&s));
    assert(s.spin_rotation == -20480 && s.spin_heading == -28672);
    in.heading = -1; s.last_surface = UINT32_MAX;
    assert(nm_boundary_spin(math,&pack,&in,&s) && s.spin_rotation == -2 && s.spin_heading == 1);
    // Duplicate surface returns before geometry/curve validation, but still shakes.
    in.selected = s.last_surface = UINT32_MAX; in.curve = 2; s.shake_ticks = 0;
    s.sound_requests = 7; s.auxiliary_request = 1;
    nm_damage_state_t before = s; before.shake_ticks = 24;
    before.sound_requests = before.auxiliary_request = 0;
    assert(nm_boundary_spin(math,&pack,&in,&s) && !memcmp(&s,&before,sizeof(s)));
    in.selected = 0; before = s;
    assert(!nm_boundary_spin(math,&pack,&in,&s) && !memcmp(&s,&before,sizeof(s)));
    in.curve = 0; in.selected = 1;
    assert(!nm_boundary_spin(math,&pack,&in,&s) && !memcmp(&s,&before,sizeof(s)));
    in.selected = 0; in.speed_index = 50;
    s = (nm_damage_state_t){.last_surface=UINT32_MAX,.sequence_counter=255,.crash_count=255};
    assert(nm_boundary_spin(math,&pack,&in,&s));
    assert((s.damage&256) && s.body_model == 100 && s.overlay_model == 1000);
    assert(s.row == 1 && s.remaining == 256 && s.inhibit_ticks == 128 && s.initial_inhibit == 129);
    assert(!s.crash_count && !s.sequence_counter && s.orientation == 3);
    assert(s.angle[0] == INT16_MIN && s.angle[1] == 3 && s.angle[2] == 7);
    assert(s.auxiliary_request && s.sound_requests == NM_CRASH_SOUND_START && s.sound_ticks == 1);
    assert(nm_crash_advance(math,0,&s));
    assert(s.row == 3 && s.remaining == 254 && s.inhibit_ticks == 126 && s.overlay_model == 1002);
    assert(!s.sound_requests && !s.auxiliary_request && s.sound_ticks == 3);
    for (unsigned other = 0; other < 2; ++other) {
        s = (nm_damage_state_t){.remaining=257,.inhibit_ticks=other ? 73 : 29};
        assert(nm_crash_advance(math,(uint8_t)other,&s));
        assert(s.override_count == (other ? 72 : 28));
    }
    math->crash_rows[0].marker = 257;
    for (unsigned large = 0; large < 2; ++large) {
        s = (nm_damage_state_t){.remaining=257,.sound_ticks=large ? 79 : 39};
        assert(nm_crash_advance(math,0,&s));
        assert(!s.sound_ticks && s.sound_requests == (large ? NM_CRASH_SOUND_LARGE : NM_CRASH_SOUND_SMALL));
    }
    math->crash_rows[0].marker = 256;
    s = (nm_damage_state_t){.remaining=257,.inhibit_ticks=INT16_MIN,.sound_ticks=INT16_MAX};
    assert(nm_crash_advance(math,0,&s));
    assert(s.inhibit_ticks == INT16_MAX && s.sound_ticks == INT16_MIN && !s.sound_requests);
    s = (nm_damage_state_t){.remaining=257,.sound_ticks=79};
    math->crash_rows[0].marker = 257;
    assert(nm_crash_advance(math,1,&s) && s.sound_ticks == 79 && !s.sound_requests);
    s = (nm_damage_state_t){.remaining=257,.row=1}; before = s;
    assert(!nm_crash_advance(math,0,&s) && !memcmp(&s,&before,sizeof(s)));
    s = (nm_damage_state_t){.row=UINT16_MAX,.overlay_model=7};
    assert(nm_crash_advance(math,0,&s) && !s.overlay_model && s.row == UINT16_MAX);
    in.global_flags = 0x08000000; s = (nm_damage_state_t){.last_surface=UINT32_MAX};
    assert(nm_boundary_spin(math,&pack,&in,&s) && s.remaining == 256 && !s.auxiliary_request && !s.sound_requests);
    s = (nm_damage_state_t){.last_surface=UINT32_MAX,.inhibit_ticks=-1};
    assert(nm_boundary_spin(math,&pack,&in,&s) && !s.remaining && !(s.damage&256));
    assert(!nm_boundary_spin(NULL,&pack,&in,&s) && !nm_boundary_spin(math,NULL,&in,&s));
    assert(!nm_boundary_spin(math,&pack,NULL,&s) && !nm_boundary_spin(math,&pack,&in,NULL));
    assert(!nm_crash_advance(NULL,0,&s) && !nm_crash_advance(math,0,NULL));
    nm_damage_pre_input_t pre_in = {.path_flags=1,.phase=2345,.yaw=-12,.restored_camera_mode=-128,.variant=2};
    nm_damage_pre_state_t pre = {.damage={.spin_ticks=1,.last_surface=7,.remaining=1,.row=256,
        .inhibit_ticks=1,.signal=-1},.auxiliary_flags=UINT32_MAX,.decay=INT16_MIN,
        .countdown=255,.camera_transition=1};
    for (unsigned i = 0; i < 6; ++i) math->camera_reset_words[i] = 42+i;
    math->camera_reset_angles[0] = -123;
    nm_damage_pre_state_t intro=pre;
    assert(nm_damage_intro_step(math,&pre_in,&intro));
    assert(nm_damage_pre_step(math,&pre_in,&pre));
    nm_damage_pre_state_t expected_intro=pre;
    expected_intro.damage.spin_ticks=1;expected_intro.damage.last_surface=7;
    assert(!memcmp(&intro,&expected_intro,sizeof(intro)));
    assert(!pre.damage.spin_ticks && pre.damage.last_surface == UINT32_MAX);
    assert(pre.path_offset == 124 && pre.heading == -12 && pre.decay == -32512 && pre.countdown == 254);
    assert(!pre.damage.remaining && pre.damage.row == 257 && !pre.damage.inhibit_ticks);
    assert(pre.control == 224 && pre.pivot_signal == 42 && !pre.damage.signal);
    assert(pre.auxiliary_flags == 0x7fffffff && pre.camera_mode == -128 && !pre.camera_transition);
    assert(!memcmp(pre.camera_words,math->camera_reset_words,sizeof(pre.camera_words)) && pre.camera_angles[0] == -123);
    // No active animation must not clear the final overlay or touch camera state.
    const uint32_t overlay = pre.damage.overlay_model;
    pre.control = 123; pre.camera_transition = 7;
    assert(nm_damage_pre_step(math,&pre_in,&pre) && pre.control == 123 && pre.camera_transition == 7);
    assert(pre.damage.overlay_model == overlay);
    pre_in.global_flags = 0x08000000;
    pre.damage.remaining = 1; pre.damage.row = 256; pre.camera_mode = 99;
    assert(nm_damage_pre_step(math,&pre_in,&pre) && pre.camera_mode == 99 && pre.camera_transition == 7);
    pre.damage.remaining = 257; pre.damage.row = 2; pre.damage.spin_ticks = 1;
    const nm_damage_pre_state_t saved_pre = pre;
    assert(!nm_damage_intro_step(math,&pre_in,&pre) && !memcmp(&pre,&saved_pre,sizeof(pre)));
    assert(!nm_damage_pre_step(math,&pre_in,&pre) && !memcmp(&pre,&saved_pre,sizeof(pre)));
    assert(!nm_damage_pre_step(NULL,&pre_in,&pre) && !nm_damage_pre_step(math,NULL,&pre));
    assert(!nm_damage_pre_step(math,&pre_in,NULL));
    assert(!nm_damage_intro_step(NULL,&pre_in,&pre) && !nm_damage_intro_step(math,NULL,&pre));
    assert(!nm_damage_intro_step(math,&pre_in,NULL));
    puts("PASS: native spin/damage/animation boundaries and atomic rejection");
}

int main(int argc, char **argv) {
    for(unsigned i=0;i<65536;++i) {
        const float value=nm_force_float(i*UINT32_C(0x10001));
        if(isfinite(value)) for(unsigned direction=0;direction<2;++direction)
            assert(nm_force_bits(nm_contact_test_neighbor(value,direction))==
                   nm_force_bits(nextafterf(value,direction ? INFINITY : -INFINITY)));
    }
    assert(nm_force_bits(nm_contact_test_neighbor(-0.0f,1))==1);
    assert(nm_force_bits(nm_contact_test_neighbor(-0.0f,0))==UINT32_C(0x80000001));
    unsigned cell = 999;
    assert(nm_contact_cell(-1024,-1024,&cell) && cell == 0);
    assert(nm_contact_cell(1024,1024,&cell) && cell == 0);
    assert(nm_contact_cell(-1008.5f,-1024,&cell) && cell == 0);
    assert(nm_contact_cell(nextafterf(-1008.5f,INFINITY),-1024,&cell) && cell == 1);
    assert(nm_contact_cell(15.5f,0,&cell) && cell == 64*128+65);
    assert(nm_contact_cell(-15.5f,0,&cell) && cell == 64*128+63);
    assert(!nm_contact_cell(NAN,0,&cell) && !nm_contact_cell(INFINITY,0,&cell));
    /* Out of int32 range the masked cvtri saturates (nm_drive_round_i32): x -> INT32_MAX. */
    assert(nm_contact_cell(2147483648.0f,0,&cell) && cell == 64*128+63 && !nm_contact_cell(0,0,NULL));
    uint16_t scenery=1234;
    assert(!nm_contact_scenery_cell(NAN,0,&scenery) && scenery==1234);
    assert(!nm_contact_scenery_cell(0,INFINITY,&scenery) && scenery==1234);
    assert(!nm_contact_scenery_cell(0,0,NULL));
    for (int x=-2048;x<=2048;x+=7) for (int z=-2048;z<=2048;z+=11) {
        assert(nm_contact_cell(x+0.5f,z-0.5f,&cell));
        assert(nm_contact_scenery_cell(x+0.5f,z-0.5f,&scenery));
        assert(scenery==(((cell>>3)&15) | ((cell>>6)&240)));
    }
    nm_contact_pack_t pack = {0};
    if (argc > 1) {
        if (!nm_contact_load(&pack,argv[1])) return 1;
        assert(!nm_contact_load(&pack,argv[1])); // Never replace a live allocation.
        printf("DNC3 loaded slot=%u bytes=%u quads=%u candidates=%u\n",
               (unsigned)pack.slot,(unsigned)pack.payload_bytes,(unsigned)pack.quad_count,(unsigned)pack.max_candidates);
        if (argc == 3) {
            nm_drive_params_t *math = nm_drive_load(argv[2]);
            assert(math);
            assert(nm_body_angles_test_hash(math) == NM_BODY_ANGLES_HASH);
            assert(nm_tilt_test_hash(math) == NM_TILT_HASH);
            const uint32_t hash = nm_contact_test_hash(&pack,math);
            printf("contact cases=%u hash=%08x\n",nm_contact_test_count(&pack),(unsigned)hash);
            assert(hash == nm_contact_expected_hash(pack.slot));
            const uint32_t ground_hash = nm_wheels_test_hash(&pack,math);
            printf("ground cases=%u hash=%08x\n",(unsigned)pack.quad_count*4,(unsigned)ground_hash);
            assert(ground_hash == nm_wheels_expected_hash(pack.slot));
            const uint32_t boundary_hash = nm_boundary_test_hash(math);
            printf("boundary hash=%08x\n",(unsigned)boundary_hash);
            assert(boundary_hash == NM_BOUNDARY_EXPECTED_HASH);
            assert(nm_boundary_observable_test_hash(math) == NM_BOUNDARY_OBSERVABLE_HASH);
            assert(nm_collision_walk_test_hash(math) == NM_COLLISION_WALK_HASH);
            const uint32_t candidate_hash = nm_collision_candidate_test_hash(math);
            printf("collision broad-phase cases=%u hash=%08x\n",NM_COLLISION_CANDIDATE_CASES,(unsigned)candidate_hash);
            assert(candidate_hash == NM_COLLISION_CANDIDATE_HASH);
            const uint32_t traffic_near_hash = nm_traffic_near_test_hash(math);
            printf("traffic proximity cases=%u hash=%08x\n",NM_TRAFFIC_NEAR_CASES,(unsigned)traffic_near_hash);
            assert(traffic_near_hash == NM_TRAFFIC_NEAR_HASH);
            const uint32_t pass_hash = nm_traffic_pass_test_hash(math);
            printf("traffic passing cases=%u hash=%08x\n",NM_TRAFFIC_PASS_CASES,(unsigned)pass_hash);
            assert(pass_hash == NM_TRAFFIC_PASS_HASH);
            const uint32_t detection_hash = nm_collision_detect_test_hash(math);
            printf("collision detection cases=%u hash=%08x\n",NM_COLLISION_DETECT_CASES,(unsigned)detection_hash);
            assert(detection_hash == NM_COLLISION_DETECT_HASH);
            for (unsigned i = 0; i < pack.quad_count*16; ++i) {
                nm_wheels_state_t w; nm_wheels_test_input(&pack,i/4,&w);
                assert(nm_wheels_transform(math,w.position,w.yaw,w.roll,w.world));
                nm_collision_probe_t a = {{w.position[0],w.position[2]},0}, b = a;
                unsigned candidate;
                assert(nm_collision_candidate(math,&a,&b,&candidate) && candidate);
                b.word5c = 0x3fd9999b;
                assert(nm_collision_candidate(math,&a,&b,&candidate) && !candidate);
                b.position[0] += 32; b.word5c = 0;
                assert(nm_collision_candidate(math,&a,&b,&candidate) && !candidate);
                nm_boundary_pivot_t p = {0};
                memcpy(p.position,w.position,12); p.yaw = w.yaw; p.speed = 1;
                p.condition = (int8_t)((int)(i%3)-1); p.rotation = nm_yaw_angle(i*131);
                uint8_t sides[4];
                for (unsigned v = 0; v < 4; ++v) sides[v] = (uint8_t)((i>>v)&1);
                assert(nm_boundary_pivot(math,pack.quads+i/16,i/16,&w,sides,i%512,1+i%10,&p));
                assert(p.side < 4 && isfinite(p.speed));
                for (unsigned a = 0; a < 3; ++a) assert(isfinite(p.position[a]));
                for (unsigned v = 0; v < 4; ++v) w.mask[v] = (uint16_t)(1u<<((i+v)%6));
                nm_boundary_selection_t selection = {w.position[1],0,0,0};
                assert(nm_boundary_select(&pack,math,&w,w.yaw,&selection));
                assert(selection.action <= NM_BOUNDARY_SPIN && isfinite(selection.height));
                nm_boundary_recovery_t recovery = {0}; recovery.speed = p.speed;
                assert(nm_boundary_recover(math,pack.quads+i/16,&w,&recovery));
                assert(recovery.height == pack.quads[i/16].recovery_height && recovery.hold_ticks == 21 && recovery.recovery_ticks == 21);
                for (unsigned a = 0; a < 3; ++a) assert(isfinite(recovery.displacement[a]));
                nm_damage_input_t damage_input = {.selected=i/16,.speed_index=INT32_MAX,.curve=i&1};
                nm_damage_state_t damage = {.last_surface=UINT32_MAX,.crash_count=(uint8_t)i,.sequence_counter=(uint8_t)i};
                assert(nm_boundary_spin(math,&pack,&damage_input,&damage));
                assert(damage.remaining == 256 && damage.row == 1 && damage.inhibit_ticks == 128);
                assert(nm_crash_advance(math,0,&damage));
                assert(damage.remaining == 254 && damage.row == 3 && damage.inhibit_ticks == 126);
                nm_damage_pre_state_t pre = {.damage=damage,.auxiliary_flags=UINT32_MAX};
                const nm_damage_pre_input_t pre_input = {.restored_camera_mode=-7};
                pre.damage.remaining = 1; pre.damage.row = 256; pre.damage.inhibit_ticks = 1;
                assert(nm_damage_pre_step(math,&pre_input,&pre));
                assert(!pre.damage.remaining && pre.camera_mode == -7 && pre.auxiliary_flags == 0x7fffffff);
                assert(!memcmp(pre.camera_words,math->camera_reset_words,sizeof(pre.camera_words)));
                assert(!memcmp(pre.camera_angles,math->camera_reset_angles,sizeof(pre.camera_angles)));
                nm_boundary_state_t response = {0}; response.ground = w; response.speed = 1;
                response.damage.last_surface = UINT32_MAX; response.heading = w.yaw;
                nm_boundary_input_t input = {.speed_index=(int32_t)(i%512),.lap=(uint8_t)(i%10),.curve=i&1};
                assert(nm_boundary_step(&pack,math,&input,&response));
                assert(response.action <= NM_BOUNDARY_SPIN);
            }
            puts("PASS: pivot/selection/recovery/damage actual-pack state sweep (not an oracle digest)");
            puts("PASS: collision broad-phase actual-pack near/far/raw-word sweep");
            free(math);
        }
        nm_contact_unload(&pack);
        assert(!pack.storage && !pack.grid && !pack.quads);
        return 0;
    }
    nm_drive_params_t *math = calloc(1,sizeof(*math));
    uint16_t *grid = calloc(16384,sizeof(*grid));
    assert(math && grid);
    check_player_early(math);
    check_collision(math);
    check_collision_damage(math);
    check_collision_impulse(math);
    check_collision_choice(math);
    check_collision_route(math);
    check_collision_candidate(math);
    check_collision_detection(math);
    check_body_angles(math);
    check_tilt(math);
    check_road_tail(math);
    assert(nm_player_end_test_hash() == NM_PLAYER_END_HASH);
    nm_player_t ending = {0};
    ending.route_transition = 255; nm_player_end_step(&ending);
    assert(!ending.route_transition && ending.proximity_circle[2] == 8);
    ending.held = 1; ending.body_yaw = 1; ending.route_transition = 255;
    nm_player_end_step(&ending);
    assert(!ending.body_yaw && !ending.route_transition); // Negative quarter step floors, not truncates.
    check_collision_preparation();
    check_traffic_order();
    check_traffic_near(math);
    check_race_signals(math);
    check_collision_walk(math);
    check_player_offcourse(math);
    check_damage(math);
    float boundary[8] = {0,0,0,1,2,0,2,1};
    nm_boundary_hit_t intersection = {{17,-19},1};
    assert(nm_boundary_intersection(math,boundary,&intersection) && !intersection.hit);
    assert(intersection.point[0] == 17 && intersection.point[1] == -19);
    const nm_boundary_hit_t saved_intersection = intersection;
    boundary[1] = NAN;
    assert(!nm_boundary_intersection(math,boundary,&intersection));
    assert(!memcmp(&intersection,&saved_intersection,sizeof(intersection)));
    assert(!nm_boundary_intersection(NULL,boundary,&intersection));
    assert(!nm_boundary_intersection(math,NULL,&intersection));
    assert(!nm_boundary_intersection(math,boundary,NULL));
    uint16_t lists[] = {2,0,1};
    nm_contact_quad_t quads[2] = {{{{0}},1,0,0,{0}},{{{0}},5,0,0,{0}}};
    pack.storage = grid; pack.grid = grid; pack.lists = lists; pack.quads = quads;
    nm_contact_result_t state = {100,123,456,0};
    // Degenerate quads produce the authored fallback height, not a miss.
    assert(nm_contact_query(&pack,math,0,0,&state));
    assert(state.height == -0.1f && state.road == 123 && state.surface == 1 && state.mask == 32);
    quads[1].surface = 0xf1;
    assert(nm_contact_query(&pack,math,0,0,&state));
    assert(state.road == 1 && state.surface == 1 && state.mask == 2);
    const nm_contact_result_t before = state;
    assert(!nm_contact_query(&pack,math,NAN,0,&state) && !memcmp(&before,&state,sizeof(state)));
    assert(!nm_contact_query(NULL,math,0,0,&state));
    assert(!nm_contact_query(&pack,NULL,0,0,&state));
    assert(!nm_contact_query(&pack,math,0,0,NULL));
    quads[0].xyz[0][0] = FLT_MAX; quads[0].xyz[0][2] = FLT_MAX;
    assert(!nm_contact_query(&pack,math,16,16,&state) && !memcmp(&before,&state,sizeof(state)));
    lists[0] = 0;
    assert(nm_contact_query(&pack,math,0,0,&state));
    assert(state.height == -0.1f && state.mask == 1 && state.road == 1 && state.surface == 1);
    const nm_contact_quad_t flat = {{{0,0,0},{0,0,16},{16,0,16},{16,0,0}},1,0,0,{8,0,8}};
    quads[0] = flat; lists[0] = 1;
    assert(nm_contact_query(&pack,math,8,8,&state));
    assert(nm_force_bits(state.height) == 0x80000000u && state.surface == 0 && state.mask == 2);
    // -0 is a hit: the wrapper tests raw height bits, not numeric equality.
    math->sine[16383] = nm_force_bits(1);
    for (unsigned v = 0; v < 4; ++v) {
        math->wheel_local[v][0] = v&1 ? 1 : -1;
        math->wheel_local[v][2] = v<2 ? 2 : -2;
    }
    pack.quad_count = 2;
    nm_wheels_state_t wheels = {0};
    nm_boundary_selection_t selection = {19,123,77,99};
    wheels.position[0] = wheels.position[2] = 8; wheels.position[1] = 3;
    for (unsigned mask = 0; mask < 64; ++mask) {
        for (unsigned v = 0; v < 4; ++v) wheels.mask[v] = (uint16_t)mask;
        selection.center_mask = 77;
        assert(nm_boundary_select(&pack,math,&wheels,0,&selection));
        const int early = mask&2 || mask&33 || mask&12;
        assert(selection.action == (!(mask&2) && (mask&33) ? NM_BOUNDARY_RECOVER : NM_BOUNDARY_NONE));
        assert(selection.center_mask == (early ? 77 : 2));
        assert(selection.height == (early ? 3 : 0));
    }
    memset(wheels.mask,0,sizeof(wheels.mask)); wheels.mask[0] = 1;
    const int angles[] = {-32768,-28672,-28671,-4096,-4095,0,4095,4096,28671,28672,32767};
    for (unsigned i = 0; i < sizeof(angles)/sizeof(*angles); ++i) {
        quads[0].heading = (uint16_t)angles[i];
        assert(nm_boundary_select(&pack,math,&wheels,0,&selection));
        const int spin = (angles[i] >= 4096 && angles[i] <= 28671) ||
                         (angles[i] >= -28671 && angles[i] <= -4096);
        assert(selection.action == (spin ? NM_BOUNDARY_SPIN : NM_BOUNDARY_PIVOT));
        assert(selection.selected == 0 && selection.center_mask == 2 && selection.height == 0);
    }
    quads[0].heading = 0; wheels.mask[0] = 0;
    // Exactly the threshold is not a spin. Test each signed edge independently.
    const unsigned height_pairs[4][2] = {{0,2},{1,3},{0,1},{2,3}};
    for (unsigned pair = 0; pair < 4; ++pair) for (unsigned sign = 0; sign < 2; ++sign) {
        const float limit = pair < 2 ? 3.5f : 2.5f;
        for (unsigned above = 0; above < 2; ++above) {
            const float value = (sign ? -1 : 1)*(above ? nextafterf(limit,INFINITY) : limit);
            for (unsigned v = 0; v < 4; ++v) wheels.height[v] = value*0.5f;
            wheels.height[height_pairs[pair][0]] = value; wheels.height[height_pairs[pair][1]] = 0;
            assert(nm_boundary_select(&pack,math,&wheels,0,&selection));
            assert(selection.action == (above ? NM_BOUNDARY_SPIN : NM_BOUNDARY_NONE));
        }
    }
    const nm_boundary_selection_t selection_before = selection;
    assert(!nm_boundary_select(NULL,math,&wheels,0,&selection));
    assert(!nm_boundary_select(&pack,NULL,&wheels,0,&selection));
    assert(!nm_boundary_select(&pack,math,NULL,0,&selection));
    assert(!nm_boundary_select(&pack,math,&wheels,0,NULL));
    wheels.mask[0] = 1; wheels.surface[0] = pack.quad_count;
    assert(!nm_boundary_select(&pack,math,&wheels,0,&selection));
    assert(!memcmp(&selection,&selection_before,sizeof(selection)));
    wheels.position[0] = NAN;
    assert(!nm_boundary_select(&pack,math,&wheels,0,&selection));
    assert(!memcmp(&selection,&selection_before,sizeof(selection)));
    memset(&wheels,0,sizeof(wheels));
    nm_boundary_recovery_t recovery = {0}; recovery.speed = -0.0f;
    wheels.yaw = INT16_MIN; wheels.road_heading = INT16_MAX;
    assert(nm_boundary_recover(math,&flat,&wheels,&recovery));
    assert(nm_force_bits(recovery.speed) == 0x80000000u && recovery.height == 0);
    assert(recovery.rotation == 65535/21 && recovery.hold_ticks == 21 && recovery.recovery_ticks == 21);
    const nm_boundary_recovery_t saved_recovery = recovery;
    assert(!nm_boundary_recover(NULL,&flat,&wheels,&recovery));
    assert(!nm_boundary_recover(math,NULL,&wheels,&recovery));
    assert(!nm_boundary_recover(math,&flat,NULL,&recovery));
    assert(!nm_boundary_recover(math,&flat,&wheels,NULL));
    wheels.position[2] = INFINITY;
    assert(!nm_boundary_recover(math,&flat,&wheels,&recovery));
    assert(!memcmp(&recovery,&saved_recovery,sizeof(recovery)));
    wheels.position[2] = 0; quads[0].recovery_height = NAN;
    assert(!nm_boundary_recover(math,quads,&wheels,&recovery));
    quads[0] = flat; quads[0].recovery_center[2] = NAN;
    assert(!nm_boundary_recover(math,quads,&wheels,&recovery));
    assert(!memcmp(&recovery,&saved_recovery,sizeof(recovery)));
    quads[0] = flat; recovery.speed = NAN;
    nm_boundary_recovery_t invalid_recovery = recovery;
    assert(!nm_boundary_recover(math,&flat,&wheels,&recovery));
    assert(!memcmp(&recovery,&invalid_recovery,sizeof(recovery)));
    memset(&wheels,0,sizeof(wheels));
    nm_boundary_pivot_t pivot = {0};
    const uint8_t pivot_sides[4] = {0,0,0,0};
    pivot.speed = 2; pivot.condition = 1;
    assert(nm_boundary_pivot(math,&flat,0,&wheels,pivot_sides,40,1,&pivot));
    assert(pivot.impact_speed == 2 && !pivot.side && !pivot.sound_request);
    nm_boundary_pivot_t pivot_before = pivot;
    assert(!nm_boundary_pivot(NULL,&flat,0,&wheels,pivot_sides,40,1,&pivot));
    assert(!nm_boundary_pivot(math,NULL,0,&wheels,pivot_sides,40,1,&pivot));
    assert(!nm_boundary_pivot(math,&flat,0,NULL,pivot_sides,40,1,&pivot));
    assert(!nm_boundary_pivot(math,&flat,0,&wheels,NULL,40,1,&pivot));
    assert(!nm_boundary_pivot(math,&flat,0,&wheels,pivot_sides,40,1,NULL));
    assert(!memcmp(&pivot,&pivot_before,sizeof(pivot)));
    pivot.condition = 0; pivot_before = pivot;
    assert(!nm_boundary_pivot(math,&flat,0,&wheels,pivot_sides,40,0,&pivot));
    assert(!memcmp(&pivot,&pivot_before,sizeof(pivot)));
    pivot.speed = INFINITY; pivot_before = pivot;
    assert(!nm_boundary_pivot(math,&flat,0,&wheels,pivot_sides,40,1,&pivot));
    assert(!memcmp(&pivot,&pivot_before,sizeof(pivot)));
    wheels.position[0] = wheels.position[2] = 8;
    nm_boundary_state_t response = {0};
    nm_boundary_input_t response_input = {.lap=255};
    response.speed = 2; response.damage.last_surface = UINT32_MAX;
    for (unsigned v = 0; v < 4; ++v) response.ground.mask[v] = 2;
    response.damage.sound_requests = response.damage.auxiliary_request = response.pivot_sound = 255;
    assert(nm_boundary_step(&pack,math,&response_input,&response));
    assert(response.action == NM_BOUNDARY_NONE && !response.damage.sound_requests &&
           !response.damage.auxiliary_request && !response.pivot_sound && response.speed == 2);
    for (unsigned v = 0; v < 4; ++v) response.ground.mask[v] = 1;
    response.ground.position[0] = response.ground.position[2] = 8;
    assert(nm_boundary_step(&pack,math,&response_input,&response));
    assert(response.action == NM_BOUNDARY_RECOVER && response.speed == 1 && response.hold_ticks == 21);
    for (unsigned v = 0; v < 4; ++v) assert(response.ground.height[v] == flat.recovery_height);
    response.ground.road = 2;
    const nm_boundary_state_t before_response = response;
    assert(!nm_boundary_step(&pack,math,&response_input,&response));
    assert(!nm_boundary_step(NULL,math,&response_input,&response));
    assert(!nm_boundary_step(&pack,NULL,&response_input,&response));
    assert(!nm_boundary_step(&pack,math,NULL,&response));
    assert(!nm_boundary_step(&pack,math,&response_input,NULL));
    assert(!memcmp(&before_response,&response,sizeof(response)));
    response.ground.road = 0;
    response.ground.mask[0] = 1;
    for (unsigned v = 1; v < 4; ++v) response.ground.mask[v] = 2;
    response_input.speed_index = 400;
    const nm_boundary_state_t before_pivot_response = response;
    assert(!nm_boundary_step(&pack,math,&response_input,&response)); // Required zero division: entire step is atomic.
    assert(!memcmp(&before_pivot_response,&response,sizeof(response)));
    math->pivot_divisor[0][255] = 2;
    assert(nm_boundary_step(&pack,math,&response_input,&response));
    assert(response.action == NM_BOUNDARY_PIVOT && response.condition == 16 && response.damage.override_count == 200);
    response.heading = 16384; response.damage.inhibit_ticks = 7;
    assert(nm_boundary_step(&pack,math,&response_input,&response));
    assert(response.action == NM_BOUNDARY_SPIN && response.damage.spin_ticks == 32 && response.damage.override_count == 32);
    for (unsigned lap = 0; lap < 256; ++lap) {
        pivot = (nm_boundary_pivot_t){.speed=2};
        math->pivot_divisor[0][lap] = 1+lap;
        assert(nm_boundary_pivot_course(math,&pack,0,&wheels,400,(uint8_t)lap,&pivot));
        assert(pivot.override_count == (uint8_t)(400/(1+lap)));
    }
    assert(!nm_boundary_pivot_course(math,&pack,pack.quad_count,&wheels,40,0,&pivot));
    pack.slot = 4;
    assert(!nm_boundary_pivot_course(math,&pack,0,&wheels,40,0,&pivot));
    pack.slot = 0;
    wheels.position[1] = 3;
    assert(nm_wheels_ground(&pack,math,&wheels));
    for (unsigned v = 0; v < 4; ++v) {
        assert(wheels.world[v][0] == 8+math->wheel_local[v][0]);
        assert(wheels.world[v][1] == 3 && wheels.world[v][2] == 8+math->wheel_local[v][2]);
        assert(wheels.mask[v] == 2 && wheels.surface[v] == 0 && wheels.height[v] == 0);
    }
    assert(wheels.position[1] == 0 && !wheels.pitch && !wheels.roll && !wheels.road_heading);
    const float road_frame[] = {-8,-8,8,8,8,-8};
    assert(!memcmp(wheels.road_frame,road_frame,sizeof(road_frame)));
    // Quarter filtering rounds negative deltas down; wheel matrices ignore pitch.
    wheels.roll = 3; wheels.pitch = -3;
    assert(nm_wheels_ground(&pack,math,&wheels));
    assert(wheels.roll == 2 && wheels.pitch == -3);
    nm_player_t car = {0}; nm_player_dynamics_input_t input = {0};
    car.boundary.ground.position[0] = car.boundary.ground.position[2] = 8;
    car.boundary.speed = 1; car.force.slip_decay = -1; input.motion_scale = 1;
    assert(nm_player_dynamics_step(math,&pack,NULL,NULL,&input,&car));
    assert(car.boundary.speed == 1 && car.boundary.ground.position[0] == 8 && car.boundary.ground.position[2] == 9);
    assert(car.boundary.ground.position[1] == 0 && car.boundary.ground.mask[0] == 2 && car.delta[1] == 1);
    assert(car.scenery_cell == 136);
    assert(car.lookahead[1] == 24); // Old Z8 plus16x movement, before ground contact.
    // Full-width brake is shared between low16 input writes and32-bit gear stores.
    // Finish the animation BEFORE input: its last inhibit tick must not suppress pedals.
    nm_player_active_input_t active = {.dynamics={.motion_scale=1},
        .controls={.steer=7,.accel=123,.brake=17},.input_counter=65,.proximity_disabled=1};
    for (unsigned g = 0; g < 4; ++g) {
        math->gear_up[g] = INT32_MAX; math->gear_down[g] = INT32_MIN;
        math->gear_rpm_delta[g][0] = -10000; math->gear_rpm_delta[g][1] = 10000;
    }
    nm_player_t prepared = car;
    prepared.brake = 0xa5a500ff; prepared.force.rpm_boost = (uint32_t)-1;
    prepared.boundary.damage.remaining = 1; prepared.boundary.damage.row = 256;
    prepared.boundary.damage.inhibit_ticks = 1;
    prepared.auxiliary_flags = UINT32_MAX; prepared.camera_transition = 7;
    active.restored_camera_mode = -3;
    assert(nm_player_prepare_step(math,&active,&prepared));
    assert(prepared.accel == 123 && prepared.brake == 0xa5a50011 && prepared.steer == 896);
    assert(prepared.filtered_rpm == -1 && !prepared.boundary.damage.inhibit_ticks);
    assert(prepared.control == 224 && prepared.camera_mode == -3 && !prepared.camera_transition);
    assert(prepared.auxiliary_flags == 0x7fffffff && !prepared.boundary.damage.remaining);
    // Signed inhibit SUM can cancel; counter64 increments override then resistance decrements it.
    prepared.input_inhibit = -2; prepared.boundary.damage.inhibit_ticks = 2;
    prepared.boundary.damage.override_count = 255; active.input_counter = 64; active.dynamics.speed_index = 64;
    assert(nm_player_prepare_step(math,&active,&prepared));
    assert(prepared.brake == 0xa5a500ff && !prepared.accel && !prepared.boundary.damage.override_count);
    active.dynamics.curve = 1; prepared.rpm = 9000; math->gear_selector[0] = -1;
    assert(nm_player_prepare_step(math,&active,&prepared));
    assert(prepared.brake == 255 && prepared.rpm == 8500); // Manual over-rev writes all32 brake bits.
    active.dynamics.curve = 0; active.dynamics.speed_index = 0; active.input_counter = 65;
    prepared = car; prepared.boundary.recovery_ticks = 2;
    prepared.boundary.displacement[2] = 1; prepared.force.tire_angle[1] = 123;
    prepared.resistance = 42; prepared.drag_state = 17; prepared.boundary.damage.override_count = 5;
    math->resistance[4] = NAN; active.proximity_disabled = 0; active.neighbor[0][0] = NAN;
    // Held motion consumes neither resistance nor neighbor positions or road/path packs.
    for (unsigned i = 0; i < 2; ++i) {
        assert(nm_player_active_step(math,&pack,NULL,NULL,&active,&prepared,NULL));
        assert(prepared.held && prepared.boundary.recovery_ticks == 1-(int)i);
        assert(prepared.boundary.ground.position[2] == 10+i && prepared.boundary.ground.position[1] == 0);
        assert(prepared.force.tire_angle[1] == (i ? 0 : 123));
        assert(prepared.resistance == 42 && prepared.drag_state == 17 && prepared.boundary.damage.override_count == 5);
        assert(prepared.delta[1] == car.delta[1] && prepared.lookahead[1] == car.lookahead[1]);
    }
    const nm_player_t held_before = prepared;
    assert(!nm_player_active_step(math,&pack,NULL,NULL,&active,&prepared,NULL));
    assert(!memcmp(&prepared,&held_before,sizeof(prepared))); // Normal next tick fails atomically.
    prepared.boundary.recovery_ticks = 1; prepared.boundary.displacement[0] = NAN;
    const nm_player_t invalid_held = prepared;
    assert(!nm_player_active_step(math,&pack,NULL,NULL,&active,&prepared,NULL));
    assert(!memcmp(&prepared,&invalid_held,sizeof(prepared))); // Late failure rolls back input and timer.
    assert(!nm_player_prepare_step(NULL,&active,&prepared) && !nm_player_prepare_step(math,NULL,&prepared));
    assert(!nm_player_prepare_step(math,&active,NULL));
    assert(!nm_player_active_step(NULL,&pack,NULL,NULL,&active,&prepared,NULL));
    assert(!nm_player_active_step(math,NULL,NULL,NULL,&active,&prepared,NULL));
    assert(!nm_player_active_step(math,&pack,NULL,NULL,NULL,&prepared,NULL));
    assert(!nm_player_active_step(math,&pack,NULL,NULL,&active,NULL,NULL));
    assert(!memcmp(&prepared,&invalid_held,sizeof(prepared)));
    math->resistance[4] = 0; active.proximity_disabled = 1; active.neighbor[0][0] = 0;
    nm_road_profile_t road_profiles[2] = {{.last_lane=7},{.last_lane=7}};
    nm_road_map_t road_maps[2] = {{1,0x123},{0,0x456}};
    nm_road_pack_t road = {.profiles=road_profiles,.mapping=road_maps,.profile_count=2,.map_count=2};
    prepared = car;
    uint8_t mark_value = 99;
    nm_road_marks_t marks = {&mark_value,1,0};
    const nm_player_t normal_before = prepared;
    marks.count = 0;
    assert(!nm_player_active_step(math,&pack,&road,NULL,&active,&prepared,&marks));
    assert(!memcmp(&prepared,&normal_before,sizeof(prepared)) && mark_value == 99 && !marks.timer);
    marks.count = 1;
    for (unsigned i = 0; i < 3; ++i) assert(nm_player_active_step(math,&pack,&road,NULL,&active,&prepared,&marks));
    assert(!prepared.held && prepared.accel == 123 && prepared.brake == 17);
    assert(prepared.boundary.ground.position[2] == 12 && prepared.phase[0] == 1);
    nm_road_awards_t awards = {0}; awards.ticks[0][1] = 100; awards.cue[0] = 42; awards.signal[0] = 17;
    road.awards = &awards;
    nm_player_tick_input_t tick = {.active=active};
    for (unsigned gate = 0; gate < 8; ++gate) {
        prepared = car; prepared.boundary.recovery_ticks = 2;
        prepared.boundary.damage.flags = gate&1 ? 1u<<21 : 0;
        tick.phase = gate&2 ? 22 : 21; tick.award.blocked = gate&4 ? 1 : 0;
        nm_checkpoint_award_t award = {.remaining=70};
        assert(nm_player_tick_step(math,&pack,&road,NULL,&tick,&prepared,&award,&marks));
        const int called = gate == 3;
        assert(award.calls == (unsigned)called && award.remaining == (unsigned)(70+100*called));
        assert(award.cue_count == 2*called && !(prepared.boundary.damage.flags&(1u<<21)));
        assert(!called || (prepared.boundary.pivot_signal&255) == 17);
    }
    prepared = car; prepared.boundary.recovery_ticks = 2; prepared.boundary.displacement[0] = NAN;
    prepared.boundary.damage.flags = 1u<<21; tick.phase = 22; tick.award.blocked = 0;
    nm_checkpoint_award_t award = {.remaining=70}; const nm_checkpoint_award_t award_before = award;
    const nm_player_t tick_before = prepared;
    assert(!nm_player_tick_step(math,&pack,&road,NULL,&tick,&prepared,&award,&marks));
    assert(!memcmp(&prepared,&tick_before,sizeof(prepared)) && !memcmp(&award,&award_before,sizeof(award)));
    assert(!nm_player_tick_step(math,&pack,&road,NULL,NULL,&prepared,&award,&marks));
    assert(!nm_player_tick_step(math,&pack,&road,NULL,&tick,&prepared,NULL,&marks));
    nm_player_t full = car;
    full.phase[0] = 7; full.phase[1] = -3; full.path_flags = 0xabcd;
    assert(nm_player_step(math,&pack,&road,NULL,&input,&full));
    assert(full.boundary.ground.position[2] == 10 && full.phase[0] == 1 && full.phase[1] == 7);
    assert(full.path_flags == 0x123 && full.previous_path_flags == 0xabcd);
    assert(full.boundary.action == NM_BOUNDARY_NONE && full.boundary.speed == 1);
    // Carry native state through subsequent ticks, without refreshing from snapshots.
    for (unsigned i = 0; i < 2; ++i) assert(nm_player_step(math,&pack,&road,NULL,&input,&full));
    assert(full.boundary.ground.position[2] == 12 && full.phase[1] == 1 && full.previous_path_flags == 0x123);
    const nm_player_t full_before = full;
    assert(!nm_player_step(NULL,&pack,&road,NULL,&input,&full));
    assert(!nm_player_step(math,NULL,&road,NULL,&input,&full));
    assert(!nm_player_step(math,&pack,NULL,NULL,&input,&full));
    assert(!nm_player_step(math,&pack,&road,NULL,NULL,&full));
    assert(!nm_player_step(math,&pack,&road,NULL,&input,NULL));
    road.slot = 1;
    assert(!nm_player_step(math,&pack,&road,NULL,&input,&full));
    road.slot = 0; road_maps[0].profile = 2;
    assert(!nm_player_step(math,&pack,&road,NULL,&input,&full)); // Late road failure rolls back dynamics too.
    assert(!memcmp(&full,&full_before,sizeof(full)));
    road_maps[0].profile = 1;
    const uint16_t rear_masks[] = {0xffff,0xffff,12,8};
    int8_t ticks[] = {-7,0,7,0};
    nm_player_surface_timers(0,rear_masks,ticks);
    assert(ticks[0] == -7 && !ticks[1] && ticks[2] == 7 && !ticks[3]);
    nm_player_surface_timers(-1,rear_masks,ticks);
    assert(ticks[0] == 32 && !ticks[1] && ticks[2] == 7 && ticks[3] == 32);
    nm_player_surface_timers(400,rear_masks,ticks);
    assert(ticks[0] == 32 && !ticks[1] && ticks[2] == 7 && ticks[3] == 32);
    nm_player_t car_before = car;
    assert(!nm_player_dynamics_step(NULL,&pack,NULL,NULL,&input,&car));
    assert(!nm_player_dynamics_step(math,NULL,NULL,NULL,&input,&car));
    assert(!nm_player_dynamics_step(math,&pack,NULL,NULL,NULL,&car));
    assert(!nm_player_dynamics_step(math,&pack,NULL,NULL,&input,NULL));
    input.course_slot = 1;
    assert(!nm_player_dynamics_step(math,&pack,NULL,NULL,&input,&car));
    input.course_slot = 0; car.gear = 4;
    assert(!nm_player_dynamics_step(math,&pack,NULL,NULL,&input,&car));
    car.gear = 0;
    assert(!memcmp(&car,&car_before,sizeof(car)));
    // A late movement/ground rejection must roll back the entire force chain.
    car.guided_ticks = 1; car_before = car;
    assert(!nm_player_dynamics_step(math,&pack,NULL,NULL,&input,&car));
    assert(!memcmp(&car,&car_before,sizeof(car)));
    nm_path_row_t rows[10] = {0};
    nm_path_pack_t path = {.count=2,.records=10,.rows=rows};
    nm_road_profile_t profile = {.first_lane=0,.last_lane=7};
    for (unsigned i = 0; i < 10; ++i) {
        rows[i].xyz[0] = 8; rows[i].xyz[2] = 9; rows[i].speed = 1;
    }
    for (unsigned i = 0; i < 8; ++i) {
        profile.lanes[i].xyz[0] = 8; profile.lanes[i].xyz[2] = 9;
    }
    // Negative condition permits profile sampling but holds heading on surface2.
    car.lateral.control = 1; input.guided_scale = 1; car.boundary.damage.initial_inhibit = 255;
    assert(nm_player_dynamics_step(math,&pack,&path,&profile,&input,&car));
    assert(!car.guided_ticks && car.profile_updates == 1 && !car.lateral.control);
    assert(car.desired_speed == 1 && car.boundary.ground.position[2] == 10 && car.boundary.ground.mask[0] == 2);
    road_profiles[0] = profile;
    full = car; full.guided_ticks = 1; full.phase[0] = 0; full.path_flags = 0; full.lateral.control = 1;
    assert(nm_player_step(math,&pack,&road,&path,&input,&full));
    assert(!full.guided_ticks && full.profile_updates == 2 && full.phase[0] == 1);
    assert(full.boundary.ground.position[2] == 11 && full.boundary.action == NM_BOUNDARY_NONE);
    // This ROM-free fixture has zero math tables: keep the route target one
    // unit away, where its stub square-root lookup remains finite.
    for (unsigned i = 0; i < 10; ++i) rows[i].xyz[2] = 12;
    full.guided_ticks = 1; full.phase[0] = -1; full.lateral.control = 1;
    nm_player_t invalid_profile = full;
    assert(!nm_player_step(math,&pack,&road,&path,&input,&full));
    assert(!memcmp(&full,&invalid_profile,sizeof(full)));
    // An invalid unused old phase must not suppress the route-row guided branch.
    full.lateral.control = 0;
    assert(nm_player_step(math,&pack,&road,&path,&input,&full));
    assert(full.phase[0] == 1 && full.phase[1] == -1 && !full.guided_ticks);
    car.guided_ticks = 0; pack.quad_count = 0; car_before = car;
    assert(!nm_player_dynamics_step(math,&pack,NULL,NULL,&input,&car));
    assert(!memcmp(&car,&car_before,sizeof(car)));
    pack.quad_count = 2;
    nm_wheels_state_t wheel_before = wheels;
    assert(!nm_wheels_ground(NULL,math,&wheels) && !nm_wheels_ground(&pack,NULL,&wheels));
    assert(!nm_wheels_ground(&pack,math,NULL) && !memcmp(&wheels,&wheel_before,sizeof(wheels)));
    wheels.position[0] = NAN; wheel_before = wheels;
    assert(!nm_wheels_ground(&pack,math,&wheels) && !memcmp(&wheels,&wheel_before,sizeof(wheels)));
    wheels.position[0] = 8; wheels.road = pack.quad_count; lists[0] = 0; wheel_before = wheels;
    assert(!nm_wheels_ground(&pack,math,&wheels) && !memcmp(&wheels,&wheel_before,sizeof(wheels)));
    // A failure in the final road-frame calculation must not publish earlier probes.
    wheels.road = 0; quads[0].xyz[0][0] = FLT_MAX; quads[0].xyz[1][0] = FLT_MAX; wheel_before = wheels;
    assert(!nm_wheels_ground(&pack,math,&wheels) && !memcmp(&wheels,&wheel_before,sizeof(wheels)));
    nm_contact_unload(&pack); free(math);
    puts("PASS: contact boundaries, wheel ground state, connected player movement/contact and whole-chain atomic rejection");
    return 0;
}
