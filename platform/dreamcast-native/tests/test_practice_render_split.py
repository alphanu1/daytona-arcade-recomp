"""Architecture/move regression, not a substitute for deferred-path hardware tests."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1] / "code"
headers = root.parent / "include"
config = (root.parent / 'local.cfg.mk').read_text()
assert not re.search(r'\bNATIVE_(WORLD|SCENE|SCENE_DIR|DRIVING|DRIVING_3D|DRIVING_TEXTURES|MODELS|MATERIALS|RENDER|LIGHT_FIPR|POSE_(SH4ZAM|BUILD_SH4ZAM|QUIET))\b', config)
assert not list(root.rglob("*.h")), "headers belong under include/"
for tree in (root, headers):
    for source in tree.rglob('*'):
        if source.suffix in ('.c', '.h'):
            assert not re.search(r'\bNATIVE_(WORLD|SCENE|MODELS|DRIVING|DRIVING_3D|DRIVING_TEXTURES|RENDER|MATERIALS|LIGHT_FIPR|POSE_(SH4ZAM|BUILD_SH4ZAM|QUIET))\b', source.read_text()), source
    assert {p.name for p in (tree / "render").iterdir() if p.is_dir()} == {
        "models", "scene", "materials", "ui"}
    assert not any(p.is_file() for p in (tree / "render").iterdir())
for source in (root / "practice.c", root / "render/scene/practice.c", headers / "practice_scene.h",
               root.parent / "local.cfg.mk"):
    assert "NATIVE_RACE_CONTROLLER" not in source.read_text(), source
    assert "NATIVE_RACE_PREVIEW" not in source.read_text(), source
    assert not re.search(r'\bNATIVE_INTRO\b', source.read_text()), source
    assert not re.search(r'\bNATIVE_(RECORD|INTENT)_DATA\b', source.read_text()), source
game = (root / "practice.c").read_text()
render = (root / "render/scene/practice.c").read_text()
assert not re.search(r"pvr_dr_(target|commit)|enj_qfont_write", game)
update = game.split("static void practice_update(", 1)[1].split("int nm_practice_run(", 1)[0]
assert "nm_practice_render_enqueue(&practice)" in update
assert not re.search(r"nm_practice_render_(pose|parts|lighting|scenery)\(", update)
assert "practice_render_frame" not in render
assert "nm_render_enqueue_objects(" in render

expected = {"practice_vertex":[1133975145],"practice_render":[981154047,1990458420],"practice_matrix":[3529732226],"practice_point":[2532397843],"practice_triangle":[2744000516],"practice_pose":[792027436],"practice_lighting":[1156485727],"practice_parts":[2224785353],"practice_scenery":[2549244377],"practice_check_transform":[1260611947],"practice_labels":[2016751644]}
assert "(!scene->attract_active && i==0) ? nm_player_part_pose(scene->math,car,0,body) :" in render
assert "(!scene->attract_active && i==0) ? nm_player_part_pose(scene->math,car,1,part) :" in render
assert "scene->intro.stages[scene->intro.stage].spawn[i].kind" in render
assert "nm_intro_scene_start_attract(&practice.intro" in game
assert "nm_intro_scene_step(&practice.intro" in game
assert "nm_intro_scene_detail(&practice.intro" in game
# Deliberate race-player/intro-car rotation split, validated by the source oracle.
expected["practice_parts"] = [3512400600]  # the two window UV rewrites register their vertices (nm_render_uv_dynamic)
# 2026-10-14: render-only level of detail for distant opponents (practice_drawn_level, NM_SMALL_CAR_PX; off by
# default): the body pose is composed before the binding, whose level may be the drawn one, not the arcade's.
expected["practice_parts"] = [820051759]
# 2026-10-13: the scenery pass also lists the world objects of the pack (meshpack.h,
# NM_PACK_WORLD_OBJECTS; previous-pass cells, selector variants). Was 1743457858.
expected["practice_scenery"] = [506796032]
# 2026-10-15: world objects are walked as selection runs, and the animated actors (die, symbol wheel,
# pit crews, video screen, leader board; meshpack.h NM_OBJECT_TURN..RANK, PLACED/SPIN/MESHES) are drawn.
expected["practice_scenery"] = [3941113789]
# 2026-10-16: no world object while the pit stop suspends the actor rows 74..138 (dd9c; pit.h).
expected["practice_scenery"] = [2634860974]
# 2026-10-17: the cones (meshpack.h NM_OBJECT_CONE) drawn as the props' tick drew them (include/props.h).
expected["practice_scenery"] = [2760162395]
# 2026-10-17 (courses, merged on the cones): a course without world packs (practice_assets.c) places no instance.
expected["practice_scenery"] = [1618294209]
# 2026-10-18: the frame-counter animations of Advanced (include/clock_actors.h, practice_clocks) after the selection runs.
expected["practice_scenery"] = [2038284187]
# World rendering is mandatory: old non-world ribbon/projection helpers are gone.
expected["practice_vertex"] = []
expected["practice_render"] = []
expected["practice_point"] = []
expected["practice_triangle"] = []
# 2026-10-12: the frame line now shows SH4 work, GPU time and late frames
# instead of the vblank-locked interval; debug text only.
# 2026-10-18: the developer labels only while the inspector is open (NATIVE_DEBUG_LABELS=1: as before).
expected["practice_labels"] = [429408269]
# 2026-10-17 (courses): the cars take the race's scene light (tunnel_light.h) instead of the constant.
expected["practice_lighting"] = [1347236094]
# 2026-10-18 (attract-courses): the attract's cars take its scene light too (intro_scene.h tunnel), the podium the normal one.
expected["practice_lighting"] = [1740567860]
# Intro/race copies the tick's camera; its per-car poses are built in parts.
# The old single-car body_view has no consumer in this build.
intro_pose = render.split("int nm_practice_render_pose(", 1)[1].split("}\nint nm_practice_render_lighting", 1)[0]
assert "shz_memcpy4(scene->view,scene->intro.view,sizeof(scene->view));" in intro_pose
assert "scene->camera_heading=scene->intro.heading;" in intro_pose
assert "return 1;" in intro_pose
assert not re.search(r"nm_(body_pose|pose_compose|follow_view)\(", intro_pose)
expected["practice_pose"] = [1222451969]
# Direct SH4ZAM affine load is independently checked by test_matrix_load.py.
expected["practice_matrix"] = [2412891019]
render = re.sub(r"nm_practice_render_(matrix|view|check_transform|pose|lighting|parts|scenery)",
                r"practice_\1", render)

def bodies(source, name):
    result = []
    for match in re.finditer(r"\b(?:void|int) " + name + r"\([^)]*\)\s*\{", source):
        start = at = match.end()
        depth = 1
        while depth:
            ch = source[at]
            at += 1
            depth += (ch == "{") - (ch == "}")
        result.append(source[start:at-1])
    return result

def fingerprint(body):
    # 2026-10-18 (attract-courses): yield points of a load in another screen's frames (scene_loader.h) are no
    # change of what a body computes: taken out before the fingerprint.
    body = re.sub(r"\n[ \t]*nm_loader_yield\(0\);[^\n]*", "", body)
    body = re.sub(r"\n[ \t]*if \(!\(i&4095u\)\) nm_loader_yield\(0\);", "", body)
    # Object-boundary bookkeeping is exercised by test_sq_quads; retain the
    # original arithmetic/body fingerprint rather than blessing new math.
    for bookkeeping in (
        "if(!scene->world.meshes[source->mesh].face_count) continue;",
        "if(scene->intro.count>=sizeof(scene->car_part_boundaries)/sizeof(scene->car_part_boundaries[0])) return 0;",
        "scene->car_part_boundaries[ordinal]=scene->intro_header.instances;",
        "scene->car_part_boundaries[scene->intro.count]=scene->intro_header.instances;",
    ):
        body = body.replace(bookkeeping, "")
    # The cinematic stage table is scene data now (attract or podium graph);
    # the kind lookup itself is unchanged.
    body = body.replace("scene->intro.stages[scene->intro.stage]", "nm_intro_stages[scene->intro.stage]")
    body = body.replace("(void)unused;", "").replace("nm_practice_scene_t *scene=data;", "")
    body = re.sub(r"\bpractice\.", "scene->", body)
    body = re.sub(r"practice_(vertex|triangle)\(scene,", r"practice_\1(", body)
    body = re.sub(r"\s+", "", body)
    value = 2166136261
    for ch in body:
        value = ((value ^ ord(ch)) * 16777619) & 0xffffffff
    return value

for name, hashes in expected.items():
    actual = [fingerprint(body) for body in bodies(render, name)]
    assert actual == hashes, (name, hashes, actual)
print("PASS: gameplay/render queue boundary; moved bodies and player-only pose selection")

main = (root / "main.c").read_text()
startup = (root.parent / "tests/startup_check.h").read_text()
assert "if(nm_startup_check()) return 1;" in main
assert "nm_race_movement_check(" not in main and "contact_diagnostic(" not in main
includes = startup.split("#ifdef NATIVE_RACE_MOVEMENT_CHECK", 1)[1].split("\nstatic int nm_startup_check", 1)[0]
body = startup.split("static int nm_startup_check(void) {\n", 1)[1].rsplit("    return 0;\n}", 1)[0]
assert fingerprint("#ifdef NATIVE_RACE_MOVEMENT_CHECK" + includes + body) == 1919220202
print("PASS: optional startup suites moved unchanged; main retains failure propagation")

probe = (root.parent / "tests/presentation_probe.h").read_text()
declarations = probe.split("#ifdef NATIVE_CAMERA", 1)[1].split("static int nm_probe_run(void)", 1)[0]
declarations = declarations.replace('#include "render/models/model_renderer.h"', '#include "meshpack.h"')
declarations = declarations.replace('render/materials/materials.h', 'materials.h')
run = probe.split("static int nm_probe_run(void) {\n", 1)[1].rsplit("}\n#endif", 1)[0]
assert fingerprint("#ifdef NATIVE_CAMERA" + declarations + run) == 775016346
assert not re.search(r'\bNATIVE_SCENE\b|scene_(load|enqueue)\(', probe)
assert not (root / 'scene.c').exists() and not (headers / 'scene.h').exists()
assert (root.parent / 'tests/reference/title_scene.c').exists()
assert 'presentation_probe.h' not in main and 'nm_probe_run' not in main
assert not re.search(r"pvr_dr_(target|commit)|enj_qfont_write|probe_t", main)
assert "return nm_practice_run(NM_ASSET_ROOT);" in main
transfer_main = (root.parent / 'tests/transfer/main.c').read_text()
assert '#include "../presentation_probe.h"' in transfer_main
assert 'return nm_probe_run();' in transfer_main
print("PASS: bring-up probe excludes retired title switch and stays outside game entry point")

assert "nm_render_" not in (headers / "meshpack.h").read_text()
assert '#include "meshpack.h"' in (headers / "render/models/model_renderer.h").read_text()
print("PASS: mesh data/loader interface is separate from model renderer API")

model_checks = (root / "render/models/model_validation.c").read_text()
model_math = (headers / "render/models/model_math.h").read_text()
checks = model_checks[model_checks.index("/* Presentation import check only."):]
helpers = model_math[model_math.index("static inline void load_matrix"):].rsplit("#endif", 1)[0]
# 2026-10-12: the scenery lighting oracle no longer fails on a face seen
# edge-on within the FTRV rounding bound (check_facing/edge_on in
# model_validation.c; hardware stage 5: 1 face of 1,741), and prints both
# facings of a side difference. Cars keep the exact gate; every other check
# body is as moved (previous fingerprint 1913524133).
# 2026-10-18: a variable specular term is accepted where the renderer evaluates it (NM_PACK_LIVE_SPECULAR, Expert;
# previous fingerprint 428101631).
assert fingerprint(checks) == 2651553557
assert fingerprint(helpers) == 3163670716 # Affine loader parity: test_matrix_load.py.
models = (root / "render/models/model_renderer.c").read_text()
assert '#include "render/models/model_math.h"' in models
assert not re.search(r"int nm_pack_check_\w+\([^;]+\)\s*\{", models)
print("PASS: model startup checks separated; shared math and check bodies unchanged")

assets = (root / "render/scene/practice_assets.c").read_text()
# 2026-10-18 (attract-courses): a yield point at each stage (scene_loader.h: a load in another screen's frames) and its
# comment are the load's only change; taken out here, the rest is checked as before.
assets = assets.replace(' nm_loader_yield(0);', '').replace(
    "/* nm_loader_yield at each stage: a load in another screen's frames (scene_loader.h) hands its slice back there. */\n", '')
assert '#include "render/scene/track_surface.h"' in assets
# 2026-10-12: the load selects the build's scenery stage (track_surface.h);
# stage 0 of a pack without scenery classes is nm_track_surface_select.
selection = '    *stage="scenery stage selection";\n    if(!nm_scenery_stage_select(&scene->world,NM_SCENERY_MATERIAL_STAGE)) goto done;\n'
assert assets.count(selection) == 1
assert assets.index('scene->scenery.slot!=scene->course') < assets.index(selection) < assets.index('scene->world_header=*scene->world.header;')
assert assets.index(selection) < assets.index('nm_render_world_attach')
assert render.count('if(!scene->world.meshes[source->mesh].face_count) continue;') == 1
load = bodies(assets, 'nm_practice_render_load')[0].split('    return 1;\ndone:', 1)[0]
load = load.replace(selection, '')
# 2026-10-17 (courses): the CPU pack reset is the function nm_practice_render_unload (the course switch of
# code/practice.c calls it too), and the world, sky and world materials are the scene's course's (course_assets.h;
# an asset set without that course's world draws none). Put back as they were before: the moved body is unchanged.
unload_call = '    nm_practice_render_unload(scene);\n'
unload_body = bodies(assets, 'nm_practice_render_unload')[0]
assert load.count(unload_call) == 1 and unload_body.startswith('\n')
load = load.replace(unload_call, unload_body[1:])
world_less = load[load.index('    if(!practice_course_world(root,scene->course)) {\n'):load.index('    } else {\n') + len('    } else {\n')]
load = load.replace(world_less, '')
# The world's frame bound (nm_render_world_capacity) before the attach: sizes only, nothing else moves.
frame_bound = load[load.index('    {\n        unsigned points,faces;\n        practice_world_bound('):]
frame_bound = frame_bound[:frame_bound.index('    }\n') + len('    }\n')]
assert 'nm_render_world_capacity(points,faces);' in frame_bound
load = load.replace(frame_bound, '')
# The windows' tunnel materials (nm_practice_render_tunnel) found after the car load: lookups only.
windows = load[load.index('    /* The window materials of the tunnel texture'):load.index('    scene->car_sources=scene->model.instances;')]
assert 'scene->window_materials[kind]=' in windows
load = load.replace(windows, '')
# 2026-10-18: the course's frame-counter animations (include/clock_actors.h, courseN-actors.dna), loaded before the
# world materials and freed with the CPU packs; nothing else moves.
clocks = '    *stage="courseN-actors.dna";\n    if(!practice_clock_load(scene,root)) goto done;\n    *stage="world.dnt/shared renderer";\n'
assert load.count(clocks) == 1
load = load.replace(clocks, '')
clock_free = '    free(scene->clock_data);scene->clock_data=NULL;scene->clock_count=0;\n'
assert load.count(clock_free) == 1
load = load.replace(clock_free, '')
# 18 October: the actors of the file (Expert's) leave the props scene with it.
actors_free = '    nm_props_world_actors(NULL,0);\n'
assert load.count(actors_free) == 1
load = load.replace(actors_free, '')
for course_name, original in (
        ('    if(!nm_course_asset(file,sizeof(file),root,scene->course,NM_COURSE_WORLD,0)) goto done;\n',
         '    snprintf(file,sizeof(file),"%sworld.dnm",root);\n'),
        ('    if(!nm_course_asset(file,sizeof(file),root,scene->course,NM_COURSE_SKY,0) || !nm_sky_load(file)) goto done;\n',
         '    snprintf(file,sizeof(file),"%ssky.dns",root);\n    if(!nm_sky_load(file)) goto done;\n'),
        ('    if(!nm_course_asset(file,sizeof(file),root,scene->course,NM_COURSE_WORLD_MATERIALS,0)) goto done;\n',
         '    snprintf(file,sizeof(file),"%sworld.dnt",root);\n'),
        ('    if (!nm_world_materials_load(file) || !nm_render_world_attach(&scene->world)) goto done;\n    }\n',
         '    if (!nm_world_materials_load(file) || !nm_render_world_attach(&scene->world)) goto done;\n'),
        ('scene->scenery.slot!=scene->course', 'scene->scenery.slot!=0')):
    assert load.count(course_name) == 1, course_name
    load = load.replace(course_name, original)
# D43 exercises the exact CPU ownership reset in test_practice_assets_reload.
# Keep the unchanged setup/math body fingerprint below.
reload_reset = '''    /* Callers release queued rendering before replacing the game-owned CPU packs. */
    nm_pack_unload(&scene->model);
    nm_pack_unload(&scene->world);
    free(scene->world_instances);scene->world_instances=NULL;
    scene->car_sources=scene->world_sources=NULL;
    scene->window_vertices=NULL;scene->window_vertex_count=0;
    for(unsigned w=0;w<2;++w) {
        scene->intro_windows[w]=NULL;scene->intro_window_count[w]=0;
    }
'''
assert load.count(reload_reset) == 1
load = load.replace(reload_reset, '')
# 2026-10-13, world objects (meshpack.h, NM_PACK_WORLD_OBJECTS): the world pack may hold
# instances behind those of the cells; the load accepts them, records their range and
# allocates for all of them. Nothing else of the moved body changes.
object_gate = ("        scene->scenery.instances>scene->world.header->instances ||\n"
               "        (scene->scenery.instances!=scene->world.header->instances &&\n"
               "         !(nm_pack_flags(&scene->world)&NM_PACK_WORLD_OBJECTS)) ||")
assert load.count(object_gate) == 1
load = load.replace(object_gate, "        scene->scenery.instances!=scene->world.header->instances ||")
object_range = ("    scene->world_object_first=scene->scenery.instances;\n"
                "    scene->world_object_count=scene->world.header->instances-scene->scenery.instances;\n"
                "    for(unsigned i=0;i<scene->world.header->instances;++i)\n"
                "        if(!(scene->world_sources[i].padding[0]&NM_OBJECT_INSTANCE)!=(i<scene->world_object_first)) goto done;\n")
assert load.count(object_range) == 1
load = load.replace(object_range, "")
# 2026-10-24: the allocation is nm_alloc (include/nm_mem.h: memalign(32) on the console, every allocation of the game
# 32-byte aligned); the call it replaced is put back, the body is otherwise as moved.
nm_allocation = 'scene->world_instances=nm_alloc(scene->world.header->instances*sizeof(*scene->world_instances));'
assert load.count(nm_allocation) == 1
load = load.replace(nm_allocation, 'scene->world_instances=memalign(32,scene->world.header->instances*sizeof(*scene->world_instances));')
object_allocation = 'scene->world_instances=memalign(32,scene->world.header->instances*sizeof(*scene->world_instances));'
assert load.count(object_allocation) == 1
load = load.replace(object_allocation, 'scene->world_instances=memalign(32,scene->scenery.instances*sizeof(*scene->world_instances));')
# Only the live instance allocation changes; preserve the moved-body fingerprint.
aligned_allocation = 'scene->world_instances=memalign(32,scene->scenery.instances*sizeof(*scene->world_instances));'
assert load.count(aligned_allocation) == 1
load = load.replace(aligned_allocation, 'scene->world_instances=malloc(scene->scenery.instances*sizeof(*scene->world_instances));')
# DNM5 and DNM6 add only format acceptance branches (DNM6 world packs must
# carry normal groups); retain the original moved-body hash.
strip_gate = '((scene->world.header->magic & ~NM_PACK_ALIGNED)!=NM_PACK_MAGIC_NORMAL_GROUPED &&\n' \
             '         (scene->world.header->magic & ~NM_PACK_ALIGNED)!=NM_PACK_MAGIC_STRIPS &&\n' \
             '         ((scene->world.header->magic & ~NM_PACK_ALIGNED)!=NM_PACK_MAGIC_LAYERED || !scene->world.normal_groups))'
assert strip_gate in load
load = load.replace(strip_gate, 'scene->world.header->magic!=NM_PACK_MAGIC_NORMAL_GROUPED')
# 2026-10-15, the player's damaged bodies (render/scene/damage_bodies.h): car-damage.dnb loaded after car.dnm, its
# light classes reserved before nm_render_init (2026-10-17: one a face with the manual car), released with the car
# pack; 2026-10-17 (views) the hood view's car-hoods.dnb the same way, the reserve the larger of the two. Nothing else
# of the moved body changes.
damage_unload = ('    nm_damage_bodies_unload(&scene->damage_bodies);\n'
                 '    nm_damage_bodies_unload(&scene->hood_variants);\n')
damage_load = ('    /* The player\'s damaged bodies: variants of the clean body\'s mesh (render/scene/damage_bodies.h). */\n'
               '    *stage="car-damage.dnb";\n'
               '    snprintf(file,sizeof(file),"%scar-damage.dnb",root);\n'
               '    if(!nm_damage_bodies_load(&scene->damage_bodies,file)) goto done;\n'
               '    /* The hood view\'s damaged hoods: variants of the clean hood\'s mesh, as the bodies. */\n'
               '    *stage="car-hoods.dnb";\n'
               '    snprintf(file,sizeof(file),"%scar-hoods.dnb",root);\n'
               '    if(!nm_damage_hoods_load(&scene->hood_variants,file)) goto done;\n'
               '    /* The manual car takes the variants in its own face order and materials (render/scene/practice.c): up to one\n'
               '     * light class a face. */\n'
               '    {\n'
               '        const nm_damage_bodies_t *sets[2]={&scene->damage_bodies,&scene->hood_variants};\n'
               '        unsigned classes=0;\n'
               '        for(unsigned i=0;i<2;++i) {\n'
               '            const unsigned need=scene->transmission && sets[i]->faces>sets[i]->classes ? sets[i]->faces : sets[i]->classes;\n'
               '            if(need>classes) classes=need;\n'
               '        }\n'
               '        nm_render_car_light_classes_reserve(classes);\n'
               '    }\n'
               '    scene->player_body_variant=NM_DAMAGE_BODY_CLEAN;\n'
               '    scene->player_hood_variant=NM_DAMAGE_HOOD_CLEAN;\n')
assert load.count(damage_unload) == 1 and load.count(damage_load) == 1
load = load.replace(damage_unload, '').replace(damage_load, '')
load = load.replace('    char file[256];\n', '')
# 2026-10-19 (attract-courses): the car pack's checks run once per power-on (car_checked): the gate's lines put back.
for text in ("    /* The car pack's checks (the FTRV gate, nm_pack_check_math, nm_pack_check_lighting) once per power-on: the same\n     * pack at every load (the attract loop reloads the scene at each course); the road's profiles every load. */\n    static int car_checked;\n", '    if(!car_checked)\n', '    car_checked=1;\n'):
    assert load.count(text) == 1, text
    load = load.replace(text, '')
once = '(!car_checked && (!nm_pack_check_math(&scene->model) ||\n        !nm_pack_check_lighting(&scene->model)))'
assert load.count(once) == 1
load = load.replace(once, '!nm_pack_check_math(&scene->model) ||\n        !nm_pack_check_lighting(&scene->model)')
# 2026-10-18 (attract-courses): the presentation FTRV check yields every 4096 vertices (scene_loader.h): its loop
# took braces for it (its yield statement is taken out with the stages' above); put back as it was.
load = load.replace("""        for (unsigned i=0; i<scene->model.header->vertices; ++i) {
            if (!(i&4095u))
            if (!nm_practice_render_check_transform(scene->model.vertices[i].position,m,1)) goto done;
        }""", """        for (unsigned i=0; i<scene->model.header->vertices; ++i)
            if (!nm_practice_render_check_transform(scene->model.vertices[i].position,m,1)) goto done;""")
load = load.replace('scene->', 'practice.').replace('(scene)', '(&practice)')
load = load.replace('*stage=', 'stage=')
assert fingerprint(load) == 2760655785
# The HUD sheets are released with the scene; the rest of the body is unchanged.
hud_unload = '    nm_hud_render_unload(); /* HUD */\n'
assert assets.count(hud_unload) == 1
assert [fingerprint(body.replace(hud_unload, '')) for body in bodies(assets, 'nm_practice_render_release')] == [1962292542]
for source in (assets, render, (root / 'render/ui/finish_render.c').read_text(),
               (root.parent / 'local.cfg.mk').read_text()):
    assert 'NATIVE_FINISH_RENDER' not in source
for source in ((root / 'gameplay/results.c').read_text(), (root / 'gameplay/name_mode.c').read_text(),
               (root / 'render/materials/result_materials.c').read_text(),
               (root / 'render/ui/result_render.c').read_text(), (root / 'render/ui/result_labels.c').read_text(),
               (headers / 'gameplay/results.h').read_text(), (headers / 'practice_scene.h').read_text(), assets):
    assert not re.search(r'^\s*#\s*(if|elif|ifdef|ifndef).*NATIVE_RESULT', source, re.M)
# Power-on enters the attract loop (phase 2), as the original after its boot screen.
assert 'if(!nm_attract_mode_start()) goto done;' in game
# Both scene entries load through the populated-view wrapper; failure still leaves through fail (the race's) or
# returns to its caller (the attract's: practice_attract_prepare, also a scene loader job, scene_loader.h).
assert game.count('&stage)) goto fail;')==1 and 'nm_practice_render_load(&practice,practice_root,stage);' in game
assert game.count('&practice_attract_stage)) return 0;')==1
assert 'pvr_poly_compile' not in game and 'nm_pack_load' not in game
assert 'nm_practice_render_release(); enj_state_flag_shutdown(NULL);' in game
assert '(scene->world.header->magic & ~NM_PACK_ALIGNED)!=NM_PACK_MAGIC_NORMAL_GROUPED' in assets
print('PASS: scene asset setup/release unchanged except explicit DNM5/DNM6 gate and aligned live allocation; failure releases before shutdown')

results = (root / 'gameplay/results.c').read_text()
result_body = bodies(results, 'nm_results_update')[0].rsplit('    return 0;', 1)[0]
# The phase29 mode handoff is exercised by test_results_phase/test_name_mode;
# retain the old fingerprint only for the unchanged result playback below it.
handoff = result_body.index('        if(scene->race.phase==29) {')
timing = result_body.index('        uint32_t dt=elapsed_us;', handoff)
result_body = result_body[:handoff] + result_body[timing:]
# Intentional rule port: the race field's results tick (4110/1210c, nm_practice_results_tick) at the end of each
# results tick and its phase26 message; normalize exactly those lines, retaining the playback fingerprint.
for added in ('            const unsigned results_phase=scene->race.phase;\n',
              '                if(!result_field_tick(scene,results_phase)) { scene->failed=1;break; }\n',
              # The engine and slip resets b260/b264 (audio/race_sound_hooks.h), sound commands only.
              '                /* b260/b264: engine and slip off, before the entry\'s channel commands. */\n',
              '                nm_race_sound_hook_results_off(&scene->results.commands,&scene->results.command_hash);\n'):
    assert added in result_body
    result_body = result_body.replace(added, '')
result_body = result_body.replace('''puts(scene->results.after_podium ?
                    "Native result phase26: field/GOAL updates disabled; authored glyph actor and silent rank schedule active; entry audio resets pending" :
                    "Native result phase26: opponents parked (4110), field interactions running (1210c), GOAL updates disabled; authored glyph actor and silent rank schedule active; entry audio resets pending");''',
    'puts("Native result phase26: field/GOAL updates disabled; authored glyph actor and silent rank schedule active; entry audio resets pending");', 1)
# Intentional rule port: START held skips the result actor's countdown (b680): the one argument that was 0.
assert '&countdown,scene->results.start_held)' in result_body
result_body = result_body.replace('&countdown,scene->results.start_held)', '&countdown,0)', 1)
result_body = result_body.replace('if(scene->failed) {', 'if(scene->failed || scene->race.phase==29) {', 1)
result_body = result_body.replace('scene->results.', 'result_')
result_body = result_body.replace('result_silent_command(&scene->results,', 'result_silent_command(')
result_body = result_body.replace('return 1;', 'return;')
# Intentional bug fix: defer cleanup after partial enqueue; only normalize
# this exact changed tail, retaining the original result arithmetic fingerprint.
result_body = result_body.replace('''/* Labels may already be queued. Keep their resources alive until
             * callbacks finish; the next updater owns failure cleanup. */
            scene->failed=1;''', 'scene->failed=1;nm_practice_render_release();enj_state_flag_shutdown(NULL);')
assert fingerprint(result_body) == 3535557818
assert 'if(nm_results_update(&practice,elapsed_us)) return;' in game
assert 'nm_result_playback_step(' not in game and 'nm_result_render_enqueue(' not in game
print('PASS: result phase moved unchanged; race updater delegates without advancing race objects')
enqueue_failure = game.split('if(!nm_practice_render_enqueue(&practice)) {', 1)[1].split('}', 1)[0]
assert 'practice.failed=1; return;' in enqueue_failure
assert 'render_release' not in enqueue_failure and 'flag_shutdown' not in enqueue_failure
assert 'if (enj_state_get()->flags.shut_down || practice.failed)' in update
print('PASS: partial scene enqueue failure defers cleanup until the next updater')

assert 'nm_practice_check_matrix_construction' not in game
assert 'nm_inspect_camera_check' not in game
assert 'nm_intro_camera_reference(' not in game and 'i<65536' not in game
print('PASS: retired gameplay startup oracles are absent from the production loop')
