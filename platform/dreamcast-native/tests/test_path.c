#include "path_check.h"
#include "guided_check.h"
#undef NDEBUG
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

/* Field by field: the state's last byte is padding, which a copy need not keep. */
static int lane_same(const nm_guided_lane_state_t *a,const nm_guided_lane_state_t *b) {
    return a->target==b->target && a->control==b->control && a->remembered==b->remembered;
}

int main(int argc, char **argv) {
    _Static_assert(sizeof(nm_path_row_t) == 28 && sizeof(nm_path_cursor_t) == 8,"path layouts");
    nm_path_pack_t p = {0};
    if (argc > 1) {
        if (!nm_path_load(&p,argv[1])) return 1;
        assert(!nm_path_load(&p,argv[1]));
        if (argc == 3) {
            nm_drive_params_t *math = nm_drive_load(argv[2]); assert(math);
            const uint32_t hash = nm_path_test_hash(&p,math);
            assert(hash == nm_path_expected_hash(p.slot,p.route));
            printf("path slot=%u route=%u cases=%u hash=%08x\n",p.slot,p.route,nm_path_test_count(&p),hash);
            const uint32_t guided = nm_guided_test_hash(&p,math);
            assert(guided == nm_guided_expected_hash(p.slot,p.route));
            printf("guided slot=%u route=%u cases=%u hash=%08x (includes rejected conversions)\n",p.slot,p.route,NM_GUIDED_CHECK_CASES,guided);
            free(math);
        }
        nm_path_unload(&p); assert(!p.rows && !p.count); return 0;
    }
    nm_drive_params_t *math = calloc(1,sizeof(*math)); assert(math);
    p.count = 2; p.records = 10; p.rows = calloc(p.records,sizeof(*p.rows)); assert(p.rows);
    p.rows[0].road = 7; p.rows[1].road = 8; p.rows[2].road = 9;
    nm_path_cursor_t s = {0,99,0x98761234};
    assert(nm_path_cursor_step(&p,math,1,0,0,&s));
    assert(s.offset == 28 && s.matched == 0 && s.road == 0x98760007);
    assert(nm_path_cursor_step(&p,math,1,0,0,&s));
    assert(s.offset == 0 && s.matched == 1 && s.road == 0x98760008);
    s.offset = 28;
    assert(nm_path_cursor_step(&p,math,-1,0,1,&s));
    assert(s.offset == 0 && s.matched == 1 && s.road == 0x98760008);
    assert(nm_path_cursor_step(&p,math,-1,0,1,&s));
    assert(s.offset == 56 && s.matched == 0 && s.road == 0x98760007);
    assert(nm_path_cursor_step(&p,math,-1,0,1,&s));
    assert(s.offset == 28 && s.matched == 2 && s.road == 0x98760009);
    const nm_path_cursor_t before = s;
    assert(!nm_path_cursor_step(NULL,math,0,0,0,&s));
    assert(!nm_path_cursor_step(&p,NULL,0,0,0,&s));
    assert(!nm_path_cursor_step(&p,math,0,0,0,NULL));
    assert(!nm_path_cursor_step(&p,math,NAN,0,0,&s));
    assert(!nm_path_cursor_step(&p,math,0,INFINITY,0,&s));
    p.rows[1].xyz[0] = -FLT_MAX;
    assert(!nm_path_cursor_step(&p,math,FLT_MAX,0,0,&s));
    assert(!memcmp(&before,&s,sizeof(s)));
    for (unsigned offset = 0; offset < 65536; ++offset) {
        s.offset = (uint16_t)offset;
        if (offset%28 || offset/28 >= p.records) assert(!nm_path_cursor_step(&p,math,0,0,0,&s));
    }
    /* The guard records after the course's rows are read as the source reads them (finish entry da58 sets
     * (profile+5)*28): the row's road, the forward wrap at count. */
    p.rows[p.count+3].road = 11; p.rows[p.count+3].xyz[0] = 0;
    s = (nm_path_cursor_t){(uint16_t)((p.count+3)*28),0,0x98760000};
    assert(nm_path_cursor_step(&p,math,1,0,0,&s) && s.matched == p.count+3 && s.road == 0x9876000b && s.offset == 4*28);
    p.rows[1].xyz[0] = 0; p.rows[1].xyz[1] = 8;
    for (unsigned i = 0; i < p.records; ++i) { p.rows[i].speed = 4; p.rows[i].lateral = 500; }
    /* Finite synthetic table for branch tests; real arithmetic uses pack/oracle hashes. */
    for (unsigned i = 0; i < 16384; ++i) math->reciprocal[i] = 0x3f800000;
    math->guided_adjust_limit = 0.25f; math->guided_player_target[7] = 0.125f;
    nm_guided_player_input_t in = {.scale=2,.condition=1,.mode=7};
    nm_guided_player_state_t g = {0};
    g.position[1] = 2; g.adjustment_target = 1; g.lateral.control = 1;
    assert(nm_guided_player_step(&p,math,NULL,&in,&g));
    assert(g.desired_speed == 8.25f && g.adjustment == 0.25f && g.adjustment_target == 1);
    assert(g.lateral.current == 500 && g.lateral.step == 0 && g.lateral.control == 1);
    assert(g.ahead_offset == g.cursor.offset+56 && !g.profile_updates);
    g.adjustment_target = g.adjustment;
    assert(nm_guided_player_step(&p,math,NULL,&in,&g));
    assert(g.desired_speed == 8 && g.adjustment == 0.25f && g.adjustment_target == 0.125f);
    in.condition = 0;
    const nm_guided_player_state_t saved = g;
    assert(!nm_guided_player_step(&p,math,NULL,&in,&g));
    assert(!memcmp(&g,&saved,sizeof(g)));
    nm_road_profile_t profile = {.first_lane=1,.last_lane=6};
    assert(nm_guided_player_step(&p,math,&profile,&in,&g));
    assert(g.lateral.control == 0 && g.profile_updates == 1);
    g.lateral.control = INT16_MIN;
    assert(nm_guided_player_step(&p,math,NULL,&in,&g));
    assert(g.lateral.control == INT16_MAX && g.profile_updates == 1);
    in.speed = NAN;
    const nm_guided_player_state_t valid = g;
    assert(!nm_guided_player_step(&p,math,&profile,&in,&g));
    assert(!memcmp(&g,&valid,sizeof(g)));
    assert(!nm_guided_player_step(NULL,math,&profile,&in,&g));
    assert(!nm_guided_player_step(&p,NULL,&profile,&in,&g));
    assert(!nm_guided_player_step(&p,math,&profile,NULL,&g));
    assert(!nm_guided_player_step(&p,math,&profile,&in,NULL));
    assert(nm_guided_shift(0xfffffffd,1) == -2 && nm_guided_shift(0x80000000,32) == -1);
    assert(nm_guided_shift(0x7fffffff,UINT32_MAX) == 0 && nm_guided_shift(0x80000000,0) == INT32_MIN);
    nm_guided_lane_car_t lane_player={.id=0},lane_car={.id=1,.speed=200,.flags=1024,.mode=1};
    nm_guided_lane_car_t lane_front={.id=2,.speed=300,.position={NAN,0}};
    nm_guided_lane_state_t lane={.target=123,.control=1,.remembered=255};
    const nm_guided_lane_state_t lane_before=lane;
    assert(!nm_guided_lane_step(math,&lane_player,&lane_car,&lane_front,NULL,512,0,&lane));
    assert(lane_same(&lane,&lane_before)); /* Late distance failure rolls back default target. */
    assert(!nm_guided_lane_step(math,&lane_player,&lane_car,NULL,NULL,512,0,&lane));
    lane_car.id=44;
    assert(!nm_guided_lane_step(math,&lane_player,&lane_car,&lane_front,NULL,512,0,&lane));
    assert(lane_same(&lane,&lane_before));
    lane_car.id=1;lane_player.flags=0x10000000u;
    nm_guided_race_input_t race={.player=&lane_player,.car=&lane_car,.heading_circle={NAN,0,8}};
    in.speed=0;
    const nm_guided_player_state_t before_race=g;
    assert(!nm_guided_step(&p,math,&profile,&in,&race,&g));
    assert(!memcmp(&g,&before_race,sizeof(g)));
    nm_path_unload(&p); free(math);
    puts("PASS: path cursor boundaries and guided player profile gate, control wrap, speed-filter lag and atomic rejection");
    return 0;
}
