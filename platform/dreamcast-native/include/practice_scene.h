#ifndef DAYTONA_NATIVE_PRACTICE_SCENE_H
#define DAYTONA_NATIVE_PRACTICE_SCENE_H
#include <enDjinn/enj_enDjinn.h>
#include <malloc.h>
#include <stddef.h>
#include "practice.h"
#include "gameplay/results.h"
#include "race_scene.h"
#include "intro_scene.h"
#include "podium.h"
#include "player_parts.h"
#include "pit_crew.h"
#include "car_effects.h"
#include "meshpack.h"
#include "clock_actors.h"
#include "render/scene/damage_bodies.h"
#include "geometry.h"
#include "visibility.h"
#include "render/scene/inspect_camera.h"
#include "shadow_volume.h"
/* Internal scene storage, owned by the game mode. Render callbacks borrow it
 * until enDjinn finishes this frame; never reset/free it while callbacks run.
 * state/race own simulation. model/instances/view/headers are presentation.
 * No collision query depends on the render meshes or transformed vertices. */
typedef struct nm_practice_scene {
    nm_drive_params_t *math;
    nm_contact_pack_t contact;
    nm_road_pack_t road;
    nm_path_pack_t path;
    nm_road_marks_t marks;
    nm_practice_t state;
    nm_results_t results;
    const char *asset_root;
    uint32_t name_notification;
    nm_race_record_t *name_record;
    nm_race_scene_t race;
    unsigned race_commands;
    uint32_t race_command_hash;
    uint64_t race_bodies; /* Submission coverage by car ID, not a visibility mask. */
    unsigned controller_samples;
    nm_controls_t controller_last;
    uint8_t start_pending; /* START DOWN_THIS_FRAME not yet consumed by a tick (53e584 bit4). */
    uint8_t transmission;  /* 501464 of this race: 0 automatic, 1 manual (code/practice.c). */
    /* 501460 of the loaded course packs and of the scene's world (0 Beginner, 1 Expert, 2 Advanced; the attract's
     * is 0), and whether they are loaded (code/practice.c practice_course_load). */
    uint8_t course, course_loaded;
    int8_t shift_pending;  /* Manual: the pad's shift request (+1 up, -1 down) not yet consumed by a race tick. */
    unsigned start_edges;  /* START edges fed to the race's, podium's and results' ticks this session (props_scene.c). */
    nm_intro_scene_t intro;
    nm_podium_t podium; /* Nonzero phase while phase27/28 owns the updater. */
    nm_path_pack_t intro_paths[8];
    nm_header_t intro_header;
    unsigned car_part_boundaries[45]; /* 44 cars plus end sentinel. */
    nm_vertex_t *intro_windows[2];
    uint16_t intro_window_uv[2][72];
    unsigned intro_window_count[2];
    uint32_t intro_special_parts,intro_special_phases;
    uint32_t intro_shadow_parts;
    uint32_t intro_crash_parts;
    uint32_t intro_special_body,intro_special_windows;
    alignas(32) pvr_poly_hdr_t header;
    float scale, center[2];
    uint64_t last_us, update_max_us;
    nm_frame_clock_t frame_clock; /* one tick per displayed frame (frame_clock.h) */
    uint64_t interval_sum_us, interval_max_us;
    uint64_t frame_window_us;
    unsigned frame_tenths_ms;
    /* SH4 work per frame: this updater plus list submission, WITHOUT the wait
     * for the PVR between them. GPU render time overlaps it; never add them.
     * late_frames counts vblanks that passed without a page flip. */
    uint64_t work_last_us,work_window_us,render_window_us;
    unsigned work_tenths_ms,render_tenths_ms,late_frames;
    size_t late_flip,late_vblank;
#if defined(NATIVE_PROFILE) || defined(NATIVE_FRAME_PROFILE)
    uint64_t simulation_us, scene_setup_us, render_prepare_us;
    unsigned update_samples;
#endif
    unsigned intervals;
    uint64_t pvr_register_sum, pvr_render_sum, pvr_register_max, pvr_render_max;
    unsigned pvr_samples;
    pvr_stats_t sampled_stats;
    struct mallinfo sampled_memory;
    unsigned sampled_vram;
    uintptr_t sampled_heap_headroom;
    int failed;
    int attract_active;
    int inspect_paused;
    int inspect_camera_active;
    uint64_t inspect_last_us;
    nm_inspect_camera_t inspect_camera;
    nm_scenery_cells_t inspect_cells;
    int16_t inspect_depth[44];
    uint8_t inspect_order[44],inspect_level[44];
    nm_pack_t model;
    nm_cached_vertex_t *points;
    alignas(32) float view[12];
    alignas(32) float body_view[12];
    int16_t camera_heading;
    unsigned triangles;
    alignas(32) nm_instance_t instance; // Live body/view, borrowed by the existing renderer.
    const nm_instance_t *car_sources;
    alignas(32) nm_instance_t car_instances[NM_INTRO_MAX_PARTS+NM_PLAYER_PARTS+NM_CAR_EFFECTS_MAX+NM_SPIN_SMOKE_PARTICLES+
        NM_PIT_CREW_DRAWS_MAX];
    /* Race player's own fixed part list (player_parts.h): car-pack bindings,
     * UINT32_MAX when an older pack lacks the part; authored wheel offsets. */
    uint32_t player_bindings[NM_PLAYER_PARTS];
    /* The body slot: the damaged bodies (car-damage.dnb) as variants written into the clean body's mesh
     * player_body_mesh, player_body_variant the one it holds (NM_DAMAGE_BODY_CLEAN after a load); the hoods
     * nm_player_hood_models as bindings, UINT32_MAX when the pack lacks them. */
    nm_damage_bodies_t damage_bodies;
    unsigned player_body_mesh,player_body_variant;
    uint32_t player_hood_bindings[4];
    /* The hood view's damaged hoods (car-hoods.dnb, damage_bodies.h) as variants written into the clean hood's mesh
     * player_hood_mesh (the manual car's in its own face order); player_hood_variant the one it holds. */
    nm_damage_bodies_t hood_variants;
    unsigned player_hood_mesh,player_hood_variant;
    /* The car effects (car_effects.h): car-pack binding of each table entry by kind (NM_FX_*), UINT16_MAX where the
     * pack lacks the model (older packs: those are not drawn); effects_drawn counts submitted contact effects (dust,
     * sparks), smoke_drawn the tyre smoke, spin_drawn the spin-smoke particles. spin_smoke: task 9e2c, stepped by
     * code/practice.c once per race tick, reset with the race scene. */
    uint16_t effect_bindings[NM_FX_KINDS][32];
    uint32_t effects_drawn, smoke_drawn, spin_drawn;
    nm_spin_smoke_t spin_smoke;
    /* The pit crew (pit_crew.h, race only): this tick's draws, and the car-pack binding of each of its models
     * found on first use (crew_models/crew_bindings, UINT16_MAX: the pack lacks it and it is not drawn; cleared
     * when the pack is bound again); crew_drawn counts the submitted crew instances. */
    alignas(32) nm_pit_crew_draw_t crew_draws[NM_PIT_CREW_DRAWS_MAX];
    uint32_t crew_models[48];
    uint16_t crew_bindings[48];
    unsigned crew_model_count;
    uint32_t crew_draw_models[NM_PIT_CREW_DRAWS_MAX]; /* draw i's model and binding last frame (0: none) */
    uint16_t crew_draw_bindings[NM_PIT_CREW_DRAWS_MAX];
    /* The figures' bounds hints (render/scene/practice.c, practice_crew_hints): per model set the radius that holds
     * every draw of a figure around its root, found on first use (crew_set_reach_ready, cleared with the bindings),
     * and this frame's hint per draw (model_renderer.h NM_RENDER_BOUNDS_HINT, 0 none). */
    float crew_set_reach[NM_PIT_CREW_SETS];
    uint16_t crew_set_reach_ready;
    uint32_t crew_draw_hints[NM_PIT_CREW_DRAWS_MAX];
    nm_pit_crew_figure_t crew_figures[NM_PIT_CREW];
    uint32_t crew_hinted; /* crew instances with a hint, for the counts at shutdown */
    uint32_t crew_drawn;
    float player_wheel_offsets[4][3];
    const void *player_pack; /* Car pack data the bindings were resolved for. */
    /* Ground shadows as modifier volumes: authored footprint outlines
     * (0 opponent 8d9e42, 1 player 8d7ab7) and this frame's posed shadows. */
    const void *shadow_pack;
    unsigned shadow_outline_count[2],shadow_count;
    float shadow_outline[2][NM_SHADOW_OUTLINE_MAX][3];
    float shadow_focus,shadow_centre_y,shadow_up[3];
    uint8_t shadow_kind[NM_SHADOW_VOLUMES_MAX];
    alignas(32) float shadow_matrix[NM_SHADOW_VOLUMES_MAX][12];
    /* The car pack's window materials 6fe0/7008 rewrite in a tunnel (render/scene/practice.c
     * nm_practice_render_tunnel): [0] the shared windows (models 1c8, 241: list 503584), [1] the player's (13b);
     * UINT16_MAX when absent. */
    uint16_t window_materials[2];
    nm_vertex_t *window_vertices;
    uint16_t window_base_uv[72];
    unsigned window_vertex_count;
    nm_pack_t world;
    nm_scenery_pack_t scenery;
    nm_header_t world_header;
    const nm_instance_t *world_sources;
    nm_instance_t *world_instances;
    /* World objects of the pack (meshpack.h, NM_PACK_WORLD_OBJECTS): the
     * source instances behind those of the scenery cells. */
    unsigned world_object_first,world_object_count;
    /* X and Z of the original's camera entity 501240 (+1c, +24), which the
     * bearing actor 223b28 reads: see practice_object_variants. */
    float world_object_origin[2];
    /* The course's frame-counter animations (include/clock_actors.h, courseN-actors.dna; NULL without), and the
     * world source instance of each of its groups. */
    void *clock_data;
    unsigned clock_count;
    uint16_t clock_source[NM_CLOCK_GROUPS_MAX];
} nm_practice_scene_t;
_Static_assert(offsetof(nm_practice_scene_t,view)%32==0 &&
    offsetof(nm_practice_scene_t,body_view)%32==0 &&
    offsetof(nm_practice_scene_t,instance)%32==0 &&
    offsetof(nm_practice_scene_t,car_instances)%32==0,"live render bases must be cache-line aligned");
#endif
