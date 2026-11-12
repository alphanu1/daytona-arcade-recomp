# Daytona USA PC: the Dreamcast-native source port (platform/dreamcast-native)
# built for Windows/Linux/macOS on SDL3 + Vulkan.
#
#   cmake -S platform/pc-native -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DDAYTONA_ASSETS=build/my-assets
#   cmake --build build/pc
#
# DAYTONA_ASSETS is the output directory of
# platform/dreamcast-native/recipes/build_assets.sh (or of
# platform/pc-native/recipes/build_pc_assets.sh, which runs it): the game
# compiles ROM-derived tables from its headers/ and reads assets-ssfar4f/ at
# run time.  Nothing derived from the ROM is in this repository.
cmake_minimum_required(VERSION 3.20)
project(daytona_pc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 23)   # enDjinn's embeds use C23 alignas
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DN "${REPO}/platform/dreamcast-native")
set(PC "${CMAKE_CURRENT_SOURCE_DIR}")
set(ENJ "${REPO}/extern/endjinn")

set(DAYTONA_ASSETS "" CACHE PATH "build_assets.sh output directory (headers/ and assets-ssfar4f/)")
if(NOT DAYTONA_ASSETS OR NOT EXISTS "${DAYTONA_ASSETS}/headers/race-header")
  message(FATAL_ERROR "Set -DDAYTONA_ASSETS to a finished build_assets.sh output directory "
                      "(it must contain headers/race-header).")
endif()
get_filename_component(DAYTONA_ASSETS "${DAYTONA_ASSETS}" ABSOLUTE)
set(HD "${DAYTONA_ASSETS}/headers")

find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(ZLIB REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)

# --------------------------------------------------------------- SH4ZAM (portable C back end)
set(SHZ_BACKEND SW CACHE STRING "" FORCE)
set(SHZ_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/extern/sh4zam" sh4zam EXCLUDE_FROM_ALL)

# --------------------------------------------------------------- the game's build options
# The Dreamcast build's defaults (local.cfg.mk) as compile definitions, captured
# from make by recipes/capture_flags.sh into DAYTONA_ASSETS/pc-flags.cmake.
set(DAYTONA_FLAGS_FILE "${DAYTONA_ASSETS}/pc-flags.cmake" CACHE FILEPATH "captured game options (recipes/capture_flags.sh)")
if(NOT EXISTS "${DAYTONA_FLAGS_FILE}")
  execute_process(
    COMMAND sh "${PC}/recipes/capture_flags.sh" "${DAYTONA_ASSETS}" "${DAYTONA_FLAGS_FILE}"
    RESULT_VARIABLE capture_result)
  if(NOT capture_result EQUAL 0)
    message(FATAL_ERROR "capture_flags.sh failed")
  endif()
endif()
include("${DAYTONA_FLAGS_FILE}")   # sets DAYTONA_GAME_DEFINES

# --------------------------------------------------------------- shaders, embedded
set(SHADER_DIR "${PC}/backend/shaders")
set(SPV_FILES)
foreach(shader flat.vert pvr.frag flat_modifier.frag)
  set(spv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")
  add_custom_command(
    OUTPUT "${spv}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${GLSLC}" -O -I "${SHADER_DIR}" "${SHADER_DIR}/${shader}" -o "${spv}"
    DEPENDS "${SHADER_DIR}/${shader}" "${SHADER_DIR}/pvr_state.glsl"
    VERBATIM)
  list(APPEND SPV_FILES "${spv}")
endforeach()
set(SHADER_INC "${CMAKE_CURRENT_BINARY_DIR}/generated/pc_shaders.inc")
add_custom_command(
  OUTPUT "${SHADER_INC}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${SHADER_INC}" "-DINPUTS=${SPV_FILES}" -P "${PC}/cmake/embed_shaders.cmake"
  DEPENDS ${SPV_FILES} "${PC}/cmake/embed_shaders.cmake"
  VERBATIM)

# --------------------------------------------------------------- sources
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS "${DN}/code/*.c")
list(REMOVE_ITEM GAME_SOURCES
  "${DN}/code/main.c"              # platform/pc-native/code/pc_main.c
  "${DN}/code/audio/sound.c"       # platform/pc-native/audio/pc_sound.cpp
  "${DN}/code/audio/sfx.c"
  "${DN}/code/audio/idle_wait.c")
file(GLOB ENJ_SOURCES CONFIGURE_DEPENDS "${ENJ}/code/*.c")
file(GLOB BACKEND_SOURCES CONFIGURE_DEPENDS "${PC}/backend/*.cpp")

add_executable(daytona
  ${GAME_SOURCES}
  ${ENJ_SOURCES}
  ${BACKEND_SOURCES}
  "${PC}/kos-src/pvr_prim.c"
  "${PC}/code/pc_main.c"
  "${PC}/code/pc_horse.c"
  "${PC}/code/pc_saturn_menu.c"
  "${PC}/code/pc_saturn_options.c"
  "${PC}/audio/pc_sound.cpp"
  "${REPO}/src/runtime/native_sound_engine.cpp"
  "${REPO}/src/runtime/native_sound_sequencer.cpp"
  "${REPO}/src/runtime/native_sample_mixer.cpp"
  "${SHADER_INC}")

target_include_directories(daytona PRIVATE
  "${HD}/pc"                       # PC-only generated headers: reva_start_camera.h (from the daytona set), if made
  "${PC}/include"                  # KOS's own PVR headers, adapted to the host
  "${PC}/backend/include"          # the rest of the KOS surface (pc-enDjinn)
  "${PC}/backend"
  "${PC}/kos-src"
  "${PC}/code"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${DN}/include"
  "${DN}/code"
  "${DN}/tests"
  "${ENJ}/include"
  "${REPO}/src"
  "${HD}/race-header" "${HD}/name" "${HD}/result-pal4" "${HD}/result-data" "${HD}/result-labels"
  "${HD}/record-data" "${HD}/intent-data" "${HD}/podium-data" "${HD}/race-start-data" "${HD}/hud"
  "${HD}/title" "${HD}/slot-glass" "${HD}/podium-screen" "${HD}/attract-2d")

target_compile_definitions(daytona PRIVATE
  ENJ_TARGET_PC_ENDJINN=1
  DAYTONA_PC=1
  NM_HOST_BAKED_WORLD=1     # the console's store-queue stream path, in C (as tests/stream_sim builds it)
  "ENJ_CBASEPATH=\"./\""   # enDjinn debug stats only
  ${DAYTONA_GAME_DEFINES})

# Gameplay arithmetic stays exact (the Dreamcast build's flags): it is checked
# tick for tick against the arcade.  Presentation may use fast math, as there.
set(GAME_C_FLAGS
  -fno-fast-math -ffp-contract=off
  "SHELL:-include ${DN}/include/finite.h"
  "SHELL:-include ${REPO}/extern/sh4zam/include/sh4zam/shz_sh4zam.h"
  -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-pointer-to-int-cast
  -Wno-int-to-pointer-cast -Wno-deprecated-declarations)
set(GAME_CXX_FLAGS -ffp-contract=off -Wno-deprecated-declarations)
target_compile_options(daytona PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:${GAME_C_FLAGS}>"
  "$<$<COMPILE_LANGUAGE:CXX>:${GAME_CXX_FLAGS}>")
file(GLOB_RECURSE RENDER_SOURCES CONFIGURE_DEPENDS "${DN}/code/render/*.c")
set_source_files_properties(${RENDER_SOURCES} "${DN}/code/geometry.c"
  PROPERTIES COMPILE_OPTIONS "-ffast-math;-fno-finite-math-only")

target_link_libraries(daytona PRIVATE sh4zam SDL3::SDL3 Vulkan::Vulkan ZLIB::ZLIB)
if(NOT WIN32)
  target_link_libraries(daytona PRIVATE m pthread)
endif()

# Run from the build directory: assets next to the executable.
add_custom_command(TARGET daytona POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E echo "Run: $<TARGET_FILE:daytona> ${DAYTONA_ASSETS}/assets-ssfar4f")
