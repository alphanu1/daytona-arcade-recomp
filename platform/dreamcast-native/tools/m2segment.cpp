// m2segment: the recompiled game under the native port's exports and checks.
// It runs a deterministic scenario, silently, and writes what the asset recipe
// converts (recipes/build_assets.sh) or compares native code with the original
// i960 and TGP. Outputs are ROM-derived: keep them under ignored build
// directories and never distribute them. Built with -DM2_DREAMCAST_NATIVE=ON.
//
// m2segment IMAGES_DIR FRAMES [--inputs FILE] [options, see usage()]
#include "runtime/game_loop.h"
#include "dreamcast_assets.h"
#include "../../../tools/common/input_script.h"
#include "../../../tools/common/nvram.h"
#include "export_scene.hpp"
#include "export_models.hpp"
#include "export_name_exit.hpp"
#include "export_course.hpp"
#include "export_objects.hpp"
#include "check_road.hpp"
#include "check_contact.hpp"
#include "check_path.hpp"
#include "check_race.hpp"
#include "trace_race.hpp"
#include "trace_frontend.hpp"
#include "export_select.hpp"
#include "trace_hud.hpp"
#include "check_visibility.hpp"
#include "check_follow_camera.hpp"
#include "check_expert_actors.hpp"
#include "export_sky.hpp"

#include <cinttypes>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Stage {
    std::string name;
    uint64_t start = 0, end = 0;
};

std::vector<uint8_t> read_image(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path + " (run the importer first)");
    return {std::istreambuf_iterator<char>(f), {}};
}

// The game's images without the sound ROMs: no sound board is built here.
rt::M2Board::Images load_images(const std::string &dir) {
    rt::M2Board::Images images;
    images.program = read_image(dir + "/program.bin");
    images.main_data = read_image(dir + "/main_data.bin");
    images.copro_tables = read_image(dir + "/copro_tables.bin");
    images.copro_data = read_image(dir + "/copro_data.bin");
    images.polygons = read_image(dir + "/polygons.bin");
    images.textures = read_image(dir + "/textures.bin");
    return images;
}

uint64_t number(const std::string &text, const char *what) {
    char *end = nullptr;
    errno = 0;
    const uint64_t value = std::strtoull(text.c_str(), &end, 10);
    if (text.empty() || text.front() < '0' || text.front() > '9' || errno == ERANGE || !end || *end)
        throw std::runtime_error(std::string("bad ") + what + ": " + text);
    return value;
}

Stage parse_stage(const char *text) {
    const std::string value(text);
    const size_t colon = value.rfind(':');
    const size_t dash = colon == std::string::npos ? std::string::npos : value.find('-', colon + 1);
    if (colon == std::string::npos || dash == std::string::npos || !colon || dash == colon + 1 || dash + 1 == value.size())
        throw std::runtime_error("stage must be NAME:START-END");
    Stage stage{value.substr(0, colon), number(value.substr(colon + 1, dash - colon - 1), "stage start"),
                number(value.substr(dash + 1), "stage end")};
    if (stage.end < stage.start) throw std::runtime_error("stage end precedes its start");
    return stage;
}

void usage() {
    std::fprintf(stderr, "--export-name-exit NEW_FILE extracts original phase5/7 name matrices after the scenario (host only).\n");
    std::fprintf(stderr, "--check-native-reflection verifies observed window geometry against ROM, then native texture control/uploads against original i960.\n");
    std::fprintf(stderr, "--dump-video NEW_DIR writes the video tables after the last frame (palram, colorxlat, lumaram, tex0, tex1 .bin: the reference dump of tools/prepare_palette_materials.py and its users).\n");
    std::fprintf(stderr, "--export-native-sky NEW_DIR prepares course0 panorama and checks original horizontal tile updates after the scenario.\n");
    std::fprintf(stderr, "--check-native-wheel-pose DND_PACK checks normal external-view wheel matrices against original i960/TGP after the scenario.\n");
    std::fprintf(stderr, "--check-native-expert-actors DND_PACK DNC_PACK checks Expert's world actors (include/expert_actors.h) against their original ticks on the i960 and TGP after an Expert scenario (course1.dnc).\n");
    std::fprintf(stderr, "--check-native-visibility DND_PACK checks native scenery camera masks against original i960/TGP after the scenario.\n");
    std::fprintf(stderr, "--check-native-follow-camera DND_PACK checks normal mode0/reset/VR3 view matrices against original i960/TGP after the scenario.\n");
    std::fprintf(stderr, "--check-native-road-pack DNR_PACK DND_PACK PATH_DIRECTORY checks profiles/flags and whole10980 using all eight courseS-routeR.dnp packs; with --check-native-drive-live and --check-native-contact also checks the connected player block through16da4.\n");
    std::fprintf(stderr, "--check-native-road checks native controls and lateral/spawn math against i960/TGP after the scenario (host only).\n");
    std::fprintf(stderr, "--check-native-drive PACK checks native vehicle operations using a DND15 pack (host only).\n");
    std::fprintf(stderr, "--check-native-contact DNC_PACK DND_PACK checks complete ground/contact after the scenario; with --check-native-drive-live also checks actual ground calls (host only).\n");
    std::fprintf(stderr, "--check-native-path DNP_PACK DND_PACK checks authored rows, cursors and PLAYER guided movement against i960/TGP (host only; includes rejected conversions).\n");
    std::fprintf(stderr, "--trace-race NEW_FILE writes a host-only per-frame race trace (phase, timer, decoded controls, every car) of the original for the free-running native comparison; not combinable with live/reflection observers.\n");
    std::fprintf(stderr, "--trace-clock NEW_FILE with --trace-race: the calls of 1786c the frame-counter animations of Advanced make (platform/dreamcast-native/include/clock_actors.h), for tests/test_clock_lockstep.py.\n");
    std::fprintf(stderr, "--trace-clock-expert with --trace-clock: also the draws of Expert's world actors with the TGP's matrix and the view each frame (platform/dreamcast-native/include/expert_actors.h), for tests/test_expert_lockstep.py.\n");
    std::fprintf(stderr, "--trace-props NEW_FILE with --trace-race: every tick of the course props as a run of platform/dreamcast-native/tests/test_props.c (spec in platform/dreamcast-native/tools/trace_race.hpp).\n");
    std::fprintf(stderr, "--dump-frames DIR FIRST LAST EVERY writes the screen of the frames FIRST..LAST (every EVERY-th) as raw 0xAARRGGBB dumps DIR/frame_N.rgb (scripts/rgb2png.py), their tile RAM DIR/tiles_N.bin and final pen colours DIR/pens_N.bin (host only, game output: keep under build/).\n");
    std::fprintf(stderr, "--dump-2d DIR FIRST LAST EVERY as --dump-frames without the screens: the tile RAM of each frame, the pens of the first (tools/prepare_attract_2d.py, tests/test_attract_2d.c).\n");
    std::fprintf(stderr, "--trace-attract with --trace-race: rows in the attract loop too (phases 2..11; cars in the cinematic and the V.R. demo, 3..7), for tests/test_attract_lockstep.py.\n");
    std::fprintf(stderr, "--trace-frontend NEW_FILE writes a host-only per-iteration trace of the original's front end (attract, game start, course/transmission select, race entry, game over: phase bytes, select state, its inputs, the frame counter) and the entity pool around the game start; combinable with --trace-race.\n");
    std::fprintf(stderr, "--trace-sound NEW_FILE writes a host-only line per frame in which the original sent sound-board UART bytes (phase, course, timer, raw bytes in hex).\n");
    std::fprintf(stderr, "--trace-hud NEW_DIR EVERY writes a host-only per-frame trace of what the original race HUD displays (tile words and source variables) and, every EVERY rows (0=never), a snapshot of the 2D state and polygon list.\n");
    std::fprintf(stderr, "--check-native-race checks native active-race timer/finish decisions against original i960 (host only).\n");
    std::fprintf(stderr, "--check-native-drive-live also checks actual player traction/yaw entries and returns during the scenario; requires --check-native-drive.\n");
    std::fprintf(stderr, "--inspect-native-vehicles verifies the live vehicle-link pool and prints the 14-entry neighborhood (not road geometry).\n");
    std::fprintf(stderr, "--export-native-models NEW_DIR extracts verified local-coordinate source meshes.\n");
    std::fprintf(stderr, "--export-native-intro-cars NEW_DIR extracts every authored course0 intro body/wheel model, snapshot materials.\n");
    std::fprintf(stderr, "--export-native-race-cars NEW_DIR includes all40 course0 cars and the intro parts.\n");
    std::fprintf(stderr, "--export-native-finish-models NEW_DIR extracts four authored finish parts; snapshot material context, no animation.\n");
    std::fprintf(stderr, "--export-native-result-models NEW_DIR extracts eight authored result parts; validation pose only.\n");
    std::fprintf(stderr, "--export-native-name-models NEW_DIR extracts29 name glyphs and marker; validation pose only.\n");
    std::fprintf(stderr, "--export-native-windows NEW_DIR extracts ROM-initialized player windows with base UVs and full-axis/all-luma materials.\n");
    std::fprintf(stderr, "--export-native-player-car NEW_DIR combines canonical windows with normal body, shell and wheel ROM parts.\n");
    std::fprintf(stderr, "--all-solid-lights includes every luma for discovered native solid materials (no extra textures).\n");
    std::fprintf(stderr, "--native-model-all-lights DECIMAL_ADDRESS selects a ROM part and exports every luma/UV union; repeat for multiple parts, all must occur.\n");
    std::fprintf(stderr, "--export-native-cells NEW_DIR inspects all authored scenery cells using a visible cell's render state; separate from --export-native-models.\n");
    std::fprintf(stderr, "--export-native-objects LISTS_TXT NEW_DIR extracts the models of the named 16-byte object lists (hex data ROM addresses) with a visible cell's render state; see platform/dreamcast-native/tools/prepare_world_objects.py.\n");
    std::fprintf(stderr, "--race-scenery-lights prepares static world-space cell materials for the normal-race light and rigid views; requires --export-native-cells.\n");
    std::fprintf(stderr, "--translation-lights RADIUS exports possible supplied-normal shades in a +/-RADIUS view-translation box; includes --all-solid-lights.\n");
    std::fprintf(stderr, "--check-native-title MOTION_BIN checks the first no-input title against native poses.\n");
    std::fprintf(stderr, "--probe-native-2d NAME:START-END reports frame changes (zero-based input range).\n");
    std::fprintf(stderr, "--export-native-select NEW_DIR exports the course/transmission select and GENTLEMEN screens' source art from a run through them (tools/prepare_select.py).\n");
    std::fprintf(stderr, "--export-native-2d NEW_DIR exports a static source-art layout (refuses 3D frames).\n");
    std::fprintf(stderr, "--export-result-label-colors NEW_FILE runs the result source oracle and exports its shared5454 palette/current colour translation.\n");
    std::fprintf(stderr, "--export-name-label-colors NEW_FILE exports the name5454+5460 palette/current colour translation after its source checks.\n");
    std::fprintf(stderr, "--nvram DIR starts from the settings EEPROM and backup RAM in DIR (ioboard_eeprom.bin, backup_ram.bin; tools/common/nvram.h), e.g. operator settings written by platform/dreamcast-native/tools/settings_eeprom.py; a missing file stays the board's erased default. --save-nvram DIR writes them as the run left them (an existing DIR).\n");
    std::fprintf(stderr, "--catalog NEW_DIR samples a frame every 120 as frame_N.tga, and logs phase and updater changes.\n");
    std::fprintf(stderr, "usage: m2segment IMAGES_DIR FRAMES [--inputs FILE] [--nvram DIR] [--save-nvram DIR] [the options above]\n");
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    try {
        const std::string images = argv[1];
        const uint64_t frames = number(argv[2], "frame count");
        if (!frames) throw std::runtime_error("frame count must be positive");
        std::string inputs, catalog, native_scene, select_output;
        std::string video_dump, nvram_dir, save_nvram_dir;
        std::string native_models, native_cells, native_drive, native_windows, native_sky, native_intro_cars;
        std::string native_contact, contact_math;
        std::string native_path, path_math;
        std::string native_road_pack, road_math, road_paths;
        daytona::native::ModelExport model_export;
        daytona::native::CourseExport course_export;
        daytona::native::ObjectExport object_export;
        std::string native_objects;
        daytona::native::IntroCarExport intro_car_export;
        bool native_race_cars=false,native_finish_models=false,native_result_models=false,native_name_models=false;
        daytona::native::PlayerWindowExport window_export;
        bool all_solid_lights = false;
        bool inspect_vehicles = false;
        bool check_reflection = false;
        bool check_native_road = false;
        std::string visibility_math, follow_camera_math, wheel_math, expert_math, expert_contact;
        bool check_live_drive = false;
        Stage probe_range;
        daytona::native::SceneProbe scene_probe;
        daytona::native::TitleCheck title_check;
        bool check_native_race=false;
        std::string result_label_colors, name_label_colors;
        std::string name_exit_output;
        std::string race_trace_output, hud_trace_output, sound_trace_output, frontend_trace_output, clock_trace_output, props_trace_output;
        bool clock_trace_expert = false;
        uint64_t hud_snapshot_every = 0;
        bool trace_attract = false;
        std::string frame_dump; uint64_t dump_first = 0, dump_last = 0, dump_every = 1; bool dump_screens = true;
        for (int i = 3; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--inputs") && i + 1 < argc) inputs = argv[++i];
            else if (!std::strcmp(argv[i], "--nvram") && i + 1 < argc) nvram_dir = argv[++i];
            else if (!std::strcmp(argv[i], "--save-nvram") && i + 1 < argc) save_nvram_dir = argv[++i];
            else if (!std::strcmp(argv[i], "--export-name-exit") && i+1<argc) name_exit_output=argv[++i];
            else if (!std::strcmp(argv[i], "--check-native-race")) check_native_race=true;
            else if (!std::strcmp(argv[i], "--trace-race") && i+1<argc) race_trace_output=argv[++i];
            else if (!std::strcmp(argv[i], "--trace-clock") && i+1<argc) clock_trace_output=argv[++i];
            else if (!std::strcmp(argv[i], "--trace-clock-expert")) clock_trace_expert=true;
            else if (!std::strcmp(argv[i], "--trace-props") && i+1<argc) props_trace_output=argv[++i];
            else if (!std::strcmp(argv[i], "--trace-attract")) trace_attract=true;
            else if ((!std::strcmp(argv[i], "--dump-frames") || !std::strcmp(argv[i], "--dump-2d")) && i+4<argc) {
                dump_screens = !std::strcmp(argv[i], "--dump-frames");
                frame_dump=argv[++i]; dump_first=std::strtoull(argv[++i],nullptr,10);
                dump_last=std::strtoull(argv[++i],nullptr,10); dump_every=std::strtoull(argv[++i],nullptr,10);
                if (!dump_every) dump_every=1;
            }
            else if (!std::strcmp(argv[i], "--trace-sound") && i+1<argc) sound_trace_output=argv[++i];
            else if (!std::strcmp(argv[i], "--trace-frontend") && i+1<argc) frontend_trace_output=argv[++i];
            else if (!std::strcmp(argv[i], "--trace-hud") && i+2<argc) { hud_trace_output=argv[++i]; hud_snapshot_every=std::strtoull(argv[++i],nullptr,10); }
            else if (!std::strcmp(argv[i], "--export-result-label-colors") && i+1<argc) {
                result_label_colors=argv[++i];check_native_race=true;
            }
            else if (!std::strcmp(argv[i], "--export-name-label-colors") && i+1<argc) {
                name_label_colors=argv[++i];check_native_race=true;
            }
            else if (!std::strcmp(argv[i], "--dump-video") && i + 1 < argc) video_dump = argv[++i];
            else if (!std::strcmp(argv[i], "--catalog") && i + 1 < argc) catalog = argv[++i];
            else if (!std::strcmp(argv[i], "--export-native-2d") && i + 1 < argc) native_scene = argv[++i];
            else if (!std::strcmp(argv[i], "--export-native-select") && i + 1 < argc) select_output = argv[++i];
            else if (!std::strcmp(argv[i], "--export-native-models") && i + 1 < argc) native_models = argv[++i];
            else if (!std::strcmp(argv[i], "--export-native-windows") && i + 1 < argc) native_windows = argv[++i];
            else if (!std::strcmp(argv[i], "--export-native-player-car") && i + 1 < argc) {
                native_windows=argv[++i];window_export.car_parts=true;
            }
            else if (!std::strcmp(argv[i], "--export-native-cells") && i + 1 < argc) native_cells = argv[++i];
            else if (!std::strcmp(argv[i], "--export-native-objects") && i + 2 < argc) { object_export.read_lists(argv[++i]); native_objects = argv[++i]; }
            else if ((!std::strcmp(argv[i], "--export-native-intro-cars") ||
                      !std::strcmp(argv[i], "--export-native-race-cars") ||
                      !std::strcmp(argv[i], "--export-native-finish-models") ||
                      !std::strcmp(argv[i], "--export-native-result-models") ||
                      !std::strcmp(argv[i], "--export-native-name-models")) && i + 1 < argc) {
                if(!native_intro_cars.empty()) throw std::runtime_error("choose one car export");
                native_race_cars=!std::strcmp(argv[i], "--export-native-race-cars");
                native_finish_models=!std::strcmp(argv[i], "--export-native-finish-models");
                native_result_models=!std::strcmp(argv[i], "--export-native-result-models");
                native_name_models=!std::strcmp(argv[i], "--export-native-name-models");
                native_intro_cars=argv[++i];
            }
            else if (!std::strcmp(argv[i], "--race-scenery-lights")) course_export.race_scenery_lights=true;
            else if (!std::strcmp(argv[i], "--all-solid-lights")) all_solid_lights = true;
            else if (!std::strcmp(argv[i], "--native-model-all-lights") && i+1<argc) {
                const uint64_t address=number(argv[++i],"ROM model address");
                if (address<0x800000 || address>=0xc00000) throw std::runtime_error("model address must be ROM-backed");
                model_export.all_lights_models.insert(uint32_t(address));
            }
            else if (!std::strcmp(argv[i], "--inspect-native-vehicles")) inspect_vehicles = true;
            else if (!std::strcmp(argv[i], "--check-native-reflection")) check_reflection=true;
            else if (!std::strcmp(argv[i], "--export-native-sky") && i+1<argc) native_sky=argv[++i];
            else if (!std::strcmp(argv[i], "--check-native-road")) check_native_road = true;
            else if (!std::strcmp(argv[i], "--check-native-visibility") && i+1<argc) visibility_math=argv[++i];
            else if (!std::strcmp(argv[i], "--check-native-follow-camera") && i+1<argc) follow_camera_math=argv[++i];
            else if (!std::strcmp(argv[i], "--check-native-wheel-pose") && i+1<argc) wheel_math=argv[++i];
            else if (!std::strcmp(argv[i], "--check-native-expert-actors") && i+2<argc) { expert_math=argv[++i]; expert_contact=argv[++i]; }
            else if (!std::strcmp(argv[i], "--check-native-road-pack") && i + 3 < argc) {
                native_road_pack = argv[++i]; road_math = argv[++i]; road_paths = argv[++i];
            }
            else if (!std::strcmp(argv[i], "--check-native-drive-live")) check_live_drive = true;
            else if (!std::strcmp(argv[i], "--check-native-drive") && i + 1 < argc) native_drive = argv[++i];
            else if (!std::strcmp(argv[i], "--check-native-contact") && i + 2 < argc) {
                native_contact = argv[++i]; contact_math = argv[++i];
            }
            else if (!std::strcmp(argv[i], "--check-native-path") && i + 2 < argc) {
                native_path = argv[++i]; path_math = argv[++i];
            }
            else if (!std::strcmp(argv[i], "--translation-lights") && i + 1 < argc) {
                const std::string radius = argv[++i];
                size_t consumed = 0;
                model_export.translation_limit = std::stof(radius, &consumed);
                if (consumed != radius.size() || !std::isfinite(model_export.translation_limit) || model_export.translation_limit <= 0)
                    throw std::runtime_error("translation radius must be finite and positive");
                all_solid_lights = true;
            }
            else if (!std::strcmp(argv[i], "--probe-native-2d") && i + 1 < argc) probe_range = parse_stage(argv[++i]);
            else if (!std::strcmp(argv[i], "--check-native-title") && i + 1 < argc) title_check.load(argv[++i]);
            else { usage(); return 2; }
        }
        if (check_live_drive && native_drive.empty()) throw std::runtime_error("live drive check requires --check-native-drive PACK");
        if (trace_attract && race_trace_output.empty())
            throw std::runtime_error("--trace-attract needs --trace-race");
        if (!clock_trace_output.empty() && race_trace_output.empty())
            throw std::runtime_error("--trace-clock needs --trace-race (its call observer)");
        if (!props_trace_output.empty() && race_trace_output.empty())
            throw std::runtime_error("--trace-props needs --trace-race");
        if (clock_trace_expert && clock_trace_output.empty())
            throw std::runtime_error("--trace-clock-expert needs --trace-clock");
        if (!race_trace_output.empty() && (check_live_drive || check_reflection))
            throw std::runtime_error("--trace-race owns the call observers; run it without live drive/reflection checks");
        if (all_solid_lights && native_models.empty())
            throw std::runtime_error("--all-solid-lights requires --export-native-models");
        if (!model_export.all_lights_models.empty() && (native_models.empty() || model_export.translation_limit))
            throw std::runtime_error("--native-model-all-lights requires --export-native-models without --translation-lights");
        if (!native_models.empty() && !native_cells.empty())
            throw std::runtime_error("choose sampled models or complete scenery-cell export, not both");
        if(!native_windows.empty() && (!native_models.empty() || !native_cells.empty()))
            throw std::runtime_error("player window export requires its own output run");
        if(!native_intro_cars.empty() && (!native_models.empty() || !native_cells.empty() || !native_windows.empty()))
            throw std::runtime_error("intro car export requires its own output run");
        if (!native_objects.empty() && (!native_models.empty() || !native_cells.empty() || !native_windows.empty() || !native_intro_cars.empty()))
            throw std::runtime_error("world object export requires its own output run");
        if (course_export.race_scenery_lights && native_cells.empty())
            throw std::runtime_error("--race-scenery-lights requires --export-native-cells");
        if (!title_check.motion.empty() && (!inputs.empty() || frames < 3362))
            throw std::runtime_error("title oracle requires no inputs and at least 3362 frames");
        if (!catalog.empty() && !std::filesystem::create_directory(catalog))
            throw std::runtime_error("catalog directory already exists; choose a new directory");

        tools::Script script;
        if (!inputs.empty()) script.load(inputs);
        rt::GameLoop game(load_images(images), false);
        if (!nvram_dir.empty()) tools::load_nvram(game, nvram_dir);
        std::unique_ptr<daytona::native::LiveDriveCheck> live_drive;
        if (check_live_drive) {
            live_drive = std::make_unique<daytona::native::LiveDriveCheck>(game.board(),native_drive.c_str(),
                native_contact.empty() ? nullptr : native_contact.c_str());
            live_drive->capture_road = !native_road_pack.empty();
            if (!native_road_pack.empty() && !native_contact.empty())
                live_drive->player_chain.load_road(native_road_pack.c_str(),road_paths.c_str());
            if (!native_road_pack.empty() && !native_contact.empty())
                live_drive->rolling_chain.load_road(native_road_pack.c_str(),road_paths.c_str());
            game.set_call_observer(daytona::native::LiveDriveCheck::observe,live_drive.get());
            game.set_return_observer(daytona::native::LiveDriveCheck::observe_return,live_drive.get());
        }
        daytona::native::ReflectionGeometryCheck reflection_geometry;
        reflection_geometry.board=&game.board();
        if(check_reflection) {
            if(live_drive) {
                reflection_geometry.next_call=daytona::native::LiveDriveCheck::observe;
                reflection_geometry.next_call_data=live_drive.get();
            }
            game.set_call_observer(daytona::native::ReflectionGeometryCheck::observe_call,&reflection_geometry);
        }
        daytona::native::RaceTrace race_trace;
        if (!race_trace_output.empty()) {
            race_trace.open(game.board(),race_trace_output);
            race_trace.attract = trace_attract;
            if (!clock_trace_output.empty()) race_trace.open_clock(clock_trace_output);
            if (!props_trace_output.empty()) race_trace.open_props(props_trace_output);
            race_trace.expert_clock = clock_trace_expert;
            game.set_call_observer(daytona::native::RaceTrace::observe_call,&race_trace);
            game.set_return_observer(daytona::native::RaceTrace::observe_return,&race_trace);
        }
        std::unique_ptr<daytona::native::SelectExport> select_export;
        if (!select_output.empty()) select_export = std::make_unique<daytona::native::SelectExport>(select_output);
        daytona::native::FrontendTrace frontend_trace;
        if (!frontend_trace_output.empty()) {
            if (check_live_drive || check_reflection || !hud_trace_output.empty())
                throw std::runtime_error("--trace-frontend shares the call observer only with --trace-race");
            frontend_trace.open(game.board(),frontend_trace_output);
            if (race_trace.file) { frontend_trace.next_call=daytona::native::RaceTrace::observe_call; frontend_trace.next_context=&race_trace; }
            game.set_call_observer(daytona::native::FrontendTrace::observe_call,&frontend_trace);
        }
        daytona::native::HudTrace hud_trace;
        if (!hud_trace_output.empty()) {
            if (!race_trace_output.empty() || check_live_drive || check_reflection)
                throw std::runtime_error("--trace-hud owns the call observer; run it without other observers");
            hud_trace.open(game.board(),hud_trace_output,hud_snapshot_every);
            game.set_call_observer(daytona::native::HudTrace::observe_call,&hud_trace);
        }
        // Host-only: the bytes the original wrote to the sound UART (no sound board
        // is built here, so nothing else takes them). Run-derived; keep under build/.
        std::FILE *sound_trace = nullptr;
        uint64_t sound_rows = 0, sound_bytes = 0;
        if (!sound_trace_output.empty()) {
            if (std::filesystem::exists(sound_trace_output)) throw std::runtime_error("sound trace output already exists; choose a new file");
            if (!(sound_trace = std::fopen(sound_trace_output.c_str(), "w"))) throw std::runtime_error("cannot create sound trace");
            std::fputs("# sound-trace v1: frame active next course remaining clock flags : UART bytes\n", sound_trace);
        }
        unsigned previous_catalog_phase = 256;
        std::array<uint32_t,4> previous_catalog_updates{};
        const auto inspect_poses = [&](const char *phase) {
            auto &board = game.board();
            daytona::native::vehicle_neighbors([&](uint32_t at) { return board.read_dword(at); },board.read_byte(0x501080));
            const uint32_t others = board.read_dword(0x501224), stride = board.read_dword(others+8);
            for (unsigned i=0; i<board.read_byte(0x501080); ++i) {
                const uint32_t at = i ? others+(i-1)*stride : board.read_dword(0x501220);
                std::printf("native_vehicle_pose phase=%s address=%08x model=%08x updater=%08x xyz_bits=%08x,%08x,%08x render_angles=%04x,%04x,%04x\n",
                    phase,at,board.read_dword(at+0x14),board.read_dword(at+0x0c),board.read_dword(at+0x1c),
                    board.read_dword(at+0x20),board.read_dword(at+0x24),
                    board.read_word(at+0x4c),board.read_word(at+0x4e),board.read_word(at+0x50));
            }
            const uint32_t camera=board.read_dword(0x501240);
            if(camera<0x500000 || camera>0x5fff00 || camera%16)
                throw std::runtime_error("camera inspection requires an initialized camera entity");
            std::printf("native_camera phase=%s address=%08x updater=%08x mode=%u target=%08x selector=%u/%u tables=%08x,%08x,%08x,%08x,%08x\n",
                phase,camera,board.read_dword(camera+12),board.read_dword(0x5014a4),board.read_dword(0x5012f0),
                board.read_byte(camera+0x84),board.read_byte(camera+0x85),
                board.read_dword(camera+0x70),board.read_dword(camera+0x74),board.read_dword(camera+0x78),
                board.read_dword(camera+0x7c),board.read_dword(camera+0x80));
        };
        for (uint64_t frame = 0; frame < frames; ++frame) {
            const rt::Inputs input = script.at(game.board().frame());
            if (inspect_vehicles && frame+1 == frames) inspect_poses("before");
            if (!native_models.empty() && frame + 1 == frames) {
                auto &geo = game.board().geometry();
                geo.record_pushes = true;
                geo.object_observer = daytona::native::ModelExport::observe;
                geo.object_observer_data = &model_export;
            }
            if(!native_intro_cars.empty() && frame+1==frames) {
                if(game.board().read_byte(0x501460)) throw std::runtime_error("intro car export requires course0");
                intro_car_export.load([&](uint32_t at){return game.board().read_dword(at);},native_race_cars,native_finish_models,native_result_models,native_name_models);
                auto &geo=game.board().geometry();geo.record_pushes=true;
                geo.object_observer=daytona::native::IntroCarExport::observe;geo.object_observer_data=&intro_car_export;
            }
            if (!native_cells.empty() && frame + 1 == frames) {
                course_export.load([&](uint32_t address) { return game.board().read_dword(address); });
                auto &geo = game.board().geometry();
                geo.record_pushes = true;
                geo.object_observer = daytona::native::CourseExport::observe;
                geo.object_observer_data = &course_export;
            }
            if (!native_objects.empty() && frame + 1 == frames) {
                object_export.load([&](uint32_t address) { return game.board().read_dword(address); });
                auto &geo = game.board().geometry();
                geo.record_pushes = true;
                geo.object_observer = daytona::native::ObjectExport::observe;
                geo.object_observer_data = &object_export;
            }
            if(!native_windows.empty() && frame+1==frames) {
                window_export.load([&](uint32_t address) {return game.board().read_dword(address);});
                auto &geo=game.board().geometry();
                geo.record_pushes=true;
                geo.object_observer=daytona::native::PlayerWindowExport::observe;
                geo.object_observer_data=&window_export;
            }
            auto &observed_geo=game.board().geometry();
            const bool recorded_pushes=observed_geo.record_pushes;
            if(check_reflection) {
                reflection_geometry.next=observed_geo.object_observer;
                reflection_geometry.next_data=observed_geo.object_observer_data;
                observed_geo.record_pushes=true;
                observed_geo.object_observer=daytona::native::ReflectionGeometryCheck::observe;
                observed_geo.object_observer_data=&reflection_geometry;
            }
            const uint64_t consumed_frame = game.board().frame();
            frontend_trace.set_frame(consumed_frame);
            race_trace.running = consumed_frame;
            game.run_frame(input);
            if (race_trace.file) race_trace.sample(consumed_frame);
            if (!frame_dump.empty() && consumed_frame>=dump_first && consumed_frame<=dump_last &&
                (consumed_frame-dump_first)%dump_every==0) {
                char path[1024];
                std::snprintf(path, sizeof path, "%s/frame_%05" PRIu64 ".rgb", frame_dump.c_str(), consumed_frame);
                if (FILE *d = dump_screens ? std::fopen(path, "wb") : nullptr) {
                    std::fwrite(game.screen().data(), 4, game.screen().size(), d);
                    std::fclose(d);
                }
                // The 2D state of the frame: tile RAM (layers, row and line tables) and the final pen colours.
                auto &b = game.board();
                std::snprintf(path, sizeof path, "%s/tiles_%05" PRIu64 ".bin", frame_dump.c_str(), consumed_frame);
                if (FILE *d = std::fopen(path, "wb")) {
                    for (uint32_t a = 0; a < 0x10000; ++a) std::fputc(b.read_byte(0x01000000 + a), d);
                    std::fclose(d);
                }
                std::snprintf(path, sizeof path, "%s/pens_%05" PRIu64 ".bin", frame_dump.c_str(), consumed_frame);
                if (FILE *d = (dump_screens || consumed_frame == dump_first) ? std::fopen(path, "wb") : nullptr) {
                    for (unsigned pen = 0; pen < 8192; ++pen) {
                        const uint32_t c = b.video().system24_pen(pen);
                        std::fwrite(&c, 4, 1, d);
                    }
                    std::fclose(d);
                }
            }
            if (hud_trace.file) hud_trace.sample(consumed_frame);
            if (sound_trace) {
                auto &b = game.board();
                const auto bytes = b.take_sound_bytes();
                if (!bytes.empty()) {
                    std::fprintf(sound_trace, "S frame=%" PRIu64 " active=%u next=%u course=%u remaining=%u clock=%u flags=%08x :",
                        consumed_frame, b.read_byte(0x5010a0), b.read_byte(0x5010a4), b.read_byte(0x501460),
                        b.read_dword(0x5010d0), b.read_dword(0x5010cc), b.read_dword(0x501090));
                    for (const uint8_t value : bytes) std::fprintf(sound_trace, " %02x", value);
                    std::fputc('\n', sound_trace);
                    ++sound_rows; sound_bytes += bytes.size();
                }
            }
            if(check_reflection) {
                observed_geo.record_pushes=recorded_pushes;
                observed_geo.object_observer=reflection_geometry.next;
                observed_geo.object_observer_data=reflection_geometry.next_data;
            }
            const rt::Video &video = game.board().video();
            if (!catalog.empty()) {
                const unsigned phase = game.board().read_byte(0x5010a4);
                if (phase != previous_catalog_phase) {
                    std::printf("catalog_transition frame=%" PRIu64 " active=%u next=%u vehicles=%u course=%u handler=%08x\n",
                        frame+1, game.board().read_byte(0x5010a0), phase,
                        game.board().read_byte(0x501080), game.board().read_byte(0x501460),
                        phase<=30 ? game.board().read_dword(0x18cc+4*phase) : 0);
                    previous_catalog_phase = phase;
                }
                auto &board=game.board();
                std::array<uint32_t,4> updates{};
                const uint32_t slots[]={0x501220,0x501224,0x501240,0x501270};
                for(unsigned i=0;i<updates.size();++i) {
                    const uint32_t entity=board.read_dword(slots[i]);
                    if(entity>=0x500000 && entity<=0x5ffff0 && !(entity&3))
                        updates[i]=board.read_dword(entity+12);
                }
                if(updates!=previous_catalog_updates) {
                    std::printf("catalog_updates frame=%" PRIu64 " player=%08x opponent=%08x camera=%08x script=%08x flags=%08x camera_mode=%08x camera_key=%08x\n",
                        frame+1,updates[0],updates[1],updates[2],updates[3],board.read_dword(0x501090),
                        board.read_dword(0x5014a4),board.read_dword(0x50149c));
                    previous_catalog_updates=updates;
                }
            }
            if (!title_check.motion.empty()) title_check.sample(video, frame + 1);
            if (select_export) select_export->sample(game.board());
            if (!probe_range.name.empty() && frame >= probe_range.start && frame <= probe_range.end)
                scene_probe.sample(video, frame + 1);
            // Reference images for locating semantic screens, never target assets.
            // ponytail: sample every 120 frames; use a focused run for transitions.
            if (!catalog.empty() && ((frame + 1) % 120 == 0 || frame + 1 == frames)) {
                tools::dreamcast::write_tga(std::filesystem::path(catalog) /
                    ("frame_" + std::to_string(frame + 1) + ".tga"),
                    {unsigned(video.width()), rt::Video::H, false, video.screen()});
                std::printf("catalog_frame=%" PRIu64 " polygons=%zu windows=%d\n",
                            frame + 1, video.gpu_polys().size(), video.gpu_windows());
            }
        }

        if (!title_check.motion.empty()) title_check.check();
        if (sound_trace) {
            const bool failed = std::ferror(sound_trace) != 0;
            if (std::fclose(sound_trace) || failed) throw std::runtime_error("cannot write sound trace");
            std::printf("sound_trace rows=%" PRIu64 " bytes=%" PRIu64 " coverage=i960-uart-only\n", sound_rows, sound_bytes);
        }
        if (hud_trace.file) { game.set_call_observer(nullptr,nullptr); hud_trace.close(); }
        if (frontend_trace.file) { game.set_call_observer(nullptr,nullptr); frontend_trace.close(); }
        if (race_trace.file) {
            game.set_call_observer(nullptr,nullptr);game.set_return_observer(nullptr,nullptr);
            race_trace.close();
        }
        if (live_drive) {
            game.set_call_observer(nullptr,nullptr);
        }
        if (inspect_vehicles) {
            auto &board = game.board();
            const auto neighbors = daytona::native::vehicle_neighbors(
                [&](uint32_t at) { return board.read_dword(at); }, board.read_byte(0x501080));
            std::printf("native_vehicle_pool PASS player=%08x others=%08x count=%u current=%08x\n",
                board.read_dword(0x501220), board.read_dword(0x501224),
                board.read_byte(0x501080), board.read_dword(0x5012f0));
            // Offline evidence only, never target pose assets. Rendering can
            // consume the prior frame; keep both sides instead of guessing lag.
            inspect_poses("after");
            for (unsigned i = 0; i < neighbors.size(); ++i) {
                const auto at = neighbors[i];
                std::printf("native_vehicle_neighbor offset=%d address=%08x updater=%08x road_index=%u spatial_index=%u xyz_bits=%08x,%08x,%08x\n",
                    int(i)-5, at, board.read_dword(at+12), board.read_word(at+0x128),
                    board.read_dword(at+0x30), board.read_dword(at+0x1c),
                    board.read_dword(at+0x20), board.read_dword(at+0x24));
            }
        }
        if(!native_intro_cars.empty()) intro_car_export.write(native_intro_cars,game.board().video());
        if (!native_cells.empty()) {
            if (game.board().read_byte(0x501460) != course_export.slot ||
                game.board().read_dword(0x501420) != course_export.table)
                throw std::runtime_error("course changed during snapshot; choose another frame");
            course_export.write(native_cells, game.board().video());
        }
        if (!native_objects.empty()) object_export.write(native_objects, game.board().video());
        if (!native_models.empty()) {
            model_export.write(native_models, game.board().video(), all_solid_lights);
            // Host evidence for the authored scenery inventory; no target board state.
            // End-of-frame list, not proof that every listed cell reached the budget.
            const unsigned count = game.board().read_byte(0x5016c0);
            if (count > 63) throw std::runtime_error("invalid scenery cell-list count");
            std::printf("native_course_context slot=%u table=%08x cells=",
                game.board().read_byte(0x501460), game.board().read_dword(0x501420));
            for (unsigned i = 0; i < count; ++i)
                std::printf("%s%u", i ? "," : "", game.board().read_byte(0x5016c1+i));
            std::puts(" coverage=end-of-frame-selection-not-complete-draw");
        }
        if (!video_dump.empty()) {
            // Raw video tables as the board holds them after the last frame.
            if (!std::filesystem::create_directory(video_dump))
                throw std::runtime_error("--dump-video directory already exists; choose a new directory");
            const auto &mem = game.board().video().gpu_mem();
            const std::pair<const char *, std::pair<const void *, size_t>> tables[] = {
                {"palram.bin", {mem.palram, 0x4000}}, {"colorxlat.bin", {mem.colorxlat, 0xc000}},
                {"lumaram.bin", {mem.lumaram, 0x20000}}, {"tex0.bin", {mem.tex0, 0x80000 * 4}},
                {"tex1.bin", {mem.tex1, 0x80000 * 4}}};
            for (const auto &[name, table] : tables) {
                std::ofstream out(std::filesystem::path(video_dump) / name, std::ios::binary);
                out.write(static_cast<const char *>(table.first), std::streamsize(table.second));
                if (!out) throw std::runtime_error("cannot write the video dump");
            }
            std::printf("video_dump=%s frames=%" PRIu64 " (local ROM-derived tables, do not distribute)\n", video_dump.c_str(), frames);
        }
        if (!native_windows.empty()) window_export.write(native_windows,game.board().video());
        if (!native_scene.empty()) daytona::native::export_scene(native_scene, game.board().video());
        if (select_export) select_export->finish();
        std::printf("native_frames=%" PRIu64 " i960_instructions=%" PRIu64 " tgp_instructions=%" PRIu64
                    " screen_hash=%016" PRIx64 "\n",
                    game.frames(), game.instructions(), game.board().tgp().tgp_instructions(), game.board().video().screen_hash());
        // The oracle uses the reference TGP after the scenario; do not count
        // those diagnostic instructions/page reads as game-frame work.
        if (!native_sky.empty()) daytona::native::export_sky(native_sky,game.board());
        if (!name_exit_output.empty()) daytona::native::export_name_exit(game.board(),name_exit_output);
        if (check_native_road) daytona::native::check_road(game.board());
        if (check_native_race) daytona::native::check_race(game.board(),result_label_colors,name_label_colors);
        if (!visibility_math.empty()) daytona::native::check_visibility(game.board(),visibility_math.c_str());
        if (!native_drive.empty()) daytona::native::check_drive(game.board(),native_drive.c_str());
        if (live_drive) live_drive->report();
        if (!native_contact.empty()) daytona::native::check_contact(game.board(),native_contact.c_str(),contact_math.c_str(),
            live_drive ? &live_drive->boundary_inputs : nullptr,live_drive ? &live_drive->boundary_pivots : nullptr,
            live_drive ? &live_drive->boundary_selections : nullptr,live_drive ? &live_drive->damage_pre : nullptr);
        if (!native_path.empty()) daytona::native::check_path(game.board(),native_path.c_str(),path_math.c_str());
        if (!native_road_pack.empty()) {
            if (live_drive && live_drive->road_pending) throw std::runtime_error("unfinished actual road selection");
            daytona::native::check_road_pack(game.board(),native_road_pack.c_str(),road_math.c_str(),road_paths.c_str(),
                live_drive ? &live_drive->road_selections : nullptr);
        }
        if (!follow_camera_math.empty()) daytona::native::check_follow_camera(game.board(),follow_camera_math.c_str());
        if (!wheel_math.empty()) daytona::native::check_wheel_pose(game.board(),wheel_math.c_str());
        if (!expert_math.empty()) daytona::native::check_expert_actors(game.board(),expert_math.c_str(),expert_contact.c_str());
        if (check_reflection) {
            reflection_geometry.report();
            daytona::native::check_reflection(game.board());
        }
        if (check_native_race && live_drive && !native_road_pack.empty())
            daytona::native::check_finish_prefix(game.board(),live_drive->params.get(),&live_drive->contact,
                native_road_pack.c_str(),road_paths.c_str());
        if (!save_nvram_dir.empty()) tools::save_nvram(game, save_nvram_dir);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "m2segment: %s\n", error.what());
        return 1;
    }
}
