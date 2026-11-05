#include "road_check.h"
#include "../include/race.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void test_selection(void) {
    nm_road_map_t map[] = {{1,0x8040},{0,0x1002}};
    nm_road_pack_t p = {.mapping=map,.profile_count=2,.map_count=2};
    nm_road_selection_t s = {.road=0,.phase=17,.previous_phase=18,.flags=19,.previous_flags=20};
    assert(nm_road_select(&p,NULL,NULL,0,UINT32_MAX,NAN,INFINITY,&s));
    assert(s.phase == 1 && s.previous_phase == 17 && s.flags == 0x8040 && s.previous_flags == 19);
    s.road = 1;
    assert(nm_road_select(&p,NULL,NULL,0,0,0,0,&s));
    assert(s.phase == 0 && s.previous_phase == 1 && s.flags == 0x1002 && s.previous_flags == 0x8040);
    nm_drive_params_t *math = calloc(1,sizeof(*math)); assert(math);
    nm_path_row_t rows[10] = {0}; rows[1].road = 1;
    nm_path_pack_t path = {.count=2,.records=10,.rows=rows};
    s.road = 0x98761234;
    assert(nm_road_select(&p,&path,math,4,0,1,0,&s));
    assert(s.road == 0x98760000 && s.offset == 28 && s.phase == 1 && s.previous_phase == 0);
    assert(nm_road_select(&p,&path,math,4,1u<<28,-1,0,&s));
    assert(s.road == 0x98760001 && s.offset == 0 && s.phase == 0 && s.previous_phase == 1);
    assert(nm_road_select(&p,&path,math,4,1u<<28,-1,0,&s));
    assert(s.road == 0x98760000 && s.offset == 56 && s.phase == 1);
    assert(s.flags == 0x1002 && s.previous_flags == 0x8040);
    const nm_road_selection_t before = s;
    assert(!nm_road_select(&p,NULL,NULL,0,0,0,0,&s)); // Normal branch must not truncate road ID.
    assert(!nm_road_select(NULL,&path,math,4,0,0,0,&s));
    assert(!nm_road_select(&p,NULL,math,4,0,0,0,&s));
    assert(!nm_road_select(&p,&path,NULL,4,0,0,0,&s));
    assert(!nm_road_select(&p,&path,math,4,0,NAN,0,&s));
    assert(!nm_road_select(&p,&path,math,4,0,0,0,NULL));
    path.slot = 1;
    assert(!nm_road_select(&p,&path,math,4,0,0,0,&s));
    path.slot = 0; rows[2].road = 2;
    assert(!nm_road_select(&p,&path,math,4,0,1,0,&s)); // Cursor succeeded; map bounds fail atomically.
    rows[2].road = 0; map[0].profile = 2;
    assert(!nm_road_select(&p,&path,math,4,0,1,0,&s));
    assert(!memcmp(&before,&s,sizeof(s)));
    free(math);
}

static void test_checkpoints(void) {
    assert(sizeof(nm_lap_t) == 12 && sizeof(nm_checkpoints_t) == 28 && sizeof(nm_lap_write_t) == 8);
    assert(nm_checkpoint_test_hash() == NM_CHECKPOINT_HASH);
    nm_checkpoint_case_t v; nm_checkpoint_test_case(3,&v);
    const nm_checkpoint_case_t before = v;
    nm_road_pack_t road = {.profile_count=196,.checkpoint_count=1};
    assert(!nm_checkpoint_step(&road,v.current,&v.previous,&v.flags,&v.lap,&v.race,&v.write));
    road.checkpoints = v.points; road.checkpoint_count = 32;
    assert(!nm_checkpoint_step(&road,v.current,&v.previous,&v.flags,&v.lap,&v.race,&v.write));
    road.checkpoint_count = 0; road.profile_count = 1;
    assert(!nm_checkpoint_step(&road,v.current,&v.previous,&v.flags,&v.lap,&v.race,&v.write));
    road.profile_count = 4098;
    assert(!nm_checkpoint_step(&road,v.current,&v.previous,&v.flags,&v.lap,&v.race,&v.write));
    road.profile_count = 196;
    assert(!nm_checkpoint_step(NULL,v.current,&v.previous,&v.flags,&v.lap,&v.race,&v.write));
    assert(!nm_checkpoint_step(&road,v.current,NULL,&v.flags,&v.lap,&v.race,&v.write));
    assert(!nm_checkpoint_step(&road,v.current,&v.previous,NULL,&v.lap,&v.race,&v.write));
    assert(!nm_checkpoint_step(&road,v.current,&v.previous,&v.flags,NULL,&v.race,&v.write));
    assert(!nm_checkpoint_step(&road,v.current,&v.previous,&v.flags,&v.lap,NULL,&v.write));
    assert(!nm_checkpoint_step(&road,v.current,&v.previous,&v.flags,&v.lap,&v.race,NULL));
    assert(!memcmp(&v,&before,sizeof(v)));
    puts("PASS: 65536 checkpoint/lap cases, original i960 digest37831200; invalid inputs atomic");
}

static void test_awards(void) {
    assert(sizeof(nm_road_awards_t) == 4224 && sizeof(nm_checkpoint_award_t) == 32 && sizeof(nm_checkpoint_award_input_t) == 16);
    nm_road_awards_t p = {0};
    for (unsigned d = 0; d < 4; ++d) {
        for (unsigned n = 0; n < 257; ++n) p.ticks[d][n] = d*1000+n;
        p.adjustment[0][d] = 7; p.adjustment[1][d] = 11;
    }
    for (unsigned n = 0; n < 16; ++n) { p.cue[n] = n; p.signal[n] = (uint8_t)(n+17); }
    for (unsigned d = 0; d < 4; ++d) for (unsigned n = 0; n < 256; ++n) {
        nm_checkpoint_award_t s = {.remaining=UINT32_MAX,.calls=UINT32_MAX,.counter=(uint8_t)n};
        nm_checkpoint_award_input_t in = {.difficulty=(uint8_t)d,.penalty=1,.player_flags=1u<<28};
        int16_t signal = -256;
        assert(nm_checkpoint_award_step(&p,&in,&s,&signal));
        assert(s.remaining == d*1000+n+4 && !s.calls && s.display == 128 && s.counter == (uint8_t)(n+1));
        assert(s.cue_count == 2 && s.cues[0] == (n&15) && s.cues[1] == 0xae2010);
        assert((uint16_t)signal == (0xff00u|(17+(n&15))));
    }
    nm_checkpoint_award_t s = {.counter=255};
    nm_checkpoint_award_input_t in = {.difficulty=4}; int16_t signal = 123;
    const nm_checkpoint_award_t before = s;
    assert(!nm_checkpoint_award_step(&p,&in,&s,&signal) && !memcmp(&before,&s,sizeof(s)) && signal == 123);
    in.difficulty = 0; p.cue[15] = 0xae2108;
    assert(nm_checkpoint_award_step(&p,&in,&s,&signal));
    assert(s.cue_count == 3 && s.cues[0] == 0xae207c && s.cues[1] == 0xae2108 && s.cues[2] == 0xae2010);
    assert(s.announcement_ticks == 220 && signal == 123 && !s.counter && s.remaining == 256);
    in.blocked = 1; in.difficulty = 255;
    assert(nm_checkpoint_award_step(NULL,&in,&s,&signal) && s.calls == 2 && !s.cue_count);
    assert(!nm_checkpoint_award_step(&p,NULL,&s,&signal) && !nm_checkpoint_award_step(&p,&in,NULL,&signal));
    assert(!nm_checkpoint_award_step(&p,&in,&s,NULL));
    puts("PASS: native checkpoint awards, wide counter lookup, silent intents and atomic invalid failure");
}
static void test_race_timer(void) {
    /* Original03560 decision boundaries; full instruction oracle lives in
     * --check-native-race. Explicit expected states keep these ROM-free. */
    const struct {
        uint8_t mode; uint16_t gate; uint32_t hold;
        uint32_t remaining,clock,finish,next_remaining,next_clock,next_finish;
        uint8_t phase;
    } cases[]={
        {0,0,0,3200,0,0,3199,1,0,22},
        {0,1,0,3200,0,0,3200,1,0,22},
        {1,1,0,3200,0,0,3199,1,0,22},
        {0,0,1,3200,0,0,3200,1,0,22},
        {0,0,0,1,7,0,0,8,0,22},
        {0,0,0,0,7,3,UINT32_MAX,7,3,25},
        {0,0,0,2,7,3,1,7,2,23},
        {0,0,1,UINT32_MAX,7,3,UINT32_MAX,7,3,25},
        {0,1,0,UINT32_MAX,7,3,UINT32_MAX,7,2,23},
        {0,0,0,2,UINT32_MAX,2,1,0,2,22},
        {0,0,0,0x80000000u,7,0,0x7fffffffu,8,0,22},
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        uint32_t remaining=cases[i].remaining,clock=cases[i].clock,finish=cases[i].finish;
        assert(nm_race_active_timer(cases[i].mode,cases[i].gate,cases[i].hold,
            &remaining,&clock,&finish)==cases[i].phase);
        assert(remaining==cases[i].next_remaining && clock==cases[i].next_clock &&
            finish==cases[i].next_finish);
    }
    puts("PASS: race timer gates, zero grace tick, timeout-before-finish and unsigned wrap");
    uint32_t finish_timer=384;
    for(unsigned tick=0;tick<384;++tick) assert(nm_race_finish_timer(&finish_timer,0,0)==24);
    assert(finish_timer==0 && nm_race_finish_timer(&finish_timer,0,0)==27);
    assert(finish_timer==UINT32_MAX);
    for(int rank=-128;rank<=127;++rank) for(unsigned mode=0;mode<2;++mode) {
        finish_timer=0;
        assert(nm_race_finish_timer(&finish_timer,(int8_t)rank,mode)==(rank>2 || mode ? 25 : 27));
    }
    finish_timer=0x80000000u;
    assert(nm_race_finish_timer(&finish_timer,0,0)==24 && finish_timer==0x7fffffffu);
    puts("PASS: finish timer384 through zero, all signed ranks, mode gate and wrap");
    for(unsigned variant=0;variant<2;++variant) {
        uint32_t mode=variant,timer=nm_race_timeout_ticks(mode);
        const unsigned duration=variant ? 481 : 673;
        for(unsigned tick=1;tick<duration;++tick) {
            assert(nm_race_timeout_timer(&timer,&mode)==26 && mode==variant);
        }
        assert(timer==128 && nm_race_timeout_timer(&timer,&mode)==29 && timer==127 && !mode);
    }
    uint32_t mode=9,timer=0x80000000u;
    assert(nm_race_timeout_timer(&timer,&mode)==26 && timer==0x7fffffffu && mode==9);
    timer=0;
    assert(nm_race_timeout_timer(&timer,&mode)==29 && timer==UINT32_MAX && mode==0);
    puts("PASS: timeout delays, strict128 boundary, mode clearing and signed wrap");
}
static void test_race_settings(void) {
    nm_road_awards_t awards = {0};
    const uint32_t ticks[] = {0,1,0x7fffffffu,0xffffffffu};
    for (unsigned d=0;d<4;++d) awards.ticks[d][0]=ticks[d];
    nm_road_pack_t road = {.awards=&awards};
    nm_race_settings_t settings = {0};
    for (unsigned slot=0;slot<4;++slot) for(unsigned laps=1;laps<=255;++laps)
    for(unsigned difficulty=0;difficulty<4;++difficulty) {
        road.slot=slot; road.total_laps=laps;
        assert(nm_race_settings(&road,difficulty,&settings));
        assert(settings.initial_ticks==ticks[difficulty] && settings.total_laps==laps &&
            settings.difficulty==difficulty);
    }
    const nm_race_settings_t before=settings;
    assert(!nm_race_settings(NULL,0,&settings));
    assert(!nm_race_settings(&road,0,NULL));
    assert(!nm_race_settings(&road,4,&settings));
    road.slot=4; assert(!nm_race_settings(&road,0,&settings));
    road.slot=0; road.total_laps=0; assert(!nm_race_settings(&road,0,&settings));
    road.total_laps=256; assert(!nm_race_settings(&road,0,&settings));
    road.total_laps=8; road.awards=NULL; assert(!nm_race_settings(&road,0,&settings));
    assert(!memcmp(&before,&settings,sizeof(settings)));
    puts("PASS: native race settings, 4080 selections, missing/invalid metadata rejected atomically");
}
int main(int argc, char **argv) {
    if (argc > 1) {
        nm_road_pack_t p = {0};
        if (!nm_road_load(&p,argv[1])) {
            assert(!p.storage && !p.profiles && !p.mapping && !p.payload_bytes);
            return 1;
        }
        assert(!nm_road_load(&p,argv[1]));
        if (argc > 2) assert(p.total_laps == (unsigned)strtoul(argv[2],NULL,10));
        if (p.grid_count) assert(p.grid && !((uintptr_t)p.grid&31));
        if (p.start) assert(p.grid_speeds && !((uintptr_t)p.grid_speeds&31) && !((uintptr_t)p.start&31));
        if (p.grid_guided_scales) assert(p.start && !((uintptr_t)p.grid_guided_scales&31));
        for (unsigned i = 0; i < p.profile_count; ++i) for (unsigned lane = 0; lane < 8; ++lane) {
            nm_road_lateral_t s = {.current=(int16_t)(lane*256),.target=(int16_t)(lane*256)};
            nm_road_pose_t pose;
            assert(nm_road_pose_step(p.profiles+i,&s,&pose));
        }
        printf("road slot=%lu profiles=%lu maps=%lu payload=%lu PASS\n",
               (unsigned long)p.slot,(unsigned long)p.profile_count,(unsigned long)p.map_count,(unsigned long)p.payload_bytes);
        printf("checkpoint award hash=%08lx\n",(unsigned long)nm_checkpoint_award_test_hash(p.awards));
        nm_road_unload(&p); nm_road_unload(&p);
        assert(!p.storage && !p.profiles && !p.mapping && !p.payload_bytes);
        return 0;
    }
    test_selection();
    test_checkpoints();
    test_awards();
    test_race_settings();
    test_race_timer();
    assert(sizeof(nm_road_profile_t) == 164 && sizeof(nm_road_lane_t) == 20);
    assert(nm_road_test_hash() == NM_ROAD_CHECK_HASH);
    assert(nm_controls_test_hash() == NM_CONTROLS_CHECK_HASH);
    for (unsigned wheel = 123; wheel <= 133; ++wheel)
        assert(nm_controls_decode(wheel,56,56,0,0).steer == 0);
    const nm_controls_t released = nm_controls_decode(128,56,56,255,255);
    assert(!released.accel && !released.brake && !released.accel_high_edge && !released.brake_high_edge);
    const nm_controls_t pressed = nm_controls_decode(255,184,152,0,0);
    assert(pressed.steer == 127 && pressed.accel == 255 && pressed.brake == 255 &&
           pressed.accel_high_edge == 1 && pressed.brake_high_edge == 1);
    nm_vehicle_input_t vehicle = {0}, vehicle_before = vehicle;
    nm_controls_t bad_controls = {128,0,0,0,0};
    assert(!nm_vehicle_input_step(&vehicle,&bad_controls,0,100,0));
    assert(!memcmp(&vehicle,&vehicle_before,sizeof(vehicle)));
    assert(!nm_vehicle_input_step(NULL,&pressed,0,100,0));
    assert(!nm_vehicle_input_step(&vehicle,NULL,0,100,0));
    nm_road_profile_t p; nm_road_lateral_t state; nm_road_pose_t pose = {{1,2,3},{4,5,6}};
    nm_road_test_case(2000, &p, &state);
    const nm_road_lateral_t before = state;
    const nm_road_pose_t before_pose = pose;
    p.first_lane = -1;
    assert(!nm_road_pose_step(&p, &state, &pose));
    p.first_lane = 0; p.last_lane = 8;
    assert(!nm_road_pose_step(&p, &state, &pose));
    p.first_lane = 7; p.last_lane = 6;
    assert(!nm_road_pose_step(&p, &state, &pose));
    p.first_lane = 0; p.last_lane = 7;
    for (unsigned lane = 0; lane < 8; ++lane) p.lanes[lane].xyz[0] = NAN;
    assert(!nm_road_pose_step(&p, &state, &pose));
    assert(!nm_road_pose_step(NULL, &state, &pose));
    assert(!nm_road_pose_step(&p, NULL, &pose));
    assert(!nm_road_pose_step(&p, &state, NULL));
    assert(!memcmp(&state, &before, sizeof(state)) && !memcmp(&pose, &before_pose, sizeof(pose)));
    printf("PASS: %u road lateral/spawn vectors, i960/TGP digest %08x; invalid inputs leave outputs unchanged\n",
           NM_ROAD_CHECK_CASES, NM_ROAD_CHECK_HASH);
    printf("PASS: %u controls vectors, i960 digest %08x; ADC deadzone/pedal edges and invalid-input guards\n",
           NM_CONTROLS_CHECK_CASES,NM_CONTROLS_CHECK_HASH);
    return 0;
}
