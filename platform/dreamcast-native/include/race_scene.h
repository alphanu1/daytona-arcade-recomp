#ifndef DAYTONA_NATIVE_RACE_SCENE_H
#define DAYTONA_NATIVE_RACE_SCENE_H
#include "nm_mem.h"
#include "race_field.h"
#include "race.h"
#include "finish_actor.h"
#include "race_records.h"
#include "race_start.h"
#include "slot_machine.h"
#include "wrong_way.h"
#include "contact_clear.h"
#include "tunnel_light.h"
/* 235db8 + c, the normal-race light (lighting.h's; race_start_data.h nm_race_light_normal, checked there). */
static const float nm_race_light_normal_default[3]={-0.45f,-0.89f,-0.45f};

/* Persistent normal-race owners. No assets, pointers into guest memory, PVR
 * lifetime, allocation or wall-clock scheduler belong here. Phase23/24 retain
 * finish intents and actor state; their UI/material binding and later results
 * phases remain incomplete. No sound device is called. */
typedef struct {
    nm_race_car_t cars[44];
    nm_race_shared_t shared;
    nm_race_order_t order;
    nm_race_collisions_t collisions;
    nm_traffic_pass_t passing;
    nm_checkpoint_award_t award;
    nm_race_intents_t intents;
    nm_race_finish_entry_t finish;
    nm_finish_actor_t finish_actor;
    uint8_t finish_branch;
    uint32_t view_ticks[4];
    uint32_t announcement_cursor;
    int32_t cooldowns[2];
    uint32_t start_flags,ticks;
    uint8_t rolling[44],count,phase,active_phase;
    uint8_t course; /* 501460, the road pack's slot: 0 Beginner, 1 Expert, 2 Advanced. */
    int16_t start_stage; /*501850: rolling-start sequence, owner nm_race_start_sequence. */
    uint32_t start_timer; /* 5010a8 in phase 20: 320 from the phase19 entry (33b8), counted by 3458. */
    uint8_t camera_restart; /* One-shot: a keyed camera sequence starts at tick0 (5c70). */
    /* The selected view 501710 (VR1..VR4 = 0..3; 33d4: 2 at the race entry), chosen by the camera pass
     * (race_view.h nm_race_view_update), and the presets 1d00220 (the race-start header's nm_race_view_presets,
     * bound by the caller; NULL: the reset record's words, and the view never leaves 2). */
    uint8_t view;
    const nm_race_view_preset_t *view_presets;
    nm_race_view_frame_t camera_frame; /* Presentation: race_camera.h writes it each camera pass. */
    nm_slot_machine_t slot_machine; /* 2204fc (actor 34, pool row 109); the start memset is its init. */
    uint8_t wrong_way; /* HUD object 501268 flags bit28 (a818); a674's clear at a68c is the start memset. */
    /* The frame counter 5010b0 on the phase20 tick (race tick0): after a tick, 5010b0 = counter_base + ticks - 1.
     * The slot machine reads its bit0, the collision pass 5010b0 & 3 (503692). nm_race_scene_start sets the
     * recorded races' value NM_SLOT_MACHINE_COUNTER_BASE; a race entered from the select screens gets the
     * count since power-on (frontend.h). */
    uint32_t counter_base;
    /* The scene light (camera object +60..+68, tunnel_light.h): stepped by the camera pass (race_camera.h). */
    nm_tunnel_light_t scene_light;
    /* Diagnostic only: the part of nm_race_scene_step that ran last
     * (NM_RACE_STAGE_*); a rejected tick reports it with shared.updating. */
    uint8_t stage;
    uint16_t accel; /* 501054 of the last tick: only the HUD reads it (d480). */
} nm_race_scene_t;
enum { NM_RACE_STAGE_CHECK=0, NM_RACE_STAGE_PHASE, NM_RACE_STAGE_MOVEMENT, NM_RACE_STAGE_INTERACTIONS,
       NM_RACE_STAGE_FINISH_ACTOR, NM_RACE_STAGE_DONE };
static inline const char *nm_race_stage_name(unsigned stage) {
    static const char *const names[]={"arguments","phase handler","movement","interactions","finish actor","done"};
    return stage<sizeof(names)/sizeof(names[0]) ? names[stage] : "?";
}

/* Phase25 record publication only, not the remaining result actor/UI entry.
 * Call once on entry. The persistent course table lives outside this scene.
 * Bit1 survives the timer consuming bit0; forced finish bit0 alone is NOT a
 * completed race. Reverse-finish metadata is source501a8c, not camera mode. */
static inline int nm_race_scene_record(const nm_race_scene_t *s,
        nm_race_record_t table[24],uint32_t penalty,unsigned *selected,uint32_t *notification) {
    if(!s || s->phase!=25 || !s->count || s->count>44) return 0;
    return nm_race_record_update(table,s->cars[0].car.checkpoints.finish_flags,
        s->shared.clock,s->view_ticks,penalty,s->cars[0].car.checkpoints.reverse,
        selected,notification);
}

/* Normal event tail after movement/interactions, before camera/scenery.
 * Remaining time is source5010d0; it is NOT elapsed ticks or a new timer.
 * Numeric output is consumed silently now, available to future audio later. */
static inline int nm_race_scene_events(nm_race_scene_t *s,const nm_race_intent_row_t *rows,
        unsigned count,unsigned alternate,const uint32_t special[2],
        const nm_race_announcement_row_t *announcements,unsigned announcement_count,
        const uint32_t ranks[9],uint32_t inhibit,nm_race_intent_output_t *out) {
    if(!s || !s->count || s->count>44 || s->cars[0].next>=s->count) return 0;
    const nm_race_car_t *next=s->cars+s->cars[0].next;
    if(next->next>=s->count) return 0;
    const nm_race_announcement_input_t view=nm_race_announcement_view(s->cars,next,
        s->cars+next->next,s->shared.flags,s->start_flags,inhibit,s->count);
    return nm_race_event_tail(s->cars,&s->intents,&s->announcement_cursor,rows,count,alternate,
        special,nm_gears_signed(s->award.remaining),announcements,announcement_count,ranks,&view,out);
}

/* Wrong way a818 (HUD object a7c0, pool row 61): after the event tail (row
 * 56) and the cameras (rows 58, 60), before the slot machine (row 109). Call
 * only after nm_race_scene_step returned 1; a7c0 runs a818 only while the
 * latched flags 501090 select phase 20 or 22 (a7c0..a7d4), so bit 28 keeps
 * its last value through phases 23/24. Sets or clears the player's flags
 * bit 28, which the player and the opponents read on the next tick. */
static inline int nm_race_scene_wrong_way(nm_race_scene_t *s,const nm_contact_pack_t *contact) {
    if(!s || !s->count || s->count>44) return 0;
    if(!(s->shared.flags&0x500000u)) return 1;
    nm_player_t *p=&s->cars[0].car;
    if(!nm_wrong_way_tick(contact,p->boundary.ground.road,p->boundary.heading,&s->wrong_way,
        &p->boundary.damage.flags)) return 0;
    /* a900: the cue, the intent lane 501a02, on every tick facing the wrong way (after the message). */
    if(nm_wrong_way_facing(contact,p->boundary.ground.road,p->boundary.heading)) p->boundary.pivot_signal=30;
    return 1;
}

/* Slot machine 220534 (row 109): after the event tail (row 56) and the camera
 * rows 57-73, before scenery 5890 republishes visible_cells, so it reads the
 * previous pass's bitmap. Call only after nm_race_scene_step returned 1: ticks
 * already counts this tick, so 5010b0 = base + ticks - 1 (race tick0 is the
 * phase20 tick). start_edge is the same edge the step fed force_recovery.
 * Returns the sound command for the silent sink (0 when none). */
static inline uint32_t nm_race_scene_slot(nm_race_scene_t *s,int start_edge) {
    /* Row 109 is one of the 64 world-actor rows the pit suspends (dd9c..ddd4, D+128 .. S+511):
     * no reel step, no START edge, no bonus; it resumes where it stopped (de58). */
    if(s->shared.pit.suspended) return 0;
    /* 2204fc is an actor of course 0's table 236940[0]; the other courses have no slot machine. */
    if(s->course) return 0;
    return nm_slot_machine_tick(&s->slot_machine,nm_slot_machine_visible(s->shared.visible_cells),
        s->counter_base+s->ticks-1u,start_edge,s->shared.flags,&s->award.remaining);
}

/* The pit crew rows 68..73 (pit.h nm_pit_crew_tick) after the spin smoke (row 66), before the slot
 * machine (row 109). Call only after nm_race_scene_step returned 1. The crew's sound cues go to
 * sounds[] in queue order (count of them in *count; each cue once per crew). */
static inline int nm_race_scene_pit_crew(nm_race_scene_t *s,uint32_t sounds[NM_PIT_CREW],unsigned *count) {
    if(!s || !sounds || !count || !s->count) return 0;
    *count=0;
    if(!s->shared.pit.crew) { s->shared.pit.drawn.valid=0;s->shared.pit.crew_drawn=0;return 1; }
    return s->shared.pit_data && nm_pit_crew_tick(s->shared.pit_data,&s->shared.pit,&s->cars[0].car,sounds,count);
}

/* The race field during the results phases 25/26 of a race that ends there
 * (time up from phase22, or a finish outside the first three from phase24),
 * one call per results tick after its phase handler. The pool still holds the
 * race cars; after a podium (27/28) it holds the podium's entities instead
 * (3de8), so the podium path does not call this.
 *  - 4110 (phase25 entry), 41b4..41dc: every opponent object of 501224
 *    (count 501084) loses flags bit31, so cde0 no longer runs: the opponents
 *    stop where they are. The player 501220 and the interaction object
 *    (1210c, pool row 56) are not disabled.
 *  - 1210c keeps running every tick with the latched flags 501090 = 1<<25
 *    (first tick) or 1<<26: with 501a80 == 0 the sort/near pass 1224c, the
 *    timers 12150, passing 125b0 and the collision pass 13f08 (12134..12144),
 *    as nm_race_interactions_step. Contacts between the parked cars still
 *    respond (speed, position, flags bit5, contact sides), and the travel
 *    words +294/+298 measure the zero movement. The rank update of 1224c and
 *    the announcements stay off (flags & 0x500000 == 0).
 * collision_phase is 503692 = 5010b0 & 3 (5818..583c), the frame counter that
 * keeps counting through the results: race ticks plus results ticks, & 3.
 * The event tail 19038 that follows (12148) is nm_race_scene_events, caller-run
 * as in the race. curve is the transmission byte 501464 the race used. */
static inline int nm_race_scene_results_field(nm_race_scene_t *s,const nm_drive_params_t *math,
        const nm_road_pack_t *road,unsigned active_phase,uint32_t inhibit,unsigned collision_phase,
        uint32_t record_flags,uint8_t curve) {
    if(!s || !math || !road || s->count<3 || s->count>44 || (active_phase!=25 && active_phase!=26) ||
       collision_phase>=4) return 0;
    if(active_phase==25)
        for(unsigned id=1;id<s->count;++id) s->cars[id].car.boundary.damage.flags&=UINT32_C(0x7fffffff);
    s->active_phase=(uint8_t)active_phase;s->shared.flags=1u<<active_phase;
    return nm_race_interactions_step(s->cars,s->count,&s->shared,&s->order,s->cooldowns,&s->passing,
        &s->collisions,math,road,inhibit,collision_phase,record_flags,curve);
}

/* Entity 0 at the start of a scene on the race's objects, after a race (the entity is never cleared): what the last
 * race left, but the wheels' masks +1ba..+1c0, which the player's start c940 zeroes in phase 2 (c990..c99c). Its
 * grid reset e20c runs next. So stay the axle loads +194/+198, the wheels' surfaces +1a8..+1b4 and heights
 * 501100..50110c, +1f0, +22c, +248, +24c, +25c, +260, +264..+26c and +270, each read before it is written in the next
 * race; the demo's player runs on none of them. */
static inline void nm_player_carried(nm_player_t *s,const nm_player_t *previous) {
    const nm_checkpoints_t checkpoints=s->checkpoints; /* the scene's own, set before */
    NM_COPY(s,previous);
    s->checkpoints=checkpoints;
    nm_memset(s->boundary.ground.mask,0,sizeof(s->boundary.ground.mask));
}

/* Cold race entry using authored grid data (road slot 0..2: Beginner, Expert, Advanced). Not an attract->race transition:
 * fields retained across arcade scene changes still need their real owners.
 * Failure leaves the scene unusable; caller must not run a partial entry. */
/* pit: the race header's nm_race_pit (pit.h), bound for the pit stop; NULL only for callers that never
 * reach the pit lane's trigger row (a pit entry then rejects the tick). previous: entity 0 as the last race left it
 * (nm_player_carried), NULL for the first game. */
static inline int nm_race_scene_start_carried(nm_race_scene_t *s,const nm_drive_params_t *math,
        const nm_contact_pack_t *contact,const nm_road_pack_t *road,
        const nm_path_pack_t paths[8],unsigned difficulty,unsigned model_variant,
        nm_road_marks_t *marks,unsigned mark_capacity,const nm_pit_data_t *pit,const nm_player_t *previous) {
    nm_race_settings_t settings;
    if(!s || !math || !contact || !road || !paths || !marks || !marks->values ||
       road->slot>2 || contact->slot!=road->slot || !road->grid || road->grid_count<3 || road->grid_count>44 ||
       road->profile_count<2 || road->checkpoint_count>31 || marks->count!=road->profile_count-1 ||
       mark_capacity<road->profile_count || model_variant>=4 ||
       !nm_race_settings(road,difficulty,&settings)) return 0;
    for(unsigned id=0;id<road->grid_count;++id) if(road->grid[id].route>=8) return 0;
    nm_memset(s,0,sizeof(*s));nm_memset(marks->values,0,mark_capacity);marks->timer=0;
    s->shared.pit_data=pit;s->course=(uint8_t)road->slot;
    /* Entry c7ec/c904 runs under phase19; the first car movement is the phase20
     * object pass (flags bit20), then phase21, before the phase22 handler. */
    s->count=(uint8_t)road->grid_count;s->phase=s->active_phase=20;s->shared.flags=1u<<20;
    s->view=2; /* 33d0..33d4 */
    s->counter_base=NM_SLOT_MACHINE_COUNTER_BASE;
    nm_tunnel_light_reset(&s->scene_light,nm_race_light_normal_default); /* 72c4 at the race entry */
    s->award.remaining=settings.initial_ticks;
    s->start_timer=5u<<6; /* 33b8..33bc */
    /* 3370..3380: the phase19 entry sets the start stage 501850 to 231e68[course]: 0 on course 0
     * (rolling start), -1 on the others (standing start: c8b0..c900 and c91c..c92c then give the
     * player the normal updater d7fc with 501854 bit0 clear, and every opponent cde0). */
    const int rolling_start=!road->slot;
    s->start_stage=(int16_t)(rolling_start ? 0 : -1);
    /* 03874/c868..c88c: shared race configuration survives the per-car resets. */
    s->cars[0].car.checkpoints=(nm_checkpoints_t){.visited=1,
        .required=UINT32_MAX>>(31-road->checkpoint_count),.total_laps=settings.total_laps};
    for(unsigned id=0;id<s->count;++id) {
        nm_race_car_t *c=s->cars+id;c->remembered=255;
        if(!id && previous) {
            nm_player_carried(&c->car,previous);
            c->car.gear=0;c->car.surface_phase=0; /* c7ec: c840, c858 */
        }
        c->car.boundary.damage.flags=0x80000000;
        const nm_path_pack_t *path=paths+road->grid[id].route;
        if(!nm_race_car_grid_reset(c,math,contact,road,path,id,model_variant)) return 0;
        /* cb60 ages its height sample by the board frame counter, and the grid
         * reset keeps the old sample. Native ticks restart at zero here, so
         * make that sample at least five ticks old: the first visible tick
         * then resamples instead of dividing by a zero interval. Inferred from
         * the select screens always separating two scenes, not traced. */
        c->effects.height.sampled_tick=(uint16_t)(0u-5u);
        if(id) {
            /* c914..c938: only cars gridded behind the line (lap byte nonzero)
             * enter the rolling formation; the rest keep the grid reset and
             * start from rest under the normal updater cde0. */
            if(rolling_start && c->car.lap.lap) {
                if((uint16_t)c->car.phase[0]>=road->profile_count ||
                   !nm_car_rolling_entry_pose(road->profiles+(uint16_t)c->car.phase[0],id,
                       &c->car,&c->effects.height.reference_height)) return 0;
                s->rolling[id]=1;
            }
        } else {
            if(rolling_start) {
                if(!nm_player_rolling_start(math,contact,road,path,model_variant,
                       &c->car,&c->effects.height.reference_height,&s->start_flags)) return 0;
                s->rolling[0]=1;
            }
            c->rank=(uint8_t)(s->count-1); /* c894..c89c: player rank byte6a=[501084]. */
            /* 5b88/5ca4: the camera object enters the keyed start sequence. */
            c->car.camera_mode=1;c->car.camera_transition=1;
            c->car.auxiliary_flags|=UINT32_C(0x80000000); /* 33a0..33a8: its object 501270 runs. */
            s->camera_restart=1;
        }
        c->next=(uint16_t)((id+1)%s->count);c->previous=(uint16_t)((id+s->count-1)%s->count);
    }
    /* The camera words before the start camera's 62b4: the reset record (presentation only: the keyed start
     * draws with its own keys; the window centre 501730 eases from here). */
    nm_memcpy(s->cars[0].car.camera_words,math->camera_reset_words,sizeof(s->cars[0].car.camera_words));
    nm_memcpy(s->cars[0].car.camera_angles,math->camera_reset_angles,sizeof(s->cars[0].car.camera_angles));
    s->passing=(nm_traffic_pass_t){s->cars[0].next,s->cars[0].previous,0};
    return nm_traffic_order_init(s->order.rank,s->order.track,s->count);
}
/* A cold start: no race before it (the recorded races, the tests). */
static inline int nm_race_scene_start(nm_race_scene_t *s,const nm_drive_params_t *math,
        const nm_contact_pack_t *contact,const nm_road_pack_t *road,
        const nm_path_pack_t paths[8],unsigned difficulty,unsigned model_variant,
        nm_road_marks_t *marks,unsigned mark_capacity,const nm_pit_data_t *pit) {
    return nm_race_scene_start_carried(s,math,contact,road,paths,difficulty,model_variant,marks,mark_capacity,pit,NULL);
}

/* One fixed gameplay tick; caller supplies latched controls and current visible
 * cells through shared. Return0 stops on error,1 completes a tick,2 requests a
 * unsupported phase entry on a SUBSEQUENT call. A tick selecting the next phase still
 * completes movement/events/presentation with its latched active flags.
 * Timer hold/gate are explicit, not inferred from rendering or frozen at zero.
 * Caller runs events, then nm_player_effect_timers (original camera6c18) and
 * nm_player_contact_clear (9088 -> 9148, contact_clear.h), then
 * presentation. Lap-record publication remains external. */
static inline int nm_race_scene_step(nm_race_scene_t *s,const nm_drive_params_t *math,
        const nm_intro_speed_params_t *speed,const nm_contact_pack_t *contact,
        const nm_road_pack_t *road,const nm_path_pack_t paths[8],
        const nm_player_tick_input_t *input,uint32_t timer_hold,uint16_t timer_gate,
        uint32_t record_flags,unsigned collision_phase,nm_road_marks_t *marks,unsigned mark_capacity,
        const nm_crash_row_t *rows,unsigned row_count) {
    if(s) s->stage=NM_RACE_STAGE_CHECK;
    if(!s || !input || !math || !speed || !contact || !road || !paths || !marks ||
       !marks->values || s->count<3 || s->count>44 || collision_phase>=4 ||
       mark_capacity<road->profile_count || marks->count+1!=road->profile_count) return 0;
    if(s->phase<20 || s->phase>24) return 2;
    s->stage=NM_RACE_STAGE_PHASE;
    s->active_phase=s->phase;s->shared.flags=1u<<s->active_phase;
    /* 62b4/637c/df08 read the view 501710 and its preset (byte +13: the mode they restore). */
    const nm_race_view_preset_t *preset=s->view_presets ? s->view_presets+(s->view&3u) : NULL;
    const int8_t restored=preset ? (int8_t)preset->mode : input->active.restored_camera_mode;
    if(s->active_phase==20) {
        /* 3458..34a4: with 5010ec == 8 laps (course 0) phase20 ends at once; otherwise 5010a8
         * counts down (subi) and phase21 follows on the tick it turns negative: 321 ticks.
         * Its calls 37e8 (sky heading copy) and 5760 (cabinet lamps) are presentation. */
        if(s->cars[0].car.checkpoints.total_laps==8) s->phase=21;
        else if((--s->start_timer)&UINT32_C(0x80000000)) s->phase=21;
    } else if(s->active_phase==21) {
        /* 34a8..3510: clears the lap display ticks 5010dc and selects phase22;
         * its sound/UI writes (3a58, 501338, object 501230) are not gameplay state. */
        s->cars[0].car.checkpoints.display_ticks=0;
        s->phase=22;
    } else if(s->active_phase==23) {
        s->finish.phase=23;
        const unsigned rank=s->cars[0].rank;
        if(!nm_race_finish_enter(input->award.penalty,(int8_t)(rank<128 ? (int)rank : (int)rank-256),&s->finish) ||
           !nm_finish_actor_init(&s->finish_actor,s->finish_actor.inherited_angle)) return 0;
        s->phase=s->finish.phase;s->rolling[0]=2;
        s->cars[0].car.camera_mode=(int32_t)s->finish.camera_mode; /* 43b8..43bc: 5014a4 := 0 in every view. */
    } else if(s->active_phase==24) {
        const unsigned rank=s->cars[0].rank;
        s->phase=nm_race_finish_timer(&s->finish.timer,(int8_t)(rank<128 ? (int)rank : (int)rank-256),input->award.penalty);
    } else {
        /* ab78..ab84: the rolling-start stages run on course 0 only. */
        if(!s->course && !nm_race_start_sequence(math,&s->start_stage,&s->cars[0].car,road->profile_count-1,
            restored,preset)) return 0;
        nm_race_view_tick(s->view_ticks,s->start_flags,s->view); /* 3538: the counter of 501710 */
        /* 3584..3598: no decrement while 501a84 != 0, read before this tick's player update (the pit's stop
         * count: D+2 .. R+1). The caller's hold is the reference mode's recorded value, else 0. */
        if(!(s->start_flags&1u)) {
            s->phase=nm_race_active_timer(input->award.suppression,timer_gate,
                timer_hold|s->shared.timer_hold,
                &s->award.remaining,&s->shared.clock,&s->cars[0].car.checkpoints.finish_flags);
            /* 35f4..3718, after 35e0's clock step: at exactly 640 and 385 left the handler writes the intent
             * lane 501a02 (pivot_signal) with 28 / 29 and returns (race_intents.h: ae2125 / ae2120); below
             * 384 it beeps (race_sound.h nm_race_sound_beep, sound only). */
            if(s->phase==22) {
                if(s->award.remaining==640u) s->cars[0].car.boundary.pivot_signal=28;
                else if(s->award.remaining==385u) s->cars[0].car.boundary.pivot_signal=29;
            }
        }
    }
    /* d834 checkpoint awards read NEXT phase5010a4; motion reads latched
     * flags501090. Do not collapse those two owners on a transition tick. */
    nm_player_tick_input_t tick; NM_COPY(&tick,input);tick.phase=s->phase;
    tick.active.restored_camera_mode=restored;
    const int pitting=s->rolling[0]==4 || s->rolling[0]==5;
    tick.active.input_counter=nm_gears_signed(s->award.remaining);
    s->shared.tick=(uint16_t)s->ticks;
    const uint32_t keyed=s->cars[0].car.auxiliary_flags>>31;
    /* The object chain 1838 runs in address order: the start signal 504900 before the player 504e00, so the drive
     * term ecc8 of the same tick reads the rpm it has just revved. */
    if(s->active_phase==20 && !nm_race_start_signal(math,&s->cars[0].car,s->start_timer,s->start_stage,
           s->cars[0].car.checkpoints.total_laps,input->active.controls.accel,restored,preset)) return 0;
    s->stage=NM_RACE_STAGE_MOVEMENT;s->accel=(uint16_t)input->active.controls.accel;
    if(!nm_race_movement_step(s->cars,s->count,&s->shared,s->rolling,&s->start_flags,
        math,speed,contact,road,paths,&tick,&s->award,marks,mark_capacity,rows,row_count)) return 0;
    /* df08..df20: the pit release resets the camera with 637c below view 2 (pit.h), with 62b4 from view 2 up:
     * the preset's words; nothing between the release and the camera pass reads them. */
    if(pitting && !s->rolling[0] && preset && s->view>=2) nm_player_view_reset(math,&s->cars[0].car,restored,preset);
    unsigned crash=s->cars[0].car.boundary.damage.auxiliary_request;
    s->stage=NM_RACE_STAGE_INTERACTIONS;
    if(!nm_race_interactions_step(s->cars,s->count,&s->shared,&s->order,s->cooldowns,&s->passing,
        &s->collisions,math,road,tick.award.penalty,collision_phase,record_flags,tick.active.dynamics.curve)) return 0;
    crash|=s->cars[0].car.boundary.damage.auxiliary_request|
        (!keyed && (s->cars[0].car.auxiliary_flags>>31));
    if(s->shared.pit.camera_arm) {
        /* The aux object 501270 runs in this tick's camera pass (row 58, after the interactions): 5af8 set
         * by the pit entry dc44 this tick, or 5b20 set by the crew's first standing tick (1b00c) the tick
         * before. Both: transition 501713 := 1, the track 234a9c / 234aac [501710], 5c70: tick 0, mode 1.
         * A crash started in the same tick (15d18 -> 5c14, later in the pit tick) overrides it below. */
        nm_player_t *p=&s->cars[0].car;
        p->auxiliary_flags|=UINT32_C(0x80000000);p->camera_mode=1;p->camera_transition=1;
        s->shared.pit.camera=s->shared.pit.camera_arm;s->shared.pit.camera_arm=0;s->camera_restart=1;
    }
    if(crash) {
        /* 15d18..15d38 -> 5c14/5c70: the crash re-arms the keyed camera object
         * with the car-relative sequence, which starts in this tick's camera pass. */
        s->cars[0].car.camera_mode=1;s->cars[0].car.camera_transition=0;s->camera_restart=1;
    }
    if(s->active_phase==23 || s->active_phase==24) {
        s->stage=NM_RACE_STAGE_FINISH_ACTOR;
        s->finish_branch=(uint8_t)nm_finish_actor_step(&s->finish_actor);
        if(!s->finish_branch) return 0;
    }
    s->stage=NM_RACE_STAGE_DONE;
    ++s->ticks;return 1;
}
#endif
