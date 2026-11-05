"""Frame tables for the stream simulator from the game's own code (FRM5, sim_main.c): the tick loop of
tests/trace_race_native.c plus the frame assembly of code/render/scene/practice.c, sliced from the tree, write the
car and world instance tables of sampled ticks. No renderer, no PVR. Every table is ROM-derived: keep it under build/.
Needs cc and SH4ZAM's headers (extern/sh4zam, platform/dreamcast-native/scripts/fetch_sh4zam.sh: the scene struct holds its types).

usage: gen_frames.py OUT.frm ASSETS HEADERS [--course N] [--script NAME] [--step TICKS] [--last TICK]
           [--views TICK:VR,TICK:VR,...] [--attract TICKS] [--work DIR]

ASSETS   an asset set (recipes/build_assets.sh: assets-ssfar4f), HEADERS its generated headers (.../headers)
--course 0 Beginner (default), 1 Expert, 2 Advanced: the race script defaults to its full race
--script a directory of HEADERS with race_script_data.h: the race's controls, replayed as the harness's production
         mode replays a reference trace. The race runs through the podium; the last frames are its cinematic
--step   every Nth tick is a frame (default 20)    --last  no frame after this tick
--views  the view buttons (1..4: VR1..VR4) pressed for one tick at the ticks given: a race's scripts hold none
--attract TICKS: the attract's cinematic instead of a race, TICKS long

Each frame carries the road class's scale practice_road_depth_pick chose for it, sliced with the rest and run on every
tick as the scene runs it: the simulator sets it as the scene does, and tests/reference_stream.h expects it. Left out,
as in the reference: the damaged bodies (the clean body is drawn), the props' ticks (the cones stand at rest) and the
frame-counter animations (their groups are not placed).
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

here = Path(__file__).resolve().parent
root = Path(os.environ.get('SIM_ROOT', here.parents[1]))
parser = argparse.ArgumentParser()
parser.add_argument('out')
parser.add_argument('assets')
parser.add_argument('headers')
parser.add_argument('--course', type=int, default=0, choices=(0, 1, 2))
parser.add_argument('--script')
parser.add_argument('--step', type=int, default=20)
parser.add_argument('--last', type=int, default=-1)
parser.add_argument('--views', default='')
parser.add_argument('--attract', type=int, default=0)
parser.add_argument('--work')
args = parser.parse_args()
out = Path(args.out).resolve()
work = Path(args.work).resolve() if args.work else out.with_suffix('.gen')
work.mkdir(parents=True, exist_ok=True)
H = Path(args.headers)

trace = (root/'tests/trace_race_native.c').read_text()
render = (root/'code/render/scene/practice.c').read_text()
def cut(a, b):
    return render[render.index(a):render.index(b)]
# The frame's assembly: what practice_prepare calls before it submits anything.
slices = (cut('int nm_practice_render_pose(', 'int nm_practice_render_tunnel(') +
          cut('/* Render-only level of detail for distant opponents', '/* Ground shadows as PVR modifier volumes') +
          cut('#define PRACTICE_SHADOW_REACH', 'static inline void practice_shadow_triangle(') +
          cut('/* Selector values of the world objects', 'int nm_practice_render_check_transform('))
slices = slices.replace('static alignas(32) pvr_mod_hdr_t practice_shadow_headers[2]; /* Other, last triangle. */\n',
                        'static pvr_mod_hdr_t practice_shadow_headers[2];\n')
# The scene struct itself, without what only the console has.
scene_h = (root/'include/practice_scene.h').read_text()
for line in ('#include <enDjinn/enj_enDjinn.h>\n', '#include <malloc.h>\n'):
    assert line in scene_h
    scene_h = scene_h.replace(line, '')
(work/'practice_scene.h').write_text(scene_h)
shim = r'''
#include <stdalign.h>
#include <stdbool.h>
#include <math.h>
#include <limits.h>
#include "meshpack.h"
#include "geometry.h"
#include "lighting.h"
typedef struct { uint32_t word[8]; } pvr_poly_hdr_t;
typedef struct { uint32_t word; } pvr_stats_t;
struct mallinfo { int word; };
typedef struct { uint32_t w[8]; } pvr_mod_hdr_t;
#define pvr_mod_compile(a,b,c,d) ((void)0)
#define pvr_set_shadow_scale(a,b) ((void)0)
#define PVR_LIST_OP_MOD 1
#define PVR_MODIFIER_OTHER_POLY 0
#define PVR_MODIFIER_INCLUDE_LAST_POLY 1
#define PVR_CULLING_NONE 0
#include "practice_scene.h"
#include "props_scene.h"
#include "render/scene/track_surface.h"
#include "render/models/model_math.h"
static const float *host_matrix;
static shz_vec4_t host_transform(shz_vec4_t p) {
    const float *m=host_matrix; shz_vec4_t r;
    r.x=p.x*m[0]+p.y*m[3]+p.z*m[6]+p.w*m[9]; r.y=p.x*m[1]+p.y*m[4]+p.z*m[7]+p.w*m[10];
    r.z=p.x*m[2]+p.y*m[5]+p.z*m[8]+p.w*m[11]; r.w=p.w; return r;
}
#undef shz_xmtrx_transform_vec4
#define shz_xmtrx_transform_vec4 host_transform
static void nm_practice_render_matrix(const float m[12]) { host_matrix=m; }
int nm_render_uv_dynamic(const nm_vertex_t *v,unsigned n) { (void)v;(void)n;return 1; } /* renderer bookkeeping */
/* No damage bodies are loaded (damage_bodies.data NULL): the frames draw the clean body; SIM_DAMAGE_VARIANT checks them. */
int nm_render_car_mesh_refresh(unsigned mesh) { (void)mesh;return 0; }
/* The frame-counter animations write the world pack's tables: no clock_data here, the frames leave those groups out. */
void nm_render_world_clock_frame(unsigned group,unsigned frame) { (void)group;(void)frame; }
void nm_render_world_clock_light(unsigned group,const float model[12],const float placed[12],const float view[12]) {
    (void)group;(void)model;(void)placed;(void)view; }
float nm_render_road_depth(float scale) { return scale; }
#define nm_slot_glass_enqueue(header,reels,count) 1
'''
driver = r'''
static nm_practice_scene_t *fs; static FILE *frames_out; static long frame_step=20,frames_written,frame_last=-1;
static int frame_setup(const char *assets) {
    char file[1024]; fs=calloc(1,sizeof(*fs)); frames_out=fopen(getenv("FRAMES_OUT"),"wb");
    if(getenv("FRAME_STEP")) frame_step=strtol(getenv("FRAME_STEP"),NULL,10);
    if(getenv("FRAME_LAST")) frame_last=strtol(getenv("FRAME_LAST"),NULL,10);
    if(!fs || !frames_out) return 0;
    fs->math=math; fs->scenery=scenery;
    snprintf(file,sizeof file,"%s/car.dnm",assets); if(!nm_pack_load(&fs->model,file)) return 0;
    const unsigned parts=fs->model.header->instances;
    if(parts!=sizeof(nm_intro_model_ids)/sizeof(nm_intro_model_ids[0])) return 0;
    for(unsigned i=0;i<parts;++i) if(fs->model.instances[i].source_address!=nm_intro_model_ids[i]) return 0;
    for(unsigned w=0;w<2;++w) {
        const unsigned binding=nm_intro_window_models[w];
        const nm_mesh_t *window=fs->model.meshes+fs->model.instances[binding].mesh;
        fs->intro_windows[w]=(nm_vertex_t *)fs->model.vertices+window->vertex_first;
        fs->intro_window_count[w]=window->vertex_count;
        for(unsigned v=0;v<window->vertex_count;++v) {
            fs->intro_window_uv[w][2*v]=fs->intro_windows[w][v].u; fs->intro_window_uv[w][2*v+1]=fs->intro_windows[w][v].v; }
    }
    fs->instance=(nm_instance_t){.source_address=0x95ff9d,.focus={310,320},.viewport={0,0,495,384},.center={248,208}};
    fs->car_sources=fs->model.instances;
    fs->model.instances=fs->car_instances; fs->intro_header=*fs->model.header; fs->model.header=&fs->intro_header;
    nm_course_asset(file,sizeof file,assets,frame_course,NM_COURSE_WORLD,0); if(!nm_pack_load(&fs->world,file)) return 0;
    fs->world_header=*fs->world.header; fs->world_sources=fs->world.instances;
    fs->world_object_first=fs->scenery.instances; fs->world_object_count=fs->world.header->instances-fs->scenery.instances;
    fs->world_instances=calloc(fs->world.header->instances,sizeof(*fs->world_instances));
    fs->world.header=&fs->world_header; fs->world.instances=fs->world_instances;
    return 1;
}
static void frame_dump(long tick,int attract) {
    if(!fs) return;
    fs->attract_active=attract;
    const int podium=attract && (presentation.flags&(UINT32_C(1)<<28));
    if(!attract || podium) fs->race=race;   /* as on the device: practice.race stays live through the podium */
    /* Every tick: the pick keeps the eye height of the frame before. */
    practice_road_depth_pick(fs);
    if(tick%frame_step || (frame_last>=0 && tick>frame_last)) return;
    fs->intro=presentation;
    fs->spin_smoke=spin_smoke;              /* task 9e2c, stepped by the harness's race loop as by code/practice.c */
    if(!attract) { fs->intro.count=race.count; fs->intro.ticks=race.ticks; }
    const int ok=nm_practice_render_pose(fs) && nm_practice_render_projection(fs,1) && nm_practice_render_lighting(fs) &&
        nm_practice_render_parts(fs) && nm_practice_render_player_car(fs) && nm_practice_render_shadows(fs) &&
        nm_practice_render_scenery(fs);
    nm_practice_render_projection(fs,0);
    if(!ok) { fprintf(stderr,"frame assembly rejected tick %ld\n",tick); exit(3); }
    unsigned player=fs->intro.count;
    if(!attract) for(unsigned i=0;i<fs->intro.count;++i) if(fs->intro.detail_order[i]==0) { player=i;break; }
    const uint32_t head[8]={0x354d5246u,(uint32_t)tick,(uint32_t)attract,fs->intro_header.instances,fs->world_header.instances,
        fs->intro.count,player,fs->shadow_count};
    fwrite(head,4,8,frames_out); fwrite(fs->car_part_boundaries,4,45,frames_out);
    fwrite(fs->car_instances,sizeof(nm_instance_t),fs->intro_header.instances,frames_out);
    fwrite(fs->world_instances,sizeof(nm_instance_t),fs->world_header.instances,frames_out);
    fwrite(fs->view,4,12,frames_out);
    fwrite(fs->shadow_kind,1,NM_SHADOW_VOLUMES_MAX+3,frames_out); fwrite(fs->shadow_matrix,48,NM_SHADOW_VOLUMES_MAX,frames_out);
    /* Reflection UVs as applied to the three owned window meshes. */
    nm_vertex_t *sets[3]={fs->intro_windows[0],fs->intro_windows[1],fs->window_vertices};
    const unsigned counts[3]={fs->intro_window_count[0],fs->intro_window_count[1],fs->window_vertex_count};
    for(unsigned s=0;s<3;++s) {
        uint32_t first=sets[s] ? (uint32_t)(sets[s]-fs->model.vertices) : 0,count=sets[s] ? counts[s] : 0;
        fwrite(&first,4,1,frames_out); fwrite(&count,4,1,frames_out);
        for(unsigned v=0;v<count;++v) { fwrite(&sets[s][v].u,2,1,frames_out); fwrite(&sets[s][v].v,2,1,frames_out); }
    }
    { const nm_scenery_cells_t cells=fs->intro.cells_previous; uint32_t trailer[24]={0};
      if(!nm_scenery_bitmaps(&cells,trailer)) { fprintf(stderr,"bitmaps rejected tick %ld\n",tick); exit(3); }
      /* Board actor 221d24 inputs: 501854, +6d of the car in 501220, 5010ec (spec-board.md). */
      const nm_player_t *p=&race.cars[0].car;
      const unsigned passes=podium ? presentation.cars[0].lap.lap : 0u;
      trailer[16]=attract && !podium ? 0 : race.start_flags;
      trailer[17]=attract && !podium ? 0 : passes ? (uint8_t)(p->lap.lap+passes) : p->lap.completed;
      trailer[18]=attract && !podium ? 0 : p->checkpoints.total_laps;
      trailer[19]=attract ? 0 : fs->race.count;
      if(!attract) { memcpy(trailer+20,&fs->race.cars[0].car.boundary.ground.position[0],4); memcpy(trailer+21,&fs->race.cars[0].car.boundary.ground.position[2],4);
        trailer[22]=fs->race.cars[0].car.camera_mode; }
      trailer[23]=fs->intro.stage|(!attract && race.shared.pit.suspended ? 0x80000000u : 0u);
      fwrite(trailer,4,24,frames_out);
      /* FRM3: the animated actors' state (reference_stream.h). 5010b0 from this generator's own tick numbering:
       * frame tick - 1 in the race and the podium, scene tick + 3 in the attract (spec-simple.md). */
      uint32_t animated[137]={0};
      for(unsigned c=0;c<256;++c) animated[c>>1]|=(uint32_t)fs->intro.object_near_ticks[c]<<(16*(c&1));
      animated[128]=attract && !podium ? (uint32_t)presentation.ticks+3u : (uint32_t)tick-1u;
      for(unsigned k=0;k<5;++k) animated[129+k]=attract ? presentation.rank_order[k] : race.order.rank[k];
      /* FRM4: reels 0..2 of the slot machine the renderer reads (render/scene/practice.c). */
      const nm_slot_machine_t *slot=attract ? &presentation.slot_machine : &race.slot_machine;
      for(unsigned k=0;k<3;++k) animated[134+k]=(uint32_t)(uint16_t)slot->reel[k];
      fwrite(animated,4,137,frames_out); }
    /* FRM5: the road class's scale of this frame. */
    fwrite(&practice_road_depth,4,1,frames_out);
    ++frames_written;
}
'''
at = trace.index('static void write_rows(')
trace = trace[:at] + shim + slices + driver + trace[at:]
def patch(old, new):
    global trace
    assert trace.count(old) == 1, old
    trace = trace.replace(old, new)
# The race loop: after this tick's detail pass. The reflection as practice.c hands it to the presentation.
patch('        write_rows(out,tick+1,r->frame,&controls);\n        if(clock_out)',
      '''        if(update.action==NM_REFLECTION_UV) { presentation.reflection_u=update.u_offset;presentation.reflection_v=update.v_offset; }
        frame_dump(tick+1,0);
        write_rows(out,tick+1,r->frame,&controls);
        if(clock_out)''')
patch('                if(advanced && podium.cleanup) {',
      '                if(advanced && podium.cinematic && podium_live) frame_dump(tick+1,1);\n                if(advanced && podium.cleanup) {')
# Assets loaded: set up; attract-only mode.
patch('    FILE *out=fopen(argv[3],"wx");', r'''    frame_course=(unsigned)course;
    if(getenv("FRAMES_OUT") && !frame_setup(argv[1])) { fputs("frame setup failed\n",stderr);return 2; }
    if(fs) fs->transmission=(uint8_t)transmission; /* the player's parts and body (player_parts.h) */
    if(getenv("FRAMES_ATTRACT")) {
        const long limit=strtol(getenv("FRAMES_ATTRACT"),NULL,10);
        if(!nm_intro_scene_start(&presentation,math,&road,paths) || !nm_intro_scene_detail(&presentation,math,&scenery)) return 1;
        frame_dump(0,1);
        while((long)presentation.ticks<limit) {
            if(!nm_intro_scene_step(&presentation,math,&contact,&road,paths) || !nm_intro_scene_detail(&presentation,math,&scenery)) { fprintf(stderr,"attract rejected tick %u\n",(unsigned)presentation.ticks);return 1; }
            frame_dump((long)presentation.ticks,1);
        }
        printf("attract frames=%ld ticks=%u stage=%u\n",frames_written,(unsigned)presentation.ticks,(unsigned)presentation.stage);
        fclose(frames_out);return 0;
    }
    FILE *out=fopen(argv[3],"wx");''')
patch('    const int failed=ferror(out);',
      '    if(frames_out) { printf("frames=%ld\\n",frames_written);fclose(frames_out); }\n    const int failed=ferror(out);')
patch('static void write_rows(FILE *out,long n,long frame,const nm_controls_t *controls) {',
      'static void write_rows(FILE *out,long n,long frame,const nm_controls_t *controls) {\n    if(getenv("FRAMES_OUT")) return;')
patch('static nm_race_scene_t race;\n', 'static nm_race_scene_t race;\nstatic unsigned frame_course;\n')
(work/'frame_gen.c').write_text(trace)
include = [work, root/'tests', root/'include', root.parents[1]/'extern/sh4zam/include', H/'race-header', H/'hud', H/'intent-data', H/'race-start-data',
           H/'record-data', H/'podium-data']
sources = ['drive', 'contact', 'road', 'path', 'visibility', 'props', 'props_scene', 'expert_actors', 'geometry', 'meshpack',
           'render/scene/track_surface', 'render/scene/damage_bodies']
cmd = ['cc', '-std=c11', '-O2', '-fno-fast-math', '-ffp-contract=off', '-Wno-unused-function', '-DNM_TRACE_PARTS=1',
       *os.environ.get('GEN_CFLAGS', '').split(), *[f'-I{i}' for i in include], str(work/'frame_gen.c'),
       *[str(root/'code'/f'{n}.c') for n in sources], '-lm', '-o', str(work/'frame_gen')]
subprocess.run(cmd, check=True)

env = dict(os.environ, FRAMES_OUT=str(out), FRAME_STEP=str(args.step), FRAME_LAST=str(args.last))
if args.attract:
    env['FRAMES_ATTRACT'] = str(args.attract)
    (work/'none.trace').write_text('')
    run = subprocess.run([str(work/'frame_gen'), args.assets, str(work/'none.trace'), str(work/'out.trace'), '1', '1', 'idle',
                          '--course', str(args.course)], env=env, capture_output=True, text=True)
else:
    # The race script as a reference trace (the fields the production mode reads), the podium's ticks after it.
    script = args.script or {0: 'script-fast', 1: 'script-expert-finish', 2: 'script-advanced-finish'}[args.course]
    text = (H/script/'race_script_data.h').read_text()
    manual = int('NM_RACE_SCRIPT_MANUAL' in text)
    ticks = re.findall(r'\{(-?\d+),(\d+),(\d+),(\d+)\}', text[text.index('nm_race_script[NM_RACE_SCRIPT_TICKS]'):])
    views = {}
    for item in filter(None, args.views.split(',')):
        tick, view = item.split(':')
        views[int(tick)] = 1 << int(view)  # the trace's vr= bits: 1 START, 2 VR1 .. 16 VR4
    rows = []
    for n in range(len(ticks)+4000):
        steer, accel, brake, edges = ticks[n] if n < len(ticks) else (0, 0, 0, 0)
        rows.append(f'T n={n+1} frame={n+1} entry=0 steer={steer} accel={accel} brake={brake} gearsel={int(edges) >> 3 & 7} '
                    f'manual={manual} difficulty=0 penalty=0 suppress=0 blocked=0 variant=0 recover={int(edges) >> 2 & 1} gate=0 hold=0 '
                    f'guided=0 vr={views.get(n, 0)}\n')
    (work/'script.trace').write_text(''.join(rows))
    (work/'out.trace').unlink(missing_ok=True)
    run = subprocess.run([str(work/'frame_gen'), args.assets, str(work/'script.trace'), str(work/'out.trace'), '1', str(len(rows)),
                          'production', '--course', str(args.course), '--counter-base', '2368',
                          '--transmission', 'manual' if manual else 'automatic'], env=env, capture_output=True, text=True)
print(run.stdout[-1500:])
if run.returncode:
    raise SystemExit(run.stderr[-3000:] or f'frame_gen exit {run.returncode}')
