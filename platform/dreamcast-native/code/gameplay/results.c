/* Result gameplay phase. The cars do not move; without a podium the race field's interaction
 * object still runs (result_field_tick). Nothing of the race is drawn. */
#include "practice_scene.h"
#include "gameplay/name_mode.h"
#include "render/scene/practice.h"
#include "render/ui/result_render.h"
#include "render/ui/result_labels.h"
#include "result_actor_data.h"
#include "audio/sound.h"
#include "audio/race_sound_hooks.h"
#include <stdio.h>

static void result_silent_command(nm_results_t *result,uint32_t command) {
    if(!command) return;
    ++result->commands;result->command_hash=(result->command_hash^command)*UINT32_C(16777619);
    nm_sound_command(command);
}

/* End of a results tick. The race field keeps its interaction object through the
 * results of a race that ends here (time up, or a finish outside the first three):
 * 4110 parks the opponents, 1210c and its event tail 19038 run every results tick
 * (race_scene.h nm_race_scene_results_field). collision_phase 503692 is the
 * frame counter 5010b0 & 3, which keeps counting: race ticks plus the results
 * ticks before this one. The transmission is automatic (curve 0). */
static int result_field_tick(nm_practice_scene_t *scene,unsigned results_phase) {
    nm_race_intent_output_t out;
    if(!nm_practice_results_tick(scene,results_phase,(scene->race.counter_base+scene->race.ticks+scene->results.ticks-1u)&3u,&out)) return 0;
    for(unsigned i=0;i<out.count;++i) /* Through 19bdc, which the results phases gate off. */
        nm_race_sound_hook_results_gated(&scene->race,&scene->results.commands,&scene->results.command_hash,out.command[i]);
    nm_race_sound_hook_events(&scene->race,scene->race.ticks+scene->results.ticks-1u); /* 19590, gated off */
    return 1;
}

int nm_results_update(nm_practice_scene_t *scene,uint32_t elapsed_us) {
    if(scene->race.phase==25 || scene->race.phase==26 || (scene->results.mode && scene->race.phase==29)) {
        if(scene->failed) {
            nm_practice_render_release();enj_state_flag_shutdown(NULL);return 1;
        }
        if(scene->race.phase==29) {
            nm_practice_render_release();
#ifdef NATIVE_RACE_SCRIPT
            /* A scripted run ends at this handoff in practice.c, before it calls here: no name
             * entry is linked, whether or not LTO inlines this function and sees it. */
            enj_state_flag_shutdown(NULL);
#else
            nm_practice_game_over(); /* what the game leaves the scenes after it (frontend.h) */
            if(!nm_name_mode_start(scene->asset_root,scene->math,scene->name_notification,
                                  scene->name_record,scene->race.cars[0].car.lap.best_ticks,
                                  scene->race.cars[0].car.lap.completed,&scene->failed)) {
                scene->failed=1;enj_state_flag_shutdown(NULL);
            }
#endif
            return 1;
        }
        uint32_t dt=elapsed_us;
        if(dt>4*NM_PRACTICE_TICK_US) { scene->state.dropped_us+=dt-4*NM_PRACTICE_TICK_US;dt=4*NM_PRACTICE_TICK_US; }
        scene->state.remainder_us+=dt;
        while(scene->state.remainder_us>=NM_PRACTICE_TICK_US) {
            scene->state.remainder_us-=NM_PRACTICE_TICK_US;++scene->results.ticks;
            const unsigned results_phase=scene->race.phase;
            if(scene->race.phase==25) {
                const unsigned rank=scene->race.cars[0].rank;
                scene->results.mode=(unsigned)nm_result_playback_init(&scene->results.playback,nm_result_initial,
                    (int8_t)(rank<128 ? (int)rank : (int)rank-256),scene->results.penalty,0);
                if(!scene->results.mode) { scene->failed=1;break; }
                scene->results.scroll=scene->results.mode==1 ? 512 : scene->results.scroll;
                scene->results.show_labels=scene->results.mode==1;
                scene->results.timer=nm_race_timeout_ticks(scene->results.penalty);
                scene->race.cars[0].car.speed_index=0;
                scene->race.phase=26;
                scene->results.announcement_cursor=0;
                /* b260/b264: engine and slip off, before the entry's channel commands. */
                nm_race_sound_hook_results_off(&scene->results.commands,&scene->results.command_hash);
                for(unsigned i=0;nm_result_entry_command(scene->results.mode,i);++i)
                    result_silent_command(&scene->results,nm_result_entry_command(scene->results.mode,i));
                puts(scene->results.after_podium ?
                    "Native result phase26: field/GOAL updates disabled; authored glyph actor and silent rank schedule active; entry audio resets pending" :
                    "Native result phase26: opponents parked (4110), field interactions running (1210c), GOAL updates disabled; authored glyph actor and silent rank schedule active; entry audio resets pending");
                puts("Native result labels: original resident art, live rank/time, horizontal slide; penalty art pending");
                if(!result_field_tick(scene,results_phase)) { scene->failed=1;break; }
            } else {
                scene->race.phase=nm_race_timeout_timer(&scene->results.timer,&scene->results.penalty);
                if(scene->results.mode==3) {
                    int32_t countdown=(int32_t)scene->results.timer;
                    if(!nm_result_playback_step(&scene->results.playback,nm_result_keys[0],21,nm_result_targets,37,&countdown,scene->results.start_held)) {
                        scene->failed=1;break;
                    }
                    scene->results.timer=(uint32_t)countdown;
                } else {
                    const unsigned effects=nm_result_wait_step(&scene->results.mode,(int32_t)scene->results.timer,&scene->results.scroll);
                    if(effects&4) result_silent_command(&scene->results,0xae1002);
                    if(effects&2) {
                        const unsigned rank=scene->race.cars[0].rank;uint32_t command=0;
                        if(rank>=44 || !nm_result_announcement_step(nm_result_announcements[rank],
                                nm_result_announcement_counts[rank],scene->results.penalty,(int32_t)scene->results.timer,
                                &scene->results.announcement_cursor,&command)) { scene->failed=1;break; }
                        result_silent_command(&scene->results,command);
                    }
                }
                if(!result_field_tick(scene,results_phase)) { scene->failed=1;break; }
                if(scene->race.phase==29) break;
            }
        }
        if(scene->results.show_labels && (scene->results.penalty || !nm_result_labels_enqueue(scene->race.cars[0].rank,
            scene->race.shared.clock,(int16_t)scene->race.cars[0].car.lap.best_ticks,scene->results.scroll))) scene->failed=1;
        if(scene->failed || (scene->results.mode==3 && !nm_result_render_enqueue(&scene->results.playback.actor,scene->math,scene->instance.center))) {
            /* Labels may already be queued. Keep their resources alive until
             * callbacks finish; the next updater owns failure cleanup. */
            scene->failed=1;
        }
        return 1; /* Disabled race objects never enqueue world/cars/GOAL here. */
    }
    return 0;
}
