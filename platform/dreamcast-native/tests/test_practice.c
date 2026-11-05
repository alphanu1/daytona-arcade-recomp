/* Run with DND23, course0 DNC3/DNR3 and course0-route0 DNP1. No board/runtime.
 * cc -std=c11 -O2 -fno-fast-math -ffp-contract=off tests/test_practice.c
 *    code/{drive,contact,road,path,geometry,meshpack,material_pack,visibility}.c -lm -lz -o test-practice
 * Uses the game's 384KiB car bank; no standalone 512KiB variant. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../include/practice.h"
#include "../include/pose.h"
#include "../include/meshpack.h"
#include "../include/geometry.h"
#include "../include/material_pack.h"
#include "../include/lighting.h"
#include "../include/visibility.h"
#include "../include/render/scene/sky.h"
#include "practice_script.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void check_lap_history(void) {
    nm_practice_t s={0};
    s.car.lap_write=(nm_lap_write_t){1234,7,0};
    nm_practice_record_lap(&s);
    assert(!s.lap_recorded[0] && !s.lap_times[7]);
    for(unsigned i=0;i<256;++i) {
        const uint32_t ticks=i ? UINT32_MAX-i : 0;
        s.car.lap_write=(nm_lap_write_t){ticks,(uint8_t)i,1};
        nm_practice_record_lap(&s);
        assert(s.last_lap_index==i && s.lap_times[i]==ticks);
        assert(s.lap_recorded[i>>3]&(1u<<(i&7)));
    }
    for(unsigned i=0;i<32;++i) assert(s.lap_recorded[i]==255);
    s.car.lap_write=(nm_lap_write_t){65537,0,1};
    nm_practice_record_lap(&s);
    assert(s.last_lap_index==0 && s.lap_times[0]==65537 && s.lap_times[255]==UINT32_MAX-255);
    s=(nm_practice_t){0};
    const nm_road_pack_t road={.profile_count=3};
    uint16_t previous=2;
    uint32_t flags=0;
    s.car.lap.furthest=2;s.car.lap.start_ticks=10;
    s.car.checkpoints=(nm_checkpoints_t){.visited=1,.required=1,.clock=70000,.total_laps=2};
    assert(nm_checkpoint_step(&road,0,&previous,&flags,&s.car.lap,
        &s.car.checkpoints,&s.car.lap_write));
    nm_practice_record_lap(&s);
    assert(s.car.lap_write.written && s.last_lap_index==1 && s.lap_times[1]==69990);
    assert(s.car.lap.last_ticks==(uint16_t)69990 && s.lap_recorded[0]==2);
    puts("PASS native lap history: all byte indices, full32-bit ticks, zero-time validity, overwrite and no-event");
}

static void check_sky_layout(void) {
    assert(!nm_sky_rects(0,0,NULL));
    unsigned maximum=0;
    for(unsigned v=0;v<1024;++v) for(unsigned u=0;u<2048;++u) {
        nm_sky_rect_t rects[NM_SKY_RECTS];
        const unsigned count=nm_sky_rects((uint16_t)(u*32),(uint16_t)v,rects);
        assert(count && count<=NM_SKY_RECTS);
        if(count>maximum) maximum=count;
        unsigned x=0,y=0,row_height=0;
        for(unsigned r=0;r<count;++r) {
            const nm_sky_rect_t *p=rects+r;
            assert(p->x==x && p->y==y && p->width && p->height);
            assert(x+p->width<=496 && y+p->height<=384);
            if(!x) row_height=p->height;
            assert(p->height==row_height);
            for(unsigned corner=0;corner<4;++corner) {
                const unsigned dx=corner&1 ? p->width-1 : 0,dy=corner&2 ? p->height-1 : 0;
                const unsigned sx=(p->x+dx-u)&2047,sy=(p->y+dy+v)&1023;
                if(sy>=48 && sy<440)
                    assert(p->page==sx/512 && p->u+dx==sx%512 && p->v+dy==sy-48);
                else assert(p->page==(sy>=440 && sy<672 ? NM_SKY_GROUND : NM_SKY_BLUE));
            }
            x+=p->width;
            if(x==496) {x=0;y+=row_height;}
        }
        assert(!x && y==384);
    }
    printf("PASS native sky layout: all2097152 scroll pairs, exact coverage, maximum%u rectangles\n",maximum);
}

static void check_pose(const nm_drive_params_t *math) {
    const float xyz[3]={99,-3,148};
    for (unsigned i=0; i<65536; ++i) {
        const int16_t angles[3]={(int16_t)i,(int16_t)(i*17),(int16_t)(i*37)};
        float m[12]; assert(nm_body_pose(math,xyz,angles,m));
        const double p=angles[0]*(6.283185307179586/65536), y=angles[1]*(6.283185307179586/65536);
        const double r=angles[2]*(6.283185307179586/65536);
        for (unsigned c=0; c<3; ++c) {
            /* Independent point-by-point roll -> pitch -> yaw application. */
            const double x0=c==0, y0=c==1, z0=c==2;
            const double x1=cos(r)*x0+sin(r)*y0, y1=-sin(r)*x0+cos(r)*y0;
            const double y2=cos(p)*y1+sin(p)*z0, z2=-sin(p)*y1+cos(p)*z0;
            const double expected[3]={cos(y)*x1-sin(y)*z2,y2,sin(y)*x1+cos(y)*z2};
            for (unsigned a=0; a<3; ++a) assert(fabs(m[c*3+a]-expected[a])<1e-5);
        }
        assert(!memcmp(m+9,xyz,12));
        float view[12], combined[12], alias[12];
        nm_player_t car={0}; memcpy(car.boundary.ground.position,xyz,sizeof(xyz));
        car.boundary.ground.pitch=angles[0];car.boundary.ground.yaw=angles[1];car.boundary.ground.roll=angles[2];
        int16_t heading;
        assert(nm_follow_view(math,&car,view,&heading) && heading==angles[1]);
        assert(nm_pose_compose(view,m,combined));
        memcpy(alias,m,sizeof(m)); assert(nm_pose_compose(view,alias,alias));
        assert(!memcmp(alias,combined,sizeof(alias)));
        /* Independent double body-frame eye position: full roll, half pitch
         * rounded toward negative infinity, then yaw. Camera pitch changes
         * orientation, NOT this world eye position. */
        float offset[3];memcpy(offset,math->camera_reset_words,sizeof(offset));
        const double half=floor(angles[0]/2.0)*(6.283185307179586/65536);
        const double x1=cos(r)*offset[0]+sin(r)*offset[1], y1=-sin(r)*offset[0]+cos(r)*offset[1];
        const double y2=cos(half)*y1+sin(half)*offset[2],z2=-sin(half)*y1+cos(half)*offset[2];
        const double eye[3]={xyz[0]+cos(y)*x1-sin(y)*z2,xyz[1]+y2,xyz[2]+sin(y)*x1+cos(y)*z2};
        for(unsigned a=0;a<3;++a)
            assert(fabs(view[a]*eye[0]+view[3+a]*eye[1]+view[6+a]*eye[2]+view[9+a])<1e-4);
        float origin[2]; uint16_t cell, expected_cell;
        assert(nm_follow_scenery_origin(math,&car,heading,origin,&cell));
        assert(fabs(origin[0]-(xyz[0]+sin(y)*(10-offset[2])))<1e-4);
        assert(fabs(origin[1]-(xyz[2]-cos(y)*(10-offset[2])))<1e-4);
        assert(nm_contact_scenery_cell(origin[0],origin[1],&expected_cell) && cell==expected_cell);
        uint16_t sky=0xabcd;
        /* Original-instruction equality is checked by m2segment; this loop
         * also runs under ASan/UBSan across all pitch bits, without a board. */
        if(nm_follow_sky_scroll(math,&car,&sky)) assert(sky<=4095);
        else assert(sky==0xabcd);
    }
    float out[12]={0}, before[12]={0};
    const float bad[3]={NAN,0,0}; const int16_t angles[3]={0};
    assert(!nm_body_pose(math,bad,angles,out) && !memcmp(out,before,sizeof(out)));
    nm_player_t invalid={0};memcpy(invalid.boundary.ground.position,bad,sizeof(bad));
    int16_t heading=123;
    assert(!nm_follow_view(math,&invalid,out,&heading) && !memcmp(out,before,sizeof(out)) && heading==123);
    float origin[2]={123,456}; uint16_t cell=78;
    assert(!nm_follow_scenery_origin(math,&invalid,0,origin,&cell) && origin[0]==123 && origin[1]==456 && cell==78);
    assert(!nm_wheel_pose(math,&invalid,4,xyz,out) && !memcmp(out,before,sizeof(out)));
    assert(!nm_wheel_pose(math,&invalid,0,bad,out) && !memcmp(out,before,sizeof(out)));
    assert(!nm_wheel_pose(NULL,&invalid,0,xyz,out) && !memcmp(out,before,sizeof(out)));
    assert(!nm_wheel_pose(math,NULL,0,xyz,out) && !memcmp(out,before,sizeof(out)));
    assert(!nm_wheel_pose(math,&invalid,0,NULL,out) && !memcmp(out,before,sizeof(out)));
    assert(!nm_wheel_pose(math,&invalid,0,xyz,NULL));
    uint16_t sky=123;
    assert(!nm_follow_sky_scroll(NULL,&invalid,&sky) && sky==123);
    assert(!nm_follow_sky_scroll(math,NULL,&sky) && sky==123);
    assert(!nm_follow_sky_scroll(math,&invalid,NULL));
    invalid.boundary.ground.position[1]=NAN;
    assert(!nm_follow_sky_scroll(math,&invalid,&sky) && sky==123);
    invalid.boundary.ground.position[1]=INFINITY;
    assert(!nm_follow_sky_scroll(math,&invalid,&sky) && sky==123);
    nm_player_t wheels={0};
    for(unsigned w=0;w<4;++w) {
        assert(nm_wheel_pose(math,&wheels,w,xyz,out));
        assert(nm_body_pose(math,xyz,angles,before));
        for(unsigned a=0;a<12;++a) assert(out[a]==before[a]);
    }
}

static uint32_t cache_peak;
static unsigned projected_car_ticks;
/* Optional host-only residency trace: real retained player, source normal follow
 * camera, ALL authored scenery, no sampled board poses. Flags 1/2 mean before/
 * after triangle clipping; whole-object rejection is independently checked.
 * Not a full-course/reverse-camera residency proof or target timing result. */
static void trace_world(const nm_drive_params_t *math, const nm_practice_t *state,
                        const nm_pack_t *world, const nm_scenery_pack_t *scenery) {
    float view[12], light[3];
    int16_t heading;
    assert(nm_follow_view(math,&state->car,view,&heading));
    const nm_view_t camera={{320,220},{0,640,0,480},0.1f};
    const float focus[2]={400,400};
    uint8_t *keys=calloc(world->header->materials,256); assert(keys);
    unsigned triangles=0;
    uint8_t selected[65536]={0}; // Host evidence only, not target stack storage.
    if (scenery->storage) {
        nm_scenery_cells_t cells;
        assert(scenery->instances==world->header->instances && scenery->slot==0);
        float origin[2]; uint16_t cell;
        assert(nm_follow_scenery_origin(math,&state->car,heading,origin,&cell));
        assert(nm_scenery_select(scenery,math,cell,(uint16_t)state->car.phase[0],origin[0],origin[1],heading,&cells));
        for (unsigned i=0;i<cells.draw_count;++i) {
            const nm_scenery_cell_t *cell=scenery->cells+cells.draw[i];
            memset(selected+cell->first,1,cell->count);
        }
        if (state->ticks==528) {
            unsigned checked=0;
            for (unsigned i=0;i<world->header->instances;++i)
                if (world->instances[i].source_address==0x801648) { assert(selected[i]); ++checked; }
            assert(checked==1); // Rear road/wall visible to follow camera; player's wedge omitted it.
            puts("PASS follow-scenery tick528: rear road/wall cell retained");
        }
    }
    for (unsigned a=0;a<3;++a)
        light[a]=nm_race_world_light[0]*view[a]+nm_race_world_light[1]*view[3+a]+nm_race_world_light[2]*view[6+a];
    for (unsigned i=0;i<world->header->instances;++i) {
        if (scenery->storage && !selected[i]) continue;
        const nm_mesh_t *mesh=world->meshes+world->instances[i].mesh;
        const int bounds=nm_bounds_visible(&camera,mesh->bounds,view,focus);
        for (unsigned f=0;f<mesh->face_count;++f) {
            const nm_face_t *face=world->faces+mesh->face_first+f;
            const nm_material_t *material=world->materials+face->material;
            float normal[3], points[4][3];
            for (unsigned a=0;a<3;++a) {
                normal[a]=face->normal[0]*view[a]+face->normal[1]*view[3+a]+face->normal[2]*view[6+a];
                for (unsigned v=0;v<face->count;++v) {
                    const float *p=world->vertices[mesh->vertex_first+face->indices[v]].position;
                    points[v][a]=p[0]*view[a]+p[1]*view[3+a]+p[2]*view[6+a]+view[9+a];
                }
            }
            const int lighting=nm_face_lighting(mesh->mode,normal,(const float (*)[3])points,light,material);
            assert(lighting>=0);
            if ((!(face->attribute & (1u<<17)) && (lighting & 256)) ||
                !(face->attribute & (3u<<8)) || (material->header[0] & 0x8000)) continue;
            unsigned key=face->material*256+(lighting&255);
            if (bounds) keys[key]|=1;
            for (unsigned t=1;t+1<face->count;++t) {
                const unsigned indices[3]={0,t,t+1};
                nm_view_vertex_t input[3]; nm_screen_vertex_t output[NM_CLIP_VERTICES];
                for (unsigned v=0;v<3;++v) {
                    const float *p=points[indices[v]];
                    input[v]=(nm_view_vertex_t){p[0]*400,p[1]*400,p[2],0,0};
                }
                const unsigned count=nm_project_triangle(&camera,input,output);
                for(unsigned v=0;v<count;++v) assert(output[v].q>NM_SKY_DEPTH);
                if (count) { assert(bounds); keys[key]|=2; triangles+=count-2; }
            }
        }
    }
    printf("WORLD_FRAME %u %g %g %g %d %u\n",state->ticks,
        (double)state->car.boundary.ground.position[0],(double)state->car.boundary.ground.position[1],
        (double)state->car.boundary.ground.position[2],heading,triangles);
    for (unsigned k=0;k<world->header->materials*256;++k)
        if (keys[k]) printf("WORLD_KEY %u %u %u\n",k/256,k%256,keys[k]);
    free(keys);
}

static void check_projected_car(const nm_drive_params_t *math, const nm_player_t *car,
                                const nm_pack_t *pack, const nm_material_pack_t *materials) {
    float body[12], view[12], body_view[12], m[12];
    assert(nm_body_pose(math,car->boundary.ground.position,car->render_angles,body));
    int16_t heading;
    assert(nm_follow_view(math,car,view,&heading));
    assert(nm_pose_compose(view,body,body_view));
    const nm_view_t camera={{320,220},{0,640,0,480},0.1f};
    unsigned triangles=0;
    uint32_t offsets[4096], used=0;
    memset(offsets,255,sizeof(offsets));
    for(unsigned part=0;part<pack->header->instances;++part) {
      const nm_instance_t *source=pack->instances+part;
      const nm_mesh_t *mesh=pack->meshes+source->mesh;
      if(!part || part>=5) memcpy(m,body_view,sizeof(m));
      else {
        float local[12];
        assert(nm_wheel_pose(math,car,part-1,source->matrix+9,local));
        assert(nm_pose_compose(body_view,local,m));
      }
      for (unsigned f=0; f<mesh->face_count; ++f) {
        const nm_face_t *face=pack->faces+mesh->face_first+f;
        if (materials->data) {
            float normal[3], points[3][3], light[3];
            for (unsigned a=0;a<3;++a) {
                light[a]=nm_race_world_light[0]*view[a]+nm_race_world_light[1]*view[3+a]+nm_race_world_light[2]*view[6+a];
                normal[a]=face->normal[0]*m[a]+face->normal[1]*m[3+a]+face->normal[2]*m[6+a];
                for (unsigned v=0;v<3;++v) {
                    const float *p=pack->vertices[mesh->vertex_first+face->indices[v]].position;
                    points[v][a]=p[0]*m[a]+p[1]*m[3+a]+p[2]*m[6+a]+m[9+a];
                }
            }
            const nm_material_t *material=pack->materials+face->material;
            const int lighting=nm_face_lighting(mesh->mode,normal,points,light,material);
            assert(lighting>=0);
            const nm_material_binding_t *b=nm_material_find(materials,material->header,lighting&255);
            assert(b);
            const nm_material_uv_t uv=nm_material_uv(materials,b);
            for(unsigned v=0;v<face->count;++v) {
                const nm_vertex_t *p=pack->vertices+mesh->vertex_first+face->indices[v];
                assert(nm_material_uv_contains(&uv,p->u,p->v));
            }
            if (b->resource!=NM_MATERIAL_SOLID)
                assert(nm_material_cache_reserve(materials,b->resource,offsets,&used,NM_MATERIAL_CACHE_BYTES));
        }
        for (unsigned j=1; j+1<face->count; ++j) {
            const unsigned indices[3]={face->indices[0],face->indices[j],face->indices[j+1]};
            nm_view_vertex_t points[3];
            for (unsigned i=0; i<3; ++i) {
                const float *p=pack->vertices[mesh->vertex_first+indices[i]].position;
                float t[3];
                for (unsigned a=0; a<3; ++a) t[a]=p[0]*m[a]+p[1]*m[3+a]+p[2]*m[6+a]+m[9+a];
                points[i]=(nm_view_vertex_t){400*t[0],400*t[1],t[2],0,0};
            }
            nm_screen_vertex_t out[NM_CLIP_VERTICES];
            const unsigned n=nm_project_triangle(&camera,points,out);
            if (n) triangles+=n-2;
            for (unsigned i=0; i<n; ++i)
                assert(isfinite(out[i].q) && out[i].q>NM_SKY_DEPTH && out[i].x>=0 && out[i].x<=640 && out[i].y>=0 && out[i].y<=480);
        }
      }
    }
    assert(triangles>0);
    if (used>cache_peak) cache_peak=used;
    ++projected_car_ticks;
}

static void check_all_car_lights(const nm_pack_t *model, const nm_material_pack_t *materials) {
    uint32_t bound=0;
    for(unsigned part=0;part<model->header->instances;++part) {
      const nm_mesh_t *mesh=model->meshes+model->instances[part].mesh;
      for (unsigned f=0;f<mesh->face_count;++f) {
        const nm_face_t *face=model->faces+mesh->face_first+f;
        uint32_t maximum=0;
        const nm_material_t *material=model->materials+face->material;
        for (unsigned l=(unsigned)material->ambient;l<256;++l) {
            const nm_material_binding_t *b=nm_material_find(materials,material->header,l);
            assert(b);
            if (b->resource!=NM_MATERIAL_SOLID) {
                const uint32_t size=materials->textures[b->resource].size-32;
                if (size>maximum) maximum=size;
            }
            const nm_material_uv_t uv=nm_material_uv(materials,b);
            for (unsigned v=0;v<face->count;++v) {
                const nm_vertex_t *p=model->vertices+mesh->vertex_first+face->indices[v];
                assert(nm_material_uv_contains(&uv,p->u,p->v));
            }
        }
        bound+=maximum;
      }
    }
    /* Preserve the old body-only guarantee. For moving wheels this loose sum
     * exceeds the bank; do NOT claim universal residency from the live trace.
     * Runtime still rejects overflow. Full camera/damage coverage is open. */
    if(model->header->instances==1) assert(bound<=NM_MATERIAL_CACHE_BYTES);
    printf("PASS all reachable car luma/UV bindings; sum-per-face=%u, VRAM cache=%u bytes; bound fits=%s; live unculled trace separately enforced\n",
        bound,NM_MATERIAL_CACHE_BYTES,bound<=NM_MATERIAL_CACHE_BYTES ? "yes" : "NO (not a universal residency proof)");
}

int main(int argc, char **argv) {
    if (argc<5 || argc>10) { fprintf(stderr,"usage: test-practice drive.dnd course0.dnc course0.dnr course0-route0.dnp [car.dnm [car.dnt [reference-scene.dnm [world.dnm [scenery.dnv]]]]]\n"); return 2; }
    if (argc>=8) {
        nm_pack_t source={0}; assert(nm_pack_load(&source,argv[7]));
        unsigned checked=0;
        for (unsigned i=0;i<source.header->instances;++i) {
            const nm_instance_t *v=source.instances+i;
            if (v->source_address!=0x80f80d) continue; // Authored world-space scenery anchor.
            for (unsigned a=0;a<3;++a) {
                const float expected=nm_race_world_light[0]*v->matrix[a]+nm_race_world_light[1]*v->matrix[3+a]+nm_race_world_light[2]*v->matrix[6+a];
                assert(fabsf(expected-v->light[a])<1e-6f);
            }
            ++checked;
        }
        assert(checked); nm_pack_unload(&source);
        printf("PASS normal-race world light against %u captured scenery instances\n",checked);
    }
    nm_drive_params_t *math = nm_drive_load(argv[1]);
    nm_contact_pack_t contact = {0}; nm_road_pack_t road = {0}; nm_path_pack_t path = {0};
    assert(math && nm_contact_load(&contact,argv[2]) && nm_road_load(&road,argv[3]) && nm_path_load(&path,argv[4]));
    check_pose(math);
    check_lap_history();
    check_sky_layout();
    if (road.grid_count) {
        nm_player_t grid={0}; float reference_height=123.0f;
        grid.phase[0]=(int16_t)road.grid[0].profile;
        grid.lateral.current=grid.lateral.target=(int16_t)road.grid[0].lateral;
        const nm_player_t before=grid;
        nm_road_pack_t invalid=road; invalid.mapping=NULL;
        assert(!nm_player_grid_pose(math,&contact,&invalid,&path,&grid,&reference_height));
        assert(!memcmp(&grid,&before,sizeof(grid)) && reference_height==123.0f);
        assert(nm_player_grid_pose(math,&contact,&road,&path,&grid,&reference_height));
        assert(grid.phase[1]==before.phase[0]);
        assert((uint16_t)grid.phase[0]==road.mapping[grid.boundary.ground.road].profile);
        for(unsigned i=0;i<4;++i) assert(!grid.boundary.ground.surface[i] && !grid.boundary.ground.mask[i]);
        const uint16_t selected=(uint16_t)grid.phase[0];
        assert(nm_player_rolling_entry_pose(road.profiles+selected,&grid,&reference_height));
        assert((uint16_t)grid.phase[0]==selected+1 && grid.path_offset==(selected+1)*28);
        puts("PASS grid pose/contact/selection to rolling entry, untouched wheel masks and late atomic failure");
        const nm_player_t dirty=grid;
        reference_height=123.0f;
        assert(!nm_player_grid_reset(math,&contact,&invalid,&path,0,1.0f,123,&grid,&reference_height));
        assert(!memcmp(&grid,&dirty,sizeof(grid)) && reference_height==123.0f);
        assert(nm_player_grid_reset(math,&contact,&road,&path,0,1.0f,123,&grid,&reference_height));
        assert(grid.boundary.damage.body_model==123);
        assert((uint16_t)grid.phase[0]==selected && grid.phase[1]==(int16_t)road.grid[0].profile);
        assert(grid.desired_speed==1.0f && grid.boundary.speed==0 && grid.lap.furthest==road.grid[0].previous);
        puts("PASS combined base/grid/pose reset and late atomic failure");
        if(road.grid_guided_scales) {
            const nm_player_t before_start=grid;
            uint32_t flags=0xa4;reference_height=123;
            nm_road_pack_t empty=road;empty.grid_count=0;
            assert(!nm_player_rolling_start(math,&contact,&empty,&path,0,&grid,&reference_height,&flags));
            assert(!nm_player_rolling_start(math,&contact,&invalid,&path,0,&grid,&reference_height,&flags));
            assert(!memcmp(&grid,&before_start,sizeof(grid)) && reference_height==123 && flags==0xa4);
            assert(nm_player_rolling_start(math,&contact,&road,&path,0,&grid,&reference_height,&flags));
            assert(flags==0xa5 && (uint16_t)grid.phase[0]==selected+1);
            assert(grid.boundary.speed==road.start->rolling_speed);
            nm_player_active_input_t in={.dynamics={.speed_flags=1u<<22,.motion_scale=1,
                .guided_scale=road.grid_guided_scales[0]},.proximity_disabled=1};
            unsigned ticks=0;uint8_t released=0;
            while(!released && ticks<4096) {
                assert(nm_player_rolling_tick(math,&contact,&road,&path,&in,&grid,&flags,&released));
                nm_player_effect_timers(&grid);
                ++ticks;
            }
            assert(released && flags==0xa4 && !grid.phase[0]);
            printf("PASS prepared reset/start to release: ticks=%u (standalone, not full race)\n",ticks);
        }
    }
    nm_pack_t model={0};
    nm_pack_t world={0};
    if (argc>=9) assert(nm_pack_load(&world,argv[8]));
    nm_scenery_pack_t scenery={0};
    if (argc==10) assert(nm_scenery_load(&scenery,argv[9]));
    if (argc>=6) assert(nm_pack_load(&model,argv[5]) &&
        ((model.header->meshes==1 && model.header->instances==1) ||
         (model.header->meshes==3 && model.header->instances==5) ||
         (model.header->meshes==5 && model.header->instances==7)));
    nm_vertex_t *window_vertices=NULL; unsigned window_count=0;
    uint16_t window_base[72]={0};
    if(model.data && model.header->instances==7) {
        const nm_mesh_t *window=model.meshes+model.instances[5].mesh;
        assert(window->vertex_count<=36 && window->face_count==9);
        window_count=window->vertex_count;
        window_vertices=(nm_vertex_t *)model.vertices+window->vertex_first;
        for(unsigned i=0;i<window_count;++i) {
            window_base[2*i]=window_vertices[i].u;window_base[2*i+1]=window_vertices[i].v;
        }
    }
    {
        nm_vertex_t vertices[2]={{{1,2,3},9,10},{{4,5,6},11,12}};
        const uint16_t base[4]={65535,65534,1,2};
        assert(!nm_reflection_apply(NULL,base,2,3,4));
        assert(!nm_reflection_apply(vertices,NULL,2,3,4));
        assert(!nm_reflection_apply(vertices,base,0,3,4));
        assert(!nm_reflection_apply(vertices,base,37,3,4));
        assert(vertices[0].u==9 && vertices[1].v==12);
        for(unsigned i=0;i<2;++i) {
            assert(nm_reflection_apply(vertices,base,2,3,4));
            assert(vertices[0].u==2 && vertices[0].v==2 && vertices[1].u==4 && vertices[1].v==6);
            assert(vertices[0].position[0]==1 && vertices[1].position[2]==6);
        }
    }
    nm_material_pack_t materials={0};
    if (argc>=7) {
        assert(!nm_material_pack_open(&materials,argv[6])); // Oversized for resident VRAM, never raise that guard.
        assert(nm_material_pack_open_cpu(&materials,argv[6]) && materials.resources<=4096);
        check_all_car_lights(&model,&materials);
    }
    nm_road_marks_t marks = {.count=road.profile_count-1}; marks.values = calloc(marks.count,1); assert(marks.values);
    nm_practice_t s = {0}, first = {0};
    assert(nm_practice_reset(&s,math,&contact,&road,&path,&marks)); first=s;
    {
        nm_player_active_input_t automatic={.dynamics={.speed_flags=1u<<24,
            .motion_scale=1,.guided_scale=1,.course_slot=contact.slot},.proximity_disabled=1};
        for(unsigned branch=0;branch<4;++branch) {
            nm_player_t car=first.car;
            assert(nm_player_finish_entry(&car));
            car.boundary.speed=1;
            car.path_flags=branch==NM_FINISH_GUIDED ? 1 : 0;
            car.boundary.heading=nm_yaw_angle((uint32_t)((int)car.boundary.ground.road_heading+
                (branch==NM_FINISH_REVERSE ? 20000 : 0)));
            car.boundary.ground.yaw=car.boundary.heading;
            car.boundary.damage.initial_inhibit=5;
            if(branch==3) car.boundary.damage.spin_ticks=8;
            const nm_player_t before=car;
            if(branch<3) assert(nm_player_finish_branch(&car)==branch);
            nm_player_t other=car;
            nm_player_active_input_t ignored=automatic;
            ignored.controls=(nm_controls_t){.steer=127,.brake=255};ignored.gear_selector=7;
            assert(nm_player_finish_prefix(math,&contact,&road,&path,&automatic,&car));
            assert(nm_player_finish_prefix(math,&contact,&road,&path,&ignored,&other));
            assert(!memcmp(&car,&other,sizeof(car)));
            assert(car.boundary.damage.initial_inhibit==4 && car.spin_body_offset==512);
            assert(!memcmp(&car.checkpoints,&before.checkpoints,sizeof(car.checkpoints)));
            nm_contact_pack_t invalid_contact=contact;invalid_contact.quad_count=0;
            other=before;
            assert(!nm_player_finish_prefix(math,&invalid_contact,&road,&path,&automatic,&other));
            assert(!memcmp(&other,&before,sizeof(other)));
            other=car;
            for(unsigned tick=1;tick<32;++tick) {
                nm_player_effect_timers(&car);nm_player_effect_timers(&other);
                assert(nm_player_finish_prefix(math,&contact,&road,&path,&automatic,&car));
                assert(nm_player_finish_prefix(math,&contact,&road,&path,&ignored,&other));
                assert(!memcmp(&car,&other,sizeof(car)));
            }
        }
        puts("PASS composed finish prefix: four seeded branches x32ticks, ignored controls, timers and late failure atomicity (not whole-prefix source parity)");
    }
    if(road.grid_guided_scales) {
        nm_practice_t single=first,batched=first;
        nm_road_marks_t a={.count=marks.count},b={.count=marks.count};
        a.values=calloc(a.count,1);b.values=calloc(b.count,1);assert(a.values && b.values);
        assert(nm_practice_start_rolling(&single,math,&contact,&road,&path));
        assert(nm_practice_start_rolling(&batched,math,&contact,&road,&path));
        const uint32_t initial_ticks=road.awards->ticks[0][0];
        assert(single.award.remaining==initial_ticks && single.race_phase==22);
        const nm_controls_t controls={.accel=255,.steer=64};
        unsigned release_tick=0;
        for(unsigned tick=0;tick<800;++tick) {
            const int rolling=single.rolling_active;
            assert(nm_practice_advance(&single,math,&contact,&road,&path,&a,&controls,NM_PRACTICE_TICK_US));
            if(rolling && !single.rolling_active) release_tick=single.ticks;
            if(rolling) assert(single.car.checkpoints.clock==0 && single.award.remaining==initial_ticks);
            if(single.rolling_active) assert(single.car.steer==0);
            if(release_tick && single.ticks==release_tick+1) {
                assert(single.car.steer==8192 && single.car.checkpoints.clock==1);
                assert(single.award.remaining==initial_ticks-1);
            }
        }
        for(unsigned tick=0;tick<800;tick+=4)
            assert(nm_practice_advance(&batched,math,&contact,&road,&path,&b,&controls,4*NM_PRACTICE_TICK_US));
        /* Integer half-filter converges to (64<<8)-1 after takeover. */
        assert(release_tick==563 && !single.rolling_active && single.rolling_enabled && single.car.steer==16383);
        assert(!memcmp(&single,&batched,sizeof(single)) && !memcmp(a.values,b.values,a.count));
        for(unsigned finish=0;finish<2;++finish) {
            nm_practice_t ended=single;
            ended.award.remaining=finish ? 100 : 0;
            ended.car.checkpoints.finish_flags=3;
            nm_practice_t grouped=ended;
            nm_road_marks_t grouped_marks=a;
            grouped_marks.values=malloc(a.count);assert(grouped_marks.values);
            memcpy(grouped_marks.values,a.values,a.count);
            const nm_player_t before=ended.car;
            assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,NM_PRACTICE_TICK_US));
            assert(ended.race_phase==(finish ? 23 : 25));
            assert(ended.car.checkpoints.clock==before.checkpoints.clock);
            assert(ended.car.checkpoints.finish_flags==(finish ? 2u : 3u));
            assert(ended.award.remaining==(finish ? 99u : UINT32_MAX));
            /* Decision tick still runs normal physics under ACTIVE phase22. */
            assert(memcmp(&ended.car,&before,sizeof(before)));
            const nm_player_t stopped=ended.car;
            if(finish) {
                assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,NM_PRACTICE_TICK_US));
                assert(ended.race_phase==24 && ended.phase_timer==384);
                assert(memcmp(ended.car.boundary.ground.position,stopped.boundary.ground.position,12));
                for(unsigned tick=0;tick<384;++tick) {
                    assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,NM_PRACTICE_TICK_US));
                    assert(ended.race_phase==24);
                }
                assert(ended.phase_timer==0);
                assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,NM_PRACTICE_TICK_US));
                assert(ended.race_phase==27 && ended.phase_timer==UINT32_MAX);
                const nm_player_t result=ended.car;
                assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,4*NM_PRACTICE_TICK_US));
                assert(!memcmp(&ended.car,&result,sizeof(result)));
            } else {
                assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,NM_PRACTICE_TICK_US));
                assert(ended.race_phase==26 && ended.phase_timer==800);
                for(unsigned tick=0;tick<672;++tick) {
                    assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,NM_PRACTICE_TICK_US));
                    assert(ended.race_phase==26);
                }
                assert(ended.phase_timer==128);
                assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,NM_PRACTICE_TICK_US));
                assert(ended.race_phase==29 && ended.phase_timer==127);
                assert(nm_practice_advance(&ended,math,&contact,&road,&path,&a,&controls,4*NM_PRACTICE_TICK_US));
                assert(!memcmp(&ended.car,&stopped,sizeof(stopped)));
            }
            assert(ended.award.remaining==(finish ? 99u : UINT32_MAX));
            const unsigned elapsed_ticks=ended.ticks-grouped.ticks;
            for(unsigned done=0;done<elapsed_ticks;) {
                const unsigned count=elapsed_ticks-done<4 ? elapsed_ticks-done : 4;
                assert(nm_practice_advance(&grouped,math,&contact,&road,&path,&grouped_marks,&controls,count*NM_PRACTICE_TICK_US));
                done+=count;
            }
            assert(!memcmp(&ended,&grouped,sizeof(ended)));
            assert(a.timer==grouped_marks.timer && !memcmp(a.values,grouped_marks.values,a.count));
            free(grouped_marks.values);
        }
        assert(!nm_practice_start_rolling(&single,math,&contact,&road,&path));
        assert(!memcmp(&single,&batched,sizeof(single)));
        assert(nm_practice_reset(&single,math,&contact,&road,&path,&a));
        assert(nm_practice_start_rolling(&single,math,&contact,&road,&path));
        batched=first;
        assert(nm_practice_start_rolling(&batched,math,&contact,&road,&path));
        assert(!memcmp(&single,&batched,sizeof(single)));
        free(a.values);free(b.values);
        puts("PASS session rolling/clock/controller and finish23->24->27,384+1 timer ticks, timeout/results hold");
    }
    {
        nm_player_t rolling=s.car;
        float reference_height;
        assert(nm_player_rolling_entry_pose(road.profiles+(uint16_t)rolling.phase[0],&rolling,&reference_height));
        assert(reference_height==rolling.boundary.ground.position[1]);
        rolling.boundary.speed=nm_force_float(0x3fa6aaab); // Source028b574c; checked by original-code entry oracle.
        const nm_player_t entry=rolling;
        nm_player_t other=rolling;
        nm_player_active_input_t automatic={.dynamics={.speed_flags=1u<<22,.course_slot=contact.slot},.proximity_disabled=1};
        nm_player_active_input_t ignored=automatic;
        ignored.controls.steer=127; ignored.controls.brake=255; ignored.gear_selector=7;
        for (unsigned tick=0;tick<32;++tick) {
            assert(nm_player_rolling_prefix(math,&contact,&road,&path,&automatic,&rolling));
            assert(nm_player_rolling_prefix(math,&contact,&road,&path,&ignored,&other));
            assert(!memcmp(&rolling,&other,sizeof(rolling))); // Player controls do not replace rolling pedals/gears.
        }
        assert(memcmp(&rolling,&s.car,sizeof(rolling)));
        assert(!memcmp(&rolling.lap,&entry.lap,sizeof(rolling.lap)));
        assert(!memcmp(&rolling.checkpoints,&s.car.checkpoints,sizeof(rolling.checkpoints)));
        assert(!memcmp(rolling.tilt,s.car.tilt,sizeof(rolling.tilt)));
        for (unsigned i=0;i<marks.count;++i) assert(!marks.values[i]);
        const nm_player_t before=rolling;
        nm_contact_pack_t invalid=contact; invalid.quad_count=0;
        assert(!nm_player_rolling_prefix(math,&invalid,&road,&path,&automatic,&rolling));
        assert(!memcmp(&rolling,&before,sizeof(rolling))); // Failure after preparation must not publish.
        puts("PASS connected rolling prefix: 32 ticks, ignored player controls and late atomic failure (not full race start)");
        for (unsigned initial_spin=2;initial_spin<=32;++initial_spin) {
            nm_player_t spin=entry;
            spin.boundary.damage.spin_ticks=(uint8_t)initial_spin;
            spin.boundary.damage.spin_heading=spin.boundary.heading;
            spin.boundary.damage.spin_rotation=12000;
            spin.boundary.damage.initial_inhibit=17;spin.spin_body_offset=-16384;
            nm_player_t expected=spin;
            assert(nm_player_rolling_prefix(math,&contact,&road,&path,&automatic,&expected)==2);
            expected.proximity_circle[0]=expected.proximity_circle[1]=expected.boundary.ground.position[0];
            expected.proximity_circle[2]=8;
            uint32_t flags=1;uint8_t released=77;
            assert(nm_player_rolling_tick(math,&contact,&road,&path,&automatic,&spin,&flags,&released));
            assert(!memcmp(&spin,&expected,sizeof(spin)) && flags==1 && !released);
            assert(spin.spin_body_offset!=-16384 && spin.boundary.damage.initial_inhibit);
            for (unsigned remaining=initial_spin-1;remaining;--remaining) {
                assert(spin.boundary.damage.spin_ticks==remaining);
                const int16_t offset=spin.spin_body_offset;
                expected=spin;
                const int branch=nm_player_rolling_prefix(math,&contact,&road,&path,&automatic,&expected);
                assert(branch==(remaining>1 ? 2 : 1));
                assert(nm_player_rolling_tick(math,&contact,&road,&path,&automatic,&spin,&flags,&released));
                assert(spin.boundary.damage.spin_ticks==remaining-1);
                if (remaining>1) assert(spin.spin_body_offset!=offset && spin.boundary.damage.initial_inhibit);
                else assert(spin.spin_body_offset==offset && !spin.boundary.damage.initial_inhibit);
            }
            assert(!spin.boundary.damage.spin_ticks);
        }
        puts("PASS rolling spin persistence: all timers2..32, decrement/offset, no early release, return to rolling");
        if (road.grid_count) {
            rolling=entry;
            uint32_t start_flags=0xa5a50001u;
            uint8_t released=0;
            unsigned ticks=0;
            while (!released && ticks<10000) {
                if (!nm_player_rolling_tick(math,&contact,&road,&path,&automatic,&rolling,&start_flags,&released)) {
                    fprintf(stderr,"rolling tick rejected tick=%u profile=%d spin=%u speed=%g\n",ticks,
                        rolling.phase[0],rolling.boundary.damage.spin_ticks,rolling.boundary.speed);
                    assert(0);
                }
                ++ticks;
            }
            fprintf(stderr,"rolling sequence: ticks=%u released=%u profile=%d speed=%g\n",ticks,released,rolling.phase[0],rolling.boundary.speed);
            assert(released && start_flags==0xa5a50000u && !rolling.phase[0] && !rolling.lap.lap);
            assert(!rolling.boundary.damage.initial_inhibit);
            nm_player_active_input_t normal=automatic;
            normal.dynamics.motion_scale=1; normal.controls.accel=255;normal.controls.steer=64;
            normal.input_counter=65;
            assert(nm_player_active_step(math,&contact,&road,&path,&normal,&rolling,&marks));
            assert(rolling.accel==255 && rolling.steer==8192);
            memset(marks.values,0,marks.count);marks.timer=0;
            puts("PASS rolling start-to-release and normal player-input takeover (host integration, not full race oracle)");
        }
        rolling=entry; rolling.phase[0]=0;rolling.boundary.damage.initial_inhibit=123;
        nm_player_t paused=rolling;
        paused.proximity_circle[0]=paused.proximity_circle[1]=paused.boundary.ground.position[0];
        paused.proximity_circle[2]=8;
        nm_player_active_input_t disabled=automatic;disabled.dynamics.speed_flags=0x80000002u;
        uint32_t flags=0xa5a50001u;uint8_t event=77;
        assert(nm_player_rolling_tick(NULL,NULL,NULL,NULL,&disabled,&rolling,&flags,&event));
        assert(!memcmp(&rolling,&paused,sizeof(rolling)) && flags==0xa5a50001u && !event);
        event=77;const nm_player_t failed=rolling;
        assert(!nm_player_rolling_tick(math,&invalid,&road,&path,&automatic,&rolling,&flags,&event));
        assert(!memcmp(&rolling,&failed,sizeof(rolling)) && flags==0xa5a50001u && event==77);
    }
    if (road.grid_count) {
        assert((uint16_t)s.car.phase[0]==road.grid[0].profile && (uint16_t)s.car.phase[1]==road.grid[0].previous);
        assert(s.car.lap.lap==(uint8_t)road.grid[0].lap && s.car.lap.furthest==road.grid[0].previous);
        assert(s.car.path_offset==road.grid[0].path_offset);
    }
    unsigned spawn_cell;
    assert(nm_contact_cell(s.car.boundary.ground.position[0],s.car.boundary.ground.position[2],&spawn_cell));
    assert(s.car.scenery_cell==(((spawn_cell>>3)&15) | ((spawn_cell>>6)&240)) && s.car.scenery_cell!=0);
    nm_controls_t input = {.accel=255};
    for (unsigned i=0; i<1800; ++i) {
        assert(nm_practice_advance(&s,math,&contact,&road,&path,&marks,&input,NM_PRACTICE_TICK_US));
        if(window_vertices) assert(nm_reflection_apply(window_vertices,window_base,window_count,s.reflection_u,s.reflection_v));
        if (model.data) check_projected_car(math,&s.car,&model,&materials);
        if (world.data) trace_world(math,&s,&world,&scenery);
    }
    fprintf(stderr,"practice throttle: ticks=%u speed=%g profile=%d start=%d lap=%u\n",
        s.ticks,(double)s.car.boundary.speed,s.car.phase[0],first.car.phase[0],s.car.lap.lap);
    const int start_lap=first.car.lap.lap<128 ? first.car.lap.lap : (int)first.car.lap.lap-256;
    const int end_lap=s.car.lap.lap<128 ? s.car.lap.lap : (int)s.car.lap.lap-256;
    const int progress=(end_lap-start_lap)*(int)(road.profile_count-1)+s.car.phase[0]-first.car.phase[0];
    assert(s.ticks==1800 && s.car.boundary.speed>0.1f && progress>100);
    assert(memcmp(first.car.boundary.ground.position,s.car.boundary.ground.position,12));
    input.accel=0; input.brake=255;
    for (unsigned i=0; i<1800; ++i) assert(nm_practice_advance(&s,math,&contact,&road,&path,&marks,&input,NM_PRACTICE_TICK_US));
    assert(s.ticks==3600 && s.car.boundary.speed==0 && !s.remainder_us && !s.dropped_us);
    const nm_practice_t stopped=s;
    for (unsigned i=0; i<120; ++i) assert(nm_practice_advance(&s,math,&contact,&road,&path,&marks,&input,NM_PRACTICE_TICK_US));
    assert(!memcmp(stopped.car.boundary.ground.position,s.car.boundary.ground.position,12));
    float steered[2][3];
    for (unsigned side=0; side<2; ++side) {
        assert(nm_practice_reset(&s,math,&contact,&road,&path,&marks));
        input=(nm_controls_t){.accel=255,.steer=side ? 60 : -60};
        for (unsigned i=0; i<300; ++i) {
            assert(nm_practice_advance(&s,math,&contact,&road,&path,&marks,&input,NM_PRACTICE_TICK_US));
            if(window_vertices) assert(nm_reflection_apply(window_vertices,window_base,window_count,s.reflection_u,s.reflection_v));
            if(model.data) check_projected_car(math,&s.car,&model,&materials);
        }
        memcpy(steered[side],s.car.boundary.ground.position,12);
    }
    assert(memcmp(steered[0],steered[1],12));
    input=(nm_controls_t){.accel=255};
    assert(nm_practice_reset(&s,math,&contact,&road,&path,&marks));
    for (unsigned i=0; i<240; ++i) assert(nm_practice_advance(&s,math,&contact,&road,&path,&marks,&input,NM_PRACTICE_TICK_US));
    const nm_practice_t reference=s;
    assert(nm_practice_reset(&s,math,&contact,&road,&path,&marks));
    for (unsigned i=0; i<480; ++i) assert(nm_practice_advance(&s,math,&contact,&road,&path,&marks,&input,NM_PRACTICE_TICK_US/2));
    assert(!memcmp(&s,&reference,sizeof(s)));
    const nm_practice_t before=s;
    input.steer=128;
    assert(!nm_practice_advance(&s,math,&contact,&road,&path,&marks,&input,1));
    assert(!memcmp(&s,&before,sizeof(s)));
    input.steer=0;
    assert(nm_practice_advance(&s,math,&contact,&road,&path,&marks,&input,10*NM_PRACTICE_TICK_US));
    assert(s.ticks==244 && s.dropped_us==6*NM_PRACTICE_TICK_US && !s.remainder_us);
    assert(nm_practice_reset(&s,math,&contact,&road,&path,&marks));
    assert(!memcmp(&s,&first,sizeof(s)));
    /* Independent one-tick input schedule vs fractional/multi-tick/stalled
     * render cadences, including both sides of the throttle/brake boundary. */
    for (unsigned tick=0;tick<NM_PRACTICE_SCRIPT_TICKS;++tick) {
        const nm_controls_t scheduled={.accel=tick<1800 ? 255 : 0,.brake=tick<1800 ? 0 : 255};
        assert(nm_practice_advance(&s,math,&contact,&road,&path,&marks,&scheduled,NM_PRACTICE_TICK_US));
        /* Resource demand deliberately precedes ALL face/triangle culling:
         * test the direct-SQ renderer's proposed texture preparation order.
         * This route evidence is not a universal camera/damage budget proof. */
        if(window_vertices) assert(nm_reflection_apply(window_vertices,window_base,window_count,s.reflection_u,s.reflection_v));
        if(model.data) check_projected_car(math,&s.car,&model,&materials);
    }
    const nm_practice_t scripted=s;
    uint8_t *expected_marks=malloc(marks.count); assert(expected_marks);
    memcpy(expected_marks,marks.values,marks.count);
    const uint32_t expected_timer=marks.timer;
    const uint32_t cadences[]={1,8333,16667,27573,38766,4*NM_PRACTICE_TICK_US,438546};
    for (unsigned c=0;c<sizeof(cadences)/sizeof(*cadences);++c) {
        assert(nm_practice_reset(&s,math,&contact,&road,&path,&marks));
        unsigned calls=0;
        while (s.ticks<NM_PRACTICE_SCRIPT_TICKS) {
            /* One microsecond is exercised at boundaries, not forty million
             * calls: alternate it with a three-tick step. */
            const uint32_t elapsed=cadences[c]==1 && (calls++&1) ? 3*NM_PRACTICE_TICK_US : cadences[c];
            assert(nm_practice_script_advance(&s,math,&contact,&road,&path,&marks,elapsed));
        }
        const uint32_t dropped=s.dropped_us;
        assert((dropped!=0)==(cadences[c]>4*NM_PRACTICE_TICK_US));
        s.dropped_us=0;
        assert(!memcmp(&s,&scripted,sizeof(s)) && marks.timer==expected_timer &&
            !memcmp(marks.values,expected_marks,marks.count));
        assert(nm_practice_script_advance(&s,math,&contact,&road,&path,&marks,UINT32_MAX));
        assert(!memcmp(&s,&scripted,sizeof(s)));
    }
    free(expected_marks);
    printf("PASS fixed-tick script: ticks=%u digest=%08x, seven cadences and full state/marks match\n",
        NM_PRACTICE_SCRIPT_TICKS,nm_practice_script_digest(&s));
    printf("Native window phase: %u UV=%u,%u\n",s.reflection_phase,s.reflection_u,s.reflection_v);
    assert(nm_practice_reset(&s,math,&contact,&road,&path,&marks));
    path.route=1;
    assert(!nm_practice_reset(&s,math,&contact,&road,&path,&marks));
    assert(!memcmp(&s,&first,sizeof(s)));
    printf("PASS persistent practice: throttle/brake/steering, 3600+ ticks, render-cadence independence, reset/stall/invalid input; state=%zu bytes\n",sizeof(s));
    printf("PASS normal body pose: 65536 mixed-angle cases; projected live-car ticks=%u\n",projected_car_ticks);
    if (materials.data) printf("PASS retained car lighting/cache:%u frames including hardware script and both steering directions, unculled resource peak=%u bytes\n",projected_car_ticks,cache_peak);
    nm_material_pack_close(&materials);
    nm_pack_unload(&model);
    nm_pack_unload(&world);
    nm_scenery_unload(&scenery);
    free(marks.values); free(math); nm_contact_unload(&contact); nm_road_unload(&road); nm_path_unload(&path);
    return 0;
}
