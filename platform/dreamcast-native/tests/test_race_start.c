/* Actual cold-start wiring only: no simulation ticks or gameplay-math oracle. */
#undef NDEBUG
#include "race_scene.h"
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
    assert(road.total_laps==8 && !road.checkpoint_count); /* Authored daytona93 course0. */
    for(unsigned i=0;i<8;++i) {
        assert(snprintf(path,sizeof(path),"%s/course0-route%u.dnp",argv[1],i)<(int)sizeof(path));
        assert(nm_path_load(paths+i,path));
    }
    static nm_race_scene_t scene,before;
    nm_road_marks_t marks={calloc(road.profile_count,1),road.profile_count-1,0};assert(marks.values);
    const unsigned counts[]={road.checkpoint_count,1,30,31};
    const uint32_t masks[]={1,3,UINT32_C(0x7fffffff),UINT32_MAX};
    for(unsigned i=0;i<4;++i) {
        nm_road_pack_t configuration=road;configuration.checkpoint_count=counts[i];
        memset(&scene,0xa5,sizeof(scene));
        assert(nm_race_scene_start(&scene,math,&contact,&configuration,paths,0,0,&marks,road.profile_count,NULL));
        const nm_checkpoints_t *c=&scene.cars[0].car.checkpoints;
        assert(c->total_laps==road.total_laps && c->visited==1 && c->required==masks[i]);
        assert(!c->clock && !c->finish_flags && !c->completed && !c->last_lap && !c->reverse);
        for(unsigned id=1;id<scene.count;++id) assert(!scene.cars[id].car.checkpoints.total_laps);
    }
    nm_road_pack_t invalid=road;invalid.checkpoint_count=32;before=scene;
    assert(!nm_race_scene_start(&scene,math,&contact,&invalid,paths,0,0,&marks,road.profile_count,NULL));
    assert(!memcmp(&scene,&before,sizeof(scene)));
    printf("PASS: actual race-start/restart wiring, laps=%u; checkpoint masks actual/1/30/31 and invalid32 unchanged; no simulation ticks\n",road.total_laps);
    free(marks.values);for(unsigned i=0;i<8;++i) nm_path_unload(paths+i);
    nm_road_unload(&road);nm_contact_unload(&contact);free(math);
}
