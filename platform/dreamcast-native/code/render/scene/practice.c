#include "render/scene/practice.h"
#include "clock_scene.h"
#include "props_scene.h"
#include "course_assets.h"
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <float.h>
#include <string.h>
#include "render/scene/sky.h"
#include "render/models/model_renderer.h"
#include "render/materials/materials.h"
#include "lighting.h"
#include "render/ui/finish_render.h"
#include "render/ui/hud_render.h" /* HUD */
#include "shadow_volume.h"
#include "render/scene/track_surface.h"
#include "props.h"
#include "render/scene/slot_glass.h"
#include "nm_mem.h"
#include "frame_trace.h"

const nm_view_t nm_practice_render_view = {{320,220},{0,640,0,480},0.1f};
void nm_practice_render_matrix(const float m[12]) {
    alignas(32) shz_mat3x4_t matrix;
    shz_memcpy4(matrix.elem, m, sizeof(matrix));
    shz_xmtrx_load_3x4(&matrix);
}
int nm_practice_render_pose(nm_practice_scene_t *scene) {
    shz_memcpy4(scene->view,scene->intro.view,sizeof(scene->view));
    scene->camera_heading=scene->intro.heading;
    return 1;
}
int nm_practice_render_lighting(nm_practice_scene_t *scene) {
    nm_practice_render_matrix(scene->view);
    /* The race's scene light (tunnel_light.h: the normal light, or a tunnel's while the player is in one), the
     * attract's likewise (its camera object is the race's; intro_scene.h tunnel); the podium keeps the normal
     * light. The scenery is baked with the normal light (its tunnel walls are not relit). */
    const float *light=scene->attract_active ? (scene->intro.tunnel ? scene->intro.scene_light.light : nm_race_world_light) :
        !scene->race.count ? nm_race_world_light : scene->race.scene_light.light;
    const shz_vec4_t v=shz_xmtrx_transform_vec4(shz_vec4_init(light[0],light[1],light[2],0));
    if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) return 0;
    scene->instance.light[0]=v.x; scene->instance.light[1]=v.y; scene->instance.light[2]=v.z;
    return 1;
}
int nm_practice_render_tunnel(nm_practice_scene_t *scene,int tunnel,int others_only) {
    int ok=1;
    for(unsigned k=0;k<2;++k) {
        const unsigned m=scene->window_materials[k];
        if(m==UINT16_MAX || (others_only && k==1) || !scene->model.materials || m>=scene->model.header->materials) continue;
        uint16_t header[4];
        nm_memcpy(header,scene->model.materials[m].header,sizeof(header));
        if(tunnel) { header[0]=(uint16_t)(header[0]|0x3c0u);header[2]=(uint16_t)((header[2]&~0x1fffu)|0x0c0cu); }
        if(!nm_render_car_material_header(m,header)) ok=0;
    }
    return ok;
}

/* Render-only level of detail for distant opponents (make NATIVE_SMALL_CAR_PX=N; 12, the default since 15 October,
 * the chosen default: hardware 14 -> 6 late frames, registration -0.42 ms; 0 is off). The
 * arcade's level (8590..85f4 from the view depth) stays as computed, and with it the detail list 503208 and the
 * drawn-car count 5014f7 the opponents read on the next tick: only what is drawn for it changes. An opponent whose
 * body is less than N output rows tall is drawn with the level-1 body (15 faces, 51 vertices; level 2's has 71 and
 * 191, level 3's 89 and 244) and level 1's parts: no wheel or window meshes. Its shadow follows the arcade's level.
 * Rows: 480/384 * focus * 1.28 (the body meshes' height at every level) / view depth of the body origin. */
#ifndef NM_SMALL_CAR_PX
#define NM_SMALL_CAR_PX 12
#endif
static inline unsigned practice_drawn_level(const nm_practice_scene_t *scene,unsigned car,unsigned level,
        const nm_instance_t *body) {
#if NM_SMALL_CAR_PX
    if(scene->attract_active || !car || level<2) return level;
    const float rows_at_unit_depth=(480.0f/384.0f)*1.28f*body->focus[1];
    return body->matrix[11]*(float)NM_SMALL_CAR_PX>rows_at_unit_depth ? 1u : level;
#else
    (void)scene;(void)car;(void)body;
    return level;
#endif
}
int nm_practice_render_parts(nm_practice_scene_t *scene) {
    const nm_player_t *cars[44];
    unsigned kinds[44];
    if(scene->intro.count>44) return 0;
    for(unsigned i=0;i<scene->intro.count;++i) {
        cars[i]=scene->attract_active ? scene->intro.cars+i : &scene->race.cars[i].car;
        kinds[i]=scene->attract_active ? scene->intro.stages[scene->intro.stage].spawn[i].kind : 0;
    }
    if(scene->inspect_paused) {
        for(unsigned i=0;i<scene->intro.count;++i) {
            scene->inspect_order[i]=(uint8_t)i;
            scene->inspect_depth[i]=INT16_MAX;
        }
        if(!nm_car_detail_step(scene->view,&scene->inspect_cells,cars,scene->intro.count,
            scene->inspect_depth,scene->inspect_order,scene->inspect_level)) return 0;
    }
    /* Shared owned CPU windows, updated before either queued PVR callback. */
    for(unsigned w=0;w<2;++w)
        if(!nm_render_uv_dynamic(scene->intro_windows[w],scene->intro_window_count[w]) ||
           !nm_reflection_apply(scene->intro_windows[w],scene->intro_window_uv[w],
            scene->intro_window_count[w],scene->intro.reflection_u,scene->intro.reflection_v)) return 0;
    scene->intro_header.instances=0;
    if(scene->intro.count>=sizeof(scene->car_part_boundaries)/sizeof(scene->car_part_boundaries[0])) return 0;
    unsigned crash_shadows=0;
    for(unsigned ordinal=0;ordinal<scene->intro.count;++ordinal) {
        scene->car_part_boundaries[ordinal]=scene->intro_header.instances;
        const unsigned i=scene->inspect_paused ? scene->inspect_order[ordinal] : scene->intro.detail_order[ordinal];
        const unsigned level=scene->inspect_paused ? scene->inspect_level[i] : scene->intro.detail_level[i];
        const nm_player_t *car=cars[i];
        const unsigned kind=kinds[i];
        if(kind==2) {
            /* d144 is submitted independently of ordinary body detail culling.
             * Shared animation clock is the last processed simulation tick. */
            const unsigned phase=(scene->intro.ticks ? scene->intro.ticks-1 : 0)&7;
            const nm_instance_t *source=scene->car_sources+nm_intro_special_phase_models[phase];
            if(scene->intro_header.instances>=NM_INTRO_MAX_PARTS) return 0;
            nm_instance_t *extra=scene->car_instances+scene->intro_header.instances++;
            float part[12];shz_memcpy4(extra,&scene->instance,sizeof(*extra));
            extra->mesh=source->mesh;extra->source_address=source->source_address;
            if(!nm_intro_special_pose(scene->math,car,part) ||
               !nm_pose_compose(scene->view,part,extra->matrix)) return 0;
            ++scene->intro_special_parts;scene->intro_special_phases|=1u<<phase;
        }
        if(!level) continue;
        if(kind!=2 && i>=sizeof(nm_intro_body_models)/sizeof(nm_intro_body_models[0])) return 0;
        if(!scene->attract_active) scene->race_bodies|=UINT64_C(1)<<i;
        if(scene->intro_header.instances>=sizeof(scene->car_instances)/sizeof(scene->car_instances[0])) return 0;
        nm_instance_t *out=scene->car_instances+scene->intro_header.instances++;
        float body[12];
        shz_memcpy4(out,&scene->instance,sizeof(*out));
        if(!((!scene->attract_active && i==0) ? nm_player_part_pose(scene->math,car,0,body) :
               nm_intro_body_pose(scene->math,car,body)) ||
           !nm_pose_compose(scene->view,body,out->matrix)) return 0;
        const unsigned drawn=practice_drawn_level(scene,i,level,out);
        const unsigned binding=kind==2 ? nm_intro_special_model : nm_intro_body_models[i][6-drawn];
        const nm_instance_t *source=scene->car_sources+binding;
        out->mesh=source->mesh;out->source_address=source->source_address;
        if(drawn<=2 && car->boundary.damage.overlay_model) {
            if(scene->intro_header.instances>=NM_INTRO_MAX_PARTS) return 0;
            /* Original8054/8310 emits attachment BEFORE body with same matrix. */
            nm_instance_t *attachment=out;
            out=scene->car_instances+scene->intro_header.instances++;
            shz_memcpy4(out,attachment,sizeof(*out));
            const nm_instance_t *mesh=scene->car_sources+nm_intro_crash_models[drawn-1];
            attachment->mesh=mesh->mesh;attachment->source_address=mesh->source_address;
            ++scene->intro_crash_parts;
        }
        scene->intro_special_body+=kind==2;
        const int phase=nm_intro_wheel_phase(drawn,(uint16_t)car->wheel_phase[0]);
        if(phase>=0) {
            if(scene->intro_header.instances+2>sizeof(scene->car_instances)/sizeof(scene->car_instances[0])) return 0;
            nm_instance_t *window=scene->car_instances+scene->intro_header.instances++;
            shz_memcpy4(window,out,sizeof(*window));
            const nm_instance_t *window_source=scene->car_sources+nm_intro_window_models[4-drawn];
            window->mesh=window_source->mesh;window->source_address=window_source->source_address;
            nm_instance_t *wheel=scene->car_instances+scene->intro_header.instances++;
            shz_memcpy4(wheel,out,sizeof(*wheel));
            const nm_instance_t *mesh=scene->car_sources+nm_intro_wheel_phase_models[phase];
            wheel->mesh=mesh->mesh;wheel->source_address=mesh->source_address;
            scene->intro_special_windows+=kind==2;
        }
        if(nm_intro_shadow_select(level,car->boundary.damage.overlay_model,&crash_shadows)) {
            if(scene->intro_header.instances>=NM_INTRO_MAX_PARTS) return 0;
            nm_instance_t *shadow=scene->car_instances+scene->intro_header.instances++;
            shz_memcpy4(shadow,out,sizeof(*shadow));
            const nm_instance_t *mesh=scene->car_sources+nm_intro_shadow_model;
            shadow->mesh=mesh->mesh;shadow->source_address=mesh->source_address;
            float part[12];
            if(!((!scene->attract_active && i==0) ? nm_player_part_pose(scene->math,car,1,part) :
                   nm_intro_shadow_pose(scene->math,car,part)) ||
               !nm_pose_compose(scene->view,part,shadow->matrix)) return 0;
            ++scene->intro_shadow_parts;
        }
    }
    scene->car_part_boundaries[scene->intro.count]=scene->intro_header.instances;
    return 1;
}
/* Original projection state. Focus is camera word 501734 as the view updater
 * 8a2c leaves it, which 17fb0..17ff8 then copies into the 3D window records
 * (502000/502150/5021b0, +4 and +8) every frame:
 * - keyed cameras (5cac stores the interpolated key focus at 60e0; view mode
 *   5014a4==1): 8b4c..8c74 moves each word one eighth of the way to the reset
 *   record 28b5714.. after the view matrix was built, so the drawn focus is
 *   key+(reset-key)/8 while the matrix keeps the key's own words;
 * - follow view: 637c reloads view preset 501710 (default 2) from the table
 *   at 1d00220, a copy of program 2320b6; its focus word 2320ea is 210 and
 *   the updater's target is that same preset, so it stays.
 * Centre is the window record's (248,270) with the CRTC row offset 128
 * removed, since car/world packs carry crtc 0. The template rests on the 2D
 * actors' logical centre. */
enum { PRACTICE_CENTER_X=248, PRACTICE_CENTER_Y=270-128, PRACTICE_UI_CENTER_Y=208 };
static const float practice_follow_focus=210.0f;
/* The race's views (race_view.h): the window centre's row is 320 plus 501730 rounded (17ffc, after 8814: -50 in
 * the race view, so 270), less the CRTC offset 128; the focus outside the keyed cameras is 501734 as 8814 left it. */
static int practice_view_centre(const nm_practice_scene_t *scene) {
    const nm_race_view_frame_t *frame=&scene->race.camera_frame;
    int32_t offset;
    if(scene->attract_active || !frame->valid || !isfinite(frame->centre) || !nm_drive_round_i32(frame->centre,&offset) ||
       offset<-200 || offset>200) return PRACTICE_CENTER_Y;
    return 320+offset-128;
}
int nm_practice_render_projection(nm_practice_scene_t *scene,int enable) {
    scene->instance.center[0]=PRACTICE_CENTER_X;
    scene->instance.center[1]=enable ? practice_view_centre(scene) : PRACTICE_UI_CENTER_Y;
    if(!enable) return 1;
    const int keyed=scene->attract_active || scene->race.cars[0].car.camera_mode==1;
    const float key=scene->intro.camera.focus,reset=nm_force_float(scene->math->camera_reset_words[5]);
    /* Source order: difference, exact scale by 2^-3, sum; single precision. */
    const float eased=(reset-key)*0.125f;
    const nm_race_view_frame_t *frame=&scene->race.camera_frame;
    const float focus=keyed ? key+eased : frame->valid ? frame->focus : practice_follow_focus;
    if(!isfinite(focus) || !(focus>0)) return 0;
    scene->instance.focus[0]=scene->instance.focus[1]=focus;
    return 1;
}
/* The car in slot 501220 (car 0; the player in the race, the Hornet in the
 * attract and podium scenes) is never in the detail list 503208 (8480..84dc
 * walks the pool at 501224 only), so nm_practice_render_parts leaves its
 * ordinal empty. The arcade draws it with the same fixed list in every scene.
 * Replace that range with the list of player_parts.h, keeping every other
 * car's instances and order. An inspector pass that levelled car 0 is
 * replaced too. A special actor (kind 2) in slot 0 would keep its own path. */
/* The manual car's body (player_parts.h nm_player_manual_models) takes car-damage.dnb's variants of the automatic
 * body: same vertices in the same order, faces in another order (the pack orders faces by material). Face i of the
 * manual mesh takes the variant's face practice_body_faces[i] (damage_bodies.h nm_damage_body_face_map). */
static uint16_t practice_body_faces[1024];
static int practice_body_mapped;
/* The same for the hoods: the manual clean hood 9506a8 takes car-hoods.dnb's variants of 94fff9 in its face order. */
static uint16_t practice_hood_faces[512];
static int practice_hood_mapped;
static int practice_body_map(nm_practice_scene_t *scene,unsigned catalog) {
    practice_body_mapped=0;
    unsigned automatic=UINT32_MAX;
    for(unsigned i=0;i<catalog && automatic==UINT32_MAX;++i)
        if(nm_intro_model_ids[i]==nm_player_part_models[NM_PLAYER_PART_BODY]) automatic=i;
    if(automatic==UINT32_MAX || !nm_damage_body_face_map(&scene->model,scene->car_sources[automatic].mesh,
            scene->player_body_mesh,practice_body_faces,sizeof(practice_body_faces)/sizeof(practice_body_faces[0]))) return 0;
    practice_body_mapped=1;
    return 1;
}
static int practice_player_bind(nm_practice_scene_t *scene) {
    const unsigned manual=scene->transmission!=0; /* 501464: 7358..7374, 7340 (player_parts.h) */
    /* The catalogue may end with the player's own six instances: glass 13b,
     * shadow 8d7ab7 and four wheel bindings whose translation is the authored
     * render offset 234af4 (tools/append_player_parts.py). Older packs keep
     * only body and shell; the loader has already matched every address. */
    static const uint32_t extras[6]={0x13b,0x8d7ab7,0x8d7b13,0x8d7c9a,0x8d7b13,0x8d7c9a};
    const unsigned listed=sizeof(nm_intro_model_ids)/sizeof(nm_intro_model_ids[0]);
    int extended=listed>6;
    for(unsigned i=0;extended && i<6;++i) extended=nm_intro_model_ids[listed-6+i]==extras[i];
    const unsigned catalog=extended ? listed-6 : listed;
    for(unsigned part=0;part<NM_PLAYER_PARTS;++part) {
        uint32_t binding=UINT32_MAX;
        if(part==NM_PLAYER_PART_BODY || part==NM_PLAYER_PART_SHELL) {
            for(unsigned i=0;i<catalog && binding==UINT32_MAX;++i)
                if(nm_intro_model_ids[i]==nm_player_part_model(part,manual)) binding=i;
            if(binding==UINT32_MAX) return 0;
        } else if(extended) {
            binding=part==NM_PLAYER_PART_GLASS ? catalog : part==NM_PLAYER_PART_SHADOW ? catalog+1 :
                catalog+2+(part-NM_PLAYER_PART_WHEEL);
            const nm_instance_t *source=scene->car_sources+binding;
            if(source->source_address!=nm_player_part_models[part]) return 0;
            if(part>=NM_PLAYER_PART_WHEEL && part<NM_PLAYER_PART_SHADOW)
                for(unsigned a=0;a<3;++a) {
                    const float offset=source->matrix[9+a];
                    if(!isfinite(offset)) return 0;
                    scene->player_wheel_offsets[part-NM_PLAYER_PART_WHEEL][a]=offset;
                }
        }
        scene->player_bindings[part]=binding;
    }
    /* The body slot's alternatives (player_parts.h): the automatic car's damaged bodies are variants of the
     * clean body's mesh (car-damage.dnb, written by practice_player_body); its hoods are bindings. */
    scene->player_body_mesh=scene->car_sources[scene->player_bindings[NM_PLAYER_PART_BODY]].mesh;
    scene->player_body_variant=NM_DAMAGE_BODY_CLEAN;
    if(scene->damage_bodies.data && (scene->damage_bodies.body!=nm_player_part_models[NM_PLAYER_PART_BODY] ||
       scene->model.meshes[scene->player_body_mesh].vertex_count!=scene->damage_bodies.vertices ||
       scene->model.meshes[scene->player_body_mesh].face_count!=scene->damage_bodies.faces)) return 0;
    practice_body_mapped=0;
    if(manual && scene->damage_bodies.data && !practice_body_map(scene,catalog)) return 0;
    printf("Native player car: %s (body %06lx, shell %06lx)\n",manual ? "manual" : "automatic",
        (unsigned long)nm_player_part_model(NM_PLAYER_PART_BODY,manual),(unsigned long)nm_player_part_model(NM_PLAYER_PART_SHELL,manual));
    for(unsigned h=0;h<4;++h) {
        scene->player_hood_bindings[h]=UINT32_MAX;
        const uint32_t hood=manual ? nm_player_manual_hood_models[h] : nm_player_hood_models[h];
        for(unsigned i=0;i<catalog;++i)
            if(nm_intro_model_ids[i]==hood) { scene->player_hood_bindings[h]=i;break; }
    }
    /* The damaged hoods as variants of the clean hood's mesh (car-hoods.dnb): the automatic car's in place, the
     * manual car's in its face order against the automatic clean hood of the same pack. */
    scene->player_hood_variant=NM_DAMAGE_HOOD_CLEAN;practice_hood_mapped=0;
    scene->player_hood_mesh=UINT32_MAX;
    if(scene->hood_variants.data && scene->player_hood_bindings[0]!=UINT32_MAX) {
        const nm_damage_bodies_t *hoods=&scene->hood_variants;
        const unsigned mesh=scene->car_sources[scene->player_hood_bindings[0]].mesh;
        if(hoods->body!=nm_player_hood_models[0] || scene->model.meshes[mesh].vertex_count!=hoods->vertices ||
           scene->model.meshes[mesh].face_count!=hoods->faces) return 0;
        if(manual) {
            unsigned automatic=UINT32_MAX;
            for(unsigned i=0;i<catalog && automatic==UINT32_MAX;++i) if(nm_intro_model_ids[i]==nm_player_hood_models[0]) automatic=i;
            if(automatic==UINT32_MAX || !nm_damage_body_face_map(&scene->model,scene->car_sources[automatic].mesh,mesh,
                   practice_hood_faces,sizeof(practice_hood_faces)/sizeof(practice_hood_faces[0]))) return 0;
            practice_hood_mapped=1;
        }
        scene->player_hood_mesh=mesh;
    }
    /* The car effects' models (car_effects.h; tools/append_player_parts.py --effects puts them before the
     * player's six parts), by address like the hoods. */
    unsigned effects=0;
    for(unsigned kind=0;kind<NM_FX_KINDS;++kind) for(unsigned index=0;index<32;++index) {
        const uint32_t model=nm_fx_model(kind,index);
        scene->effect_bindings[kind][index]=UINT16_MAX;
        for(unsigned i=0;model && i<catalog;++i)
            if(nm_intro_model_ids[i]==model) { scene->effect_bindings[kind][index]=(uint16_t)i;++effects;break; }
    }
    printf("Native car effects: %u of the effect table entries bound (dust %s, sparks %s, smoke %s, spin smoke %s)\n",
        effects,scene->effect_bindings[NM_FX_DUST][1]!=UINT16_MAX ? "drawn" : "absent: not drawn",
        scene->effect_bindings[NM_FX_SPARK][9]!=UINT16_MAX ? "drawn" : "absent: not drawn",
        scene->effect_bindings[NM_FX_SMOKE][1]!=UINT16_MAX ? "drawn" : "absent: not drawn",
        scene->effect_bindings[NM_FX_SPIN][0]!=UINT16_MAX ? "drawn" : "absent: not drawn");
    if(extended) {
        const nm_mesh_t *glass=scene->model.meshes+scene->car_sources[catalog].mesh;
        const nm_mesh_t *shadow=scene->model.meshes+scene->car_sources[catalog+1].mesh;
        if(glass->face_count!=9 || !glass->vertex_count || glass->vertex_count>36 || shadow->face_count!=6) return 0;
        scene->window_vertices=(nm_vertex_t *)scene->model.vertices+glass->vertex_first;
        scene->window_vertex_count=glass->vertex_count;
        for(unsigned i=0;i<glass->vertex_count;++i) {
            scene->window_base_uv[2*i]=scene->window_vertices[i].u;
            scene->window_base_uv[2*i+1]=scene->window_vertices[i].v;
        }
    }
    scene->crew_model_count=0;
    nm_memset(scene->crew_draw_models,0,sizeof(scene->crew_draw_models));
    scene->crew_set_reach_ready=0;
    scene->player_pack=scene->model.data;
    return 1;
}
/* The car-pack binding of a pit crew model (tools/append_player_parts.py --crew puts them after the effects;
 * the held wheel is the catalogue's 8d7c9a), by address like the effects, remembered per pack. */
static uint16_t practice_crew_binding(nm_practice_scene_t *scene,uint32_t model) {
    for(unsigned i=0;i<scene->crew_model_count;++i) if(scene->crew_models[i]==model) return scene->crew_bindings[i];
    const unsigned listed=sizeof(nm_intro_model_ids)/sizeof(nm_intro_model_ids[0]);
    uint16_t binding=UINT16_MAX;
    for(unsigned i=0;i<listed && binding==UINT16_MAX;++i) if(nm_intro_model_ids[i]==model) binding=(uint16_t)i;
    /* A manual twin (models[1], pit.h) the pack lacks: the automatic crew's part, so the crew is drawn. */
    const nm_pit_crew_art_t *const art=scene->race.shared.pit_data ? scene->race.shared.pit_data->art : NULL;
    for(unsigned set=0;binding==UINT16_MAX && art && set<art->set_count && set<NM_PIT_CREW_SETS;++set)
        for(unsigned k=0;k<NM_PIT_CREW_PARTS;++k) if(art->sets[set].models[1][k]==model) {
            const uint32_t twin=art->sets[set].models[0][k];
            for(unsigned i=0;i<listed && binding==UINT16_MAX;++i) if(nm_intro_model_ids[i]==twin) binding=(uint16_t)i;
            break;
        }
    if(scene->crew_model_count<sizeof(scene->crew_models)/sizeof(scene->crew_models[0])) {
        scene->crew_models[scene->crew_model_count]=model;scene->crew_bindings[scene->crew_model_count++]=binding;
    }
    return binding;
}
/* The view the model renderer classifies a car instance against (model_math.h instance_view, the same
 * operations), here for the live instance's viewport and centre (every crew draw's). */
static nm_view_t practice_instance_view(const nm_practice_scene_t *scene) {
    const nm_header_t *header=scene->model.header;const nm_instance_t *inst=&scene->instance;
    return (nm_view_t){
        {(float)(header->crtc[0]+inst->center[0]),(float)(384-inst->center[1]+header->crtc[1])},
        {nm_view_limit_low(inst->viewport[0]+header->render[0],0),nm_view_limit_high(inst->viewport[2]+header->render[0]+1,496),
         nm_view_limit_low(384-inst->viewport[3]+header->render[1],0),nm_view_limit_high(384-inst->viewport[1]+header->render[1]+1,384)},0.01f};
}
/* Sum over the axes of the larger of |min| and |max| of a mesh's bounds: at least the distance of any of its
 * points from its origin. */
static float practice_bounds_reach(const float bounds[6]) {
    float sum=0;
    for(unsigned a=0;a<3;++a) { const float lo=fabsf(bounds[a]),hi=fabsf(bounds[a+3]); sum+=lo>hi ? lo : hi; }
    return sum;
}
/* The radius around part 0's origin that holds every part of model set `set` (pit_crew.h nm_pit_crew_reach: the
 * chains; plus each drawn mesh's reach), both transmissions; 0 when a bound is not finite. */
static float practice_crew_set_reach(nm_practice_scene_t *scene,const nm_pit_crew_art_t *art,unsigned set) {
    if(set>=NM_PIT_CREW_SETS) return 0;
    if(scene->crew_set_reach_ready&(1u<<set)) return scene->crew_set_reach[set];
    float chain[NM_PIT_CREW_PARTS],reach=0;
    nm_pit_crew_reach(art,chain);
    for(unsigned manual=0;manual<2;++manual) for(unsigned k=0;k<NM_PIT_CREW_PARTS;++k) {
        const uint16_t binding=practice_crew_binding(scene,art->sets[set].models[manual][k]);
        if(binding==UINT16_MAX) continue;
        const float r=chain[k]+practice_bounds_reach(scene->model.meshes[scene->car_sources[binding].mesh].bounds);
        if(r>reach) reach=r;
    }
    if(!isfinite(reach) || !(reach<0x1p30f)) reach=0;
    scene->crew_set_reach[set]=reach;scene->crew_set_reach_ready|=(uint16_t)(1u<<set);
    return reach;
}
/* Bounds hints of the crew's parts (model_renderer.h NM_RENDER_BOUNDS_HINT), per figure: the cube of side 2R'
 * around part 0's origin (the pose's translation in the root frame; R' the set's reach plus 1%, plus 2^-10 of R,
 * of that translation and of the root's distance, plus 10^-3) through the root frame, classified by
 * nm_bounds_quick_classify. Why that decides every part as the planes would: each part's matrix is the root's
 * times a rigid motion (part 0: the pose's translation and rotations; the others: their chain's translations and
 * rotations; the feet: the chain's translation, then a rotation of their own in view space, which the root's
 * near-orthonormal 3x3 maps from a ball of the same radius), so every point of a drawn part lies, in the root's
 * coordinates, within the set's reach of part 0's origin: inside the cube with room for the chains' rounding
 * (about 100 epsilon of the root's magnitudes; the 2^-10 covers 80 times that) and for the root's deviation from
 * a rotation (checked below 1/256: its singular values within 0.2% of 1, the extra 1%). A quick decision clears
 * every plane by 2^-12 of the cube's magnitude, at least 16 times what the planes' error term can reach for a
 * part (whose magnitude cannot exceed the cube's: its coordinates are the cube's, its matrix the root's times
 * rotations). So a part is outside (0) or inside (2) exactly when its figure's cube is decided so; otherwise,
 * and for the held items, no hint. */
static void practice_crew_hints(nm_practice_scene_t *scene,unsigned draws,unsigned figures) {
    for(unsigned i=0;i<draws;++i) scene->crew_draw_hints[i]=0;
    const nm_pit_crew_art_t *const art=scene->race.shared.pit_data->art;
    if(!art) return;
    const nm_view_t view=practice_instance_view(scene);
    for(unsigned f=0;f<figures;++f) {
        const nm_pit_crew_figure_t *const figure=scene->crew_figures+f;
        const float reach=practice_crew_set_reach(scene,art,figure->set);
        if(!(reach>0) || figure->first+figure->count>draws || figure->items+NM_PIT_CREW_PARTS!=figure->count) continue;
        const float *const m=figure->root,*const o=figure->pose;
        float gamma=0;
        for(unsigned i=0;i<3;++i) for(unsigned j=i;j<3;++j) {
            const float g=m[3*i]*m[3*j]+m[3*i+1]*m[3*j+1]+m[3*i+2]*m[3*j+2];
            gamma+=i==j ? fabsf(g-1.0f) : 2*fabsf(g);
        }
        if(!(gamma<1.0f/256)) continue;
        const float distance=fabsf(m[9])+fabsf(m[10])+fabsf(m[11])+fabsf(o[0])+fabsf(o[1])+fabsf(o[2]);
        if(!(distance<0x1p30f)) continue;
        const float r=reach*1.01f+(reach+distance)*0x1p-10f+1e-3f;
        const float cube[6]={o[0]-r,o[1]-r,o[2]-r,o[0]+r,o[1]+r,o[2]+r};
        const int quick=nm_bounds_quick_classify(&view,cube,m,scene->instance.focus);
        if(quick!=0 && quick!=2) continue;
        for(unsigned i=figure->first+figure->items;i<figure->first+figure->count;++i)
            scene->crew_draw_hints[i]=NM_RENDER_BOUNDS_HINT|(uint32_t)quick;
    }
}
/* 7328..7350: the hood nm_player_hood names. With car-hoods.dnb the clean hood's mesh holds it (as the body slot
 * below: a change rewrites positions, normals, bounds and that mesh's tables); else the pack's binding of that hood,
 * the clean one where the pack lacks it, none in a pack without hoods. Returns the binding (UINT32_MAX: none) or
 * UINT32_MAX-1 on a failed rewrite. */
static uint32_t practice_player_hood(nm_practice_scene_t *scene,uint32_t damage) {
    const unsigned hood=nm_player_hood(damage);
    if(scene->player_hood_mesh==UINT32_MAX) {
        const uint32_t binding=scene->player_hood_bindings[hood];
        return binding!=UINT32_MAX ? binding : scene->player_hood_bindings[0];
    }
    if(hood!=scene->player_hood_variant) {
        if(!nm_damage_body_apply_mapped(&scene->hood_variants,hood,&scene->model,scene->player_hood_mesh,
               practice_hood_mapped ? practice_hood_faces : NULL) ||
           !nm_render_car_mesh_refresh(scene->player_hood_mesh)) return UINT32_MAX-1u;
        scene->player_hood_variant=hood;
    }
    return scene->player_hood_bindings[0];
}
/* 7358 draws +14: the clean body or the damaged one nm_player_damage_body names. The mesh holds one at a time;
 * a change (a wall hit or crash, at most a few a race; back to clean in the next scene) rewrites its positions,
 * normals and bounds and the renderer's tables of that mesh (about 900 values and 325 faces). */
static int practice_player_body(nm_practice_scene_t *scene,uint32_t damage) {
    const int choice=nm_player_damage_body(damage);
    const unsigned variant=choice<0 || !scene->damage_bodies.data ? NM_DAMAGE_BODY_CLEAN : (unsigned)choice;
    if(variant==scene->player_body_variant) return 1;
    if(!nm_damage_body_apply_mapped(&scene->damage_bodies,variant,&scene->model,scene->player_body_mesh,
           practice_body_mapped ? practice_body_faces : NULL) ||
       !nm_render_car_mesh_refresh(scene->player_body_mesh)) return 0;
    scene->player_body_variant=variant;
    return 1;
}
int nm_practice_render_player_car(nm_practice_scene_t *scene) {
    if(scene->attract_active && scene->intro.stages[scene->intro.stage].spawn[0].kind==2) return 1;
    /* A reload replaces the owned pack (and its unmodified base UVs). */
    if(scene->player_pack!=scene->model.data && !practice_player_bind(scene)) return 0;
    const unsigned cars=scene->intro.count;
    unsigned ordinal=cars;
    for(unsigned i=0;i<cars;++i)
        if((scene->inspect_paused ? scene->inspect_order[i] : scene->intro.detail_order[i])==0) { ordinal=i;break; }
    if(ordinal==cars) return 0;
    alignas(32) float matrices[NM_PLAYER_PARTS][12];
    const nm_player_t *car=scene->attract_active ? scene->intro.cars : &scene->race.cars[0].car;
    /* The draw (camera row 60) runs before the pit crew rows 68..73: it sees the crew's writes (+208 wheel bits,
     * +5c, the wheel phases, 501a98) one tick late (pit.h nm_pit_drawn_t). */
    alignas(32) nm_player_t drawn_car;
    unsigned wheels_off=0;
    if(!scene->attract_active) car=nm_pit_drawn_player(&scene->race.shared.pit,car,&drawn_car,&wheels_off);
    /* View mode 5014a4 and eye word 501728 of what is drawn (player_parts.h, nm_player_show): 1 for the keyed
     * cameras (attract, podium), else the race's camera pass's (race_view.h: the driver's view draws no car,
     * the hood view the hood). */
    const nm_race_view_frame_t *frame=&scene->race.camera_frame;
    const int keyed=scene->attract_active || !frame->valid;
    const unsigned show=nm_player_show(keyed ? 1u : (uint32_t)frame->mode,keyed ? 0u : frame->eye),mask=nm_player_part_mask(show);
    if(!nm_player_parts_shown(scene->math,car,scene->view,scene->player_wheel_offsets,show,matrices)) return 0;
    /* The body slot: 7358 draws +14 (practice_player_body), the hood view 7328..7350 its hood. */
    uint32_t bindings[NM_PLAYER_PARTS];
    for(unsigned part=0;part<NM_PLAYER_PARTS;++part)
        bindings[part]=mask&(1u<<part) ? scene->player_bindings[part] : UINT32_MAX;
    /* 73b0..7664: wheel k is not drawn while 501a98 bit (1, 0, 3, 2)[k] is set (the pit crew has it off). */
    for(unsigned wheel=0;wheel<4;++wheel)
        if(wheels_off&(1u<<nm_pit_wheel_bits[wheel])) bindings[NM_PLAYER_PART_WHEEL+wheel]=UINT32_MAX;
    const uint32_t damage=car->boundary.damage.damage;
    if(show&NM_PLAYER_SHOW_HOOD) {
        bindings[NM_PLAYER_PART_BODY]=practice_player_hood(scene,damage);
        if(bindings[NM_PLAYER_PART_BODY]==UINT32_MAX-1u) return 0;
    } else if((show&NM_PLAYER_SHOW_BODY) && !practice_player_body(scene,damage)) return 0;
    /* The contact effects 9088 draws after the body and wheels (car_effects.h), race only (the attract's car 0
     * has no timer decrement 9014; the original's attract never sets them), view preset 2: dust and sparks
     * (punch-through), tyre smoke (the translucent class: the model renderer draws it on the translucent list).
     * Then the spin smoke's drawn particles (task 9e2c, world space, in slot order). All of them are instances
     * of the player's part range; a pack without a model leaves it out. */
    nm_car_effect_t effects[NM_CAR_EFFECTS_MAX];
    uint16_t effect_binding[NM_CAR_EFFECTS_MAX],spin_binding[NM_SPIN_SMOKE_PARTICLES];
    uint8_t spin_slot[NM_SPIN_SMOKE_PARTICLES];
    unsigned effect_count=0,smoke_count=0,spin_count=0;
    alignas(32) float effect_frame[12];
    if(!scene->attract_active) {
        const unsigned n=nm_car_effects(car,frame->valid ? frame->view : 2u,effects);
        for(unsigned i=0;i<n;++i) {
            const uint16_t binding=scene->effect_bindings[effects[i].kind][effects[i].index&31];
            if(binding==UINT16_MAX) continue;
            smoke_count+=effects[i].translucent;
            effects[effect_count]=effects[i];effect_binding[effect_count++]=binding;
        }
        if(effect_count && !nm_car_effect_frame(scene->math,car,show&NM_PLAYER_SHOW_WHEELS,
            matrices[NM_PLAYER_PART_BODY],effect_frame)) return 0;
        /* The task runs (and draws) in the race phases only: in the results phases nothing steps it. */
        const nm_spin_smoke_t *spin=&scene->spin_smoke;
        for(unsigned i=0;nm_spin_smoke_phase(scene->race.active_phase) && i<NM_SPIN_SMOKE_PARTICLES;++i) {
            if(!(spin->drawn&(1u<<i)) || spin->life[i]>=NM_FX_SPIN_LIVES) continue;
            const uint16_t binding=scene->effect_bindings[NM_FX_SPIN][spin->life[i]];
            if(binding==UINT16_MAX) continue;
            spin_slot[spin_count]=(uint8_t)i;spin_binding[spin_count++]=binding;
        }
    }
    /* The pit crew's draws (pit_crew.h) on the ticks its rows draw (D+129..S+511; the opponents are hidden then),
     * after the player's: rows 68..73 run after the camera row 60. Same light, focus and view as the car. */
    unsigned crew_count=0;
    if(!scene->attract_active && scene->race.shared.pit_data) {
        unsigned n=0,figures=0;
        if(!nm_pit_crew_draws_figures(scene->math,scene->race.shared.pit_data,&scene->race.shared.pit,scene->view,
            scene->camera_heading,scene->crew_draws,&n,scene->crew_figures,&figures)) return 0;
        practice_crew_hints(scene,n,figures);
        for(unsigned i=0;i<n;++i) {
            /* The draws come in the same order every frame: draw i's binding is remembered with its model. */
            const uint32_t model=scene->crew_draws[i].model;
            uint16_t binding;
            if(scene->crew_draw_models[i]==model) binding=scene->crew_draw_bindings[i];
            else {
                binding=practice_crew_binding(scene,model);
                scene->crew_draw_models[i]=model;scene->crew_draw_bindings[i]=binding;
            }
            if(binding==UINT16_MAX) continue;
            if(crew_count!=i) shz_memcpy4(scene->crew_draws+crew_count,scene->crew_draws+i,sizeof(scene->crew_draws[i]));
            scene->crew_draw_hints[crew_count]=scene->crew_draw_hints[i];
            scene->crew_draws[crew_count++].model=binding; /* from here: the binding */
        }
    }
    unsigned added=effect_count+spin_count+crew_count;
    for(unsigned part=0;part<NM_PLAYER_PARTS;++part) added+=bindings[part]!=UINT32_MAX;
    const unsigned first=scene->car_part_boundaries[ordinal];
    const unsigned old=scene->car_part_boundaries[ordinal+1]-first;
    const unsigned total=scene->intro_header.instances-old+added;
    if(total>sizeof(scene->car_instances)/sizeof(scene->car_instances[0])) return 0;
    nm_memmove(scene->car_instances+first+added,scene->car_instances+first+old,
        (scene->intro_header.instances-first-old)*sizeof(*scene->car_instances));
    nm_instance_t *out=scene->car_instances+first;
    for(unsigned part=0;part<NM_PLAYER_PARTS;++part) {
        if(bindings[part]==UINT32_MAX) continue;
        const nm_instance_t *source=scene->car_sources+bindings[part];
        shz_memcpy4(out,&scene->instance,sizeof(*out));
        out->mesh=source->mesh;out->source_address=source->source_address;
        shz_memcpy4(out->matrix,matrices[part],sizeof(matrices[part]));
        ++out;
    }
    for(unsigned i=0;i<effect_count;++i) {
        const nm_instance_t *source=scene->car_sources+effect_binding[i];
        shz_memcpy4(out,&scene->instance,sizeof(*out));
        out->mesh=source->mesh;out->source_address=source->source_address;
        if(!nm_car_effect_matrix(scene->math,effect_frame,effects+i,out->matrix)) return 0;
        ++out;
    }
    for(unsigned i=0;i<spin_count;++i) {
        const nm_instance_t *source=scene->car_sources+spin_binding[i];
        shz_memcpy4(out,&scene->instance,sizeof(*out));
        out->mesh=source->mesh;out->source_address=source->source_address;
        if(!nm_spin_smoke_matrix(scene->math,scene->view,&scene->spin_smoke,spin_slot[i],out->matrix)) return 0;
        ++out;
    }
    for(unsigned i=0;i<crew_count;++i) {
        const nm_instance_t *source=scene->car_sources+scene->crew_draws[i].model;
        shz_memcpy4(out,&scene->instance,sizeof(*out));
        out->mesh=source->mesh;out->source_address=source->source_address;
        shz_memcpy4(out->matrix,scene->crew_draws[i].matrix,sizeof(out->matrix));
        out->padding[0]=scene->crew_draw_hints[i];scene->crew_hinted+=scene->crew_draw_hints[i]!=0;
        ++out;
    }
    scene->effects_drawn+=effect_count-smoke_count; scene->smoke_drawn+=smoke_count; scene->spin_drawn+=spin_count;
    scene->crew_drawn+=crew_count;
    for(unsigned i=ordinal+1;i<=cars;++i) scene->car_part_boundaries[i]+=added-old;
    scene->intro_header.instances=total;
    if(!scene->attract_active) scene->race_bodies|=1;
    /* Same owned-CPU reflection update as the opponents' two windows. */
    if(scene->window_vertices && (!nm_render_uv_dynamic(scene->window_vertices,scene->window_vertex_count) ||
        !nm_reflection_apply(scene->window_vertices,scene->window_base_uv,
        scene->window_vertex_count,scene->intro.reflection_u,scene->intro.reflection_v))) return 0;
    return 1;
}
/* The road class's depth scale of this frame's camera (render/models/model_renderer.h, NM_ROAD_DEPTH_SCALE). In a
 * race view (the race frame's camera mode 0 or 2, race_view.h): h/(h+NM_ROAD_SINK) for h the highest of the four
 * presets' eye heights (VR1..VR4 1.05, 1.10, 1.85, 3.74: 0.636, 0.647, 0.755, 0.862) that the drawn eye has
 * reached. The eye height word 501724 moves an eighth of the way to its target a tick (8814): from the last view's
 * after a change, from the reset record's (1.85) after the crash or start camera, to the reset record's at the
 * finish; the view's own preset would put the scale above what the eye's height allows on the way up (VR1 to
 * VR4: some 20 ticks) and cut the wheels again. The frame draws the word the previous tick's update left: the
 * lower of the last frame's and this one's. Every other camera (attract, podium, the keyed race cameras, the
 * inspector): NM_ROAD_DEPTH_SCALE. A change rewrites the road class (nm_render_road_depth): at most three times a
 * view change, never per frame. Render only: the gameplay words are read, none is written. */
#define PRACTICE_ROAD_REACHED (1.0f/1024.0f) /* an eased word stops some ulps short of its target */
static float practice_road_depth=NM_ROAD_DEPTH_SCALE; /* this frame's; the shadow prisms take it too */
static __attribute__((noinline)) void practice_road_depth_pick(const nm_practice_scene_t *scene) {
#ifndef NM_ROAD_DEPTH_FIXED
    static float last; /* the eye height word after the last frame's tick */
    const nm_race_view_frame_t *const frame=&scene->race.camera_frame;
    const float now=nm_force_float(scene->race.cars[0].car.camera_words[1]),eye=now<last ? now : last;
    last=now;
    float scale=NM_ROAD_DEPTH_SCALE,reached=0;
    if(!scene->attract_active && !scene->inspect_paused && frame->valid && (frame->mode==0 || frame->mode==2) &&
       scene->race.view_presets) {
        for(unsigned k=0;k<4;++k) {
            const float h=nm_force_float(scene->race.view_presets[k].words[0]);
            if(h>reached && h<=eye+PRACTICE_ROAD_REACHED) reached=h;
        }
        if(reached>0) scale=shz_divf(reached,reached+NM_ROAD_SINK);
    }
    if(scale!=practice_road_depth) practice_road_depth=nm_render_road_depth(scale);
#else
    (void)scene; /* make NATIVE_DIAG_ROAD_DEPTH: every camera at NM_ROAD_DEPTH_SCALE */
#endif
}
/* Ground shadows as PVR modifier volumes. The original draws the authored
 * footprints (opponents 8d9e42 solid black; player 8d7ab7 black on every other
 * pixel of a screen checkerboard, raster checker bit) as flat polygons sorted
 * over the road. A depth buffer cannot place a coplanar quad on a road mesh
 * that only approximates the contact surface, so each footprint outline is
 * extruded along world-vertical into a closed prism on the opaque modifier
 * list; road-surface polygons carry the cheap-shadow bit (compile_draw_header)
 * and are darkened by the global shadow scale where they pass through a
 * prism. The surface is written at this frame's road depth scale
 * (practice_road_depth) of its true depth, so the prisms are too; rails and
 * walls keep true depth and do not receive.
 * Selection and pose are unchanged: every shadow instance the part lists
 * produce is taken out of the mesh stream and becomes one volume. */
#define PRACTICE_SHADOW_REACH 1.0f /* Up and down from the posed footprint, world units. */
#define PRACTICE_SHADOW_SCALE 0.5f /* Checker coverage: half of the pixels black. */
static const uint32_t practice_shadow_models[2]={0x8d9e42,0x8d7ab7};
static alignas(32) pvr_mod_hdr_t practice_shadow_headers[2]; /* Other, last triangle. */

static int practice_shadow_bind(nm_practice_scene_t *scene) {
    const nm_header_t *original=scene->model.data;
    scene->shadow_outline_count[0]=scene->shadow_outline_count[1]=0;
    for(unsigned i=0;i<original->instances;++i) for(unsigned kind=0;kind<2;++kind) {
        const nm_instance_t *source=scene->car_sources+i;
        if(source->source_address!=practice_shadow_models[kind] || scene->shadow_outline_count[kind]) continue;
        scene->shadow_outline_count[kind]=nm_shadow_outline(&scene->model,
            scene->model.meshes+source->mesh,scene->shadow_outline[kind]);
    }
    pvr_mod_compile(practice_shadow_headers,PVR_LIST_OP_MOD,PVR_MODIFIER_OTHER_POLY,PVR_CULLING_NONE);
    pvr_mod_compile(practice_shadow_headers+1,PVR_LIST_OP_MOD,PVR_MODIFIER_INCLUDE_LAST_POLY,PVR_CULLING_NONE);
    pvr_set_shadow_scale(true,PRACTICE_SHADOW_SCALE);
    scene->shadow_pack=scene->model.data;
    printf("Native shadow volumes: outlines opponent=%u player=%u vertices; %u/%u triangles per volume\n",
        scene->shadow_outline_count[0],scene->shadow_outline_count[1],
        nm_shadow_volume_triangles(scene->shadow_outline_count[0]),
        nm_shadow_volume_triangles(scene->shadow_outline_count[1]));
    return 1;
}
/* After the part lists: move every shadow instance out of the mesh stream. */
/* 7684..768c: while the pit's stop count 501a84 is above 1 the camera's draw pass skips the opponents' draw
 * (7668, their shadows included); the detail pass that feeds the guided mode is not gated (gameplay, 8480..).
 * Keep only the player's part range. Race only. */
static void practice_hide_opponents(nm_practice_scene_t *scene) {
    if(scene->attract_active || !scene->race.count || (int32_t)scene->race.shared.timer_hold<=1) return;
    const unsigned cars=scene->intro.count;
    for(unsigned i=0;i<cars;++i) {
        if((scene->inspect_paused ? scene->inspect_order[i] : scene->intro.detail_order[i])!=0) continue;
        const unsigned first=scene->car_part_boundaries[i],count=scene->car_part_boundaries[i+1]-first;
        if(first) nm_memmove(scene->car_instances,scene->car_instances+first,count*sizeof(*scene->car_instances));
        for(unsigned k=0;k<=cars;++k) scene->car_part_boundaries[k]=k<=i ? 0 : count;
        scene->intro_header.instances=count;
        return;
    }
}
int nm_practice_render_shadows(nm_practice_scene_t *scene) {
    practice_hide_opponents(scene);
    scene->shadow_count=0;
    if(scene->shadow_pack!=scene->model.data && !practice_shadow_bind(scene)) return 0;
    const unsigned cars=scene->intro.count;
    unsigned kept=0,car=0;
    for(unsigned i=0;i<scene->intro_header.instances;++i) {
        while(car<cars && i>=scene->car_part_boundaries[car+1]) scene->car_part_boundaries[++car]=kept;
        const nm_instance_t *inst=scene->car_instances+i;
        const unsigned kind=inst->source_address==practice_shadow_models[0] ? 0 :
            inst->source_address==practice_shadow_models[1] ? 1 : 2;
        if(kind<2 && scene->shadow_outline_count[kind]) {
            if(scene->shadow_count==NM_SHADOW_VOLUMES_MAX) return 0;
            scene->shadow_kind[scene->shadow_count]=(uint8_t)kind;
            shz_memcpy4(scene->shadow_matrix[scene->shadow_count++],inst->matrix,sizeof(inst->matrix));
            continue;
        }
        if(kept!=i) shz_memcpy4(scene->car_instances+kept,inst,sizeof(*inst));
        ++kept;
    }
    while(car<cars) scene->car_part_boundaries[++car]=kept;
    scene->intro_header.instances=kept;
    scene->shadow_focus=scene->instance.focus[0];
    scene->shadow_centre_y=scene->instance.center[1]; /* the race projection; the HUD pass changes it before the volumes draw */
    shz_memcpy4(scene->shadow_up,scene->view+3,sizeof(scene->shadow_up));
    return 1;
}
static inline void practice_shadow_triangle(const pvr_mod_hdr_t *header,
        const float *a,const float *b,const float *c) {
    if(header) { pvr_mod_hdr_t *out=pvr_dr_target();*out=*header;pvr_dr_commit(out); }
    pvr_modifier_vol_t *first,*second;
    enj_draw_pvr_dr64_init((void **)&first,(void **)&second);
    first->flags=PVR_CMD_VERTEX_EOL;
    first->ax=a[0];first->ay=a[1];first->az=a[2];
    first->bx=b[0];first->by=b[1];first->bz=b[2];
    first->cx=c[0];
    enj_draw_pvr_dr64_commit_1st();
    second->cy=c[1];second->cz=c[2];
    enj_draw_pvr_dr64_commit_2nd();
}
/* Shadow prisms are closed volumes; one that crosses the near plane is cut
 * there and closed with its cross-section, so a car beside the camera (a slow
 * overtake) keeps its shadow for as long as the car itself is drawn. */
#define PRACTICE_SHADOW_NEAR 0.1f
#define PRACTICE_SHADOW_POLY (NM_SHADOW_OUTLINE_MAX+4)
#define PRACTICE_SHADOW_CUTS (2*NM_SHADOW_OUTLINE_MAX+8)
#define PRACTICE_SHADOW_TRIS (8*NM_SHADOW_OUTLINE_MAX)
typedef struct {
    float cx,cy,focus;
    float tri[PRACTICE_SHADOW_TRIS][3][3];
    float cut[PRACTICE_SHADOW_CUTS][3];
    unsigned tris,cuts,left,right,above,below,overflow;
} practice_shadow_build_t;
static void practice_shadow_project(practice_shadow_build_t *b,const float *v,float *out) {
    const float q=shz_invf_fsrra(v[2]);
    out[0]=b->cx+v[0]*b->focus*q;out[1]=b->cy-v[1]*b->focus*q;out[2]=q*practice_road_depth;
    b->left&=out[0]<0;b->right&=out[0]>496;b->above&=out[1]<0;b->below&=out[1]>384;
    out[0]*=(640.0f/496.0f)*ENJ_XSCALE;out[1]*=480.0f/384.0f;
}
/* Projects a convex view-space polygon (all in front) as a fan. */
static void practice_shadow_fan(practice_shadow_build_t *b,float (*poly)[3],unsigned count) {
    if(count<3) return;
    if(b->tris+count-2>PRACTICE_SHADOW_TRIS) { b->overflow=1;return; }
    float first[3],prev[3],cur[3];
    practice_shadow_project(b,poly[0],first);practice_shadow_project(b,poly[1],prev);
    for(unsigned i=2;i<count;++i,++b->tris) {
        practice_shadow_project(b,poly[i],cur);
        float (*t)[3]=b->tri[b->tris];
        for(unsigned k=0;k<3;++k) { t[0][k]=first[k];t[1][k]=prev[k];t[2][k]=cur[k]; }
        for(unsigned k=0;k<3;++k) prev[k]=cur[k];
    }
}
/* Clips one face against the near plane (Sutherland-Hodgman), keeps the
 * points it creates on the plane for the closing cap, and fans the rest. */
static void practice_shadow_face(practice_shadow_build_t *b,const float *const *in,unsigned count) {
    float out[PRACTICE_SHADOW_POLY][3];
    unsigned kept=0;
    for(unsigned i=0;i<count;++i) {
        const float *a=in[i],*c=in[i+1<count ? i+1 : 0]; /* no division an edge */
        const int ain=a[2]>PRACTICE_SHADOW_NEAR,cin=c[2]>PRACTICE_SHADOW_NEAR;
        if(ain && kept<PRACTICE_SHADOW_POLY) { out[kept][0]=a[0];out[kept][1]=a[1];out[kept][2]=a[2];++kept; }
        if(ain!=cin && kept<PRACTICE_SHADOW_POLY) {
            const float t=shz_divf(PRACTICE_SHADOW_NEAR-a[2],c[2]-a[2]);
            float *p=out[kept++];
            p[0]=shz_lerpf(a[0],c[0],t);p[1]=shz_lerpf(a[1],c[1],t);p[2]=PRACTICE_SHADOW_NEAR;
            if(b->cuts<PRACTICE_SHADOW_CUTS) {
                float *k=b->cut[b->cuts++];k[0]=p[0];k[1]=p[1];k[2]=p[2];
            } else b->overflow=1;
        }
    }
    practice_shadow_fan(b,out,kept);
}
/* The cross-section of a convex prism with the near plane is convex: order
 * the cut points around their centre (pseudo-angle, no trigonometry). */
static void practice_shadow_close(practice_shadow_build_t *b) {
    if(b->cuts<3) return;
    float mx=0,my=0;
    for(unsigned i=0;i<b->cuts;++i) { mx+=b->cut[i][0];my+=b->cut[i][1]; }
    mx=shz_divf(mx,(float)b->cuts);my=shz_divf(my,(float)b->cuts);
    float key[PRACTICE_SHADOW_CUTS];
    for(unsigned i=0;i<b->cuts;++i) {
        const float dx=b->cut[i][0]-mx,dy=b->cut[i][1]-my,s=fabsf(dx)+fabsf(dy);
        const float r=s>0 ? shz_divf(dx,s) : 0;
        key[i]=dy>=0 ? 1-r : 3+r;
    }
    for(unsigned i=1;i<b->cuts;++i)
        for(unsigned j=i;j>0 && key[j-1]>key[j];--j) {
            const float k=key[j];key[j]=key[j-1];key[j-1]=k;
            for(unsigned c=0;c<3;++c) { const float t=b->cut[j][c];b->cut[j][c]=b->cut[j-1][c];b->cut[j-1][c]=t; }
        }
    practice_shadow_fan(b,b->cut,b->cuts);
}
static void practice_shadow_volumes(void *data) {
    nm_practice_scene_t *scene=data;
    const float *up=scene->shadow_up;
    practice_shadow_build_t b;
    for(unsigned s=0;s<scene->shadow_count;++s) {
        const unsigned kind=scene->shadow_kind[s],n=scene->shadow_outline_count[kind];
        float ring[2][NM_SHADOW_OUTLINE_MAX][3];
        nm_practice_render_matrix(scene->shadow_matrix[s]);
        unsigned behind=0;
        for(unsigned i=0;i<n;++i) {
            const float *p=scene->shadow_outline[kind][i];
            const shz_vec4_t t=shz_xmtrx_transform_vec4(shz_vec4_init(p[0],p[1],p[2],1));
            for(unsigned side=0;side<2;++side) {
                const float reach=side ? -PRACTICE_SHADOW_REACH : PRACTICE_SHADOW_REACH;
                float *v=ring[side][i];
                v[0]=t.x+up[0]*reach;v[1]=t.y+up[1]*reach;v[2]=t.z+up[2]*reach;
                behind+=!nm_float_positive(v[2]-PRACTICE_SHADOW_NEAR); /* a sign test: no libgcc unordered call a point */
            }
        }
        if(behind==2*n) continue;
        b.cx=PRACTICE_CENTER_X;b.cy=384-scene->shadow_centre_y;b.focus=scene->shadow_focus;
        b.tris=b.cuts=b.overflow=0;b.left=b.right=b.above=b.below=1;
        /* The walls, then both caps; with a near cut, the cross-section closes it. */
        for(unsigned i=0;i<n;++i) {
            const unsigned j=i+1<n ? i+1 : 0;
            const float *quad[4]={ring[0][i],ring[0][j],ring[1][j],ring[1][i]};
            practice_shadow_face(&b,quad,4);
        }
        const float *cap[2][NM_SHADOW_OUTLINE_MAX];
        for(unsigned i=0;i<n;++i) { cap[0][i]=ring[0][i];cap[1][i]=ring[1][n-1-i]; }
        practice_shadow_face(&b,cap[0],n);
        practice_shadow_face(&b,cap[1],n);
        if(behind) practice_shadow_close(&b);
        /* An open volume would darken everything behind it: drop it instead. */
        if(b.overflow || b.tris<4 || b.left || b.right || b.above || b.below) continue;
        for(unsigned i=0;i<b.tris;++i)
            practice_shadow_triangle(i==0 ? practice_shadow_headers : i+1==b.tris ? practice_shadow_headers+1 : NULL,
                b.tri[i][0],b.tri[i][1],b.tri[i][2]);
    }
}
/* Selector values of the world objects (meshpack.h, NM_OBJECT_*): which
 * variant of each kind the arcade's actor draws this tick.
 * BOARD, 221d64..221db0: "START" while 501854 bit 0 (rolling start), else
 * "GOAL" when total laps 5010ec minus the completed laps +6d of the car in
 * 501220 is 0 or 1 as an UNSIGNED word (cmpobge 1: a count past the total is
 * "CHECK"), else "CHECK". In the podium that car is still the race's player
 * entity: its spawn cf74 keeps +6d/+6e and its updater d230 counts each pass
 * of the line (1365c: +6e+1 into +6e and +6d), so GOAL holds until the podium
 * car passes the line (profile 195 -> 0), then CHECK (8-9). Every attract
 * entry resets +6d (c940 -> e20c -> e3f4) with 5010ec 8: "CHECK".
 * BEARING, 223b48..223b94: command 0f0f of (-380, x, z, 210) with x, z of
 * the camera entity 501240, i.e. the angle of (-380-z, x-210) as in
 * check_road.hpp, negated, >>5, &63. That entity's position is not the eye:
 * the follow view copies the viewed car's position into it; the cinematic
 * cameras leave it alone: zero in attract, podium and through the
 * rolling-start camera (reference display lists: model 27 there), and the
 * last follow position during the crash camera (not compared with the
 * original).
 * SCREEN, 2202a8..2202b4: (5010b0>>3)&3. 5010b0 counts main-loop ticks since
 * power-on (1778); against the scene's ticks it is the race tick - 1 in the
 * race, the run's tick - 1 in the podium and the scene tick + 3 in the
 * attract (reference display lists).
 * RANK k, 221e3c..221e88: list 238dfc[byte +4 of car 503080[k]], the spawn
 * slot of the car in position k+1: the race's order after this tick's
 * traffic pass (1225c, phases 20 and 22; frozen after), the cinematic
 * scenes' order as their entry set it (121d4; never sorted with flags 8 or
 * 1<<28). 501a80 (Start held at transmission select: every panel "41") is
 * never set by this port.
 * SLOT k, 22061c/2206b0/220744: list 2385f8[r_k] with r_k the reel after
 * this tick's 220534 (nm_slot_machine_tick), drawn on the ticks it ran:
 * its visibility test is the draw bitmap of the cells the scene draws
 * objects from (cells_previous). The race's actor is gameplay
 * (nm_race_scene_t), the cinematic scenes' visual (nm_intro_scene_t).
 * List 64 is list 0. */
static unsigned practice_board(const nm_practice_scene_t *scene) {
    const int podium=scene->attract_active && (scene->intro.flags&(UINT32_C(1)<<28));
    if(!scene->race.count || (scene->attract_active && !podium)) return 0u;
    const nm_player_t *player=&scene->race.cars[0].car;
    if(scene->race.start_flags&1u) return 2u;
    const unsigned passes=podium ? scene->intro.cars[0].lap.lap : 0u; /* nm_intro_road_step from 0 */
    const unsigned completed=passes ? (uint8_t)(player->lap.lap+passes) : player->lap.completed;
    return (uint32_t)player->checkpoints.total_laps-completed<=1u ? 1u : 0u;
}
static int practice_object_variants(nm_practice_scene_t *scene,unsigned variant[NM_OBJECT_KINDS]) {
    variant[NM_OBJECT_ALWAYS]=0;
    variant[NM_OBJECT_BOARD]=practice_board(scene);
    const int podium=scene->attract_active && (scene->intro.flags&(UINT32_C(1)<<28));
    if(!scene->attract_active && scene->race.count) {
        const nm_player_t *player=&scene->race.cars[0].car;
        if(player->camera_mode!=1) {
            scene->world_object_origin[0]=player->boundary.ground.position[0];
            scene->world_object_origin[1]=player->boundary.ground.position[2];
        }
    } else scene->world_object_origin[0]=scene->world_object_origin[1]=0;
    const int32_t angle=nm_force_angle(scene->math,-380.0f-scene->world_object_origin[1],scene->world_object_origin[0]-210.0f);
    if(angle==INT32_MIN) return 0;
    variant[NM_OBJECT_BEARING]=(unsigned)((-angle)>>5)&63u;
    const uint32_t clock=!scene->attract_active ? scene->race.ticks-1u :
        podium ? scene->race.ticks+NM_PODIUM_CLOCK_OFFSET+scene->intro.ticks : scene->intro.ticks+3u;
    variant[NM_OBJECT_SCREEN]=(clock>>3)&3u;
    variant[NM_OBJECT_TURN]=variant[NM_OBJECT_CREW]=0; /* per cell: practice_object_value */
    for(unsigned k=0;k<NM_OBJECT_RANKS;++k)
        variant[NM_OBJECT_RANK+k]=!scene->attract_active ? scene->race.order.rank[k] : scene->intro.rank_order[k];
    const nm_slot_machine_t *slot=!scene->attract_active ? &scene->race.slot_machine : &scene->intro.slot_machine;
    for(unsigned k=0;k<NM_OBJECT_SLOTS;++k) variant[NM_OBJECT_SLOT+k]=(unsigned)slot->reel[k]&63u;
    variant[NM_OBJECT_CONE]=0; /* practice_cones */
    return 1;
}
/* The variant a run of kind `kind` in cell `cell` draws. */
static inline unsigned practice_object_value(const nm_practice_scene_t *scene,const unsigned variant[NM_OBJECT_KINDS],
        unsigned kind,unsigned cell) {
    const unsigned n=scene->intro.object_near_ticks[cell];
    return kind==NM_OBJECT_TURN ? n&255u : kind==NM_OBJECT_CREW ? n%25u/8u : variant[kind];
}
/* One world object instance into the frame: mesh `mesh` (a MESHES instance's
 * chosen one), drawn with the view matrix, or with the view matrix times the
 * instance's placement (PLACED) times the wheel's turn about Z (SPIN). */
static int practice_object_place(nm_practice_scene_t *scene,const nm_instance_t *source,uint32_t mesh,unsigned cell) {
    if(!scene->world.meshes[mesh].face_count) return 1;
    if(scene->world_header.instances>=scene->scenery.instances+scene->world_object_count) return 0;
    nm_instance_t *out=scene->world_instances+scene->world_header.instances++;
    shz_memcpy4(out,&scene->instance,sizeof(*out));
    out->mesh=mesh; out->source_address=source->source_address;
    const uint32_t word=source->padding[0];
    out->padding[0]=word;
    if(word&NM_OBJECT_PLACED) {
        float model[12];
        shz_memcpy4(model,source->matrix,sizeof(model));
        if(word&NM_OBJECT_SPIN) {
            /* 220b14: Rz(256n) after the placement; model points p map to
             * (c x + s y, -s x + c y, z): columns (c,-s,0), (s,c,0). */
            const uint16_t angle=(uint16_t)(256u*scene->intro.object_near_ticks[cell]);
#ifdef __DREAMCAST__
            const shz_sincos_t sc=shz_sincosu16(angle);
            const float c=sc.cos,sn=sc.sin;
#else
            const float turn=(float)angle*(6.28318530717958647692f/65536.0f),c=cosf(turn),sn=sinf(turn);
#endif
            const float spin[12]={c,-sn,0, sn,c,0, 0,0,1, 0,0,0};
            if(!nm_pose_compose(source->matrix,spin,model)) return 0;
        }
        if(!nm_pose_compose(scene->view,model,out->matrix)) return 0;
    }
    return 1;
}
/* A cone (NM_OBJECT_CONE, include/props.h) as its props tick drew it (223528..22360c):
 * view * T(x, y, z) * Rx(h54) * Rz(h56) * Ry(yaw) * Rz(he) * Ry(hc) * Rx(ha), each rotation the TGP's
 * (spec-cones.md section 6: Rx c1' = c1 c - c2 s, c2' = c1 s + c2 c; Ry c0' = c0 c + c2 s,
 * c2' = c2 c - c0 s; Rz c0' = c0 c - c1 s, c1' = c0 s + c1 c). At rest every angle is 0. */
static void practice_cone_rotate(float m[12],unsigned axis,int16_t angle) {
    if(!angle) return;
#ifdef __DREAMCAST__
    const shz_sincos_t sc=shz_sincosu16((uint16_t)angle);
    const float c=sc.cos,sn=sc.sin;
#else
    const float turn=(float)(uint16_t)angle*(6.28318530717958647692f/65536.0f),c=cosf(turn),sn=sinf(turn);
#endif
    float *const a=m+(axis==0 ? 3 : 0),*const b=m+(axis==2 ? 3 : 6);   /* X: c1, c2; Y: c0, c2; Z: c0, c1 */
    for(unsigned r=0;r<3;++r) {
        const float u=a[r],v=b[r];
        if(axis==1) { a[r]=u*c+v*sn; b[r]=v*c-u*sn; }
        else { a[r]=u*c-v*sn; b[r]=u*sn+v*c; }
    }
}
static int practice_cone_place(nm_practice_scene_t *scene,const nm_instance_t *source,const nm_prop_t *cone) {
    if(!scene->world.meshes[source->mesh].face_count) return 1;
    if(scene->world_header.instances>=scene->scenery.instances+scene->world_object_count) return 0;
    nm_instance_t *out=scene->world_instances+scene->world_header.instances++;
    shz_memcpy4(out,&scene->instance,sizeof(*out));
    out->mesh=source->mesh; out->source_address=source->source_address; out->padding[0]=source->padding[0];
    float model[12]={1,0,0, 0,1,0, 0,0,1, cone->x,cone->draw_y,cone->z};
    practice_cone_rotate(model,0,cone->h54); practice_cone_rotate(model,2,cone->h56);
    practice_cone_rotate(model,1,cone->yaw); practice_cone_rotate(model,2,cone->he);
    practice_cone_rotate(model,1,cone->hc); practice_cone_rotate(model,0,cone->ha);
    return nm_pose_compose(scene->view,model,out->matrix);
}
/* The cones of one selection run: each as the props drew it, or at rest by the run's cell without them. */
static int practice_cones(nm_practice_scene_t *scene,const nm_instance_t *sources,unsigned first,unsigned end,int cell_drawn) {
    const nm_props_t *props=!scene->attract_active ? nm_props_race() : nm_props_intro();
    for(unsigned k=first;k<end;++k) {
        const unsigned v=NM_OBJECT_VARIANT(sources[k].padding[0]);
        if(props && v<props->count) {
            if(props->cone[v].drawn && !practice_cone_place(scene,sources+k,props->cone+v)) return 0;
        } else if(cell_drawn && !practice_object_place(scene,sources+k,sources[k].mesh,NM_OBJECT_CELL(sources[k].padding[0])))
            return 0;
    }
    return 1;
}
/* Whether the model renderer rejects a world instance whole (model_submission.h sq_prepare_source: the view of
 * model_math.h instance_view from the world pack's header and the instance, nm_bounds_quick_classify of the mesh's
 * bounds with its matrix and focus, the planes where that does not decide; an invalid view draws nothing). 1: not a
 * face of it is drawn, so nothing of it need be lit. */
static int practice_world_instance_rejected(const nm_practice_scene_t *scene,const nm_instance_t *inst) {
    const nm_header_t *header=scene->world.header;
    const nm_view_t view={
        {(float)(header->crtc[0]+inst->center[0]),(float)(384-inst->center[1]+header->crtc[1])},
        {nm_view_limit_low(inst->viewport[0]+header->render[0],0),nm_view_limit_high(inst->viewport[2]+header->render[0]+1,496),
         nm_view_limit_low(384-inst->viewport[3]+header->render[1],0),nm_view_limit_high(384-inst->viewport[1]+header->render[1]+1,384)},0.01f};
    if(!nm_view_valid(&view)) return 1;
    const float *bounds=scene->world.meshes[inst->mesh].bounds;
    int kind=nm_bounds_quick_classify(&view,bounds,inst->matrix,inst->focus);
    if(kind==3) {
        nm_bounds_planes_t planes;
        nm_bounds_planes_prepare(&planes,&view,inst->matrix,inst->focus);
        kind=nm_bounds_planes_classify(&planes,bounds);
    }
    return kind==0;
}
/* A PLACED group of the world actors (include/expert_actors.h): its instance drawn with the view times the
 * placement of its actor's tick, and its lumas for that placement (model_renderer.c) unless the renderer rejects
 * the instance (the lumas of a rejected instance are never read: lit when it comes into view). */
static __attribute__((noinline)) int practice_clock_place(nm_practice_scene_t *scene,const nm_instance_t *source,
        unsigned group,const nm_xa_draw_t *draw) {
    if(!scene->world.meshes[source->mesh].face_count) return 1;
    if(scene->world_header.instances>=scene->scenery.instances+scene->world_object_count) return 0;
    nm_instance_t *out=scene->world_instances+scene->world_header.instances++;
    shz_memcpy4(out,&scene->instance,sizeof(*out));
    out->mesh=source->mesh; out->source_address=source->source_address; out->padding[0]=source->padding[0];
    float model[12];
    nm_xa_pose_matrix(draw,model);
    if(!nm_pose_compose(scene->view,model,out->matrix)) return 0;
    if(practice_world_instance_rejected(scene,out)) return 1;
    nm_render_world_clock_light(group,model,out->matrix,scene->view);
    return 1;
}
/* The frame-counter animations (include/clock_actors.h): each group's condition and frame from this tick's values as
 * its handler reads them (clock_scene.h), or, for an actor's group, from the actor's last tick (expert_actors.h,
 * code/props_scene.c), the frame written into the world pack's tables and its instance placed. */
static __attribute__((noinline)) int practice_clocks(nm_practice_scene_t *scene,const uint32_t bitmaps[16]) {
    const nm_clock_group_t *const groups=nm_clock_groups(scene->clock_data);
    nm_clock_input_t in;
    /* The podium's entry tick (03b84 arms the graph; its cars spawn on the next): the original's actors draw nothing.
     * The race scene after its last tick otherwise draws on: the results' ticks. */
    if(scene->podium.phase && scene->podium.entered) return 1;
    if(!nm_clock_scene_input(&in,&scene->race,&scene->intro,scene->attract_active,
            scene->podium.phase ? 0u : scene->results.ticks,bitmaps)) return 1;
    const nm_xa_draw_t *const draws=nm_props_world_actor_draws(scene->attract_active);
#ifdef NM_DIAG_FRAME_TRACE
    NM_FT_BEGIN(ft_clocks);
#endif
    for(unsigned i=0;i<scene->clock_count;++i) {
        const nm_clock_group_t *const g=groups+i;
        const nm_xa_draw_t *const draw=g->cond==NM_CLOCK_ACTOR && draws ? draws+i : NULL;
        const int frame=g->cond==NM_CLOCK_ACTOR ? (draw ? draw->frame : -1) : nm_clock_frame(g,&in);
        if(frame<0 || frame>=(int)g->frames) continue;
        const nm_instance_t *source=scene->world_sources+scene->clock_source[i];
        nm_render_world_clock_frame(i,(unsigned)frame);
        if(g->flags&NM_CLOCK_GROUP_PLACED ? !practice_clock_place(scene,source,i,draw) :
           !practice_object_place(scene,source,source->mesh,NM_OBJECT_CELL(source->padding[0]))) return 0;
    }
#ifdef NM_DIAG_FRAME_TRACE
    NM_FT_END(NM_FT_S_CLOCKS,ft_clocks);
#endif
    return 1;
}
/* The reels (kinds NM_OBJECT_SLOT..+2) among this frame's world object instances, to the glass. */
static int practice_slot_glass(const nm_practice_scene_t *scene) {
    const nm_instance_t *reels[NM_OBJECT_SLOTS];
    unsigned count=0;
    for(unsigned i=scene->world_header.instances;i-- && count<NM_OBJECT_SLOTS;) {   /* objects follow the cells */
        const uint32_t word=scene->world_instances[i].padding[0];
        const unsigned kind=NM_OBJECT_KIND(word);
        if(!(word&NM_OBJECT_INSTANCE)) break;   /* the cells' instances */
        if(kind>=NM_OBJECT_SLOT && kind<NM_OBJECT_SLOT+NM_OBJECT_SLOTS)
            reels[count++]=scene->world_instances+i;
    }
    return nm_slot_glass_enqueue(scene->world.header,reels,count);
}
#ifdef NM_DIAG_FRAME_TRACE
/* The opponents' levels of this frame (include/frame_trace.h): after nm_practice_render_parts. */
static void practice_ft_cars(const nm_practice_scene_t *scene) {
    nm_ft_car_count=0;
    for(unsigned ordinal=0;ordinal<scene->intro.count && nm_ft_car_count<NM_FT_CARS;++ordinal) {
        const unsigned i=scene->intro.detail_order[ordinal],level=scene->intro.detail_level[i];
        if(!level) continue;
        const nm_instance_t *body=scene->car_instances+scene->car_part_boundaries[ordinal];
        const float rows=(480.0f/384.0f)*1.28f*body->focus[1]/body->matrix[11];
        nm_ft_car_t *c=nm_ft_cars+nm_ft_car_count++;
        c->first=(uint16_t)scene->car_part_boundaries[ordinal];c->level=(uint8_t)level;
        c->drawn=(uint8_t)practice_drawn_level(scene,i,level,body);
        c->rows=(uint8_t)(rows<0.0f ? 255.0f : rows>254.0f ? 254.0f : rows);
    }
}
#endif
int nm_practice_render_scenery(nm_practice_scene_t *scene) {
    nm_scenery_cells_t cells;
    cells=scene->intro.cells;
    if(scene->inspect_paused) cells=scene->inspect_cells;
    /* Every selected scenery instance uses this same camera matrix. */
    shz_memcpy4(scene->instance.matrix,scene->view,sizeof(scene->view));
    scene->world_header.instances=0;
    if(!scene->world_sources) return 1; /* No world pack (a course without one, practice_assets.c). */
    for (unsigned c=0;c<cells.draw_count;++c) {
        const nm_scenery_cell_t *cell=scene->scenery.cells+cells.draw[c];
        for (unsigned j=0;j<cell->count;++j) {
            if (scene->world_header.instances>=scene->scenery.instances+scene->world_object_count) return 0;
            const nm_instance_t *source=scene->world_sources+cell->first+j;
            if(!scene->world.meshes[source->mesh].face_count) continue;
            nm_instance_t *out=scene->world_instances+scene->world_header.instances++;
            shz_memcpy4(out,&scene->instance,sizeof(*out));
            out->mesh=source->mesh; out->source_address=source->source_address;
        }
    }
    /* World objects: each actor of the original tests the bit of its own
     * cell in the draw bitmap 501520 or the near bitmap 501540 and then picks
     * its model. The actors run before the scenery object rewrites the
     * bitmaps, so they see the cells of the pass before (reference display
     * lists, every tick: 3,113 of 3,114 race ticks and 1,800 of 1,800 attract
     * ticks agree with the previous cells; 40 and 12 differ with these). */
    /* While the pit crew works (D+128..S+511) dd9c suspends the actor creator and all 64 world-actor rows: none
     * of them runs or draws (race_field.h nm_race_shared_t.pit; code/practice.c freezes their counters). */
    if(scene->world_object_count && !(!scene->attract_active && scene->race.count && scene->race.shared.pit.suspended)) {
        uint32_t bitmaps[16];
        unsigned variant[NM_OBJECT_KINDS];
        const nm_scenery_cells_t *seen=scene->inspect_paused ? &scene->inspect_cells : &scene->intro.cells_previous;
        if(!nm_scenery_bitmaps(seen,bitmaps) || !practice_object_variants(scene,variant)) return 0;
        /* Selection runs (meshpack.h): one bitmap test per run; the wanted
         * variant is tried at its dense position, then looked for. */
        const nm_instance_t *const sources=scene->world_sources+scene->world_object_first;
        const unsigned count=scene->world_object_count;
        for(unsigned j=0;j<count;) {
            const uint32_t word=sources[j].padding[0],run=NM_OBJECT_RUN(word);
            unsigned end=j+1;
            while(end<count && NM_OBJECT_RUN(sources[end].padding[0])==run) ++end;
            const unsigned cell=NM_OBJECT_CELL(word),kind=NM_OBJECT_KIND(word);
            if(kind==NM_OBJECT_CONE) {
                if(!practice_cones(scene,sources,j,end,(int)(bitmaps[cell>>5]>>(cell&31)&1u))) return 0;
            } else if(kind<NM_OBJECT_KINDS && bitmaps[(word&NM_OBJECT_NEAR ? 8u : 0u)+(cell>>5)]>>(cell&31)&1u) {
                const unsigned want=practice_object_value(scene,variant,kind,cell),first=NM_OBJECT_VARIANT(word);
                unsigned k=want>=first && j+(want-first)<end ? j+(want-first) : j;
                if(NM_OBJECT_VARIANT(sources[k].padding[0])!=want) k=j;
                if(word&NM_OBJECT_MESHES) {
                    /* One instance for a run of meshes: the value picks the mesh. */
                    if(want<NM_OBJECT_MESH_COUNT(word) && !practice_object_place(scene,sources+j,sources[j].mesh+want,cell))
                        return 0;
                } else for(;k<end;++k) {
                    const uint32_t chosen=sources[k].padding[0];
                    if(NM_OBJECT_VARIANT(chosen)!=want) { if(NM_OBJECT_VARIANT(chosen)>want) break; continue; }
                    if(!practice_object_place(scene,sources+k,sources[k].mesh,cell)) return 0;
                }
            }
            j=end;
        }
        if(scene->clock_data && !practice_clocks(scene,bitmaps)) return 0;
    }
    return 1;
}
int nm_practice_render_check_transform(const float xyz[3], const float m[12], float w) {
    const shz_vec4_t p=shz_xmtrx_transform_vec4(shz_vec4_init(xyz[0],xyz[1],xyz[2],w));
    const float actual[3]={p.x,p.y,p.z};
    for (unsigned a=0; a<3; ++a) {
        const float expected=xyz[0]*m[a]+xyz[1]*m[3+a]+xyz[2]*m[6+a]+w*m[9+a];
        const float magnitude=fabsf(xyz[0]*m[a])+fabsf(xyz[1]*m[3+a])+fabsf(xyz[2]*m[6+a])+fabsf(w*m[9+a]);
        if (!isfinite(actual[a]) || !isfinite(expected) || !isfinite(magnitude) ||
            fabsf(actual[a]-expected)>8*FLT_EPSILON*magnitude+FLT_MIN) {
            printf("FAIL practice FTRV: xyz=%g,%g,%g w=%g axis=%u actual=%g expected=%g magnitude=%g bound=%g\n",
                (double)xyz[0],(double)xyz[1],(double)xyz[2],(double)w,a,
                (double)actual[a],(double)expected,(double)magnitude,(double)(8*FLT_EPSILON*magnitude+FLT_MIN));
            return 0;
        }
    }
    return p.w==w; // Positions retain 1; directions retain 0 (no translation).
}
static void practice_labels(void *data) {
    nm_practice_scene_t *scene=data;
#if !defined(NATIVE_HUD_DEBUG_TEXT) && !defined(NATIVE_DEBUG_LABELS)
    /* The developer labels (inspector keys, SH4/GPU times, attract stage) only while the inspector is open
     * (NATIVE_DEBUG_LABELS=1: whenever the HUD is not drawn, as before). */
    if(!scene->inspect_paused) return;
#endif
    char line[100]; enj_font_scale_set(1);
    if(scene->inspect_paused) {
        enj_qfont_write("Stick: look | triggers: fly | D-pad: strafe/up/down",20,308,PVR_LIST_PT_POLY);
        enj_qfont_write("A: fast | B: reset camera | resident assets only",20,330,PVR_LIST_PT_POLY);
    }
    enj_qfont_write(scene->inspect_paused ? "X: resume | simulation frozen" :
        "X: freeze simulation",20,352,PVR_LIST_PT_POLY);
    enj_qfont_write(nm_render_inspect_strips() ?
        "Y: materials | STRIPS: solid geometry, not a benchmark" :
        "Y: inspect submitted strips",20,376,PVR_LIST_PT_POLY);
    if(scene->intervals>=16) {
        /* SH4 = updater + list submission, no vblank wait; budget is 16.7 ms.
         * GPU render overlaps it. Late = vblanks without a page flip. */
        snprintf(line,sizeof(line),"SH4 %u.%u of 16.7 ms | GPU %u.%u | late %u",
            scene->work_tenths_ms/10,scene->work_tenths_ms%10,
            scene->render_tenths_ms/10,scene->render_tenths_ms%10,scene->late_frames);
        enj_qfont_write(line,20,400,PVR_LIST_PT_POLY);
    }
    if(scene->attract_active) {
        enj_qfont_write("Daytona cinematic attract",20,20,PVR_LIST_PT_POLY);
        snprintf(line,sizeof(line),"Stage %lu / %lu cars / tick %lu",(unsigned long)scene->intro.stage,
            (unsigned long)scene->intro.count,(unsigned long)scene->intro.ticks);
        enj_qfont_write(line,20,40,PVR_LIST_PT_POLY);
        enj_qfont_write("START/A: race / START+A+B+X+Y exits",20,60,PVR_LIST_PT_POLY);
        return;
    }
    enj_qfont_write("Daytona 40-car race / controller",20,20,PVR_LIST_PT_POLY);
    snprintf(line,sizeof(line),"Phase %u / tick %lu / time %lu",scene->race.phase,
        (unsigned long)scene->race.ticks,(unsigned long)scene->race.award.remaining);
    enj_qfont_write(line,20,40,PVR_LIST_PT_POLY);
    enj_qfont_write("Stick: steer  R: gas  L: brake  B: restart",20,60,PVR_LIST_PT_POLY);
    enj_qfont_write("Partial race / START+A+B+X+Y exits",20,80,PVR_LIST_PT_POLY);
    return;
}
#ifdef NM_DIAG_FILL_PROBE
/* make NATIVE_DIAG_FILL_PROBE=pt|tr|presort|add (measurement of the PVR's
 * fill price before any effect exists; not an effect): while the race runs,
 * two layers of one 256x256-pixel quad (64 whole tiles, x 192..448,
 * y 224..480: 131,072 pixel samples, what the arcade's tyre smoke covers in
 * an average frame that has smoke) in front of the scene, one texel a pixel
 * from an 8x8 ARGB1555 pattern with a quarter of its texels solid, the nearer
 * layer's pattern one texel aside:
 *   pt (1)       punch-through list, as the arcade's screen-door smoke
 *   tr (2)       translucent list, half transparent, hardware auto-sort
 *   presort (3)  translucent list in pre-sort mode (code/main.c), submitted
 *                back to front
 *   add (4)      pre-sort, untextured, additive (ONE, ONE): the plainest
 *                blended pixel */
static pvr_ptr_t practice_probe_texture;
alignas(32) static pvr_poly_hdr_t practice_probe_header;
static int practice_probe_prepare(void) {
    if(practice_probe_texture) return 1;
    practice_probe_texture=pvr_mem_malloc(8*8*sizeof(uint16_t));
    if(!practice_probe_texture) return 0;
    /* Twiddled order: index bits 0 and 1 are the low bits of y and x. */
    uint16_t *texels=(uint16_t *)practice_probe_texture;
    for(unsigned i=0;i<64;++i) texels[i]=(i&3u) ? 0x0000u : 0xffffu;
    const pvr_list_t list=NM_DIAG_FILL_PROBE==1 ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY;
    pvr_poly_cxt_t context;
    if(NM_DIAG_FILL_PROBE==4) {
        pvr_poly_cxt_col(&context,list);
        context.blend.src=PVR_BLEND_ONE;context.blend.dst=PVR_BLEND_ONE;
    } else {
        pvr_poly_cxt_txr(&context,list,PVR_TXRFMT_ARGB1555,8,8,practice_probe_texture,PVR_FILTER_NONE);
        if(NM_DIAG_FILL_PROBE!=1) context.txr.env=PVR_TXRENV_MODULATEALPHA; /* vertex alpha one half */
    }
    context.gen.culling=PVR_CULLING_NONE;
    context.depth.comparison=PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&practice_probe_header,&context);
    nm_memset((char *)&practice_probe_header+16,0,16);
    printf("DIAG fill probe %d: 2 layers of a 256x256 quad, 131072 pixel samples a race frame, list %d\n",NM_DIAG_FILL_PROBE,(int)list);
    return 1;
}
static void practice_probe(void *unused) {
    (void)unused;
    pvr_poly_hdr_t *header=pvr_dr_target();*header=practice_probe_header;pvr_dr_commit(header);
    for(unsigned layer=0;layer<2;++layer) for(unsigned corner=0;corner<4;++corner) { /* far layer first */
        const float x=corner>=2 ? 448.0f : 192.0f,y=corner&1 ? 224.0f : 480.0f;
        pvr_vertex_t *v=pvr_dr_target();
        v->flags=corner==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x=x*ENJ_XSCALE;v->y=y;v->z=layer ? 0.9f : 0.8f;
        v->u=(x+(float)layer)*0.125f;v->v=y*0.125f;
        v->argb=NM_DIAG_FILL_PROBE==4 ? 0xff101010u : 0x80ffffffu;v->oargb=0;
        pvr_dr_commit(v);
    }
}
#endif
#define PRACTICE_INSPECT_CHORD(c) (((c)->button.START&1) && (c)->button.B==ENJ_BUTTON_DOWN_THIS_FRAME)
static int practice_prepare(nm_practice_scene_t *scene) {
    const enj_ctrlr_state_t *controller=NULL;
    enj_ctrlr_state_t **controllers=enj_ctrl_get_states();
    for(size_t i=0;i<enj_ctrl_states_length();++i) if(controllers[i]) {
        controller=controllers[i];
        /* START held + B pressed toggles the inspector (A/X shift, Y and the D-pad pick the view, B alone resets
         * the session: code/practice.c). */
        if(PRACTICE_INSPECT_CHORD(controllers[i])) scene->inspect_paused=!scene->inspect_paused;
        /* Y steps the race view (code/practice.c) unless the inspector is paused. */
        if(scene->inspect_paused && controllers[i]->button.Y==ENJ_BUTTON_DOWN_THIS_FRAME)
            nm_render_inspect_set_strips(!nm_render_inspect_strips());
        break;
    }
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
    const uint64_t render_started=timer_ns_gettime64()/1000;
#endif
    if (!nm_practice_render_pose(scene)) {
        return 0;
    }
    if(scene->inspect_paused) {
        const uint64_t now=timer_ns_gettime64()/1000;
        if(!scene->inspect_camera_active ||
            (controller && controller->button.B==ENJ_BUTTON_DOWN_THIS_FRAME && !(controller->button.START&1))) {
            nm_inspect_camera_reset(&scene->inspect_camera,scene->view);
            scene->inspect_last_us=now;
        }
        scene->inspect_camera_active=1;
        const float dt=(float)(now-scene->inspect_last_us)*0.000001f;
        scene->inspect_last_us=now;
        shz_vec3_t movement=shz_vec3_init(0,0,0);
        float yaw=0,pitch=0;
        if(controller) {
            const float speed=(controller->button.A&1) ? 80.0f : 20.0f;
            yaw=controller->joyx>16 || controller->joyx<-16 ? controller->joyx*(2.0f/128.0f) : 0;
            pitch=controller->joyy>16 || controller->joyy<-16 ? controller->joyy*(2.0f/128.0f) : 0;
            movement=shz_vec3_init(
                ((int)(controller->button.RIGHT&1)-(int)(controller->button.LEFT&1))*speed,
                ((int)(controller->button.UP&1)-(int)(controller->button.DOWN&1))*speed,
                ((int)controller->rtrigger-(int)controller->ltrigger)*(1.0f/255.0f)*speed);
        }
        nm_inspect_camera_step(&scene->inspect_camera,dt,yaw,pitch,movement,scene->view);
        const shz_vec3_t position=nm_inspect_camera_position(scene->view);
        uint16_t cell;
        /* Arbitrary pitch/position: do not reuse the player's profile masks
         * or horizontal wedge. Existing bounded 5x5 neighbourhood, no uploads. */
        if(!nm_contact_scenery_cell(position.x,position.z,&cell) ||
            !nm_scenery_cells(scene->scenery.orders[0],cell,0,0x1ffffffu,&scene->inspect_cells)) return 0;
    } else scene->inspect_camera_active=0;
    /* The road class's depth scale of this frame's camera: before the shadow prisms and the world's lists. */
    practice_road_depth_pick(scene);
    /* Scenery by stage (render/scene/track_surface.h). The sky is four
     * opaque quads at NM_SKY_DEPTH behind everything the renderer accepts. */
#ifdef NM_DIAG_FRAME_TRACE
    uint32_t ft_at=nm_ft_clock();
#define PRACTICE_FT_STEP(call,group) ({ const int ft_ok=(call);NM_FT_MARK(group,ft_at);ft_at=nm_ft_clock();ft_ok; })
#else
#define PRACTICE_FT_STEP(call,group) (call)
#endif
#ifdef NM_DIAG_FRAME_TRACE
#define PRACTICE_FT_CARS() ({ practice_ft_cars(scene);1; })
#else
#define PRACTICE_FT_CARS() 1
#endif
    const int setup_failed=!nm_practice_render_projection(scene,1) ||
        !nm_practice_render_lighting(scene) || !PRACTICE_FT_STEP(nm_practice_render_parts(scene),NM_FT_S_PARTS) || !PRACTICE_FT_CARS()
        || !PRACTICE_FT_STEP(nm_practice_render_player_car(scene),NM_FT_S_PLAYER)
        || !PRACTICE_FT_STEP(nm_practice_render_shadows(scene),NM_FT_S_SHADOWS)
        || !PRACTICE_FT_STEP(nm_practice_render_scenery(scene),NM_FT_S_SCENERY)
        ;
    nm_practice_render_projection(scene,0); /* Instances own their copies; 2D actors keep theirs. */
#ifdef NM_DIAG_NO_CARS
    /* make NATIVE_DIAG_NO_CARS=1 (measurement of the PVR render time): no car
     * model reaches the renderer. The shadow instances were taken out above
     * and still become volumes. Gameplay's own detail pass (code/practice.c,
     * the drawn-car count of the original's 6b68) does not read this list. */
    scene->intro_header.instances=0;
    for(unsigned i=0;i<=scene->intro.count;++i) scene->car_part_boundaries[i]=0;
#endif
#ifdef NM_DIAG_NO_WORLD
    /* make NATIVE_DIAG_NO_WORLD=1 (measurement): no world instance either. */
    scene->world_header.instances=0;
#endif
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
    const uint64_t setup_done=timer_ns_gettime64()/1000;
#endif
    /* The slot machine's glass over the reels drawn this frame (slot_glass.h): on the translucent list before
     * the cars' effects. */
    if(!setup_failed && !practice_slot_glass(scene)) return 0;
    unsigned player=scene->intro.count; /* Cinematic cars are all opponents. */
    if(!scene->attract_active) for(unsigned i=0;i<scene->intro.count;++i)
        if((scene->inspect_paused ? scene->inspect_order[i] : scene->intro.detail_order[i])==0) { player=i; break; }
    if (setup_failed || !nm_render_enqueue_objects(
            scene->car_part_boundaries,scene->intro.count
            ,player)) {
        return 0;
    }
#if NM_SCENERY_SKY && !defined(NM_DIAG_NO_SKY) /* make NATIVE_DIAG_NO_SKY=1 (measurement): flat background, code/main.c */
    if(scene->world_sources && !nm_sky_enqueue((uint16_t)scene->camera_heading,scene->intro.sky_scroll)) return 0;
#endif
#ifndef NM_DIAG_NO_SHADOW_VOLUMES
    if(scene->shadow_count) enj_render_list_add(PVR_LIST_OP_MOD,practice_shadow_volumes,scene);
#else /* make NATIVE_DIAG_NO_SHADOWS=1|volumes (measurement): no volume is submitted */
    (void)practice_shadow_volumes;
#endif
    if(!scene->attract_active && scene->race.finish_branch &&
       !nm_finish_render_enqueue(&scene->race.finish_actor,scene->race.finish_branch,scene->instance.center)) {
        return 0;
    }

#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
    scene->scene_setup_us+=setup_done-render_started;
    scene->render_prepare_us+=timer_ns_gettime64()/1000-setup_done;
#endif
    /* HUD: the arcade HUD replaces the debug text while the race runs (phases
     * 20..24). The text stays in inspect/pause mode (X), in every other scene,
     * without HUD assets, and beside the HUD with NATIVE_HUD_DEBUG_TEXT=1. */
#ifdef NM_DIAG_FILL_PROBE
    if(!scene->attract_active && !scene->inspect_paused && scene->race.active_phase>=20 &&
       scene->race.active_phase<=24 && practice_probe_prepare())
        enj_render_list_add(NM_DIAG_FILL_PROBE==1 ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY,practice_probe,NULL);
#endif
    const int hud=!scene->attract_active && !scene->inspect_paused &&
        scene->race.active_phase>=20 && scene->race.active_phase<=24 &&
#ifdef NM_DIAG_NO_HUD
        1; /* make NATIVE_DIAG_NO_HUD=1 (measurement): no HUD, and no debug text where it would be. */
#else
        /* By depth, unless this frame holds an overlay drawn without a depth
         * test after the 3D picture (the finish actor, the debug text): then flat. */
#ifdef NATIVE_HUD_DEBUG_TEXT
        nm_hud_render_enqueue(1);
#else
        nm_hud_render_enqueue(scene->race.finish_branch!=0);
#endif
#endif
#ifndef NATIVE_FRAME_COMPARE
#ifndef NATIVE_HUD_DEBUG_TEXT
    if(!hud)
#else
    (void)hud;
#endif
    enj_render_list_add(PVR_LIST_PT_POLY,practice_labels,scene);
#endif
    return 1;
}
#ifdef NM_DIAG_PAL1555
/* make NATIVE_DIAG_PAL1555=1 (measurement of the PVR render time): palette
 * RAM in ARGB1555. Every loader of this program writes ARGB8888 entries and
 * sets that format first (pvr_set_pal_format, also inside enDjinn's palette
 * loaders) and several read their entries back, so the loaders stay as they
 * are: before a frame is queued, if the format register is not ARGB1555, each
 * entry that is not a value written here is taken as a fresh ARGB8888 colour
 * and rewritten (alpha bit = alpha of 128 or more, five bits a channel).
 * Coverage of punch-through texels is kept; colours lose their low bits. */
static void practice_diag_palette(void) {
    static uint16_t written[1024];
    static uint8_t valid[1024];
    if((PVR_GET(PVR_PALETTE_CFG)&3u)==PVR_PAL_ARGB1555) return;
    unsigned changed=0;
    for(unsigned i=0;i<1024;++i) {
        const uint32_t colour=PVR_GET(PVR_PALETTE_TABLE_BASE+4*i);
        if(valid[i] && colour==written[i]) continue;
        const uint32_t packed=((colour>>31)<<15)|(((colour>>19)&31u)<<10)|(((colour>>11)&31u)<<5)|((colour>>3)&31u);
        PVR_SET(PVR_PALETTE_TABLE_BASE+4*i,packed);
        written[i]=(uint16_t)packed;valid[i]=1;++changed;
    }
    pvr_set_pal_format(PVR_PAL_ARGB1555);
    printf("DIAG palette RAM: ARGB1555, %u entries converted from ARGB8888\n",changed);
}
#endif
int nm_practice_render_enqueue(nm_practice_scene_t *scene) {
    if(!scene || scene->failed) return 0;
#ifdef NM_DIAG_PAL1555
    practice_diag_palette();
#endif
    /* No renderer far limit: the world is whatever the arcade's visible-cell
     * selection lists, and cars follow the detail list out to view z 256. */
    /* Build instance poses and cull bounds before enDjinn's wait. Resident
     * objects defer face lighting/transforms until their own list callbacks. */
    return practice_prepare(scene);
}
