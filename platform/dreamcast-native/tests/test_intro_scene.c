#include "../include/intro_scene.h"
#include "../include/race_field.h"
#include "../include/lighting.h"
#include "../include/material_pack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
/* Assembly check against separately invoked, source-oracled primitives.
 * This checks field ownership/order/aliases; not a new whole-arcade oracle. */
static void check_response_wiring(const nm_intro_scene_t *scene,const nm_drive_params_t *math,
        const nm_road_pack_t *road) {
    static nm_intro_scene_t trial,before;
    static nm_race_car_t field[44],field_before[44];
    unsigned crashes=0,shakes=0;
    for(unsigned test=0;test<128;++test) {
        trial=*scene;
        const unsigned a=test%3,b=test&4 ? a : (a+1)%3,branch=(test>>3)&1;
        const uint8_t ids[2]={(uint8_t)a,(uint8_t)b};
        const unsigned slot[2]={a,b};
        for(unsigned i=0;i<3;++i) {
            nm_player_t *p=trial.cars+i;
            p->delta[0]=i==a ? (test&16 ? 20.0f : 0.25f) : 0;
            p->delta[1]=0;p->boundary.damage.flags=0;p->boundary.damage.inhibit_ticks=0;
            p->input_inhibit=0;p->response_ticks=0;p->correction[0]=11;p->correction[1]=-7;
            trial.correction_yaw[i]=-123;trial.collision_other_yaw[i]=456;
        }
        trial.cars[0].boundary.damage.sequence_counter=7;
        trial.cars[0].boundary.damage.shake_ticks=0;
        const nm_collision_contact_t contact={.flags=3,.angle={0,16384},
            .side={(test>>5)&3,(test>>5)&3},.depth={0.5f,-0.75f}};
        const nm_collision_damage_input_t input={.global_flags=8,.control=0,.player_mode=0,.curve=0};
        before=trial;
        const unsigned other=a==b ? 0 : 1;
        nm_collision_body_t pos[2];nm_collision_reaction_t reaction[2];
        nm_collision_damage_body_t damage[2];nm_collision_impulse_body_t impulse[2];
        for(unsigned i=0;i<=other;++i) {
            const nm_player_t *p=before.cars+slot[i];
            pos[i]=(nm_collision_body_t){{p->boundary.ground.position[0],p->boundary.ground.position[2]},before.collision_side[slot[i]]};
        }
        assert(nm_collision_separate(math,contact.angle[branch],contact.depth[branch],contact.side[branch],
            pos+(branch ? other : 0),pos+(branch ? 0 : other)));
        for(unsigned i=0;i<=other;++i) {
            const nm_player_t *p=before.cars+slot[i];
            reaction[i]=(nm_collision_reaction_t){{pos[i].position[0],pos[i].position[1]},p->boundary.heading,
                p->lateral.current,(uint16_t)p->phase[0],p->correction_ticks,p->boundary.damage.initial_inhibit};
        }
        int16_t relative;assert(nm_collision_react(math,road,reaction,reaction+other,&relative));
        for(unsigned i=0;i<=other;++i) {
            const nm_player_t *p=before.cars+slot[i];
            damage[i]=(nm_collision_damage_body_t){p->boundary.damage,{p->delta[0],p->delta[1]},p->input_inhibit,ids[i],pos[i].side};
            damage[i].damage.initial_inhibit=reaction[i].initial_inhibit;
            damage[i].damage.sequence_counter=7;damage[i].damage.shake_ticks=0;
        }
        int32_t severity;assert(nm_collision_damage(math,&input,damage,damage+other,&severity));
        for(unsigned i=0;i<=other;++i) {
            const nm_player_t *p=before.cars+slot[i];
            impulse[i]=(nm_collision_impulse_body_t){p->boundary.speed,{p->delta[0],p->delta[1]},
                {11,-7},p->boundary.ground.yaw,-123,456,damage[i].damage.inhibit_ticks,0,0,ids[i],pos[i].side};
        }
        nm_collision_impulse_shared_t shared={(uint16_t)before.cars[0].boundary.damage.signal,damage[other].damage.shake_ticks};
        const nm_collision_impulse_input_t force={test&32 ? 0x18000u : 0,3,severity};
        assert(nm_collision_impulse(math,&force,impulse,impulse+other,&shared));
        memset(field,0xa5,sizeof(field));
        for(unsigned i=0;i<trial.count;++i) {
            field[i].car=before.cars[i];field[i].collision_side=before.collision_side[i];
            field[i].effects.correction_yaw=before.correction_yaw[i];field[i].other_yaw=before.collision_other_yaw[i];
        }
        /* A stale per-car counter must not override the shared owner. */
        field[0].car.boundary.damage.sequence_counter=199;
        nm_race_shared_t context={.crash_counter=7},expected_context=context;
        memcpy(field_before,field,sizeof(field));
        assert(nm_race_collision_response(field,trial.count,&context,math,road,a,b,&contact,branch,force.flags,&input));
        assert(nm_intro_scene_collision_response(&trial,math,road,a,b,ids,&contact,branch,force.flags,&input));
        for(unsigned i=0;i<=other;++i) {
            nm_player_t expected=before.cars[slot[i]];
            expected.boundary.ground.position[0]=pos[i].position[0];expected.boundary.ground.position[2]=pos[i].position[1];
            expected.lateral.current=reaction[i].lateral;expected.correction_ticks=reaction[i].correction_ticks;
            const int8_t shake=expected.boundary.damage.shake_ticks;
            const uint8_t sequence=expected.boundary.damage.sequence_counter;
            expected.boundary.damage=damage[i].damage;
            expected.boundary.damage.shake_ticks=shake;expected.boundary.damage.sequence_counter=sequence;
            expected.boundary.speed=impulse[i].speed;expected.response_ticks=impulse[i].response_ticks;
            memcpy(expected.correction,impulse[i].correction,sizeof(expected.correction));
            if(!slot[i]) {
                expected.boundary.damage.shake_ticks=shared.shake_ticks;expected.boundary.damage.signal=nm_yaw_angle(shared.signal);
                expected.boundary.damage.sequence_counter=damage[other].damage.sequence_counter;
                if(damage[other].damage.auxiliary_request) expected.auxiliary_flags|=UINT32_C(0x80000000);
            }
            assert(!memcmp(&expected,trial.cars+slot[i],sizeof(expected)));
            assert(trial.collision_side[slot[i]]==pos[i].side && trial.correction_yaw[slot[i]]==impulse[i].own_yaw &&
                trial.collision_other_yaw[slot[i]]==impulse[i].other_yaw);
        }
        assert(trial.cars[0].boundary.damage.sequence_counter==damage[other].damage.sequence_counter);
        assert(trial.cars[0].boundary.damage.shake_ticks==shared.shake_ticks);
        for(unsigned i=0;i<trial.count;++i) if(i!=a && i!=b) {
            nm_player_t expected=before.cars[i];
            if(!i) {
                expected.boundary.damage.sequence_counter=damage[other].damage.sequence_counter;
                expected.boundary.damage.shake_ticks=shared.shake_ticks;
                expected.boundary.damage.signal=nm_yaw_angle(shared.signal);
                if(damage[other].damage.auxiliary_request) expected.auxiliary_flags|=UINT32_C(0x80000000);
            }
            assert(!memcmp(&expected,trial.cars+i,sizeof(expected)));
            assert(trial.collision_side[i]==before.collision_side[i] &&
                trial.correction_yaw[i]==before.correction_yaw[i] && trial.collision_other_yaw[i]==before.collision_other_yaw[i]);
        }
        crashes+=trial.cars[0].boundary.damage.sequence_counter!=7;
        shakes+=shared.shake_ticks!=0;
        expected_context.crash_counter=trial.cars[0].boundary.damage.sequence_counter;
        assert(!memcmp(&context,&expected_context,sizeof(context)));
        for(unsigned i=0;i<trial.count;++i) {
            field_before[i].car=trial.cars[i];field_before[i].collision_side=trial.collision_side[i];
            field_before[i].effects.correction_yaw=trial.correction_yaw[i];field_before[i].other_yaw=trial.collision_other_yaw[i];
        }
        field_before[0].car.boundary.damage.sequence_counter=199;
        assert(!memcmp(field,field_before,sizeof(field)));
        before=trial;
        nm_collision_contact_t invalid=contact;invalid.depth[branch]=INFINITY;
        assert(!nm_race_collision_response(field,trial.count,&context,math,road,a,b,&invalid,branch,0,&input));
        assert(!memcmp(field,field_before,sizeof(field)) && !memcmp(&context,&expected_context,sizeof(context)));
        assert(!nm_intro_scene_collision_response(&trial,math,road,a,b,ids,&invalid,branch,0,&input));
        assert(!memcmp(&trial,&before,sizeof(trial)));
        field[b].car.phase[0]=-1;memcpy(field_before,field,sizeof(field));
        assert(!nm_race_collision_response(field,trial.count,&context,math,road,a,b,&contact,branch,0,&input));
        assert(!memcmp(field,field_before,sizeof(field)) && !memcmp(&context,&expected_context,sizeof(context)));
        trial.cars[b].phase[0]=-1;before=trial;
        assert(!nm_intro_scene_collision_response(&trial,math,road,a,b,ids,&contact,branch,0,&input));
        assert(!memcmp(&trial,&before,sizeof(trial)));
    }
    assert(crashes && shakes);
    printf("PASS: response assembly128cases, crashes=%u shakes=%u, both branches, aliases, shared state, atomic rejection; primitive-oracle wiring check\n",crashes,shakes);
    puts("PASS: native field response128cases, shared counter overrides stale player copy, untouched metadata, atomic rejection");
}
static void check_crash_wiring(const nm_intro_scene_t *scene) {
    static nm_intro_scene_t trial,before;
    for(unsigned phase=0;phase<2;++phase) {
        trial=*scene;trial.ticks=phase;trial.cars[0].boundary.damage.sequence_counter=255;
        for(unsigned i=1;i<=2;++i) {
            trial.cars[i].boundary.damage.flags|=64;
            trial.cars[i].boundary.damage.inhibit_ticks=0;trial.cars[i].input_inhibit=0;
            trial.cars[i].boundary.damage.remaining=0;
            nm_damage_state_t expected=trial.cars[i].boundary.damage;
            const uint8_t old=expected.sequence_counter;
            expected.sequence_counter=trial.cars[0].boundary.damage.sequence_counter;
            assert(nm_intro_crash_step(nm_intro_crash_sequences[phase].rows,nm_intro_crash_sequences[phase].count,
                (uint8_t)i,0,&expected));
            const uint8_t shared=expected.sequence_counter;expected.sequence_counter=old;
            assert(nm_intro_scene_crash(&trial,i));
            assert(!memcmp(&expected,&trial.cars[i].boundary.damage,sizeof(expected)));
            assert(trial.cars[0].boundary.damage.sequence_counter==shared);
        }
        assert(trial.crash_starts==2 && trial.crash_ticks==2);
        assert(trial.cars[1].boundary.damage.orientation==3 && trial.cars[2].boundary.damage.orientation==0);
        before=trial;assert(!nm_intro_scene_crash(&trial,0));assert(!memcmp(&before,&trial,sizeof(trial)));
        for(unsigned tick=0;tick<400;++tick) { ++trial.ticks;assert(nm_intro_scene_crash(&trial,1)); }
        assert(!trial.cars[1].boundary.damage.remaining && !trial.cars[1].boundary.damage.overlay_model);
        assert(trial.crash_starts==2);
    }
    puts("PASS: opponent crash wiring, both frame parities, shared counter wrap, actor order and completion");
}
static void check_live_collision_wiring(const nm_intro_scene_t *scene,const nm_drive_params_t *math,
        const nm_road_pack_t *road) {
    static nm_intro_scene_t trial;
    static nm_race_car_t field[44],expected_field[44];
    unsigned contact_signals=0;
    for(unsigned size=0;size<2;++size) for(unsigned phase=0;phase<4;++phase) {
        trial=*scene;
        if(size) {
            trial.count=40;
            for(unsigned i=scene->count;i<trial.count;++i) trial.cars[i]=scene->cars[i%scene->count];
        }
        for(unsigned i=0;i<trial.count;++i) {
            trial.traffic[i].next=(uint16_t)((i+1)%trial.count);
            trial.traffic[i].previous=(uint16_t)((i+trial.count-1)%trial.count);
            trial.cars[i].boundary.damage.flags=0;
            trial.cars[i].boundary.damage.overlay_model=0;
            trial.cars[i].boundary.ground.position[0]=100.0f*(float)i;
            trial.cars[i].boundary.ground.position[2]=0;
            trial.cars[i].phase[0]=100;
        }
        trial.cars[1].boundary.ground.position[0]=0.5f;
        trial.passing.signal=0x6789;
        trial.cars[0].boundary.damage.signal=0x3456;
        trial.cars[0].boundary.pivot_signal=0x1234;
        trial.cars[0].boundary.damage.inhibit_ticks=0;
        trial.cars[0].boundary.ground.yaw=0;trial.cars[1].boundary.ground.yaw=2048;
        assert(nm_intro_scene_collision_prepare(&trial,road));
        for(unsigned i=0;i<trial.count;++i) {
            assert(trial.collision_side[i]==255);
            if(i) assert(!(trial.cars[i].boundary.damage.flags&((1u<<26)|32u)));
        }
        const nm_collision_damage_input_t input={.global_flags=8};
        memset(field,0xa5,sizeof(field));
        for(unsigned i=0;i<trial.count;++i) {
            field[i].car=trial.cars[i];field[i].next=trial.traffic[i].next;
            field[i].collision_candidates=trial.collision_candidates[i];field[i].collision_side=trial.collision_side[i];
            field[i].collision_motion=trial.collision_motion[i];field[i].other_yaw=trial.collision_other_yaw[i];
            field[i].effects.correction_yaw=trial.correction_yaw[i];
        }
        field[0].car.boundary.damage.sequence_counter=199;
        memcpy(expected_field,field,sizeof(field));
        nm_race_shared_t shared={.crash_counter=trial.cars[0].boundary.damage.sequence_counter,.heading_circle={3,7,8}};
        nm_race_collisions_t collisions={.contact=trial.collision_contact,
            .pairs=trial.collision_pairs,.responses=trial.collision_responses};
        assert(nm_race_collide(field,trial.count,&shared,math,road,trial.collision_order,phase,0,&input,&collisions));
        assert(nm_intro_scene_collide(&trial,math,road,phase,0,&input));
        for(unsigned i=0;i<trial.count;++i) {
            expected_field[i].car=trial.cars[i];expected_field[i].collision_side=trial.collision_side[i];
            expected_field[i].other_yaw=trial.collision_other_yaw[i];expected_field[i].effects.correction_yaw=trial.correction_yaw[i];
        }
        expected_field[0].car.boundary.damage.sequence_counter=199;
        assert(!memcmp(field,expected_field,sizeof(field)));
        assert(!memcmp(&collisions.contact,&trial.collision_contact,sizeof(collisions.contact)));
        assert(collisions.pairs==trial.collision_pairs && collisions.responses==trial.collision_responses);
        assert(shared.crash_counter==trial.cars[0].boundary.damage.sequence_counter);
        assert(nm_force_bits(shared.heading_circle[2])==0x40a54413u);
        /* Empty pass preserves the prior circle/contact/counters and every car. */
        for(unsigned i=0;i<trial.count;++i) field[i].collision_candidates=0;
        memcpy(expected_field,field,sizeof(field));
        shared.heading_circle[0]=3;shared.heading_circle[1]=7;shared.heading_circle[2]=8;
        const nm_race_shared_t previous_shared=shared;const nm_race_collisions_t previous_collisions=collisions;
        assert(nm_race_collide(field,trial.count,&shared,math,road,trial.collision_order,phase,0,&input,&collisions));
        assert(!memcmp(field,expected_field,sizeof(field)) && !memcmp(&shared,&previous_shared,sizeof(shared)) &&
            !memcmp(&collisions,&previous_collisions,sizeof(collisions)));
        field[0].next=(uint16_t)trial.count;memcpy(expected_field,field,sizeof(field));
        assert(!nm_race_collide(field,trial.count,&shared,math,road,trial.collision_order,phase,0,&input,&collisions));
        assert(!memcmp(field,expected_field,sizeof(field)) && !memcmp(&shared,&previous_shared,sizeof(shared)) &&
            !memcmp(&collisions,&previous_collisions,sizeof(collisions)));
        assert(trial.passing.signal==0x6789); /*501a02 contact must not touch501a04 view. */
        contact_signals+=trial.cars[0].boundary.pivot_signal!=0x1234;
        assert(trial.collision_pairs && trial.collision_responses && trial.collision_pairs<=7);
        assert(trial.cars[0].boundary.ground.position[0]!=0 || trial.cars[1].boundary.ground.position[0]!=0.5f);
        assert(!nm_intro_scene_collide(&trial,math,road,4,0,&input));
    }
    assert(contact_signals);
    puts("PASS: live preparation/walk/detection/response wiring, all4phases, overlapping cars move");
    puts("PASS: native field live collision pass, intro/40 cars, all4phases, shared owners, empty circle and invalid link");
}

static void check_race_interactions(const nm_intro_scene_t *scene,const nm_drive_params_t *math,
        const nm_road_pack_t *road) {
    static nm_race_car_t cars[44],expected[44];
    for(unsigned attract=0;attract<2;++attract) for(unsigned inhibit=0;inhibit<2;++inhibit)
    for(unsigned phase=0;phase<4;++phase) {
        memset(cars,0,sizeof(cars));
        nm_race_order_t order={0};
        for(unsigned i=0;i<40;++i) {
            cars[i].car=scene->cars[i%scene->count];
            cars[i].car.boundary.damage.flags=0;
            cars[i].car.boundary.damage.overlay_model=0;
            cars[i].car.boundary.ground.position[0]=100.0f*(float)i;
            cars[i].car.boundary.ground.position[2]=0;
            cars[i].car.phase[0]=100;
            cars[i].next=(uint16_t)((i+1)%40);cars[i].previous=(uint16_t)((i+39)%40);
            order.rank[i]=order.track[i]=(uint16_t)i;
        }
        cars[1].car.boundary.ground.position[0]=0.5f;
        cars[0].car.control=15;
        cars[0].car.boundary.damage.signal=0x3456;
        cars[0].car.correction_ticks=3;cars[0].car.correction[0]=1.25f;cars[0].car.correction[1]=-2.5f;
        memcpy(expected,cars,sizeof(cars));
        nm_race_shared_t shared={.flags=8u|(attract ? UINT32_C(0x10000000) : 0),
            .event_timer=21,.heading_circle={3,7,8}},wanted_shared=shared;
        nm_race_order_t wanted_order=order;
        int32_t events[2]={17,19},wanted_events[2]={17,19};
        nm_traffic_pass_t passing={1,39,0x6789},wanted_passing=passing;
        nm_race_collisions_t collisions={0},wanted_collisions=collisions;
        /*Source1210c/121c4: bit28 takes precedence over inhibit. */
        if(attract || !inhibit) {
            uint16_t window[14];
            assert(nm_race_traffic_step(expected,40,&wanted_order,&wanted_shared,math));
            if(!attract) assert(nm_race_signals_step(expected,40,&wanted_shared,wanted_events,&wanted_passing,math));
            assert(nm_race_collision_prepare(expected,40,road->profile_count-1,window));
            const nm_collision_damage_input_t input={wanted_shared.flags,expected[0].car.control,
                expected[0].car.lap.completed,0};
            assert(nm_race_collide(expected,40,&wanted_shared,math,road,window,phase,0,&input,&wanted_collisions));
        }
        assert(nm_race_interactions_step(cars,40,&shared,&order,events,&passing,&collisions,
            math,road,inhibit,phase,0,0));
        assert(!memcmp(cars,expected,sizeof(cars)) && !memcmp(&shared,&wanted_shared,sizeof(shared)));
        assert(!memcmp(&order,&wanted_order,sizeof(order)) && !memcmp(events,wanted_events,sizeof(events)));
        assert(!memcmp(&passing,&wanted_passing,sizeof(passing)) && !memcmp(&collisions,&wanted_collisions,sizeof(collisions)));
        assert(!nm_race_interactions_step(cars,40,&shared,&order,events,&passing,&collisions,
            math,road,inhibit,4,0,0));
        assert(!memcmp(cars,expected,sizeof(cars)) && !memcmp(&shared,&wanted_shared,sizeof(shared)));
        assert(!memcmp(&order,&wanted_order,sizeof(order)) && !memcmp(events,wanted_events,sizeof(events)));
        assert(!memcmp(&passing,&wanted_passing,sizeof(passing)) && !memcmp(&collisions,&wanted_collisions,sizeof(collisions)));
    }
    puts("PASS: native race interactions, normal/attract/inhibited precedence, all4phases, invalid phase unchanged");
}

/*501a04 is shared by passing and damage; the cached passing view is not its
 *owner. No overtake here: a fresh damage signal must survive the next call. */
static void check_signal_ownership(const nm_intro_scene_t *scene,const nm_drive_params_t *math) {
    static nm_intro_scene_t trial;trial=*scene;
    static nm_race_car_t field[44];memset(field,0,sizeof(field));
    for(unsigned i=0;i<trial.count;++i) {
        trial.traffic[i].next=(uint16_t)((i+1)%trial.count);
        trial.traffic[i].previous=(uint16_t)((i+trial.count-1)%trial.count);
        field[i].car=trial.cars[i];field[i].next=trial.traffic[i].next;field[i].previous=trial.traffic[i].previous;
    }
    trial.passing=(nm_traffic_pass_t){trial.traffic[0].next,trial.traffic[0].previous,0x6789};
    nm_traffic_pass_t passing=trial.passing;nm_race_shared_t shared={0};int32_t events[2]={0};
    for(unsigned i=0;i<2;++i) {
        const int16_t signal=(int16_t)(0x3456+i);
        trial.cars[0].boundary.damage.signal=field[0].car.boundary.damage.signal=signal;
        trial.cars[0].boundary.pivot_signal=field[0].car.boundary.pivot_signal=0x1234;
        assert(nm_intro_scene_signals(&trial,math));
        assert(nm_race_signals_step(field,trial.count,&shared,events,&passing,math));
        assert(trial.passing.signal==(uint16_t)signal && trial.cars[0].boundary.damage.signal==signal);
        assert(passing.signal==(uint16_t)signal && field[0].car.boundary.damage.signal==signal);
        assert(trial.cars[0].boundary.pivot_signal==0x1234 && field[0].car.boundary.pivot_signal==0x1234);
    }
    puts("PASS: intro/race passing borrows current damage signal, preserves distinct contact signal");
}
/* Wiring oracle uses spawn indices directly, not the implementation's rotated
 * reset-pool view. Seed aliases/nonzero flags so zero-filled cars cannot hide
 * accidental player reset or loss of the upper gear halfword. */
static void check_near_wiring(const nm_intro_scene_t *scene,const nm_drive_params_t *math) {
    static nm_intro_scene_t trial;trial=*scene;
    assert(nm_intro_scene_near(&trial,math));
    assert(!memcmp(trial.cars,scene->cars,sizeof(trial.cars)) &&
           !memcmp(trial.near_marker,scene->near_marker,sizeof(trial.near_marker)));
    uint32_t flags[44],marker[44];
    for(unsigned i=0;i<trial.count;++i) {
        trial.cars[i].gear=UINT32_C(0xabcd1234)+i;
        trial.cars[i].boundary.damage.flags=UINT32_C(0xa5a56400)|i;
        trial.near_marker[i]=(uint16_t)(0x9876+i);
        flags[i]=trial.cars[i].boundary.damage.flags;
        marker[i]=trial.near_marker[i]|((trial.cars[i].gear&65535u)<<16);
        if(i) { flags[i]&=~UINT32_C(0x6400);marker[i]&=UINT32_C(0xffff0000); }
    }
    unsigned at[2]={0,0};
    for(unsigned step=0;step<8;++step) for(unsigned side=0;side<2;++side) {
        at[side]=side ? trial.traffic[at[side]].previous : trial.traffic[at[side]].next;
        const unsigned i=at[side];assert(i<trial.count);
        marker[i]=8-step;
        const float d=nm_force_length(math,trial.cars[i].boundary.ground.position[0]-trial.cars[0].boundary.ground.position[0],
            trial.cars[i].boundary.ground.position[2]-trial.cars[0].boundary.ground.position[2]);
        if(d<=32) {
            flags[i]|=1u<<10;
            if(!side && d<=8) flags[i]|=1u<<14;
            if(side && step<2) flags[i]|=1u<<13;
        }
    }
    trial.cars[0].profile_updates=99;
    assert(nm_intro_scene_near(&trial,math) && !trial.cars[0].profile_updates);
    for(unsigned i=0;i<trial.count;++i) {
        assert(trial.cars[i].boundary.damage.flags==flags[i]);
        assert(trial.near_marker[i]==(uint16_t)marker[i]);
        assert(trial.cars[i].gear==(UINT32_C(0xabcd0000)|(marker[i]>>16)));
    }
}
/* Generated-header include dir + drive/contact/road/path C readers; owned assets. */
/* Independent host demand check: same body selection and clipped viewport as
 * the partial scene. Not a SH4 numerical or universal full-race budget proof. */
static unsigned special_phase_mask;
static unsigned special_body_parts[3];
static unsigned texture_demand(const nm_intro_scene_t *s,const nm_drive_params_t *math,
        const nm_pack_t *pack,const nm_material_pack_t *materials,uint32_t *offsets,int windows) {
    unsigned used=0,parts=0,crash_shadows=0;
    memset(offsets,255,materials->resources*sizeof(*offsets));
    const nm_view_t view={{248,176},{0,496,0,384},0.01f};
    float light[3];
    for(unsigned a=0;a<3;++a) light[a]=nm_race_world_light[0]*s->view[a]+
        nm_race_world_light[1]*s->view[3+a]+nm_race_world_light[2]*s->view[6+a];
    for(unsigned ordinal=0;ordinal<s->count;++ordinal) {
        const unsigned i=s->detail_order[ordinal];
        const unsigned kind=nm_intro_stages[s->stage].spawn[i].kind;
        if(!s->detail_level[i] && kind!=2) continue;
        const int phase=nm_intro_wheel_phase(s->detail_level[i],(uint16_t)s->cars[i].wheel_phase[0]);
        const unsigned ordinary=s->detail_level[i] ? (phase>=0 ? 2u+(unsigned)windows : 1u) : 0;
        const unsigned shadows=nm_intro_shadow_select(s->detail_level[i],s->cars[i].boundary.damage.overlay_model,&crash_shadows);
        const unsigned attachment=s->detail_level[i] && s->detail_level[i]<=2 && s->cars[i].boundary.damage.overlay_model;
        for(unsigned part=0;part<ordinary+(kind==2)+attachment+shadows;++part) {
        const int extra=kind==2 && part==0;
        const int crash=attachment && part==(kind==2);
        const int shadow=shadows && part==ordinary+(kind==2)+attachment;
        const unsigned body_part=extra || crash ? 0 : part-(kind==2)-attachment;
        if(kind==2 && !extra && !shadow && !crash) ++special_body_parts[body_part];
        assert(++parts<=NM_INTRO_MAX_PARTS);
        if(extra) special_phase_mask|=1u<<((s->ticks ? s->ticks-1 : 0)&7);
        const unsigned binding=crash ? nm_intro_crash_models[s->detail_level[i]-1] : shadow ? nm_intro_shadow_model : extra ? nm_intro_special_phase_models[(s->ticks ? s->ticks-1 : 0)&7] :
            body_part==2 ? nm_intro_window_models[4-s->detail_level[i]] :
            body_part==1 ? nm_intro_wheel_phase_models[phase] :
            kind==2 ? nm_intro_special_model : nm_intro_body_models[i][6-s->detail_level[i]];
        const nm_mesh_t *mesh=pack->meshes+pack->instances[binding].mesh;
        float body[12],m[12];
        assert(shadow ? nm_intro_shadow_pose(math,s->cars+i,body) : extra ? nm_intro_special_pose(math,s->cars+i,body) : nm_intro_body_pose(math,s->cars+i,body));
        assert(nm_pose_compose(s->view,body,m));
        for(unsigned f=0;f<mesh->face_count;++f) {
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            const nm_material_t *material=pack->materials+face->material;
            float normal[3],points[3][3];
            for(unsigned a=0;a<3;++a) {
                normal[a]=face->normal[0]*m[a]+face->normal[1]*m[3+a]+face->normal[2]*m[6+a];
                for(unsigned v=0;v<3;++v) {
                    const float *p=pack->vertices[mesh->vertex_first+face->indices[v]].position;
                    points[v][a]=p[0]*m[a]+p[1]*m[3+a]+p[2]*m[6+a]+m[9+a];
                }
            }
            const int lighting=nm_face_lighting(mesh->mode,normal,points,light,material);
            assert(lighting>=0);
            if((!(face->attribute&(1u<<17)) && (lighting&256)) ||
                !(face->attribute&(3u<<8)) || (material->header[0]&0x8000)) continue;
            const nm_material_binding_t *b=nm_material_find(materials,material->header,lighting&255);
            assert(b);
            if(b->resource==NM_MATERIAL_SOLID) continue;
            for(unsigned t=1;t+1<face->count;++t) {
                const unsigned corners[3]={0,t,t+1};nm_view_vertex_t input[3];
                for(unsigned v=0;v<3;++v) {
                    const float *p=pack->vertices[mesh->vertex_first+face->indices[corners[v]]].position;
                    float q[3];
                    for(unsigned a=0;a<3;++a) q[a]=p[0]*m[a]+p[1]*m[3+a]+p[2]*m[6+a]+m[9+a];
                    input[v]=(nm_view_vertex_t){q[0]*s->camera.focus,q[1]*s->camera.focus,q[2],0,0};
                }
                nm_screen_vertex_t output[NM_CLIP_VERTICES];
                if(nm_project_triangle(&view,input,output)<3) continue;
                assert(nm_material_cache_reserve(materials,b->resource,offsets,&used,UINT32_MAX));
                break;
            }
        }
        }
    }
    return used;
}
#if NATIVE_INTRO_CRASH_TEST == 1
/* Host-only diagnostic: sample the actual shadow against visible world
 * triangles, not the car's approximate contact plane. No target math/path. */
static void inspect_shadow_ground(const nm_intro_scene_t *s,const nm_drive_params_t *math,
        const nm_pack_t *cars,const nm_scenery_pack_t *scenery,const nm_contact_pack_t *contact,const char *root) {
    nm_pack_t world={0};char file[1024];snprintf(file,sizeof(file),"%s/world.dnm",root);
    assert(nm_pack_load(&world,file));
    const nm_mesh_t *shadow=cars->meshes+cars->instances[nm_intro_shadow_model].mesh;
    float pose[12];assert(nm_intro_part_pose_reference(math,s->cars+1,2,pose));
    unsigned found=0,above=0,below=0;float minimum=INFINITY,maximum=-INFINITY;
    uint32_t min_source=0,max_source=0;unsigned min_face=0,max_face=0;
    float contact_min=INFINITY,contact_max=-INFINITY;unsigned contact_hits=0;
    for(unsigned f=0;f<shadow->face_count;++f) {
        const nm_face_t *face=cars->faces+shadow->face_first+f;
        for(unsigned u=0;u<5;++u) for(unsigned v=0;v<5;++v) {
            const float a=u*0.25f,b=v*0.25f,weights[4]={(1-a)*(1-b),a*(1-b),a*b,(1-a)*b};
            float local[3]={0},point[3];
            for(unsigned k=0;k<4;++k) for(unsigned axis=0;axis<3;++axis)
                local[axis]+=weights[k]*cars->vertices[shadow->vertex_first+face->indices[k]].position[axis];
            for(unsigned axis=0;axis<3;++axis)
                point[axis]=local[0]*pose[axis]+local[1]*pose[3+axis]+local[2]*pose[6+axis]+pose[9+axis];
            float nearest=INFINITY;
            uint32_t nearest_source=0;unsigned nearest_face=0;
            for(unsigned c=0;c<s->cells.draw_count;++c) {
                const nm_scenery_cell_t *cell=scenery->cells+s->cells.draw[c];
                for(unsigned j=0;j<cell->count;++j) {
                    const nm_mesh_t *mesh=world.meshes+world.instances[cell->first+j].mesh;
                    if(point[0]<mesh->bounds[0] || point[0]>mesh->bounds[3] ||
                       point[2]<mesh->bounds[2] || point[2]>mesh->bounds[5]) continue;
                    for(unsigned k=0;k<mesh->face_count;++k) {
                        const nm_face_t *surface=world.faces+mesh->face_first+k;
                        for(unsigned t=1;t+1<surface->count;++t) {
                            const float *p=world.vertices[mesh->vertex_first+surface->indices[0]].position;
                            const float *q=world.vertices[mesh->vertex_first+surface->indices[t]].position;
                            const float *r=world.vertices[mesh->vertex_first+surface->indices[t+1]].position;
                            const float x=q[0]-p[0],z=q[2]-p[2],rx=r[0]-p[0],rz=r[2]-p[2];
                            const float determinant=x*rz-z*rx;
                            if(fabsf(determinant)<1e-8f) continue;
                            const float dx=point[0]-p[0],dz=point[2]-p[2];
                            const float bq=(dx*rz-dz*rx)/determinant,br=(x*dz-z*dx)/determinant;
                            if(bq<0 || br<0 || bq+br>1) continue;
                            const float delta=point[1]-(p[1]+bq*(q[1]-p[1])+br*(r[1]-p[1]));
                            if(fabsf(delta)<fabsf(nearest)) {
                                nearest=delta;nearest_source=world.instances[cell->first+j].source_address;
                                nearest_face=mesh->face_first+k;
                            }
                        }
                    }
                }
            }
            if(fabsf(nearest)>1) continue; // Exclude distant structures; report unmatched samples.
            nm_contact_result_t hit={s->cars[1].boundary.ground.position[1],s->cars[1].boundary.ground.road,0,0};
            assert(nm_contact_query(contact,math,point[0],point[2],&hit));
            if(hit.mask==2) {
                const float delta=hit.height-(point[1]-nearest);
                contact_min=fminf(contact_min,delta);contact_max=fmaxf(contact_max,delta);++contact_hits;
            }
            ++found;above+=nearest>0;below+=nearest<0;
            if(nearest<minimum) { min_source=nearest_source;min_face=nearest_face; }
            if(nearest>maximum) { max_source=nearest_source;max_face=nearest_face; }
            minimum=fminf(minimum,nearest);maximum=fmaxf(maximum,nearest);
        }
    }
    assert(found && isfinite(minimum) && isfinite(maximum));
    assert(contact_hits==found && isfinite(contact_min) && isfinite(contact_max));
    printf("Shadow/world sample tick=%u actor1 matched=%u/100 deltaY=[%g,%g] positive/negative=%u/%u; nearest visible XZ surface, not pixel depth or arcade raster parity\n",
        s->ticks,found,(double)minimum,(double)maximum,above,below);
    printf("Shadow/world extrema source/face=%08x/%u,%08x/%u\n",min_source,min_face,max_source,max_face);
    printf("Contact/world tick=%u road-hits=%u/%u deltaY=[%g,%g] max-query-candidates=%u\n",
        s->ticks,contact_hits,found,(double)contact_min,(double)contact_max,contact->max_candidates);
    nm_pack_unload(&world);
}
#endif
int main(int argc,char **argv) {
    if(argc!=5 && argc!=7) return 2;
    nm_pack_t model={0};nm_material_pack_t materials={0};uint32_t *offsets=NULL;
    nm_vertex_t *windows[2]={0};uint16_t base_uv[2][72];unsigned window_counts[2]={0};
    nm_scenery_pack_t scenery={0};
    if(argc==7) {
        assert(nm_pack_load(&model,argv[5]) && nm_material_pack_open_cpu(&materials,argv[6]));
        for(unsigned w=0;w<2;++w) {
            const nm_mesh_t *mesh=model.meshes+model.instances[nm_intro_window_models[w]].mesh;
            assert(mesh->vertex_count && mesh->vertex_count<=36);
            windows[w]=(nm_vertex_t *)model.vertices+mesh->vertex_first;window_counts[w]=mesh->vertex_count;
            for(unsigned v=0;v<mesh->vertex_count;++v) {
                base_uv[w][2*v]=windows[w][v].u;base_uv[w][2*v+1]=windows[w][v].v;
            }
        }
        offsets=malloc(materials.resources*sizeof(*offsets));assert(offsets);
        char file[1024];snprintf(file,sizeof(file),"%s/scenery.dnv",argv[4]);
        assert(nm_scenery_load(&scenery,file));
    }
    nm_drive_params_t *math=nm_drive_load(argv[1]);
    nm_contact_pack_t contact={0};nm_road_pack_t road={0};nm_path_pack_t paths[8]={0};
    assert(math && nm_contact_load(&contact,argv[2]) && nm_road_load(&road,argv[3]));
    for(unsigned i=0;i<8;++i) {
        char file[1024];snprintf(file,sizeof(file),"%s/course0-route%u.dnp",argv[4],i);
        assert(nm_path_load(paths+i,file));
    }
    static nm_intro_scene_t scene;
    assert(nm_intro_scene_start(&scene,math,&road,paths) && scene.count==20);
    check_response_wiring(&scene,math,&road);
    check_crash_wiring(&scene);
    check_live_collision_wiring(&scene,math,&road);
    check_race_interactions(&scene,math,&road);
    check_signal_ownership(&scene,math);
    for(unsigned variant=0;variant<2;++variant) {
        static nm_intro_scene_t restored;restored=scene;
        restored.ticks=variant;restored.restored_camera_mode=variant ? -128 : 2;
        nm_player_t *car=restored.cars;
        car->boundary.damage.remaining=1;car->boundary.damage.row=NM_CRASH_ROWS-1;
        car->boundary.damage.inhibit_ticks=1;car->boundary.damage.spin_ticks=1;
        car->boundary.damage.last_surface=7;car->auxiliary_flags=UINT32_C(0x80000000);
        car->camera_mode=9;car->camera_transition=1;
        assert(nm_intro_scene_step(&restored,math,&contact,&road,paths));
        assert(restored.player_crash_ticks==1 && !car->boundary.damage.remaining);
        assert(car->camera_mode==restored.restored_camera_mode && !car->camera_transition);
        assert(!memcmp(car->camera_words,math->camera_reset_words,sizeof(car->camera_words)));
        assert(!memcmp(car->camera_angles,math->camera_reset_angles,sizeof(car->camera_angles)));
        assert(!(car->auxiliary_flags&UINT32_C(0x80000000)) && car->control==223);
        assert(car->boundary.pivot_signal==(variant ? 42 : 41));
        assert(car->boundary.damage.spin_ticks==1 && car->boundary.damage.last_surface==7);
    }
    puts("PASS: live leader crash completion, selected camera restoration, control and preserved spin");
    {
        static nm_intro_scene_t corrected;corrected=scene;
        corrected.cars[1].correction_ticks=3;
        corrected.correction_yaw[1]=0;corrected.collision_other_yaw[1]=2048;
        assert(nm_intro_scene_step(&corrected,math,&contact,&road,paths));
        assert(corrected.correction_yaw[1]==256 && corrected.cars[1].correction_ticks==2);
        puts("PASS: live scene pose consumes persisted collision other-yaw");
    }
    /* Synthetic nonzero/signed timer inputs exercise scene wiring; these are
     * not claimed as the original boot's timer values. */
    scene.race_timers.short_ticks=3;scene.race_timers.event[0]=2;
    scene.race_timers.event[1]=-5;scene.cars[0].control=4;
    for(unsigned i=0;i<scene.count;++i)
        assert(scene.rank_order[i]==scene.count-1-i && scene.track_order[i]==scene.count-1-i);
    if(offsets) assert(nm_intro_scene_detail(&scene,math,&scenery));
    unsigned movement=0,seen21=0,peak=0,peak_tick=0,window_peak=0,window_tick=0,body_pitch_ticks=0;
    unsigned passing_candidates=0,passing_near=0,passing_changes=0;
    for(unsigned tick=0;tick<6000;++tick) {
#ifdef NATIVE_INTRO_CRASH_TEST
        if(tick==300) {
#if NATIVE_INTRO_CRASH_TEST == 2
            assert(nm_crash_begin(math,0,0,8,&scene.cars[0].boundary.damage));
            if(scene.cars[0].boundary.damage.auxiliary_request) scene.cars[0].auxiliary_flags|=UINT32_C(0x80000000);
#else
            scene.cars[1].boundary.damage.flags|=64;
#endif
        }
#endif
        if(offsets) {
#if NATIVE_INTRO_CRASH_TEST == 1
            if(tick==351 || tick==380 || tick==409) inspect_shadow_ground(&scene,math,&model,&scenery,&contact,argv[4]);
#endif
            unsigned demand=texture_demand(&scene,math,&model,&materials,offsets,0);
            if(demand>peak) { peak=demand;peak_tick=tick; }
            if(!tick) printf("Intro initial clipped texture demand=%u bytes\n",demand);
            // Prospective integration budget; full-axis textures make UV phase
            // irrelevant to residency. This does not claim animation parity.
            demand=texture_demand(&scene,math,&model,&materials,offsets,1);
            if(demand>window_peak) { window_peak=demand;window_tick=tick; }
        }
        const float before=scene.cars[0].boundary.ground.position[0];
        const uint16_t previous_reflection=scene.reflection_phase;
        uint16_t expected_track[44];memcpy(expected_track,scene.track_order,sizeof(expected_track));
        const int previous_lap=scene.traffic_lap;
        const int entering=scene.pending_stage!=UINT32_MAX;
        const nm_race_timers_t old_timers=scene.race_timers;
        const uint32_t old_control=scene.cars[0].control;
        const int old_crash_remaining=scene.cars[0].boundary.damage.remaining;
        const nm_traffic_pass_t old_passing=scene.passing;
        const uint16_t old_damage_signal=(uint16_t)scene.cars[0].boundary.damage.signal;
        const uint16_t old_next=scene.traffic[0].next,old_previous=scene.traffic[0].previous;
        nm_collision_travel_t old_motion[44];memcpy(old_motion,scene.collision_motion,sizeof(old_motion));
        uint32_t old_relative[44];memcpy(old_relative,scene.collision_relative,sizeof(old_relative));
        if(!nm_intro_scene_step(&scene,math,&contact,&road,paths)) {
            fprintf(stderr,"Rejected tick%u stage%u event%u\n",tick,scene.stage,scene.events.tick);return 1;
        }
        if(offsets) assert(nm_intro_scene_detail(&scene,math,&scenery));
        /* Independent recurrence over post-movement profiles, never the
         * rendering order. No calls to the implementation being checked. */
        if(entering) {
            assert(!memcmp(old_motion,scene.collision_motion,sizeof(old_motion)));
            assert(!memcmp(old_relative,scene.collision_relative,sizeof(old_relative)));
            for(unsigned i=0;i<scene.count;++i) expected_track[i]=(uint16_t)(scene.count-1-i);
            assert(scene.traffic_lap==previous_lap);
        } else {
            const unsigned raw=scene.cars[expected_track[0]].lap.lap;
            const int first_lap=raw<128 ? (int)raw : (int)raw-256;
            if(first_lap>previous_lap) {
                unsigned prefix=1;
                while(prefix<4 && scene.cars[expected_track[prefix]].lap.lap==raw &&
                      (uint16_t)scene.cars[expected_track[prefix]].phase[0]<8) ++prefix;
                uint16_t rotated[44];
                for(unsigned i=0;i<scene.count;++i) rotated[i]=expected_track[(i+prefix)%scene.count];
                memcpy(expected_track,rotated,scene.count*sizeof(*rotated));
            } else for(unsigned i=1;i<scene.count;++i) {
                if((uint16_t)scene.cars[expected_track[i]].phase[0] >
                   (uint16_t)scene.cars[expected_track[i-1]].phase[0]) {
                    const uint16_t old=expected_track[i-1];
                    expected_track[i-1]=expected_track[i];expected_track[i]=old;
                }
            }
            for(unsigned i=0;i<scene.count;++i) {
                const unsigned car=expected_track[i];
                assert(scene.traffic[car].next==expected_track[(i+scene.count-1)%scene.count]);
                assert(scene.traffic[car].previous==expected_track[(i+1)%scene.count]);
                assert(scene.traffic[car].profile==(uint16_t)scene.cars[car].phase[0]);
                assert((uint8_t)scene.traffic[car].lap==scene.cars[car].lap.lap);
            }
            assert((uint8_t)scene.traffic_lap==scene.cars[expected_track[0]].lap.lap);
        }
        assert(!memcmp(expected_track,scene.track_order,sizeof(expected_track)));
        for(unsigned i=0;i<scene.count;++i) assert(scene.rank_order[i]==scene.count-1-i);
        if(entering) {
            assert(!memcmp(&old_timers,&scene.race_timers,sizeof(old_timers)));
            assert(scene.cars[0].control==old_control);
            assert(!scene.passing.signal && scene.passing.next==old_next && scene.passing.previous==old_previous);
            assert(!scene.cars[0].boundary.damage.signal && !scene.cars[0].boundary.pivot_signal);
        } else {
            unsigned selected[44]={0},at=0;
            for(unsigned i=0;i<5;++i) at=scene.traffic[at].previous;
            const int threshold=(int)scene.cars[at].phase[0]-3;
            for(unsigned i=0;i<14;++i) {
                assert(scene.collision_order[i]==at && !selected[at]);selected[at]=1;
                assert(scene.collision_relative[at]==i-5u);
                const unsigned profile=(uint16_t)scene.cars[at].phase[0];
                assert(scene.collision_adjusted[at]==(uint16_t)(profile+((int)profile<threshold ? road.profile_count-1 : 0)));
                for(unsigned a=0;a<2;++a) {
                    /* History is the pre-response position, not the moved car. */
                    const float position=scene.collision_motion[at].previous[a];
                    assert(scene.collision_motion[at].travel[a]==position-old_motion[at].previous[a]);
                }
                assert(scene.collision_candidates[at]<14);
                at=scene.traffic[at].next;
            }
            for(unsigned i=0;i<scene.count;++i) {
                if(!selected[i]) {
                    assert(!memcmp(scene.collision_motion+i,old_motion+i,sizeof(*old_motion)));
                    assert(scene.collision_relative[i]==(i ? (old_relative[i]&0xffffff00u)|127u : old_relative[i]));
                }
            }
            /* Proximity precedes collisions; verify its own idempotence on
             * a refreshed copy, not against flags changed by a response. */
            { static nm_intro_scene_t near;near=scene;
              assert(nm_intro_scene_near(&near,math));check_near_wiring(&near,math); }
            assert(scene.race_timers.short_ticks==(old_timers.short_ticks>0 ? old_timers.short_ticks-1 : 0));
            for(unsigned i=0;i<2;++i)
                assert(scene.race_timers.event[i]==(old_timers.event[i]>0 ? old_timers.event[i]-1 : 0));
            assert(scene.cars[0].control==(old_crash_remaining ? 223 :
                old_control && old_control<0x80000000u ? old_control-1 : 0));
            if(old_crash_remaining==1) {
                assert(scene.cars[0].camera_mode==scene.restored_camera_mode);
                assert(!scene.cars[0].camera_transition);
                assert(!memcmp(scene.cars[0].camera_words,math->camera_reset_words,sizeof(scene.cars[0].camera_words)));
                assert(!memcmp(scene.cars[0].camera_angles,math->camera_reset_angles,sizeof(scene.cars[0].camera_angles)));
            }
            assert((uint32_t)scene.race_timers.control==scene.cars[0].control);
            const unsigned next=scene.traffic[0].next,previous=scene.traffic[0].previous;
            assert(scene.passing.next==next && scene.passing.previous==previous);
            unsigned signal=old_damage_signal;
            const int first=previous==old_passing.next;
            if(first || next==old_passing.previous) {
                ++passing_candidates;
                const nm_player_t *a=scene.cars,*b=scene.cars+(first ? previous : next);
                const float distance=nm_force_length(math,
                    b->boundary.ground.position[0]-a->boundary.ground.position[0],
                    b->boundary.ground.position[2]-a->boundary.ground.position[2]);
                if(distance<=15.0f) {
                    ++passing_near;
                    assert(a->speed_index>=0 && a->speed_index<=INT16_MAX &&
                           b->speed_index>=0 && b->speed_index<=INT16_MAX);
                    const int gap=abs(b->speed_index-a->speed_index); // Observed intro range0..272.
                    if(first && a->lateral.current<b->lateral.current) signal=gap<=80 ? 13 : 12;
                    else {
                        const unsigned choices[3][4]={{4,8,7,3},{14,15,16,16},{9,10,11,11}};
                        const unsigned row=first ? 0 : a->lateral.current<b->lateral.current ? 1 : 2;
                        signal=choices[row][gap<=40 ? 0 : gap<=80 ? 1 : gap<=120 ? 2 : 3];
                    }
                }
            }
            assert(scene.passing.signal==signal);
            passing_changes+=scene.passing.signal!=old_passing.signal;
        }
        // Independent integer recurrence across ALL ticks, including stage
        // transitions. Repeated visibility/preparation must not advance UVs.
        const uint16_t expected_phase=(uint16_t)((uint32_t)previous_reflection+
            ((tick&7)==0)-((uint32_t)scene.cars[0].speed_index>>6));
        assert(scene.reflection_phase==expected_phase && scene.reflection_u==(expected_phase&32767));
        assert(scene.reflection_v==(((uint32_t)(int32_t)scene.heading>>4)&32767));
        if(offsets) for(unsigned w=0;w<2;++w) {
            for(unsigned repeat=0;repeat<2;++repeat) {
                assert(nm_reflection_apply(windows[w],base_uv[w],window_counts[w],scene.reflection_u,scene.reflection_v));
                for(unsigned v=0;v<window_counts[w];++v) {
                    assert(windows[w][v].u==(uint16_t)(base_uv[w][2*v]+scene.reflection_u));
                    assert(windows[w][v].v==(uint16_t)(base_uv[w][2*v+1]+scene.reflection_v));
                }
            }
        }
        assert(scene.sky_scroll<4096);
        const nm_player_t *leader=scene.cars;
        assert(leader->render_angles[0]==nm_yaw_angle((uint32_t)
            ((int)leader->boundary.ground.pitch+leader->filtered_pitch)));
        assert(leader->filtered_pitch>=-256 && leader->filtered_pitch<=256);
        body_pitch_ticks+=leader->filtered_pitch!=0;
        if(tick==1199) printf("Intro1200 window phase=%u UV=%u,%u sky=%u\n",
            scene.reflection_phase,scene.reflection_u,scene.reflection_v,scene.sky_scroll);
        if(tick==1199) printf("Intro live collision1200: pairs=%u responses=%u\n",scene.collision_pairs,scene.collision_responses);
        if(tick==1199) printf("Intro crashes1200: starts=%u active_car_ticks=%u\n",scene.crash_starts,scene.crash_ticks);
        movement+=scene.cars[0].boundary.ground.position[0]!=before;
        seen21+=scene.count==21;
        assert(scene.camera.focus>0 && isfinite(scene.camera.focus));
        for(unsigned i=0;i<12;++i) assert(isfinite(scene.view[i]));
        for(unsigned i=0;i<scene.count;++i) {
            assert((uint16_t)scene.cars[i].phase[0]<road.profile_count);
            assert(isfinite(scene.cars[i].boundary.speed));
        }
    }
    assert(scene.ticks==6000 && scene.transitions>=5 && movement>5000 && seen21>5000);
    assert(body_pitch_ticks>0);
    printf("PASS: traffic order/ring6000ticks, stage reset and retained lap; scene bytes=%zu\n",sizeof(scene));
    puts("PASS: timer/control wiring and passing signal6000ticks; stage resets signal, not timers");
    puts("PASS: proximity reset excludes player, ordered neighbor flags/markers and overlapping gear writes");
    puts("PASS: fourteen-car collision window, retained travel history, profile wrap and opponent reset");
    printf("Intro live collision6000: pairs=%u responses=%u\n",scene.collision_pairs,scene.collision_responses);
    printf("Intro crashes6000: starts=%u active_car_ticks=%u\n",scene.crash_starts,scene.crash_ticks);
#if NATIVE_INTRO_CRASH_TEST == 2
    assert(scene.player_crash_ticks==192 && !scene.cars[0].boundary.damage.remaining);
#elif NATIVE_INTRO_CRASH_TEST == 1
    assert(scene.crash_starts>=1 && scene.crash_ticks>=190);
#endif
    printf("Intro player crash ticks=%u\n",scene.player_crash_ticks);
    assert(scene.collision_pairs && scene.collision_responses);
    printf("Passing observed: candidates=%u near=%u changes=%u\n",passing_candidates,passing_near,passing_changes);
    printf("Intro signal owners: contact=%u damage/passing=%u last-passing-view=%u\n",
        (unsigned)(uint16_t)scene.cars[0].boundary.pivot_signal,
        (unsigned)(uint16_t)scene.cars[0].boundary.damage.signal,(unsigned)scene.passing.signal);
    printf("PASS: intro leader body pitch active on %u ticks; source helper validated separately\n",body_pitch_ticks);
    nm_road_pack_t invalid_road=road;invalid_road.slot=1;
    assert(!nm_intro_scene_start(&scene,math,&invalid_road,paths));
    const nm_road_map_t tunnel={0,256};
    invalid_road=road;invalid_road.mapping=&tunnel;invalid_road.map_count=1;
    assert(!nm_intro_scene_start(&scene,math,&invalid_road,paths));
    assert(scene.ticks==6000); // Invalid entry does not clear retained state.
    puts("PASS: shared reflection6000ticks, phase retained across stages, unsupported road flags rejected");
    printf("PASS: partial native scene6000ticks,transitions=%u,moving=%u,21car-ticks=%u; not full arcade callback parity\n",
        scene.transitions,movement,seen21);
    if(offsets) {
        assert(special_phase_mask==255);
        assert(special_body_parts[0] && special_body_parts[1] && special_body_parts[2]);
        printf("PASS: special body/wheel/window demand cases=%u/%u/%u\n",
            special_body_parts[0],special_body_parts[1],special_body_parts[2]);
        puts("PASS: all eight special phases included in clipped demand; instance bound holds");
        printf("Intro clipped texture demand peak=%u bytes at tick%u; bodies/wheel phases, host arithmetic\n",peak,peak_tick);
        assert(peak<=window_peak); // Window-inclusive demand is the actual scene; same bank below.
        printf("Body/wheel/window texture peak=%u bytes at tick%u; bank=%u; host arithmetic\n",
            window_peak,window_tick,NM_INTRO_MATERIAL_CACHE_BYTES);
        assert(window_peak<=NM_INTRO_MATERIAL_CACHE_BYTES);
    }
    free(offsets);nm_material_pack_close(&materials);nm_pack_unload(&model);
    nm_scenery_unload(&scenery);
    for(unsigned i=0;i<8;++i) nm_path_unload(paths+i);
    nm_road_unload(&road);nm_contact_unload(&contact);free(math);
}
