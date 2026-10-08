// Native Vita frontend: shared board/recompiled code, SDL2 presentation/audio.
#define SDL_MAIN_HANDLED
#include "audio.h"
#include "controls.h"
#include "diagnostic_log.h"
#include "performance.h"
#include "text.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// This is an up-front reservation, not a limit that grows on demand. Leave
// memory outside newlib for the executable, driver allocations and stacks.
// Read by newlib (not LTO code): kept by "used" in the release (LTO) build.
extern "C" { __attribute__((used)) unsigned int _newlib_heap_size_user = 192 * 1024 * 1024; }
namespace {
constexpr const char *kDirectory = "ux0:data/daytona93";
constexpr const char *kRom = "ux0:data/daytona93/daytona93.zip";
constexpr int kDisplayWidth = 960, kDisplayHeight = 544;
vita::DiagnosticLog diagnostics; // only the main thread writes this log


// Do not use stdio or allocate in this error path: the heap may be missing,
// or freopen may have closed stderr before failing to open its replacement.
void startup_error(const char *message, size_t length) {
    diagnostics.append(message, length);
    const SceUID fd = sceIoOpen("ux0:data/daytona93/vita.log",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    while (length) {
        const int written = sceIoWrite(fd, message, length);
        if (written <= 0 || size_t(written) > length) break;
        message += written;
        length -= size_t(written);
    }
    sceIoClose(fd);
}

bool initialize_logging() {
    // A failed startup heap reservation leaves newlib's allocator unusable.
    // Check before freopen/stdio, which can themselves need heap storage.
    void *probe = std::malloc(1024);
    if (!probe) {
        static constexpr char error[] =
            "startup: newlib heap unavailable; stopped before SDL/ROM loading.\n"
            "Use the 192 MiB startup-heap build and keep the matching ELF.\n";
        startup_error(error, sizeof(error) - 1);
        return false;
    }
    std::free(probe);
    if (!std::freopen("ux0:data/daytona93/vita.log", "w", stderr)) {
        static constexpr char error[] =
            "startup: cannot open the stdio log; stopped without using closed stderr.\n"
            "Check ux0:data/daytona93/ and available storage.\n";
        startup_error(error, sizeof(error) - 1);
        return false;
    }
    // No dynamically allocated line buffer is needed for the diagnostic log.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

// Vita process-time microseconds also work before SDL timer initialization.
uint64_t profile_ticks() { return sceKernelGetProcessTimeWide(); }

vita::Pad read_pad() {
    SceCtrlData native{};
    vita::Pad pad;
    if (sceCtrlPeekBufferPositive(0, &native, 1) <= 0) return pad;
    pad.lx = native.lx; pad.ry = native.ry;
    const struct { uint32_t native, portable; } map[] = {
        {SCE_CTRL_CROSS, vita::Cross}, {SCE_CTRL_CIRCLE, vita::Circle},
        {SCE_CTRL_SQUARE, vita::Square}, {SCE_CTRL_TRIANGLE, vita::Triangle},
        {SCE_CTRL_UP, vita::Up}, {SCE_CTRL_DOWN, vita::Down},
        {SCE_CTRL_LEFT, vita::Left}, {SCE_CTRL_RIGHT, vita::Right},
        {SCE_CTRL_LTRIGGER, vita::L}, {SCE_CTRL_RTRIGGER, vita::R},
        {SCE_CTRL_START, vita::Start}, {SCE_CTRL_SELECT, vita::Select}
    };
    for (auto entry : map) if (native.buttons & entry.native) pad.buttons |= entry.portable;
    return pad;
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    // Bound the allocation even if a save has been replaced by a huge file.
    std::vector<uint8_t> temporary(size);
    const bool ok = std::fread(temporary.data(), 1, size, file) == size &&
                    std::fgetc(file) == EOF && !std::ferror(file);
    std::fclose(file);
    if (ok) std::copy(temporary.begin(), temporary.end(), data);
    return ok;
}
bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string temporary = path + ".tmp", backup = path + ".bak";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(data, 1, size, file) == size;
    if (std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) return false;
    // Vita rename need not replace an existing destination. Retain one
    // complete previous generation and restore it if the final rename fails.
    bool had_old = false;
    if (auto *old = std::fopen(path.c_str(), "rb")) {
        std::fclose(old);
        std::remove(backup.c_str());
        if (std::rename(path.c_str(), backup.c_str()) != 0) return false;
        had_old = true;
    }
    if (std::rename(temporary.c_str(), path.c_str()) == 0) return true;
    if (had_old) std::rename(backup.c_str(), path.c_str());
    return false;
}
template<class C> void load_nv(const std::string &name, C &data) {
    const std::string path = std::string(kDirectory) + "/" + name;
    if (!load_bytes(path, data.data(), data.size()))
        load_bytes(path + ".bak", data.data(), data.size());
}
void sdl_check(bool ok, const char *operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

int run_app() {
    diagnostics.literal("stage: run_app begin\n");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
    using Renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
    using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
    diagnostics.literal("stage: create window begin\n");
    Window window(SDL_CreateWindow("Daytona Recomp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   kDisplayWidth, kDisplayHeight, SDL_WINDOW_FULLSCREEN), SDL_DestroyWindow);
    sdl_check(bool(window), "create window");
    diagnostics.literal("stage: create window done; create renderer begin\n");
    Renderer renderer(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC), SDL_DestroyRenderer);
    if (!renderer) renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED));
    sdl_check(bool(renderer), "create Vita renderer");
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer.get(), &info);
    diagnostics.log("renderer: %s\n", info.name ? info.name : "unknown");
    diagnostics.literal("stage: logical size begin\n");
    sdl_check(SDL_RenderSetLogicalSize(renderer.get(), kDisplayWidth, kDisplayHeight) == 0, "logical size");
    diagnostics.literal("stage: logical size done; create texture begin\n");
    Texture screen(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                      rt::GameLoop::kWidth, rt::GameLoop::kHeight), SDL_DestroyTexture);
    sdl_check(bool(screen), "create screen texture");
    diagnostics.literal("stage: create texture done\n");
    SDL_SetTextureBlendMode(screen.get(), SDL_BLENDMODE_NONE);
    vita::Audio audio;
    diagnostics.literal("stage: audio subsystem begin\n");
    const int audio_init = SDL_InitSubSystem(SDL_INIT_AUDIO);
    diagnostics.log("stage: audio subsystem returned %d\n", audio_init);
    if (audio_init == 0) {
        diagnostics.literal("stage: audio device and resamplers begin\n");
        const bool audio_ok = audio.open();
        diagnostics.log("stage: audio device and resamplers returned %d (%s)\n",
                        int(audio_ok), audio_ok ? "OK" : SDL_GetError());
    } else diagnostics.log("audio unavailable: %s\n", SDL_GetError());
    diagnostics.literal("stage: settings load begin\n");
    uint8_t mute_setting = 0;
    const std::string settings = std::string(kDirectory) + "/mute.bin";
    if (!load_bytes(settings, &mute_setting, 1)) load_bytes(settings + ".bak", &mute_setting, 1);
    bool muted = mute_setting != 0;
    diagnostics.literal("stage: settings loaded; audio mute begin\n");
    audio.mute(muted);
    diagnostics.literal("stage: audio mute done\n");

    std::unique_ptr<rt::GameLoop> game;
    std::vector<uint8_t> saved_eeprom, saved_backup;
    vita::Controls controls;
    vita::FrameClock clock(rt::GameLoop::kFrameHz, 1); // one complete game frame per presentation
    bool running = true, menu = true, have_frame = false, wait_release = true;
    bool background = false;
    int selection = 0;
    uint8_t pulse = 0;
    uint32_t previous_buttons = 0;
    constexpr double frequency = 1000000.0; // profile_ticks() is native microseconds
    vita::Performance performance(frequency);
    diagnostics.log("performance diagnostics enabled; max game steps per present: 1; clock settings unchanged\n");
#ifdef M2_VITA_RENDER_OPT
    diagnostics.literal("render_opt: OPT03 enabled - exact tile/layer cache, mip setup, lighting LUT and packed texture reads\n");
#else
    diagnostics.literal("render_opt: OPT03 reference renderer - optimization disabled\n");
#endif
#ifdef NDEBUG
    diagnostics.log("build: NDEBUG defined (assertions disabled)\n");
#else
    diagnostics.log("build: assertions enabled; check Release optimization flags\n");
#endif
    uint64_t previous_counter = profile_ticks();
    double save_elapsed = 0;
    std::string status = "Place your daytona93.zip in ux0:data/daytona93/ then select START GAME.";
    char performance_screen[160] = "WAITING FOR COMPLETED FRAMES";
    unsigned traced_frames = 0;
    unsigned startup_presents = 0;
    auto draw_diagnostics = [&](int y) {
        char line[200];
        const unsigned error = unsigned(diagnostics.error() ? diagnostics.error() : diagnostics.sync_error());
        std::snprintf(line, sizeof line, "OPT03 LOG %s %08X", diagnostics.state(), error);
        SDL_SetRenderDrawColor(renderer.get(), 16, 18, 24, 255);
        SDL_Rect background_rect{0, y, kDisplayWidth, 36};
        SDL_RenderFillRect(renderer.get(), &background_rect);
        SDL_SetRenderDrawColor(renderer.get(), 235, 235, 235, 255);
        vita::text(renderer.get(), line, 18, y + 2, 2, 76, 1);
        vita::text(renderer.get(), performance_screen, 18, y + 19, 2, 76, 1);
    };

    auto save = [&]() {
        if (!game) return true;
        auto save_changed = [&](const char *name, const auto &data, std::vector<uint8_t> &previous) {
            if (data.size() == previous.size() && std::equal(data.begin(), data.end(), previous.begin())) return true;
            if (!save_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size())) return false;
            previous.assign(data.begin(), data.end());
            return true;
        };
        // Do not short-circuit: attempt both saves even if one failed.
        const bool eeprom_ok = save_changed("ioboard_eeprom.bin", game->board().io().eeprom, saved_eeprom);
        const bool backup_ok = save_changed("backup_ram.bin", game->board().backup_ram(), saved_backup);
        if (!eeprom_ok || !backup_ok) {
            status = "Save failed. Check free space and ux0:data/daytona93/. Previous saves are kept as .bak.";
            diagnostics.log("%s\n", status.c_str());
        }
        return eeprom_ok && backup_ok;
    };
    auto draw_menu = [&]() {
        SDL_SetRenderDrawColor(renderer.get(), 16, 18, 24, 255);
        SDL_RenderClear(renderer.get());
        SDL_SetRenderDrawColor(renderer.get(), 235, 235, 235, 255);
        vita::text(renderer.get(), "DAYTONA RECOMP - OPT03", 30, 28, 3, 49, 1);
        draw_diagnostics(60);
        const std::string labels[] = {game ? "RESUME GAME" : "START GAME", "RESET GAME", "TEST SWITCH", "SERVICE COIN",
                                      muted ? "SOUND: MUTED" : "SOUND: ON", "SAVE AND QUIT"};
        for (int i = 0; i < 6; ++i) {
            if (i == selection) SDL_SetRenderDrawColor(renderer.get(), 255, 200, 70, 255);
            else SDL_SetRenderDrawColor(renderer.get(), 220, 220, 225, 255);
            vita::text(renderer.get(), (i == selection ? "> " : "  ") + labels[i], 42, 108 + i * 38, 2, 70, 1);
        }
        SDL_SetRenderDrawColor(renderer.get(), 220, 220, 225, 255);
        vita::text(renderer.get(), status, 30, 364, 2, 74, 7);
        vita::text(renderer.get(), "CROSS: SELECT  CIRCLE: RESUME  START+SELECT: MENU", 30, 516, 2, 74, 1);
    };
    auto reset_clock = [&]() {
        clock.reset(); previous_counter = profile_ticks(); wait_release = true;
        performance.reset(previous_counter, menu);
        std::snprintf(performance_screen, sizeof performance_screen, "WAITING FOR COMPLETED FRAMES");
    };
    auto open_menu = [&]() {
        diagnostics.literal("stage: pause/menu begin\n");
        menu = true; audio.pause(); save(); reset_clock();
        diagnostics.literal("stage: pause/menu done\n");
    };
    auto start = [&]() {
        diagnostics.literal("stage: start/reset begin\n");
        traced_frames = 0;
        if (!save()) return false;
        game.reset(); have_frame = false; audio.pause();
        status = "Loading and checking your ROM set...";
        draw_menu();
        diagnostics.literal("stage: loading screen present begin\n");
        SDL_RenderPresent(renderer.get());
        diagnostics.literal("stage: loading screen present done\n");
        try {
            diagnostics.literal("stage: ROM import begin\n");
            auto images = rt::import_rom_set(kRom);
            diagnostics.literal("stage: ROM import done; board creation begin\n");
            game = std::make_unique<rt::GameLoop>(std::move(images));
            diagnostics.literal("stage: board creation done; save loading begin\n");
            game->set_profile_clock(profile_ticks);
            game->board().video().set_profile_clock(profile_ticks);
            load_nv("ioboard_eeprom.bin", game->board().io().eeprom);
            load_nv("backup_ram.bin", game->board().backup_ram());
            saved_eeprom.assign(game->board().io().eeprom.begin(), game->board().io().eeprom.end());
            saved_backup = game->board().backup_ram();
            diagnostics.literal("stage: save loading done\n");
            controls = vita::Controls{};
            pulse = 0; save_elapsed = 0;
            status = "L/R: BRAKE/GAS. LEFT STICK: STEER. RIGHT STICK: ANALOG PEDALS. UP/DOWN: GEARS. FACE BUTTONS: VIEWS.";
            reset_clock();
            return true;
        } catch (const std::exception &error) {
            status = error.what(); diagnostics.log("start: %s\n", error.what());
            reset_clock();
            return false;
        }
    };

    diagnostics.literal("stage: main loop entered\n");
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            else if (event.type == SDL_APP_WILLENTERBACKGROUND) { background = true; open_menu(); }
            else if (event.type == SDL_APP_DIDENTERFOREGROUND) { background = false; reset_clock(); }
        }
        if (!running) break;
        if (background) { SDL_Delay(20); continue; }
        const auto pad = read_pad();
        uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if (wait_release) {
            pressed = 0;
            controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }
        if (!menu && !wait_release && vita::menu_chord(pad.buttons)) open_menu();
        if (menu && !wait_release) {
            if (pressed & vita::Up) selection = (selection + 5) % 6;
            if (pressed & vita::Down) selection = (selection + 1) % 6;
            if ((pressed & vita::Circle) && game) { menu = false; reset_clock(); }
            else if (pressed & vita::Cross) {
                switch (selection) {
                case 0: if (game || start()) { menu = false; reset_clock(); } break;
                case 1: if (start()) { menu = false; reset_clock(); } break;
                case 2: case 3:
                    if (game) { pulse = selection == 2 ? 0x04 : 0x08; menu = false; reset_clock(); }
                    else status = "Start the game before using the cabinet switches.";
                    break;
                case 4:
                    muted = !muted; audio.mute(muted); mute_setting = muted ? 1 : 0;
                    if (!save_bytes(settings, &mute_setting, 1)) status = "Unable to save sound setting.";
                    break;
                case 5: if (save()) running = false; break;
                }
            }
        }
        const uint64_t now = profile_ticks();
        const double elapsed = double(now - previous_counter) / frequency;
        previous_counter = now;
        performance.begin(now, menu);
        bool trace_frame = false;
        if (game && !menu) {
            const int frames = clock.advance(elapsed);
            trace_frame = frames > 0 && traced_frames < 2;
            try {
                for (int n = 0; n < frames; ++n) {
                    const auto input = controls.sample(wait_release ? vita::Pad{} : pad);
                    rt::Inputs mapped;
                    mapped.steer = input.steer; mapped.accel = input.accel; mapped.brake = input.brake;
                    mapped.in0 = uint8_t(input.in0 & ~pulse); mapped.in1 = input.in1; mapped.in2 = input.in2;
                    if (trace_frame) diagnostics.log("stage: frame %u run_frame begin\n", traced_frames);
                    game->run_frame(mapped); pulse = 0;
                    if (trace_frame) diagnostics.log("stage: frame %u run_frame done\n", traced_frames);
                    performance.frame(game->last_profile());
                    performance.video(game->board().video().last_profile());
                    if (game->sound()) {
                        const uint64_t before_audio = profile_ticks();
                        if (trace_frame) diagnostics.literal("stage: audio push begin\n");
                        audio.push(*game->sound());
                        if (trace_frame) diagnostics.literal("stage: audio push done\n");
                        performance.span(vita::Performance::Audio, before_audio, profile_ticks());
                    }
                    have_frame = true;
                    ++traced_frames;
                }
                if (frames) {
                    if (trace_frame) diagnostics.literal("stage: texture upload begin\n");
                    const uint64_t before_upload = profile_ticks();
                    sdl_check(SDL_UpdateTexture(screen.get(), nullptr, game->screen().data(),
                              rt::GameLoop::kWidth * int(sizeof(uint32_t))) == 0, "upload screen");
                    performance.span(vita::Performance::Upload, before_upload, profile_ticks());
                    if (trace_frame) diagnostics.literal("stage: texture upload done\n");
                }
                save_elapsed += elapsed;
                if (save_elapsed >= 5.0) {
                    const uint64_t before_save = profile_ticks();
                    save(); save_elapsed = 0;
                    performance.span(vita::Performance::Save, before_save, profile_ticks());
                }
            } catch (const std::exception &error) {
                status = error.what(); diagnostics.log("runtime: %s\n", error.what());
                open_menu(); game.reset(); have_frame = false;
            }
        } else clock.reset();
        if (startup_presents < 2 || trace_frame) diagnostics.literal("stage: draw begin\n");
        const uint64_t before_draw = profile_ticks();
        if (menu) draw_menu();
        else {
            SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255);
            SDL_RenderClear(renderer.get());
            if (have_frame) {
                // Preserve the same square-pixel framebuffer aspect as desktop.
                const int width = kDisplayHeight * rt::GameLoop::kWidth / rt::GameLoop::kHeight;
                const SDL_Rect destination{(kDisplayWidth - width) / 2, 0, width, kDisplayHeight};
                SDL_RenderCopy(renderer.get(), screen.get(), nullptr, &destination);
            }
        }
        if (!menu) draw_diagnostics(kDisplayHeight - 36);
        if (startup_presents < 2 || trace_frame) diagnostics.literal("stage: draw done; present begin\n");
        const uint64_t before_present = profile_ticks();
        performance.span(vita::Performance::Draw, before_draw, before_present);
        SDL_RenderPresent(renderer.get());
        const uint64_t after_present = profile_ticks();
        if (startup_presents < 2 || trace_frame) diagnostics.literal("stage: present done\n");
        if (startup_presents < 2) ++startup_presents;
        performance.span(vita::Performance::Present, before_present, after_present);
        performance.presented();
        if (performance.ready(after_present)) {
            char line[768];
            const int count = performance.format(line, sizeof line, after_present,
                scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetBusClockFrequency());
            // One bounded write every two seconds; not a frame-by-frame trace.
            if (count > 0) diagnostics.append(line, std::min(size_t(count), sizeof(line) - 1));
            performance.format_overlay(performance_screen, sizeof performance_screen, after_present);
            performance.reset(after_present, menu);
        }
        SDL_Delay(1); // also bounds polling when the driver cannot enable vsync
    }
    diagnostics.literal("stage: save/quit begin\n");
    save();
    diagnostics.literal("stage: save/quit done\n");
    return 0;
}
} // namespace

int main(int, char **) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(kDirectory, 0777);
    diagnostics.begin(); // log begins BEFORE heap, stdio, SDL, texture or audio setup
    diagnostics.literal("stage: heap and stdio probe begin\n");
    if (!initialize_logging()) {
        // Bypass libc teardown when its heap or stderr could not be prepared.
        sceKernelExitProcess(1);
        return 1;
    }
    diagnostics.log("build: OPT03 %s %s; native diagnostic path: %s\n", __DATE__, __TIME__, vita::DiagnosticLog::kPath);
    diagnostics.literal("stage: heap and stdio probe done; SDL initialization begin\n");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        diagnostics.log("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    diagnostics.literal("stage: SDL initialization done\n");
    int result = 0;
    try { result = run_app(); }
    catch (const std::exception &error) { diagnostics.log("fatal: %s\n", error.what()); result = 1; }
    diagnostics.literal("stage: SDL quit begin\n");
    SDL_Quit();
    diagnostics.log("stage: clean exit %d\n", result);
    return result;
}
