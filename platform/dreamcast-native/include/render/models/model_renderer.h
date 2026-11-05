#ifndef DAYTONA_NATIVE_MODEL_RENDERER_H
#define DAYTONA_NATIVE_MODEL_RENDERER_H
#include "meshpack.h"

/* Optional SH4 hardware check, implemented only in the model diagnostic build. */
int nm_pack_check_math(const nm_pack_t *pack);
int nm_pack_check_projection(const nm_pack_t *pack);
int nm_pack_check_lighting(const nm_pack_t *pack);
int nm_pack_check_materials(const nm_pack_t *pack);
/* Model renderer, with optional diagnostic camera translation.
 * Packs, instances and textures must outlive queued object callbacks.
 * Release before enDjinn shuts PVR down, not after enj_state_run returns. */
int nm_render_init(const nm_pack_t *pack);
int nm_pack_check_scenery_lighting(const nm_pack_t *pack);
/* Attach scenery resources; visible instances queue their own OP/PT calls. */
int nm_render_world_attach(const nm_pack_t *pack);
/* Before nm_render_world_attach: the most world points (instance mesh vertices) and faces one frame can admit,
 * which size the world's point cache and face queues instead of every instance's (0, 0: every instance's).
 * code/render/scene/practice_assets.c computes it from the visibility pack: a view draws at most the 5x5
 * cells around its centre cell, and the world objects. */
void nm_render_world_capacity(unsigned points,unsigned faces);
/* Before nm_render_world_attach: whether the world gets the whole prebuilt world stream tables (NM_SQ_STREAM; default
 * 1). 0: the record cache (model_submission.h NM_SQ_STREAM_POOL; nm_render_world_stream_idle builds it): the meshes
 * without records take the queue path (the same words, more SH4 time); with NM_SQ_STREAM_POOL=0 every world batch. */
void nm_render_world_streams(int enabled);
/* Between two frames (code/audio/idle_wait.c, the idle wait for the PVR before the registration, never while a scene
 * load runs): a world without the whole stream tables builds the records of the meshes the last frame walked into its
 * record cache (model_submission.h NM_SQ_STREAM_POOL) while BUDGET_US holds a mesh's estimate. Returns the
 * microseconds spent. */
unsigned nm_render_world_stream_idle(unsigned budget_us);
/* After nm_render_init: the car pack's material MATERIAL draws from then on with the bindings of HEADER (its lumas
 * as before; the material record is not changed): the windows' tunnel texture (render/scene/practice.c,
 * nm_practice_render_tunnel), and back with the material's own header. 0: no renderer, or the material pack lacks
 * a binding of HEADER for a luma the material draws (nothing changed). A face's binding is looked up when it is
 * drawn (the window meshes are never in a prebuilt stream: their u,v are rewritten). */
int nm_render_car_material_header(unsigned material,const uint16_t header[4]);
/* Before nm_render_world_attach: the course's frame-counter animations (include/clock_actors.h, a DNA1 file the
 * caller validated against the world pack and keeps until nm_render_release), or NULL. 0: a world is attached. */
int nm_render_world_clocks(const void *data);
/* Group `group` of those animations draws frame `frame` this frame: its mesh's positions, normals, u, v and bindings
 * are rewritten (nothing when they hold that frame already). Call before the frame's world batches are walked
 * (render/scene/practice.c). */
void nm_render_world_clock_frame(unsigned group,unsigned frame);
/* A PLACED group (clock_actors.h) drawn with `placed` (view times model, its instance's matrix) this frame: its faces'
 * lumas for that placement (lighting.h's diffuse and specular terms of the race light, the model's normals carried by
 * `model`, the view-space z of `placed`), each face's binding of that luma. Call after nm_render_world_clock_frame. */
void nm_render_world_clock_light(unsigned group,const float model[12],const float placed[12],const float view[12]);
int nm_render_enqueue(void);
/* Borrowed car-part boundaries, count+1 entries. Player is its detail-order
 * ordinal (count means none); opponents share one callback per list. Player
 * precedes opponents; OP/PT use depth testing. Scenery remains per object. */
int nm_render_enqueue_objects(const unsigned *car_parts,unsigned count,unsigned player);
void nm_render_release(void);
/* Vertices of a loaded pack whose u,v the caller rewrites while the scene
 * runs: their faces take texture coordinates from the vertex table at draw
 * time. 0: too many ranges. */
int nm_render_uv_dynamic(const nm_vertex_t *vertices,unsigned count);
/* Before nm_render_init: the car light class cache holds at least `classes`
 * classes of one mesh (a mesh rewritten later can have more than the pack's). */
void nm_render_car_light_classes_reserve(unsigned classes);
/* After the caller rewrote the positions, supplied normals and bounds of car
 * mesh `mesh` of the initialised pack (same faces, materials and u,v; vertices
 * that share a position representative keep sharing it): the renderer's
 * tables of that mesh again. 0: the mesh does not fit them. */
int nm_render_car_mesh_refresh(unsigned mesh);
/* A car instance's bounds classification given by the scene (nm_instance_t padding[0]): NM_RENDER_BOUNDS_HINT
 * plus 0 (outside the view) or 2 (inside every plane). The scene sets it only where nm_bounds_planes_classify of
 * that instance's mesh bounds, matrix, focus and view would return the same (render/scene/practice.c, the pit
 * crew's figures); the renderer then skips its own test. Every other instance has 0 there. */
#define NM_RENDER_BOUNDS_HINT UINT32_C(0x4e480000)
/* Runtime geometry inspection; solid colours replace textures, not geometry. */
void nm_render_inspect_set_strips(int enabled);
int nm_render_inspect_strips(void);
/* Depth written for road-surface world polygons, relative to true 1/z. The
 * arcade sorts whole polygons, so a car is never cut by the road it stands
 * on; its wheels and sills do lie below the coarse road mesh (host measure on
 * the arcade's own matrices: up to 0.57 units in a race lap). With a depth
 * buffer a car pixel wins against the road only if the sink is less than
 * (1/scale-1) times the camera's height h above that road polygon; the lower
 * the scale, the farther geometry buried under the ground (rocks) shows
 * through it: at 0.5, one camera height, it pokes through far away. So the
 * scale is the camera's (nm_render_road_depth): in a race view
 * h/(h+NM_ROAD_SINK), h the eye height, VR1..VR4 0.636, 0.647, 0.755, 0.862
 * (render/scene/practice.c; hardware: 0.76 cleared VR3's cases), and
 * NM_ROAD_DEPTH_SCALE for every other camera (attract, podium, the keyed race
 * cameras, the inspector) and until a race view sets its own. Rails, walls
 * and everything standing up keep true depth and occlude cars normally; the
 * scale is uniform within a frame, so road polygons keep their order.
 * Anything else lying on the road (shadow volumes) must use the same scale.
 * make NATIVE_DIAG_ROAD_DEPTH=<factor> fixes it for every camera
 * (NM_ROAD_DEPTH_FIXED) in a measurement build. */
#ifndef NM_ROAD_DEPTH_SCALE
#define NM_ROAD_DEPTH_SCALE 0.5f
#endif
/* The sink a race view's scale leaves room for: the 0.57 measured, rounded up. */
#define NM_ROAD_SINK 0.6f
/* The road-surface depth scale from the next submission on, 0 < scale < 1 (another is ignored); returns the one in
 * effect. When it differs from the current one, the world's road classes and the prebuilt stream records of road
 * faces are rewritten (model_submission.h sq_road_depth_set; a whole-table course's ~15,000 records, a few
 * milliseconds: once per change, not per frame). Call between frames or in a frame's setup, never while its lists
 * are submitted. A world attached later is built at the current scale. Render only: no gameplay word depends on it. */
float nm_render_road_depth(float scale);
#endif
