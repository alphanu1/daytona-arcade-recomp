/* Scene presentation assets; gameplay owns the scene and engine lifetime. */
#include "render/scene/practice.h"
#include "render/models/model_renderer.h"
#include "render/materials/materials.h"
#include "render/materials/result_materials.h"
#include "render/ui/finish_render.h"
#include "render/ui/result_render.h"
#include "render/ui/result_labels.h"
#include "render/ui/hud_render.h" /* HUD */
#include "render/scene/sky.h"
#include "render/scene/track_surface.h"
#include "lighting.h"
#include "course_assets.h"
#include "scene_loader.h"
#include "pose.h"
#include "heap.h"
#include "props_scene.h"
#include "../../../tests/sky_cases.h"
#include <sh4zam/shz_sh4zam.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "nm_mem.h"

/* The game-owned CPU packs of the scene's presentation (car, world, damaged bodies). Callers release queued
 * rendering first (nm_practice_render_release). */
void nm_practice_render_unload(nm_practice_scene_t *scene) {
    nm_props_world_actors(NULL,0);
    free(scene->clock_data);scene->clock_data=NULL;scene->clock_count=0;
    nm_pack_unload(&scene->model);
    nm_pack_unload(&scene->world);
    free(scene->world_instances);scene->world_instances=NULL;
    scene->car_sources=scene->world_sources=NULL;
    scene->window_vertices=NULL;scene->window_vertex_count=0;
    for(unsigned w=0;w<2;++w) {
        scene->intro_windows[w]=NULL;scene->intro_window_count[w]=0;
    }
    nm_damage_bodies_unload(&scene->damage_bodies);
    nm_damage_bodies_unload(&scene->hood_variants);
}

/* The most world points and faces one frame admits (nm_render_world_capacity): the largest 5x5 neighbourhood of
 * scenery cells (nm_scenery_cells: every view draws a subset of the 25 cells around its centre cell) plus every
 * world object. After the scenery stage selection, as the renderer sizes from the live meshes. */
static void practice_world_bound(const nm_practice_scene_t *scene,unsigned *points,unsigned *faces) {
    unsigned cell_points[256]={0},cell_faces[256]={0},object_points=0,object_faces=0;
    const nm_instance_t *sources=scene->world_sources;
    for(unsigned c=0;c<256;++c) for(unsigned j=0;j<scene->scenery.cells[c].count;++j) {
        const nm_mesh_t *mesh=scene->world.meshes+sources[scene->scenery.cells[c].first+j].mesh;
        cell_points[c]+=mesh->vertex_count;cell_faces[c]+=mesh->face_count;
    }
    for(unsigned i=scene->world_object_first;i<scene->world_object_first+scene->world_object_count;++i) {
        const nm_mesh_t *mesh=scene->world.meshes+sources[i].mesh;
        object_points+=mesh->vertex_count;object_faces+=mesh->face_count;
    }
    unsigned best_points=0,best_faces=0;
    for(int z=0;z<16;++z) for(int x=0;x<16;++x) {
        unsigned p=0,f=0;
        for(int dz=-2;dz<=2;++dz) for(int dx=-2;dx<=2;++dx)
            if(x+dx>=0 && x+dx<16 && z+dz>=0 && z+dz<16) { p+=cell_points[x+dx+16*(z+dz)];f+=cell_faces[x+dx+16*(z+dz)]; }
        if(p>best_points) best_points=p;
        if(f>best_faces) best_faces=f;
    }
    *points=best_points+object_points;*faces=best_faces+object_faces;
}

/* The course's frame-counter animations (include/clock_actors.h) when its world has instances of kind
 * NM_OBJECT_CLOCK: courseN-actors.dna, checked against the world pack, each group matched to its instance, handed
 * to the renderer before the world is attached. 0: the file is missing or does not fit the pack. */
static int practice_clock_load(nm_practice_scene_t *scene,const char *root) {
    unsigned instances=0;
    for(unsigned i=scene->world_object_first;i<scene->world_object_first+scene->world_object_count;++i)
        instances+=NM_OBJECT_KIND(scene->world_sources[i].padding[0])==NM_OBJECT_CLOCK;
    if(!instances) return nm_render_world_clocks(NULL);
    char file[256];
    if(!nm_course_asset(file,sizeof(file),root,scene->course,NM_COURSE_ACTORS,0)) return 0;
    FILE *f=fopen(file,"rb");
    if(!f) { printf("FAIL: the world has %u frame-counter animations but the asset set no %s\n",instances,file); return 0; }
    long size=-1;
    if(!fseek(f,0,SEEK_END)) size=ftell(f);
    void *data=size>0 && size<=(long)(1u<<20) && !fseek(f,0,SEEK_SET) ? nm_heap_alloc("world clock frames",32,(size_t)size) : NULL;
    const int read=data && fread(data,1,(size_t)size,f)==(size_t)size;
    fclose(f);
    /* Face counts are checked by the renderer (a scenery stage below the objects' empties the meshes). */
    int fits=read && nm_clock_validate(data,(size_t)size,NULL,scene->world_header.meshes) &&
        ((const nm_clock_header_t *)data)->groups==instances;
    for(unsigned g=0;fits && g<instances;++g)
        fits=scene->world.meshes[nm_clock_groups(data)[g].mesh].vertex_count==nm_clock_groups(data)[g].vertices;
    if(!fits) {
        printf("FAIL: %s does not fit the world pack\n",file); free(data); return 0;
    }
    const nm_clock_group_t *groups=nm_clock_groups(data);
    for(unsigned g=0;g<instances;++g) {
        unsigned found=0;
        for(unsigned i=scene->world_object_first;i<scene->world_object_first+scene->world_object_count;++i) {
            const uint32_t word=scene->world_sources[i].padding[0];
            if(NM_OBJECT_KIND(word)==NM_OBJECT_CLOCK && scene->world_sources[i].mesh==groups[g].mesh &&
               NM_OBJECT_VARIANT(word)==groups[g].variant) { scene->clock_source[g]=(uint16_t)i; ++found; }
        }
        if(found!=1) { printf("FAIL: %s group %u has %u instances in the world pack\n",file,g,found); free(data); return 0; }
    }
    scene->clock_data=data; scene->clock_count=instances;
    /* The actors of the file (Expert's: include/expert_actors.h) tick with the props (code/props_scene.c). */
    nm_props_world_actors(data,scene->course);
    printf("Native world clock animations: %u groups, %u actors, %ld bytes (%s)\n",instances,nm_clock_actor_count(data),size,file);
    return nm_render_world_clocks(data);
}

/* Whether the asset set holds COURSE's world (course_assets.h NM_COURSE_WORLD); course 0's always must. */
static int practice_course_world(const char *root,unsigned course) {
    char file[256];
    if(!course) return 1;
    if(!nm_course_asset(file,sizeof(file),root,course,NM_COURSE_WORLD,0)) return 0;
    FILE *f=fopen(file,"rb");
    if(f) fclose(f);
    return f!=NULL;
}

/* nm_loader_yield at each stage: a load in another screen's frames (scene_loader.h) hands its slice back there. */
int nm_practice_render_load(nm_practice_scene_t *scene,const char *root,const char **stage) {
    char file[256];
    /* Callers release queued rendering before replacing the game-owned CPU packs. */
    nm_practice_render_unload(scene);
    *stage="car.dnm"; nm_loader_yield(0);
    snprintf(file,sizeof(file),"%scar.dnm",root);
    if (!nm_pack_load(&scene->model,file) ||
        scene->model.header->crtc[0] || scene->model.header->crtc[1] ||
        scene->model.header->render[0] || scene->model.header->render[1] ||
        !nm_view_valid(&nm_practice_render_view)) goto done;
    const unsigned parts=scene->model.header->instances;
    if(parts!=sizeof(nm_intro_model_ids)/sizeof(nm_intro_model_ids[0])) goto done;
    for(unsigned i=0;i<parts;++i)
        if(scene->model.instances[i].source_address!=nm_intro_model_ids[i]) goto done;
    for(unsigned w=0;w<2;++w) {
        const unsigned binding=nm_intro_window_models[w];
        if(binding>=parts || scene->model.instances[binding].source_address!=(w ? 0x241u : 0x1c8u)) goto done;
        const nm_mesh_t *window=scene->model.meshes+scene->model.instances[binding].mesh;
        if(window->face_count!=(w ? 5u : 7u) || !window->vertex_count || window->vertex_count>36) goto done;
        scene->intro_windows[w]=(nm_vertex_t *)scene->model.vertices+window->vertex_first;
        scene->intro_window_count[w]=window->vertex_count;
        for(unsigned v=0;v<window->vertex_count;++v) {
            scene->intro_window_uv[w][2*v]=scene->intro_windows[w][v].u;
            scene->intro_window_uv[w][2*v+1]=scene->intro_windows[w][v].v;
        }
    }
    /* The player's damaged bodies: variants of the clean body's mesh (render/scene/damage_bodies.h). */
    *stage="car-damage.dnb"; nm_loader_yield(0);
    snprintf(file,sizeof(file),"%scar-damage.dnb",root);
    if(!nm_damage_bodies_load(&scene->damage_bodies,file)) goto done;
    /* The hood view's damaged hoods: variants of the clean hood's mesh, as the bodies. */
    *stage="car-hoods.dnb"; nm_loader_yield(0);
    snprintf(file,sizeof(file),"%scar-hoods.dnb",root);
    if(!nm_damage_hoods_load(&scene->hood_variants,file)) goto done;
    /* The manual car takes the variants in its own face order and materials (render/scene/practice.c): up to one
     * light class a face. */
    {
        const nm_damage_bodies_t *sets[2]={&scene->damage_bodies,&scene->hood_variants};
        unsigned classes=0;
        for(unsigned i=0;i<2;++i) {
            const unsigned need=scene->transmission && sets[i]->faces>sets[i]->classes ? sets[i]->faces : sets[i]->classes;
            if(need>classes) classes=need;
        }
        nm_render_car_light_classes_reserve(classes);
    }
    scene->player_body_variant=NM_DAMAGE_BODY_CLEAN;
    scene->player_hood_variant=NM_DAMAGE_HOOD_CLEAN;
    *stage="body/view pose"; nm_loader_yield(0);
    if (!nm_practice_render_pose(scene)) goto done;
    *stage="presentation FTRV check"; nm_loader_yield(0);
    /* The car pack's checks (the FTRV gate, nm_pack_check_math, nm_pack_check_lighting) once per power-on: the same
     * pack at every load (the attract loop reloads the scene at each course); the road's profiles every load. */
    static int car_checked;
    /* Same cancellation-aware FTRV gate as the textured renderer, evaluated
     * on actual road/car vertices. Test rotations too, not only identity. */
    if(!car_checked)
    for (unsigned k=0; k<4; ++k) {
        nm_loader_yield(0);
        const int16_t angles[3]={(int16_t)(k*1700),(int16_t)(k*15371),(int16_t)(k*-2500)};
        float m[12];
        if (!nm_body_pose(scene->math,scene->state.car.boundary.ground.position,angles,m) ||
            !nm_pose_compose(scene->view,m,m)) goto done;
        nm_practice_render_matrix(m);
        for (unsigned i=0; i<scene->model.header->vertices; ++i) {
            if (!(i&4095u)) nm_loader_yield(0);
            if (!nm_practice_render_check_transform(scene->model.vertices[i].position,m,1)) goto done;
        }
        if (!nm_practice_render_check_transform(nm_race_world_light,m,0)) goto done;
    }
    nm_practice_render_matrix(scene->view);
    for (unsigned i=0; i<scene->road.profile_count; ++i) for (unsigned j=0; j<8; ++j)
        if (!nm_practice_render_check_transform(scene->road.profiles[i].lanes[j].xyz,scene->view,1)) goto done;
    printf("Native practice 3D: local body=%lu bytes, vertex scratch=%lu bytes; FTRV gate PASS; source mode0/reset follow camera, projection provisional\n",
        (unsigned long)scene->model.header->size,
        (unsigned long)(scene->points ? scene->model.header->vertices*sizeof(*scene->points) : 0));
    if(parts==7) {
        const nm_mesh_t *window=scene->model.meshes+3;
        if(window->face_count!=9 || !window->vertex_count || window->vertex_count>36) goto done;
        /* nm_pack_load owns writable CPU memory; GPU never reads this mesh.
         * Existing renderer copies transformed vertices before SQ submission. */
        scene->window_vertices=(nm_vertex_t *)scene->model.vertices+window->vertex_first;
        scene->window_vertex_count=window->vertex_count;
        for(unsigned i=0;i<window->vertex_count;++i) {
            scene->window_base_uv[2*i]=scene->window_vertices[i].u;
            scene->window_base_uv[2*i+1]=scene->window_vertices[i].v;
        }
    }
    /* Map our live 640x480 view into the existing renderer's 496x384 logical
     * projection. No source/captured transform is used at runtime. */
    scene->instance=(nm_instance_t){.source_address=0x95ff9d,
        .focus={310,320},.viewport={0,0,495,384},.center={248,208}};
    /* The window materials of the tunnel texture (nm_practice_render_tunnel). */
    scene->window_materials[0]=scene->window_materials[1]=UINT16_MAX;
    for(unsigned i=0;i<scene->model.header->instances;++i) {
        const uint32_t source=scene->model.instances[i].source_address;
        const unsigned kind=source==0x1c8 || source==0x241 ? 0u : source==0x13b ? 1u : 2u;
        const nm_mesh_t *mesh=scene->model.meshes+scene->model.instances[i].mesh;
        if(kind<2 && mesh->face_count) scene->window_materials[kind]=scene->model.faces[mesh->face_first].material;
    }
    scene->car_sources=scene->model.instances;
    scene->model.instances=scene->car_instances;
    scene->intro_header=*scene->model.header;scene->model.header=&scene->intro_header;
    snprintf(file,sizeof(file),"%scar.dnt",root);
    *stage="car materials/math/renderer"; nm_loader_yield(0);
    if (!nm_practice_render_lighting(scene) || !nm_practice_render_parts(scene) || !nm_materials_load(file) || (!car_checked && (!nm_pack_check_math(&scene->model) ||
        !nm_pack_check_lighting(&scene->model))) || !nm_render_init(&scene->model)) goto done;
    car_checked=1;
    if(!practice_course_world(root,scene->course)) {
    /* An asset set without this course's world, sky and world materials: the race draws the cars, their
     * shadows and the HUD only (no world instance, world_draw.pack NULL, no sky). */
    scene->world_header=(nm_header_t){0};
    scene->world_object_first=scene->world_object_count=0;
    printf("Native course %u: no world, sky or world materials in this asset set; cars, shadows and HUD only\n",
        (unsigned)scene->course);
    } else {
    *stage="world.dnm/scenery.dnv"; nm_loader_yield(0);
    if(!nm_course_asset(file,sizeof(file),root,scene->course,NM_COURSE_WORLD,0)) goto done;
    if (!nm_pack_load(&scene->world,file) ||
        ((scene->world.header->magic & ~NM_PACK_ALIGNED)!=NM_PACK_MAGIC_NORMAL_GROUPED &&
         (scene->world.header->magic & ~NM_PACK_ALIGNED)!=NM_PACK_MAGIC_STRIPS &&
         ((scene->world.header->magic & ~NM_PACK_ALIGNED)!=NM_PACK_MAGIC_LAYERED || !scene->world.normal_groups))) goto done;
    if (scene->scenery.slot!=scene->course ||
        scene->scenery.instances>scene->world.header->instances ||
        (scene->scenery.instances!=scene->world.header->instances &&
         !(nm_pack_flags(&scene->world)&NM_PACK_WORLD_OBJECTS)) ||
        scene->scenery.profiles!=scene->road.profile_count ||
        scene->world.header->crtc[0] || scene->world.header->crtc[1] ||
        scene->world.header->render[0] || scene->world.header->render[1]) goto done;
    *stage="scenery stage selection"; nm_loader_yield(0);
    if(!nm_scenery_stage_select(&scene->world,NM_SCENERY_MATERIAL_STAGE)) goto done;
    scene->world_header=*scene->world.header;
    scene->world_sources=scene->world.instances;
    scene->world_object_first=scene->scenery.instances;
    scene->world_object_count=scene->world.header->instances-scene->scenery.instances;
    for(unsigned i=0;i<scene->world.header->instances;++i)
        if(!(scene->world_sources[i].padding[0]&NM_OBJECT_INSTANCE)!=(i<scene->world_object_first)) goto done;
    scene->world_instances=nm_alloc(scene->world.header->instances*sizeof(*scene->world_instances));
    if (!scene->world_instances) goto done;
    scene->world.header=&scene->world_header; scene->world.instances=scene->world_instances;
    *stage="native scenery placement/math"; nm_loader_yield(0);
    if (!nm_practice_render_scenery(scene) || !scene->world_header.instances || !nm_pack_check_math(&scene->world) ||
        !nm_pack_check_scenery_lighting(&scene->world)) goto done;
    printf("Native scenery start: cell=%u instances=%lu; authored world, source normal follow view; no debug-road substitute\n",
        (unsigned)scene->state.car.scenery_cell,(unsigned long)scene->world_header.instances);
    *stage="sky math"; nm_loader_yield(0);
    const uint32_t sky_hash=nm_sky_test_hash(scene->math);
    if(sky_hash!=NM_SKY_TEST_HASH) goto done;
    printf("Native sky source-math startup: 8 cases hash=%08lx PASS\n",(unsigned long)sky_hash);
    *stage="sky.dns"; nm_loader_yield(0);
    if(!nm_course_asset(file,sizeof(file),root,scene->course,NM_COURSE_SKY,0) || !nm_sky_load(file)) goto done;
    *stage="world.dnt/shared renderer"; nm_loader_yield(0);
    if(!nm_course_asset(file,sizeof(file),root,scene->course,NM_COURSE_WORLD_MATERIALS,0)) goto done;
    {
        unsigned points,faces;
        practice_world_bound(scene,&points,&faces);
        nm_render_world_capacity(points,faces);
        printf("Native world frame bound: %u points, %u faces (5x5 cells and the world objects)\n",points,faces);
        /* Prebuilt world streams by course (course_assets.h NM_COURSE_WORLD_STREAMS). */
        nm_render_world_streams(nm_course_world_streams(scene->course));
    }
    *stage="courseN-actors.dna"; nm_loader_yield(0);
    if(!practice_clock_load(scene,root)) goto done;
    *stage="world.dnt/shared renderer"; nm_loader_yield(0);
    if (!nm_world_materials_load(file) || !nm_render_world_attach(&scene->world)) goto done;
    }
    *stage="finish PAL4"; nm_loader_yield(0);
    if(!nm_finish_render_load(root)) goto done;
    *stage="result PAL4"; nm_loader_yield(0);
    if(!nm_result_materials_load(root)) goto done;
    *stage="result renderer"; nm_loader_yield(0);
    if(!nm_result_render_load(root,scene->math)) goto done;
    *stage="result labels"; nm_loader_yield(0);
    if(!nm_result_labels_load(root)) goto done;
    /* Normal-race light; camera/lifecycle remain provisional. */
    pvr_poly_cxt_t context; pvr_poly_cxt_col(&context,PVR_LIST_OP_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&scene->header,&context);
    return 1;
done:
    return 0;
}

void nm_practice_render_release(void) {
    nm_hud_render_unload(); /* HUD */
    nm_result_labels_unload();
    nm_result_render_unload();
    nm_result_materials_unload();
    nm_finish_render_unload();
    /* Must run inside the engine lifecycle, BEFORE PVR shutdown. */
    nm_render_release();
    nm_world_materials_unload();
    nm_sky_unload();
    nm_materials_unload();
}
