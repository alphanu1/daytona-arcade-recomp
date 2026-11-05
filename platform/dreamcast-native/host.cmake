# The Dreamcast-native port's host side (platform/dreamcast-native): the reference
# tools the asset recipe runs and the port's host tests. Included from the root
# CMakeLists.txt with -DM2_DREAMCAST_NATIVE=ON, after the root's targets; the
# default build does not read this file.
set(DN ${CMAKE_SOURCE_DIR}/platform/dreamcast-native)

# A C target of the port's: C11, its headers (implementation and headers have
# separate roots on host and SH4), libm where that is a library of its own.
function(native_c_target target)
    set_property(TARGET ${target} PROPERTY C_STANDARD 11)
    target_include_directories(${target} PRIVATE ${DN}/include)
    if(UNIX AND NOT APPLE)
        target_link_libraries(${target} PRIVATE m)
    endif()
endfunction()

# The shared runtime again, with the hooks of M2_DC_NATIVE_HOST (call and return
# observers, geometry inspection): `runtime` stays as the default build has it.
get_target_property(NATIVE_RUNTIME_SOURCES runtime SOURCES)
add_library(native_runtime STATIC ${NATIVE_RUNTIME_SOURCES})
target_include_directories(native_runtime PUBLIC src)
target_compile_definitions(native_runtime PUBLIC M2_DC_NATIVE_HOST M2_ROMSET="${M2_ROMSET}")
target_link_libraries(native_runtime PUBLIC trace i960 ymfm)
if(TARGET lzma7z)
    target_link_libraries(native_runtime PUBLIC lzma7z)
    target_compile_definitions(native_runtime PRIVATE M2_HAVE_7Z)
endif()
add_library(native_refcore STATIC src/refcore/i960_ref.cpp)
target_link_libraries(native_refcore PUBLIC native_runtime)

# The ROM set importer of the recipe: the sound ROMs are not needed.
add_executable(m2import_silent ${DN}/tools/m2import_silent.cpp)
target_link_libraries(m2import_silent PRIVATE native_runtime)

# The recompiled game under the port's exports and checks, without the 68000
# (recipes/build_assets.sh generates no code for it).
if(EXISTS ${GEN_DIR}/gen_table.cpp AND EXISTS ${TGP_GEN})
    file(GLOB GEN_SOURCES ${GEN_DIR}/*.cpp)
    add_executable(m2segment ${DN}/tools/m2segment.cpp ${DN}/tools/no_sound.cpp
        ${DN}/code/drive.c ${DN}/code/contact.c ${DN}/code/path.c ${DN}/code/road.c
        ${DN}/code/expert_actors.c ${GEN_SOURCES} ${TGP_GEN})
    target_link_libraries(m2segment PRIVATE native_refcore)
    target_include_directories(m2segment PRIVATE ${DN}/include)
    if(NOT MSVC)
        set_source_files_properties(${GEN_SOURCES} ${TGP_GEN} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    endif()
endif()

add_executable(test_native_host_hooks ${DN}/tests/test_host_hooks.cpp)
target_link_libraries(test_native_host_hooks PRIVATE native_runtime)
add_test(NAME native_host_hooks COMMAND test_native_host_hooks)

add_executable(test_native_models ${DN}/tests/test_export_models.cpp)
target_include_directories(test_native_models PRIVATE src)
target_compile_definitions(test_native_models PRIVATE M2_DC_NATIVE_HOST)
add_test(NAME native_models COMMAND test_native_models)
add_executable(test_native_visibility ${DN}/tests/test_visibility.c ${DN}/code/visibility.c)
native_c_target(test_native_visibility)
add_test(NAME native_visibility COMMAND test_native_visibility)
add_executable(test_native_road ${DN}/tests/test_road.c ${DN}/code/road.c)
native_c_target(test_native_road)
add_test(NAME native_road COMMAND test_native_road)
add_executable(test_native_drive ${DN}/tests/test_drive.c ${DN}/code/drive.c)
native_c_target(test_native_drive)
add_test(NAME native_drive COMMAND test_native_drive)
add_executable(test_native_contact ${DN}/tests/test_contact.c ${DN}/code/contact.c ${DN}/code/drive.c)
native_c_target(test_native_contact)
add_test(NAME native_contact COMMAND test_native_contact)
set(NATIVE_RACE_TEST_ASSETS "" CACHE PATH "Owned prepared course0 packs for the autonomous native race check")
set(NATIVE_RACE_TEST_HEADERS "" CACHE PATH "Owned generated intro_drive_data.h directory for the native race check")
set(NATIVE_RACE_TEST_INTENTS "" CACHE PATH "Owned generated race_intent_data.h directory for silent native race events")
if(NATIVE_RACE_TEST_ASSETS AND NATIVE_RACE_TEST_HEADERS)
    add_executable(test_native_race_movement ${DN}/tests/test_race_movement.c
        ${DN}/code/drive.c ${DN}/code/contact.c
        ${DN}/code/road.c ${DN}/code/path.c ${DN}/code/visibility.c)
    native_c_target(test_native_race_movement)
    target_include_directories(test_native_race_movement PRIVATE ${NATIVE_RACE_TEST_HEADERS})
    if(NATIVE_RACE_TEST_INTENTS)
        target_include_directories(test_native_race_movement PRIVATE ${NATIVE_RACE_TEST_INTENTS})
        target_compile_definitions(test_native_race_movement PRIVATE NATIVE_INTENT_DATA)
    endif()
    add_test(NAME native_race_movement COMMAND test_native_race_movement
        ${NATIVE_RACE_TEST_ASSETS}/drive.dnd ${NATIVE_RACE_TEST_ASSETS}/course0.dnc
        ${NATIVE_RACE_TEST_ASSETS}/course0.dnr ${NATIVE_RACE_TEST_ASSETS})
endif()
add_executable(test_native_path ${DN}/tests/test_path.c ${DN}/code/path.c ${DN}/code/drive.c)
native_c_target(test_native_path)
add_test(NAME native_path COMMAND test_native_path)
add_executable(test_native_slot_machine ${DN}/tests/test_slot_machine.c)
native_c_target(test_native_slot_machine)
add_test(NAME native_slot_machine COMMAND test_native_slot_machine)
add_executable(test_native_wrong_way ${DN}/tests/test_wrong_way.c)
native_c_target(test_native_wrong_way)
add_test(NAME native_wrong_way COMMAND test_native_wrong_way)
add_executable(test_native_contact_clear ${DN}/tests/test_contact_clear.c)
native_c_target(test_native_contact_clear)
add_test(NAME native_contact_clear COMMAND test_native_contact_clear)
add_executable(check_native_meshpack ${DN}/tests/check_meshpack.c ${DN}/code/meshpack.c)
native_c_target(check_native_meshpack)
# material_pack.c inflates DNT3 textures through zlib (the console has KOS's).
# The recipe needs check_native_material_pack; without zlib the targets are left out.
find_package(ZLIB)
if(ZLIB_FOUND)
    add_executable(check_native_material_pack ${DN}/tests/check_material_pack.c ${DN}/code/material_pack.c)
    native_c_target(check_native_material_pack)
    target_link_libraries(check_native_material_pack PRIVATE ZLIB::ZLIB)
    add_executable(test_native_practice ${DN}/tests/test_practice.c
        ${DN}/code/drive.c ${DN}/code/contact.c
        ${DN}/code/road.c ${DN}/code/path.c
        ${DN}/code/geometry.c ${DN}/code/meshpack.c
        ${DN}/code/material_pack.c ${DN}/code/visibility.c)
    native_c_target(test_native_practice)
    # Requires user-owned prepared packs, passed explicitly; no ROM test fixture.
    target_link_libraries(test_native_practice PRIVATE ZLIB::ZLIB)
else()
    message(STATUS "zlib not found: skipping check_native_material_pack and test_native_practice")
endif()
add_executable(test_native_geometry ${DN}/tests/test_geometry.cpp
    ${DN}/code/geometry.c ${DN}/code/meshpack.c)
native_c_target(test_native_geometry)
target_include_directories(test_native_geometry PRIVATE src)
target_compile_options(test_native_geometry PRIVATE -ffp-contract=off)
add_test(NAME native_geometry COMMAND test_native_geometry)
# The tight HUD geometry (platform/dreamcast-native/include/render/ui/hud_draw.h) against the whole quads it
# replaced: every recorded HUD state rasterised both ways, compared pixel for pixel
# (platform/dreamcast-native/tests/check_hud_tight.c). NATIVE_HUD_TEST_PACK: tools/prepare_hud.py output
# (hud_data.h, hud/); NATIVE_HUD_TEST_ROWS: files of `trace_race_native --hud` rows, ";" between them.
set(NATIVE_HUD_TEST_PACK "" CACHE PATH "Prepared HUD pack (hud_data.h, hud/) for the tight HUD geometry check")
set(NATIVE_HUD_TEST_ROWS "" CACHE STRING "HUD row files of recorded races (trace_race_native --hud) for the tight HUD geometry check")
if(NATIVE_HUD_TEST_PACK AND NATIVE_HUD_TEST_ROWS)
    add_executable(check_native_hud_tight ${DN}/tests/check_hud_tight.c)
    native_c_target(check_native_hud_tight)
    target_include_directories(check_native_hud_tight PRIVATE ${NATIVE_HUD_TEST_PACK})
    target_compile_options(check_native_hud_tight PRIVATE -fno-fast-math -ffp-contract=off)
    add_test(NAME native_hud_tight COMMAND check_native_hud_tight ${NATIVE_HUD_TEST_PACK} ${NATIVE_HUD_TEST_ROWS})
endif()

add_executable(test_native_assets ${DN}/tests/test_dreamcast_assets.cpp)
target_link_libraries(test_native_assets PRIVATE native_runtime)
add_test(NAME native_assets COMMAND test_native_assets)

add_executable(test_native_pvr_geometry ${DN}/tests/test_pvr_geometry.cpp)
target_include_directories(test_native_pvr_geometry PRIVATE src)
add_test(NAME native_pvr_geometry COMMAND test_native_pvr_geometry)

add_executable(test_native_system24 ${DN}/tests/test_system24.cpp)
target_link_libraries(test_native_system24 PRIVATE native_runtime)
add_test(NAME native_system24 COMMAND test_native_system24)

# Slot machine lockstep (platform/dreamcast-native/tests/test_slot_lockstep.py): the native race harness in production mode
# against the original's runs with START presses. ROM output only, passed explicitly: INCLUDES the generated header
# directories (intro_drive_data.h of work/intro, race_intent_data.h, race_start_data.h, race_record_data.h,
# podium_data.h; ";" between them), ASSETS the course0 packs, TRACES the directories holding reference-slot-*.trace
# and reference-finish-fast.trace (m2segment --trace-race; ";" between them).
set(NATIVE_SLOT_TEST_INCLUDES "" CACHE STRING "Header dirs for tests/trace_race_native.c (';' between them)")
set(NATIVE_SLOT_TEST_ASSETS "" CACHE PATH "drive.dnd, course0.dn*, scenery.dnv, course0-route*.dnp")
set(NATIVE_SLOT_TEST_TRACES "" CACHE STRING "Dirs with reference-slot-*.trace / reference-finish-fast.trace (';')")
set(NATIVE_DAMAGE_TEST_TRACES "" CACHE STRING "Dirs with reference-damage-*.trace (';')")
if(Python3_FOUND AND NATIVE_SLOT_TEST_INCLUDES AND NATIVE_SLOT_TEST_ASSETS)
    add_executable(trace_race_native ${DN}/tests/trace_race_native.c ${DN}/code/drive.c
        ${DN}/code/contact.c ${DN}/code/road.c ${DN}/code/path.c
        ${DN}/code/visibility.c ${DN}/code/meshpack.c ${DN}/code/render/scene/damage_bodies.c
        ${DN}/code/props.c ${DN}/code/props_scene.c ${DN}/code/expert_actors.c)
    native_c_target(trace_race_native)
    target_include_directories(trace_race_native PRIVATE ${NATIVE_SLOT_TEST_INCLUDES})
    target_compile_options(trace_race_native PRIVATE -O2 -fno-fast-math -ffp-contract=off)
    target_compile_definitions(trace_race_native PRIVATE NM_TRACE_PARTS=1) # --parts (meshpack.c, damage_bodies.c)
    # Live-play robustness (platform/dreamcast-native/tests/fuzz_race_native.c): seeded random races with abusive pad input
    # (grass, spins, walls, wrong way, START at the slot machine, time out) through the production tick of
    # code/practice.c; the original never rejects a tick, so any reject fails, naming the step and the state.
    add_executable(fuzz_race_native ${DN}/tests/fuzz_race_native.c ${DN}/code/drive.c
        ${DN}/code/contact.c ${DN}/code/road.c ${DN}/code/path.c
        ${DN}/code/visibility.c)
    native_c_target(fuzz_race_native)
    target_include_directories(fuzz_race_native PRIVATE ${NATIVE_SLOT_TEST_INCLUDES})
    target_compile_options(fuzz_race_native PRIVATE -O2 -fno-fast-math -ffp-contract=off)
    # Seeds 1-300 (289: a contact leaves the player backwards, speed index -1, NaN drag; one race in three aims at
    # the pit lane at every lap's end: about 400 pit stops), plus seed 435: a race finished on profile 195 (the
    # finish entry's path cursor reads the route's guard records).
    add_test(NAME native_race_fuzz COMMAND fuzz_race_native ${NATIVE_SLOT_TEST_ASSETS} 1 300 40000)
    add_test(NAME native_race_fuzz_finish COMMAND fuzz_race_native ${NATIVE_SLOT_TEST_ASSETS} 435 1 40000)
    add_test(NAME native_slot_lockstep
        COMMAND Python3::Interpreter ${DN}/tests/test_slot_lockstep.py
                $<TARGET_FILE:trace_race_native> ${NATIVE_SLOT_TEST_ASSETS} "${NATIVE_SLOT_TEST_TRACES}")
    set_tests_properties(native_slot_lockstep PROPERTIES SKIP_RETURN_CODE 77)
    # Damage/wrong-way lockstep (tests/test_damage_lockstep.py): every player field on every row of the original's
    # wall-hit runs (reference-damage-*.trace from recipes/inputs/inputs-damage-*.txt, m2segment --trace-race), and
    # every opponent field (production: but the previous scene's tracking words; carry mode: all).
    add_test(NAME native_damage_lockstep
        COMMAND Python3::Interpreter ${DN}/tests/test_damage_lockstep.py
                $<TARGET_FILE:trace_race_native> ${NATIVE_SLOT_TEST_ASSETS} "${NATIVE_DAMAGE_TEST_TRACES}")
    set_tests_properties(native_damage_lockstep PROPERTIES SKIP_RETURN_CODE 77)
    # Pit stop lockstep (tests/test_pit_lockstep.py, include/pit.h): every player, timeline and opponent field on every
    # row of the original's five pit-lane runs (reference-pit-{P,E,C,F,H}.trace of spec-pitstop.md, m2segment
    # --trace-race), the pit camera's view and the native pit state by the spec's rules (production and carry modes).
    set(NATIVE_PIT_TEST_TRACES "" CACHE STRING "Dirs with reference-pit-*.trace (';')")
    add_test(NAME native_pit_lockstep
        COMMAND Python3::Interpreter ${DN}/tests/test_pit_lockstep.py
                $<TARGET_FILE:trace_race_native> ${NATIVE_SLOT_TEST_ASSETS} "${NATIVE_PIT_TEST_TRACES}")
    set_tests_properties(native_pit_lockstep PROPERTIES SKIP_RETURN_CODE 77)
    # Pit presentation (tests/test_pit_render.py): the native race of run P (--parts, --hud) against the pit log
    # log-P.bin (m2pit display lists and CPU records): the player's parts and matrices (wheels off, the jack's lift),
    # opponents hidden while 501a84 > 1, "PIT WORK!", no world actor while the crew works, and the six pit crew
    # figures (include/pit_crew.h): models, order and matrices of every crew draw. Runs E, C, F, H too where their
    # logs are in the log directories.
    set(NATIVE_PIT_TEST_LOGS "" CACHE STRING "Dirs with the pit log log-P.bin (';')")
    add_test(NAME native_pit_render
        COMMAND Python3::Interpreter ${DN}/tests/test_pit_render.py
                $<TARGET_FILE:trace_race_native> ${NATIVE_SLOT_TEST_ASSETS} "${NATIVE_PIT_TEST_TRACES}" "${NATIVE_PIT_TEST_LOGS}")
    set_tests_properties(native_pit_render PROPERTIES SKIP_RETURN_CODE 77)
    # The same with the crew's SH4ZAM chains (pit_crew.h NM_PIT_CREW_SHZ; SH4ZAM's software back-end from a checkout,
    # NATIVE_SH4ZAM_DIR with include/ and source/): every crew matrix also against the scalar reference.
    set(NATIVE_SH4ZAM_DIR "" CACHE PATH "SH4ZAM checkout (include/, source/) for the host check of the crew's SH4ZAM chains")
    if(NATIVE_SH4ZAM_DIR)
        add_executable(trace_race_native_shz ${DN}/tests/trace_race_native.c ${DN}/code/drive.c
            ${DN}/code/contact.c ${DN}/code/road.c ${DN}/code/path.c
            ${DN}/code/visibility.c ${DN}/code/meshpack.c ${DN}/code/render/scene/damage_bodies.c
            ${NATIVE_SH4ZAM_DIR}/source/sw/shz_xmtrx_sw.c ${NATIVE_SH4ZAM_DIR}/source/shz_xmtrx.c
            ${NATIVE_SH4ZAM_DIR}/source/shz_matrix.c)
        native_c_target(trace_race_native_shz)
        set_property(TARGET trace_race_native_shz PROPERTY C_EXTENSIONS ON)
        target_include_directories(trace_race_native_shz PRIVATE ${NATIVE_SLOT_TEST_INCLUDES}
            ${NATIVE_SH4ZAM_DIR}/include)
        target_compile_options(trace_race_native_shz PRIVATE -O2 -fno-fast-math -ffp-contract=off)
        target_compile_definitions(trace_race_native_shz PRIVATE NM_TRACE_PARTS=1 NM_PIT_CREW_SHZ=1)
        add_test(NAME native_pit_render_shz
            COMMAND Python3::Interpreter ${DN}/tests/test_pit_render.py
                    $<TARGET_FILE:trace_race_native_shz> ${NATIVE_SLOT_TEST_ASSETS} "${NATIVE_PIT_TEST_TRACES}" "${NATIVE_PIT_TEST_LOGS}")
        set_tests_properties(native_pit_render_shz PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    # Front-end lockstep (tests/test_frontend_lockstep.py, include/select.h, include/frontend.h): the native
    # course/transmission select on every iteration of the original's front-end traces (frontend-X.trace of
    # recipes/inputs/frontend-X.txt, m2segment --trace-frontend), and the races entered through the native attract,
    # game start and select (trace_race_native --frontend) on every row and field, collision tracking words included.
    set(NATIVE_FRONTEND_TEST_TRACES "" CACHE STRING "Dirs with frontend-*.trace and race-*.trace (';')")
    add_executable(trace_frontend_native ${DN}/tests/trace_frontend_native.c)
    native_c_target(trace_frontend_native)
    add_test(NAME native_frontend_lockstep
        COMMAND Python3::Interpreter ${DN}/tests/test_frontend_lockstep.py
                $<TARGET_FILE:trace_frontend_native> $<TARGET_FILE:trace_race_native> ${NATIVE_SLOT_TEST_ASSETS}
                "${NATIVE_FRONTEND_TEST_TRACES}")
    set_tests_properties(native_frontend_lockstep PROPERTIES SKIP_RETURN_CODE 77)
    # Games one after another (tests/test_games_lockstep.py, include/frontend_pool.h): the scenes chained through
    # trace_attract_native and trace_race_native against the original's runs of recipes/inputs/games-X.txt
    # (frontend-X.trace, race-X.trace: m2segment --trace-frontend --trace-race --trace-attract).
    set(NATIVE_GAMES_TEST_TRACES "" CACHE STRING "Dirs with frontend-X.trace and race-X.trace of games-X.txt (';')")
    add_executable(trace_attract_native ${DN}/tests/trace_attract_native.c ${DN}/code/drive.c
        ${DN}/code/contact.c ${DN}/code/road.c ${DN}/code/path.c ${DN}/code/visibility.c
        ${DN}/code/props.c ${DN}/code/props_scene.c ${DN}/code/expert_actors.c)
    native_c_target(trace_attract_native)
    target_include_directories(trace_attract_native PRIVATE ${NATIVE_SLOT_TEST_INCLUDES})
    target_compile_options(trace_attract_native PRIVATE -O2 -fno-fast-math -ffp-contract=off)
    add_test(NAME native_games_lockstep
        COMMAND Python3::Interpreter ${DN}/tests/test_games_lockstep.py
                $<TARGET_FILE:trace_race_native> $<TARGET_FILE:trace_attract_native> ${NATIVE_SLOT_TEST_ASSETS}
                "${NATIVE_GAMES_TEST_TRACES}")
    set_tests_properties(native_games_lockstep PROPERTIES SKIP_RETURN_CODE 77)
    # Live-play edge states (tests/test_negative_speed_lockstep.py): a negative speed index (the drag words before
    # and after the tables, NaN included) and the yaw step's cvtri overflow, against the original's runs in
    # NATIVE_NEGSPEED_TEST_TRACES (m2segment --trace-race, the speed written by M2_POKE_ROW/M2_POKE_SPEED).
    set(NATIVE_NEGSPEED_TEST_TRACES "" CACHE STRING "Dirs with reference-negspeed-*.trace, reference-yaw-overflow.trace (';')")
    add_test(NAME native_negative_speed_lockstep
        COMMAND Python3::Interpreter ${DN}/tests/test_negative_speed_lockstep.py
                $<TARGET_FILE:trace_race_native> ${NATIVE_SLOT_TEST_ASSETS} "${NATIVE_NEGSPEED_TEST_TRACES}")
    set_tests_properties(native_negative_speed_lockstep PROPERTIES SKIP_RETURN_CODE 77)
    # The player's drawn parts with damage (tests/test_damage_render.py): the rules of player_parts.h on every
    # race frame of the original's display-list logs (log-E.bin, log-V.bin of spec-damage.md: damage_log v3), and
    # the native race of run E (trace_race_native --parts) against log E, model by model and matrix by matrix.
    set(NATIVE_DAMAGE_TEST_LOGS "" CACHE PATH "Dir with the damage display-list logs log-E.bin, log-V.bin")
    add_executable(player_parts_check ${DN}/tests/player_parts_check.c ${DN}/code/drive.c
        ${DN}/code/road.c)
    native_c_target(player_parts_check)
    target_compile_options(player_parts_check PRIVATE -O2 -fno-fast-math -ffp-contract=off)
    add_test(NAME native_damage_render
        COMMAND Python3::Interpreter ${DN}/tests/test_damage_render.py
                $<TARGET_FILE:trace_race_native> $<TARGET_FILE:player_parts_check> ${NATIVE_SLOT_TEST_ASSETS}
                "${NATIVE_DAMAGE_TEST_TRACES}" "${NATIVE_DAMAGE_TEST_LOGS}")
    set_tests_properties(native_damage_render PROPERTIES SKIP_RETURN_CODE 77)
    # The player's car effects (tests/test_effects_render.py, include/car_effects.h): the rules on every 9088 call of
    # the original's effects logs (fx-*.fx of build/dc-native-effects.*/ref/record.sh: draws, order, local matrices),
    # and the native races of the runs with a reference trace against the logs' display lists.
    set(NATIVE_FX_TEST_LOGS "" CACHE STRING "Dirs with the effects logs fx-{A,C,E,F,G,H,V,fast}.fx (';')")
    set(NATIVE_FX_TEST_ROM "" CACHE PATH "Dir with the daytona93 images program.bin, main_data.bin")
    add_executable(car_effects_check ${DN}/tests/car_effects_check.c ${DN}/code/drive.c)
    native_c_target(car_effects_check)
    target_compile_options(car_effects_check PRIVATE -O2 -fno-fast-math -ffp-contract=off)
    add_test(NAME native_effects_render
        COMMAND Python3::Interpreter ${DN}/tests/test_effects_render.py
                $<TARGET_FILE:car_effects_check> $<TARGET_FILE:trace_race_native> ${NATIVE_SLOT_TEST_ASSETS}
                "${NATIVE_DAMAGE_TEST_TRACES};${NATIVE_SLOT_TEST_TRACES}" "${NATIVE_FX_TEST_LOGS}" "${NATIVE_FX_TEST_ROM}")
    set_tests_properties(native_effects_render PROPERTIES SKIP_RETURN_CODE 77)
endif()
if(Python3_FOUND)
    add_test(NAME native_race_intent_assets
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_race_intents.py)
    add_test(NAME native_visibility_pack
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_visibility.py $<TARGET_FILE:test_native_visibility>)
    add_test(NAME native_road_pack
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_road.py $<TARGET_FILE:test_native_road>)
    add_test(NAME native_path_pack
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_path.py $<TARGET_FILE:test_native_path>)
    add_test(NAME native_contact_pack
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_contact.py $<TARGET_FILE:test_native_contact>)
    add_test(NAME native_drive_pack
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_drive.py $<TARGET_FILE:test_native_drive>)
    add_test(NAME native_course
        COMMAND Python3::Interpreter ${DN}/tools/inspect_course.py --self-test)
    add_test(NAME native_camera
        COMMAND Python3::Interpreter ${DN}/tools/check_camera.py --self-test)
    add_test(NAME native_material_decode
        COMMAND Python3::Interpreter ${DN}/tests/test_material_decode.py)
    if(TARGET check_native_material_pack)
        add_test(NAME native_material_pack
            COMMAND Python3::Interpreter ${DN}/tests/test_prepare_materials.py
                    $<TARGET_FILE:check_native_material_pack>)
    endif()
    # The model renderer's PVR stream against the host reference renderer
    # (platform/dreamcast-native/tests/reference_stream.h) over a recorded script, stages 3 and 5.
    set(NATIVE_STREAM_TEST_ASSETS "" CACHE PATH "Prepared car/world packs for the native stream reference check")
    set(NATIVE_STREAM_TEST_FRAMES "" CACHE FILEPATH "Frame table (.frm) of a recorded script for the native stream reference check")
    if(NATIVE_STREAM_TEST_ASSETS AND NATIVE_STREAM_TEST_FRAMES)
        add_test(NAME native_stream_reference
            COMMAND Python3::Interpreter ${DN}/tests/stream_sim/check_stream.py
                    ${NATIVE_STREAM_TEST_ASSETS} ${NATIVE_STREAM_TEST_FRAMES})
    endif()
    # The console heap of a build from its packs (platform/dreamcast-native/tools/heap_budget.py): allocator model,
    # pack arithmetic, the hardware runs of 13 October 2026 when their asset sets are under build/, and,
    # with NATIVE_HEAP_TEST_ASSETS, that set against the budget (2 MiB never taken at the peak, 3 MiB free
    # while the scene runs) at NATIVE_HEAP_TEST_STAGE, without and with the sample profile's buffer.
    set(NATIVE_HEAP_TEST_ASSETS "" CACHE PATH "Prepared packs whose Dreamcast heap must stay inside the budget")
    set(NATIVE_HEAP_TEST_STAGE "6" CACHE STRING "Scenery stage of the native heap budget check")
    if(NATIVE_HEAP_TEST_ASSETS)
        add_test(NAME native_heap_budget
            COMMAND Python3::Interpreter ${DN}/tests/test_heap_budget.py
                    ${NATIVE_HEAP_TEST_ASSETS} ${NATIVE_HEAP_TEST_STAGE})
    else()
        add_test(NAME native_heap_budget
            COMMAND Python3::Interpreter ${DN}/tests/test_heap_budget.py)
    endif()
    if(ZLIB_FOUND)
        add_test(NAME native_material_residency
            COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}"
                    $<TARGET_FILE:Python3::Interpreter> ${DN}/tests/test_material_residency.py)
    endif()
    # Palette RAM format (make NATIVE_PAL16): conversion rules, and with the heap check's race assets (and the HUD
    # pack) every palette the program writes against its ARGB8888 source, default build and ARGB1555.
    set(NATIVE_PALETTE_TEST_ARGS)
    if(NATIVE_HEAP_TEST_ASSETS)
        list(APPEND NATIVE_PALETTE_TEST_ARGS ${NATIVE_HEAP_TEST_ASSETS})
        if(NATIVE_HUD_TEST_PACK)
            list(APPEND NATIVE_PALETTE_TEST_ARGS ${NATIVE_HUD_TEST_PACK})
        endif()
    endif()
    add_test(NAME native_palette_ram
        COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}"
                $<TARGET_FILE:Python3::Interpreter> ${DN}/tests/test_palette_ram.py
                ${NATIVE_PALETTE_TEST_ARGS})
    add_test(NAME native_meshpack
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_models.py
                $<TARGET_FILE:check_native_meshpack>)
    add_test(NAME native_checker_prune
        COMMAND Python3::Interpreter ${DN}/tests/test_prune_checker_faces.py
                $<TARGET_FILE:check_native_meshpack>)
    add_test(NAME native_flat_cutouts
        COMMAND Python3::Interpreter ${DN}/tests/test_flat_cutouts.py
                $<TARGET_FILE:check_native_meshpack>)
    add_test(NAME native_asset_rules
        COMMAND Python3::Interpreter ${DN}/tests/test_asset_rules.py
                $<TARGET_FILE:check_native_meshpack>)
    add_test(NAME native_strip_pack
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_strips.py
                $<TARGET_FILE:check_native_meshpack>)
    add_test(NAME native_face_order
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_face_order.py
                $<TARGET_FILE:check_native_meshpack>)
    add_test(NAME native_split_cutouts
        COMMAND Python3::Interpreter ${DN}/tests/test_split_cutouts.py
                $<TARGET_FILE:check_native_meshpack>)
    set_tests_properties(native_split_cutouts PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME native_opaque_cores
        COMMAND Python3::Interpreter ${DN}/tests/test_opaque_cores.py
                $<TARGET_FILE:check_native_meshpack>)
    set_tests_properties(native_opaque_cores PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME native_lod_groups
        COMMAND Python3::Interpreter ${DN}/tests/test_lod_groups.py
                $<TARGET_FILE:check_native_meshpack>)
    set_tests_properties(native_lod_groups PROPERTIES SKIP_RETURN_CODE 77)
    # NATIVE_DIAG_LATE_CAPTURE (platform/dreamcast-native/code/late_capture.c): frame buffer copy and 2x2 box against numpy,
    # slot policy, file and index, tools/late_capture_report.py (needs numpy and Pillow, else skipped).
    add_test(NAME native_late_capture
        COMMAND Python3::Interpreter ${DN}/tests/test_late_capture.py)
    set_tests_properties(native_late_capture PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME native_meshpack_alignment
        COMMAND Python3::Interpreter ${DN}/tests/test_align_meshpack.py
                $<TARGET_FILE:check_native_meshpack>)
    add_test(NAME native_prepare_textures
        COMMAND Python3::Interpreter ${DN}/tests/test_prepare_dreamcast_textures.py)
endif()
