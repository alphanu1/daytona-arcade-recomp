/* Podium lifecycle wiring with the actual course0 packs: the phase27/28 timer
 * gates the authored stage graph through the shared cinematic scene. Asserts
 * ownership and stage flow only; it is not a gameplay-math oracle. */
#undef NDEBUG
#include "intro_scene.h"
#include "podium_data.h"
#include "podium.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv) {
    assert(argc==2);
    char path[1024];
    assert(snprintf(path,sizeof(path),"%s/drive.dnd",argv[1])<(int)sizeof(path));
    nm_drive_params_t *math=nm_drive_load(path);assert(math);
    nm_contact_pack_t contact={0};nm_road_pack_t road={0};nm_path_pack_t paths[8]={0};
    assert(snprintf(path,sizeof(path),"%s/course0.dnc",argv[1])<(int)sizeof(path));
    assert(nm_contact_load(&contact,path));
    assert(snprintf(path,sizeof(path),"%s/course0.dnr",argv[1])<(int)sizeof(path));
    assert(nm_road_load(&road,path));
    for(unsigned i=0;i<8;++i) {
        assert(snprintf(path,sizeof(path),"%s/course0-route%u.dnp",argv[1],i)<(int)sizeof(path));
        assert(nm_path_load(paths+i,path));
    }
    const unsigned stages=sizeof(nm_podium_stages)/sizeof(nm_podium_stages[0]);
    assert(stages==4 && nm_podium_stages[0].cars==40 && nm_podium_stages[3].cars==10);
    static nm_intro_scene_t scene;
    /* A car on the last profile needs the exported overrun record. */
    unsigned last_profile=0;
    for(unsigned i=0;i<nm_podium_stages[0].cars;++i)
        last_profile+=nm_podium_stages[0].spawn[i].profile==road.profile_count-1;
    assert(last_profile);
    assert(nm_intro_scene_start_stages(&scene,nm_podium_stages,stages,1u<<28,NULL,math,&road,paths));
    assert(!nm_intro_scene_step(&scene,math,&contact,&road,paths));
    assert(!nm_intro_scene_start_stages(&scene,nm_podium_stages,stages,1u<<22,
        &nm_podium_overrun_profile,math,&road,paths));

    nm_podium_t podium={.phase=27};
    unsigned cinematic=0,entered[4]={0},overrun_ticks=0,crash_ticks=0;
    while(podium.phase!=25) {
        assert(nm_podium_step(&podium));
        /* As code/practice.c: entry only arms the graph; the scene spawns one tick later. */
        if(!podium.cinematic || podium.entered) continue;
        if(podium.spawn) {
            assert(nm_intro_scene_start_stages(&scene,nm_podium_stages,stages,1u<<28,
                &nm_podium_overrun_profile,math,&road,paths));
            assert(scene.count==40 && scene.stage==0 && scene.flags==(1u<<28));
        } else {
            for(unsigned i=0;i<scene.count;++i)
                overrun_ticks+=(uint16_t)scene.cars[i].phase[0]==road.profile_count;
            assert(nm_intro_scene_step(&scene,math,&contact,&road,paths));
        }
        assert(scene.stage<stages && scene.count==nm_podium_stages[scene.stage].cars);
        for(unsigned i=0;i<scene.count;++i) {
            assert((uint16_t)scene.cars[i].phase[0]<=road.profile_count);
            crash_ticks+=scene.cars[i].boundary.damage.remaining!=0;
        }
        if(!entered[scene.stage]) entered[scene.stage]=cinematic+1;
        ++cinematic;
    }
    assert(cinematic==1214 && scene.ticks==1213);
    /* Each spawned overrun is consumed by that car's first movement tick. */
    assert(overrun_ticks==last_profile);
    assert(entered[0]==1 && entered[1] && entered[2]>entered[1] && entered[3]>entered[2] &&
           scene.stage==3 && scene.transitions==3);
    printf("PASS: podium scene 1214 ticks after the entry tick; stages entered at %u/%u/%u/%u; overrun reads=%u crash car-ticks=%u pairs=%lu responses=%lu\n",
        entered[0],entered[1],entered[2],entered[3],overrun_ticks,crash_ticks,
        (unsigned long)scene.collision_pairs,(unsigned long)scene.collision_responses);
    for(unsigned i=0;i<8;++i) nm_path_unload(paths+i);
    nm_road_unload(&road);nm_contact_unload(&contact);free(math);
    return 0;
}
