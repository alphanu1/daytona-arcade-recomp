#ifndef DAYTONA_NATIVE_RACE_CAMERA_H
#define DAYTONA_NATIVE_RACE_CAMERA_H
#include "race_scene.h"
#include "intro_scene.h"
#include "race_view.h"

/* The race's camera pass, once per gameplay tick after the race step and its events (code/practice.c and
 * tests/trace_race_native.c): the view of the camera mode (keyed: the crash camera 5c14 or the start / pit
 * tracks; else the selected view, race_view.h), the scenery anchor, the detail pass and the drawn-car count the
 * next tick's opponents read, the world objects' near ticks, the cell bitmaps, then the view updater 8814 with
 * the held view buttons. presentation owns only the presentation arrays; race is the simulation. */
typedef struct {
    const nm_race_keyed_track_t *tracks;          /* by nm_pit_t.camera: the start, the pit entry and stop (view 2) */
    const nm_race_keyed_track_t (*pit_views)[4];  /* the pit entry and stop by view 501710 (5af8/5b20), or NULL */
    const nm_intro_key_t *crash_keys;
    const uint32_t *crash_times;
    unsigned crash_count;
    const nm_tunnel_lights_t *tunnel;             /* the course's scene light ranges (tunnel_light.h), or NULL */
} nm_race_cameras_t;

static inline const nm_race_keyed_track_t *nm_race_keyed_track(const nm_race_cameras_t *c,unsigned camera,unsigned view) {
    if(camera && camera<3 && c->pit_views && view<4) return &c->pit_views[camera-1][view];
    return camera<3 ? c->tracks+camera : NULL;
}

/* Returns NULL, or the step that rejected. held: 53e120 (race_view.h NM_VIEW_HELD_*). */
static inline const char *nm_race_camera_pass(nm_race_scene_t *race,nm_intro_scene_t *pr,
        const nm_drive_params_t *math,const nm_scenery_pack_t *scenery,const nm_race_cameras_t *cams,uint32_t held) {
    if(!race || !pr || !math || !scenery || !cams || !race->count || race->count>44 || race->view>3) return "arguments";
    const nm_player_t *players[44];
    for(unsigned i=0;i<race->count;++i) players[i]=&race->cars[i].car;
    nm_player_t *player=&race->cars[0].car;
    float origin[2];uint16_t cell;nm_scenery_cells_t next;
    if(race->camera_restart) { pr->camera_cursor=(nm_intro_cursor_t){0};race->camera_restart=0; }
    /* The driver's view scales the eye's forward offset in the keyed modes too (6938, nm_race_view_eye_z); the crash
     * camera turns it with the car's pitch as well (69b4, nm_race_view_eye_turn), the start and pit tracks hold +28 at 0. */
    const int pulled=race->view==0 && player->inhibit_angle;
    if(player->camera_mode==1 && !player->camera_transition) {
        /* Crash camera (5c14): car-relative keys until 637c restores the view. */
        if(!nm_race_crash_camera(player,math,scenery,cams->crash_keys,cams->crash_times,cams->crash_count,
            &pr->camera_cursor,&pr->camera,pr->view,&pr->heading,&pr->sky_scroll,&next)) return "crash camera";
        const nm_wheels_state_t *g=&player->boundary.ground;
        float eye[3]={pr->camera.position[0],pr->camera.position[1],pr->camera.position[2]};
        eye[2]=nm_race_view_eye_z(eye[2],race->view,player->inhibit_angle);
        if(nm_race_view_eye_turn(math,race->course,race->view,g->pitch,eye) || pulled) {
            int16_t authored[3];
            const int16_t angles[3]={g->pitch,player->body_yaw,g->roll};
            if(!nm_intro_camera_angles(math,&pr->camera,authored) ||
               !nm_car_relative_view(math,g->position,angles,eye,authored,pr->view)) return "crash camera eye";
        }
    } else if(player->camera_mode==1) {
        /* Rolling-start camera (5cac with table 6474) until 62b4 selects the view; the pit's tracks. */
        const nm_race_keyed_track_t *track=nm_race_keyed_track(cams,race->shared.pit.camera,race->view);
        if(!track || !nm_race_start_camera(race->cars,race->count,math,scenery,track->keys,track->times,track->count,
            pr->anchors,&pr->camera_cursor,&pr->camera,pr->view,&pr->heading,&pr->sky_scroll,&next)) return "start camera";
        if(pulled) {
            nm_intro_camera_t camera=pr->camera;int16_t heading;
            camera.position[2]=nm_race_view_eye_z(camera.position[2],0,player->inhibit_angle);
            if(!nm_intro_camera_view(math,&camera,pr->view,&heading,NULL)) return "start camera eye";
        }
    } else {
        if(!nm_race_view_camera(math,player,race->course,race->view,pr->view,&pr->heading)) return "view";
        if(!nm_race_view_scenery_origin(math,player,pr->heading,origin,&cell)) return "scenery origin";
        if(!nm_scenery_select(scenery,math,cell,(uint16_t)player->phase[0],
            origin[0],origin[1],pr->heading,&next)) return "scenery select";
        if(!nm_race_view_sky_scroll(math,player,&pr->sky_scroll)) return "sky scroll";
    }
    /* View pass 6b58..6b68 (requests 84e0, count 7668) reads bitmap 501520 before
     * the scenery object 5890 rewrites it: this pass's view with the PREVIOUS
     * cells. A cold start has no previous cells, so its first count is0.
     * The list 503208 (8480..84dc) holds the opponents only: car 0 is unlisted. */
    if(!nm_race_detail_step(pr->view,&pr->cells,players,race->count,
        pr->detail_depth,pr->detail_order,pr->detail_level)) return "detail step";
    /* Original 6b68: the drawn-car count feeds the next tick's opponents. */
    if(!nm_car_detail_guided_mode(pr->detail_level,race->count,race->shared.flags,
            race->shared.timer_hold,race->course,&race->shared.guided_mode)) return "guided mode";
    pr->cells_previous=pr->cells;
    pr->cells=next;
    /* The world actors' counters stand still while the pit suspends them (rows 74..138, pit.h).
     * Original scenery runs after movement: publish for the NEXT tick.
     * The initial detail call seeds our native cold-start grid as well. */
    if(!race->shared.pit.suspended && !nm_world_objects_tick(pr->object_near_ticks,&pr->cells_previous)) return "world objects";
    if(!nm_scenery_bitmaps(&pr->cells,race->shared.visible_cells)) return "scenery bitmaps";
    /* What this tick's frame draws with: the player's draw 6c0c and its effects 9088 run before 8814. */
    nm_race_view_frame_t *frame=&race->camera_frame;
    frame->mode=player->camera_mode;frame->eye=player->camera_words[2];frame->view=race->view;
    if(race->view_presets && !nm_race_view_update(math,race->view_presets,race->shared.flags,held,&race->view,player))
        return "view update";
    frame->focus=nm_force_float(player->camera_words[5]);frame->centre=nm_force_float(player->camera_words[4]);
    frame->valid=1;
    /* 6850 -> 7024: the scene light from the camera's car (the player), after its movement this tick. */
    if(cams->tunnel && !nm_tunnel_light_step(&race->scene_light,cams->tunnel,player->boundary.ground.position,
        (uint16_t)player->phase[0],player->boundary.damage.flags,player->path_flags,(uint32_t)player->speed_index))
        return "scene light";
    return NULL;
}
#endif
