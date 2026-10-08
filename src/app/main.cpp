// daytona: the recompiled game with its launcher, in one window. SDL3 for the
// window, input and GPU (SDL_GPU: Vulkan or Direct3D 12 on Windows, Vulkan
// on Linux, Metal on macOS); Dear ImGui for the launcher. The game runs on
// the native board runtime (rt::GameLoop), loaded straight from the user's
// ROM zip (rt::import_rom_set); each composed frame is uploaded to a GPU
// texture and scaled onto the swapchain, and the sound board's output is
// played through SDL audio.
//
//   daytona [--rom FILE.zip] [--autostart] [--gpu vulkan|direct3d12|metal]
//           (the launcher's "Skip launcher" is a saved --autostart)
//           [--fullscreen] [--frames N] [--audio native|reference] [--profile NAME]
//           (--profile: a separate data folder, e.g. a second cabinet for link play)
//
// In the game: Esc opens the launcher (resume, reset, controls), F11
// toggles fullscreen. Controls are set in the launcher and saved.

#include "app/config.h"
#include "app/ffb.h"
#include "app/pacing.h"
#include "runtime/test_hold.h"
#include "app/link_socket.h"
#include "app/gpu/gpu_renderer.h"
#include "app/launcher.h"
#include "app/native_audio.h"
#include "runtime/native_sound_engine.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui.h"
#ifdef M2_MOBILE
#include "app/touch_overlay.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h> // AttachConsole
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> // Windows: the WinMain entry point a WIN32 (GUI) program links against

#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

constexpr double kArcadeHz = rt::GameLoop::kFrameHz; // 16 MHz / (656 x 424), the board's frame rate

// The sound board's two outputs (the YM3438, and the two MultiPCMs mixed) go
// to their own SDL audio streams at the chips' own rates; SDL resamples and
// mixes them on the device. The game advances on the display's clock and the
// device plays on its own, so a small speed trim on both streams holds the
// queue near kLatency instead of letting it drift into a gap or a backlog.
class Audio {
public:
    static constexpr double kLatency = 0.06; // seconds queued
    ~Audio() { close(); }

    bool open(double fm_rate, double pcm_rate) {
        dev_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (!dev_) return false;
        fm_rate_ = int(fm_rate + 0.5);
        const SDL_AudioSpec fm{SDL_AUDIO_F32, 2, fm_rate_}, pcm{SDL_AUDIO_F32, 2, int(pcm_rate + 0.5)};
        fm_ = SDL_CreateAudioStream(&fm, nullptr);
        pcm_ = SDL_CreateAudioStream(&pcm, nullptr);
        if (!fm_ || !pcm_ || !SDL_BindAudioStream(dev_, fm_) || !SDL_BindAudioStream(dev_, pcm_)) {
            close(); return false;
        }
        return true;
    }
    // speed: the game's rate over the arcade's (Sync to display: 1.043 at 60
    // frames/s), so the sound keeps up with the game; the trim works around it.
    void push(snd::SoundBoard &sb, float gain, double speed = 1.0) {
        if (!fm_) return;
        const std::vector<float> fm = sb.take_fm(), pcm = sb.take_pcm();
        SDL_PutAudioStreamData(fm_, fm.data(), int(fm.size() * sizeof(float)));
        SDL_PutAudioStreamData(pcm_, pcm.data(), int(pcm.size() * sizeof(float)));
        const double queued = double(SDL_GetAudioStreamQueued(fm_)) / (8.0 * fm_rate_);
        if (queued > kLatency * 4) { // a stall (window drag, debugger): drop the backlog
            SDL_ClearAudioStream(fm_);
            SDL_ClearAudioStream(pcm_);
        }
        const double err = std::clamp((queued - kLatency) / kLatency, -1.0, 1.0);
        const float ratio = float(speed * (1.0 + 0.005 * err)); // the trim at most 0.5%: inaudible
        SDL_SetAudioStreamFrequencyRatio(fm_, ratio);
        SDL_SetAudioStreamFrequencyRatio(pcm_, ratio);
        SDL_SetAudioStreamGain(fm_, gain);
        SDL_SetAudioStreamGain(pcm_, gain);
    }
    void clear() {
        if (fm_) SDL_ClearAudioStream(fm_);
        if (pcm_) SDL_ClearAudioStream(pcm_);
    }
    void close() {
        if (fm_) SDL_DestroyAudioStream(fm_);
        if (pcm_) SDL_DestroyAudioStream(pcm_);
        if (dev_) SDL_CloseAudioDevice(dev_);
        fm_ = pcm_ = nullptr;
        dev_ = 0;
    }

private:
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioStream *fm_ = nullptr, *pcm_ = nullptr;
    int fm_rate_ = 1;
};

// The launcher's link play status line.
std::string link_status(const rt::CommBoard *board, const app::TcpLink *link, const app::Config &cfg) {
    if (!cfg.link) return "off";
    if (!board || !link) return "not started";
    switch (board->link()) {
    case rt::CommBoard::Link::Off: return "on (the game has not started the link: set LINK ID in test mode)";
    case rt::CommBoard::Link::Waiting: {
        // which half of the ring is missing
        std::string s = "waiting: ";
        s += link->tx_open() ? "next cabinet " + cfg.link_next + " reached; "
                             : "next cabinet " + cfg.link_next + " not reached yet (is it listening on that port?); ";
        s += link->rx_open() ? "a cabinet is connected to port " + std::to_string(cfg.link_port)
                             : "no cabinet has connected to port " + std::to_string(cfg.link_port) + " yet";
        return s;
    }
    case rt::CommBoard::Link::Up:
        return "linked: cabinet " + std::to_string(board->id()) + " of " + std::to_string(board->count());
    case rt::CommBoard::Link::Lost: return "lost (reset to try again)";
    }
    return "";
}

std::string pref_file(const char *name) { return app::Config::pref_dir() + name; } // per ROM set and profile
template <typename C> void load_file(const std::string &path, C &into) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
    if (d.size() == into.size()) std::copy(d.begin(), d.end(), into.begin());
}
template <typename C> void save_file(const std::string &path, const C &from) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(from.data()), std::streamsize(from.size()));
}

int fail(const char *what) {
	char msg[4096];

    std::snprintf(
        msg,
        sizeof(msg),
        "%s failed: %s",
        what,
        SDL_GetError());

	SDL_Log("%s failed: %s", what, SDL_GetError());   // add this line
    std::fprintf(stderr, "daytona: %s: %s\n", what, SDL_GetError());
	SDL_ShowSimpleMessageBox(
	SDL_MESSAGEBOX_ERROR,
	"Daytona Error",
	msg,
	nullptr);
    return 1;
}

} // namespace

// Windows builds daytona as a GUI program, which has no console of its own:
// nothing it prints is seen. Started from a command window, attach to that
// window; otherwise write to daytona.log beside the settings.
void open_log() {
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        std::printf("\n");
        return;
    }
    const std::string log = pref_file("daytona.log");
    std::freopen(log.c_str(), "w", stdout);
    std::freopen(log.c_str(), "a", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#endif
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) // before anything reads or writes the data folder
        if (!std::strcmp(argv[i], "--profile")) app::Config::profile = argv[i + 1];
    open_log();
    app::Config cfg;
    cfg.load();
    uint64_t max_frames = 0;
    bool autostart = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!std::strcmp(argv[i], "--gpu") && i + 1 < argc) cfg.gpu = argv[++i];
        else if (!std::strcmp(argv[i], "--audio")) {
            if (i + 1 == argc || (std::strcmp(argv[i + 1], "native") && std::strcmp(argv[i + 1], "reference"))) {
                std::fprintf(stderr, "daytona: --audio expects native or reference\n");
                return 1;
            }
            cfg.native_audio = !std::strcmp(argv[++i], "native");
        }
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--fullscreen")) cfg.fullscreen = true;
        else if (!std::strcmp(argv[i], "--autostart")) autostart = true;
        else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) ++i; // read above
    }

    // the name graphics overlays and drivers see (patches/sdl3: Vulkan's application name)
    SDL_SetAppMetadata("Daytona USA", nullptr, "daytona-recomp");
#ifdef M2_MOBILE
    // Mobile shells are landscape-only and always occupy the display.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    cfg.fullscreen = true;
#endif
    if (!cfg.gpu.empty()) SDL_SetHint(SDL_HINT_GPU_DRIVER, cfg.gpu.c_str());
    if (cfg.legacy_logitech_wheels) { // before the joysticks start: SDL reads it when it finds the devices
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_LG4FF, "0");
        std::fprintf(stderr, "daytona: Logitech wheels through the system's driver\n");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail("SDL_Init");
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) std::fprintf(stderr, "daytona: no force feedback (%s)\n", SDL_GetError());
    Audio audio;
    app::NativeAudio<snd::NativeSoundEngine> native_audio;
    const bool audio_initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
    bool have_audio = false, native_active = false, native_fault = false;
    if (audio_initialized) std::printf("daytona: audio driver %s\n", SDL_GetCurrentAudioDriver());
    else std::fprintf(stderr, "daytona: no audio output (%s)\n", SDL_GetError());

    constexpr int W = rt::GameLoop::kWidth, H = rt::GameLoop::kHeight;
    const int kMaxW = W + 2 * rt::GameLoop::wide_margin(app::Config::kMaxAspect);
    int screen_w = W; // this frame's width: W, or wider with widescreen
    SDL_Window *window = SDL_CreateWindow("Daytona USA", W * 2, H * 2,
                                          SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!window) return fail("SDL_CreateWindow");
    app::apply_fullscreen_mode(window, cfg.fullscreen_mode);
    constexpr SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!dev && !cfg.gpu.empty()) {
        // The chosen API is not available here (Vulkan on a Mac without MoltenVK): use the automatic choice.
        std::fprintf(stderr, "daytona: %s; falling back to automatic\n", SDL_GetError());
        SDL_ResetHint(SDL_HINT_GPU_DRIVER);
        cfg.gpu.clear();
        dev = SDL_CreateGPUDevice(formats, false, nullptr);
    }
    if (!dev) return fail("SDL_CreateGPUDevice");
    if (!SDL_ClaimWindowForGPUDevice(dev, window)) return fail("SDL_ClaimWindowForGPUDevice");
    std::printf("daytona: GPU driver %s\n", SDL_GetGPUDeviceDriver(dev));

    // Dear ImGui on SDL3 + SDL_GPU (its shaders ship precompiled for every backend)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
#ifdef SDL_PLATFORM_IOS
    // UIKit reports logical points; the SDL backend handles Retina framebuffer
    // scaling. Applying the display scale here again makes controls oversized.
    ImGui::GetStyle().FontSizeBase = 17.0f;
    ImGui::GetStyle().FramePadding = ImVec2(8, 6);
    ImGui::GetStyle().ScrollbarSize = 20.0f;
#else
    ImGui::GetStyle().ScaleAllSizes(SDL_GetWindowDisplayScale(window));
#endif
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo ii;
    ii.Device = dev;
    ii.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, window);
    ImGui_ImplSDLGPU3_Init(&ii);

    // Hardware renderer (launcher > Renderer): the 3D on the GPU. If it cannot
    // start here, the software renderer is used and the launcher says why.
    app::GpuRenderer gpu;
    if (!gpu.init(dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM))
        std::fprintf(stderr, "daytona: hardware renderer unavailable: %s\n", gpu.error().c_str());

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the screen's 0xAARRGGBB words, little-endian
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; // the hardware renderer draws into it
    ti.width = kMaxW; // the widest screen (widescreen); each frame uses its own width
    ti.height = H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *screen = SDL_CreateGPUTexture(dev, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = Uint32(kMaxW) * H * 4;
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!screen || !upload) return fail("SDL_CreateGPUTexture");
    // The hardware renderer at a higher internal resolution (launcher >
    // Super sampling) draws into its own texture, made when the scale changes,
    // with mip levels: the window shows the level nearest its size, so a
    // frame bigger than the window is averaged down (supersampled).
    SDL_GPUTexture *hires = nullptr;
    int hires_scale = 1, hires_levels = 1;
    SDL_GPUTexture *shown = screen; // the last frame's texture, and its scale
    int shown_scale = 1;

    app::Launcher launcher(cfg, window);
    app::ForceFeedback ffb; // the drive board on the steering device
    std::unique_ptr<app::TcpLink> link; // link play (declared first: outlives the game, which holds it)
    std::unique_ptr<rt::GameLoop> game;
    const std::string eeprom_path = pref_file("ioboard_eeprom.bin"), backup_path = pref_file("backup_ram.bin");
    auto save_nv = [&] {
        if (!game) return;
        save_file(eeprom_path, game->board().io().eeprom);
        save_file(backup_path, game->board().backup_ram());
    };
    auto start_game = [&] {
        save_nv();
        ffb.reset(); // a new game: the drive board as at power-on
        native_audio.close(); // joins callback before replacing its ROMs/engine
        audio.close();
        game.reset();
        link.reset();
        have_audio = false;
        native_fault = false;
        native_active = cfg.native_audio;
        try {
            auto images = rt::import_rom_set(cfg.rom_path);
            if (native_active) {
                if (!audio_initialized) throw std::runtime_error("Native audio needs an available audio device.");
                auto engine = std::make_unique<snd::NativeSoundEngine>(std::move(images.sound_program),
                    std::move(images.pcm1), std::move(images.pcm2));
                if (!native_audio.open(std::move(engine)))
                    throw std::runtime_error(std::string("Cannot open native audio: ") + SDL_GetError());
                native_audio.volume(cfg.volume);
                native_audio.volumes(cfg.music_volume, cfg.effects_volume);
                native_audio.mute(cfg.mute);
            } else if (audio_initialized) {
                have_audio = audio.open(snd::SoundBoard::kYmClock / 144.0, snd::SoundBoard::kPcmClock / 224.0);
                if (!have_audio) std::fprintf(stderr, "daytona: reference audio unavailable (%s)\n", SDL_GetError());
            }
            game = std::make_unique<rt::GameLoop>(std::move(images), !native_active);
            std::printf("daytona: audio backend %s\n", native_active
                ? "native (experimental, 48000 Hz device clock; no reference sound board)" : "reference");
            load_file(eeprom_path, game->board().io().eeprom);
            load_file(backup_path, game->board().backup_ram());
            if (cfg.link) { // link play: the communication board on TCP
                link = std::make_unique<app::TcpLink>(uint16_t(cfg.link_port), cfg.link_next);
                if (!link->error().empty()) throw std::runtime_error("Link play: " + link->error());
                game->board().set_link(link.get(), cfg.link_framesync);
                std::printf("daytona: link play: listening on port %d, next cabinet %s\n", cfg.link_port, cfg.link_next.c_str());
            }
            launcher.set_error("");
            return true;
        } catch (const std::exception &e) {
            native_audio.close();
            audio.close();
            game.reset();
            link.reset();
            launcher.set_error(e.what());
            return false;
        }
    };

    bool in_launcher = true, running = true, have_frame = false, new_frame = false;
    bool cursor_hidden = false; // the launcher's "Hide mouse cursor in game", in effect now
    int reported_hardware = -1; // the renderer last reported (-1: none yet)
    // Skip launcher (saved) or --autostart: straight into the game when the ROM
    // set checks out; otherwise the launcher shows, with the reason.
    if ((autostart || cfg.skip_launcher) && launcher.rom_ok() && start_game()) in_launcher = false;
    auto sync_native_audio = [&] {
        if (native_fault) in_launcher = true;
        if (!native_audio.available()) return;
        native_audio.volume(cfg.volume);
        native_audio.volumes(cfg.music_volume, cfg.effects_volume);
        native_audio.mute(cfg.mute);
        if (in_launcher || !running) native_audio.pause();
        else if (!native_audio.resume()) {
            launcher.set_error(std::string("Native audio resume failed; reset the game: ") + SDL_GetError());
            in_launcher = native_fault = true;
            native_audio.pause();
        }
    };
    sync_native_audio();
    app::Devices devices; // the gamepad and every joystick (wheels, pedals, shifters)
    // Frame pacing (launcher > Display; app/pacing.h): the arcade's speed on
    // the wall clock unless a setting syncs to the display or holds frames for VRR.
    static_assert(app::kArcadeFrameHz == kArcadeHz);
    auto display_refresh = [&] {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        if (!mode) return 0.0;
        if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
            return double(mode->refresh_rate_numerator) / mode->refresh_rate_denominator;
        return double(mode->refresh_rate);
    };
    app::Pacer pacer;
    pacer.set(app::Pacing{}, SDL_GetTicksNS());
    rt::TestHold test_hold; // the launcher's "Hold Test button"
#ifdef M2_MOBILE
    // Preserve startup detection for a controller paired before launching.
    int pad_count = 0;
    SDL_JoystickID *pads = SDL_GetGamepads(&pad_count);
    if (pads && pad_count > 0) devices.pad = SDL_OpenGamepad(pads[0]);
    SDL_free(pads);
#endif

#ifdef M2_MOBILE
    app::TouchControls touch;
    SDL_Rect touch_area{};
    int touch_width = 0, touch_height = 0;
#endif
    while (running) {
#ifdef M2_MOBILE
        int tw = 0, th = 0;
        SDL_GetWindowSize(window, &tw, &th);
        SDL_Rect area{0, 0, tw, th};
        SDL_GetWindowSafeArea(window, &area);
        if (tw != touch_width || th != touch_height || area.x != touch_area.x || area.y != touch_area.y ||
            area.w != touch_area.w || area.h != touch_area.h) {
            touch.layout(float(area.x), float(area.y), float(area.w), float(area.h));
            touch_area = area; touch_width = tw; touch_height = th;
        }
        if (in_launcher) touch.clear();
#endif
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
#ifdef M2_MOBILE
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST || e.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
                touch.clear();
                in_launcher = true;
            }
            if (!in_launcher && (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION ||
                e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED)) {
                const float tx = e.tfinger.x * touch_width, ty = e.tfinger.y * touch_height;
                if (e.type == SDL_EVENT_FINGER_DOWN)
                    in_launcher = touch.down(e.tfinger.touchID, e.tfinger.fingerID, tx, ty);
                else if (e.type == SDL_EVENT_FINGER_MOTION)
                    touch.move(e.tfinger.touchID, e.tfinger.fingerID, tx, ty);
                else if (e.type == SDL_EVENT_FINGER_UP)
                    touch.up(e.tfinger.touchID, e.tfinger.fingerID);
                else if (e.type == SDL_EVENT_FINGER_CANCELED)
                    touch.cancel(e.tfinger.touchID, e.tfinger.fingerID);
            }
#endif
            devices.handle_event(e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11) {
                cfg.fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(window, cfg.fullscreen);
                cfg.save();
            }
            if (in_launcher) {
                if (!launcher.handle_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
                in_launcher = true; // pause and show the launcher
            }
        }

#ifdef M2_MOBILE
        if (in_launcher) touch.clear();
        cfg.controls.touch = touch.values;
        cfg.controls.touch_steering = touch.steering;
#endif
        // The mouse cursor: hidden while the game plays if the launcher says so,
        // shown whenever the launcher is open. ImGui sets the cursor on every
        // frame it draws (mobile draws its touch controls in play), so it is
        // told to leave the cursor alone while hidden.
        if (const bool hide = cfg.hide_cursor && game && !in_launcher; hide != cursor_hidden) {
            hide ? SDL_HideCursor() : SDL_ShowCursor();
            if (hide) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            else ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            cursor_hidden = hide;
        }

        sync_native_audio();
        if (native_active) {
            const auto health = native_audio.stats();
            // An effect the native mixer lacks (the MultiPCM LFO: vibrato, which
            // linked play's music uses) plays without it: said once, not a fault.
            static uint32_t unsupported_reported = 0;
            if (health.unsupported > unsupported_reported) {
                if (!unsupported_reported)
                    std::fprintf(stderr, "daytona: native audio: an effect it does not have yet (vibrato); playing without it\n");
                unsupported_reported = health.unsupported;
            }
            if (health.failed || health.invalid) {
                native_audio.pause();
                char message[192];
                std::snprintf(message, sizeof message,
                    "Native audio fault (callback %u, invalid %u, unsupported %u); reset or select reference audio.",
                    health.failed, health.invalid, health.unsupported);
                launcher.set_error(message);
                in_launcher = native_fault = true;
            }
        }

        // Game: arcade speed (57.52 frames/s), presented at the display's rate,
        // unless a frame pacing setting says otherwise.
        const double refresh = display_refresh();
        if (const app::Pacing p = app::choose_pacing(refresh, {cfg.pace_smooth, cfg.pace_sync_display, cfg.pace_vrr});
            !(p == pacer.pacing())) {
            pacer.set(p, SDL_GetTicksNS());
            std::printf("daytona: frame pacing: %s\n", p.describe(refresh).c_str());
        }
        launcher.set_pacing_status(pacer.pacing().describe(refresh));
        if (game && !in_launcher)
            if (const uint64_t wait = pacer.wait_ns(SDL_GetTicksNS())) SDL_DelayPrecise(wait); // VRR: hold the frame
        const uint64_t now = SDL_GetTicksNS();
        if (game && !in_launcher) {
            game->set_aspect(cfg.aspect_ratio()); // widescreen: no-op unless it changed
            game->set_hud_edges(cfg.hud_edges);
            game->set_frame_skip(cfg.draw_mode);
            game->board().video().set_external_3d(cfg.renderer == "hardware" && gpu.ok(), true);
            game->set_stretch_backdrop(cfg.stretch_backdrop);
            rt::GameLoop::set_draw_distance(cfg.draw_distance);
            for (int frames = pacer.frames(now); frames > 0; --frames) {
                rt::Inputs inputs = cfg.controls.sample(SDL_GetKeyboardState(nullptr), devices);
                if (cfg.hold_test != test_hold.armed()) cfg.hold_test ? test_hold.arm() : test_hold.cancel();
                if (test_hold.apply(game->frames(), inputs.in0)) cfg.hold_test = false; // held: the box clears
                game->run_frame(inputs);
#ifdef M2_MOBILE
                touch.consumed();
                cfg.controls.touch = touch.values;
                cfg.controls.touch_steering = touch.steering;
#endif
                if (native_active) {
                    const auto bytes = game->board().take_sound_bytes();
                    if (!native_audio.send(bytes.data(), bytes.size())) {
                        native_audio.pause();
                        launcher.set_error("Native audio command queue failed; reset the game or select reference audio.");
                        in_launcher = native_fault = true;
                        break;
                    }
                }
                new_frame = have_frame = true;
                if (max_frames && game->frames() >= max_frames) running = false;
            }
            // the drive board's commands this frame, as force feedback
            ffb.update(std::exchange(game->board().io().drive_commands, {}), devices, cfg.controls, cfg.ffb_strength,
                       cfg.ffb_invert, cfg.ffb_log, game->frames(),
                       (float(game->board().io().inputs.steer) - 128.0f) / 96.0f,
#ifdef __linux__
                       cfg.ffb_centring_override); // Linux only: drivers without a spring effect
#else
                       false);
#endif
            launcher.set_ffb_device(ffb.device_kind());
            launcher.set_link_status(link_status(game->board().comm_board(), link.get(), cfg));
            if (game->sound()) {
                game->sound()->set_volumes(cfg.music_volume, cfg.effects_volume);
                if (have_audio) audio.push(*game->sound(), cfg.mute ? 0.0f : cfg.volume, pacer.pacing().game_hz / kArcadeHz);
                else game->sound()->take_fm(), game->sound()->take_pcm(); // nowhere to play it
            }
        } else {
            pacer.pause(now);
            ffb.stop(); // paused in the launcher: let the wheel go
        }

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (!cmd) return fail("SDL_AcquireGPUCommandBuffer");
        const bool hardware = game && game->board().video().external_3d();
        if (game && hardware != reported_hardware) { // say which renderer is drawing, whenever it changes
            std::printf("daytona: renderer %s\n", hardware ? "hardware (GPU)" : "software (CPU)");
            reported_hardware = hardware;
        }
        if (new_frame && hardware) {
            screen_w = game->screen_width();
            const int scale = std::clamp(cfg.supersampling, 1, 4);
            if (scale > 1 && scale != hires_scale) {
                if (hires) SDL_ReleaseGPUTexture(dev, hires);
                SDL_GPUTextureCreateInfo hi = ti;
                hi.width = Uint32(kMaxW * scale);
                hi.height = Uint32(H * scale);
                hires_levels = scale >= 4 ? 3 : 2;
                hi.num_levels = Uint32(hires_levels);
                hires = SDL_CreateGPUTexture(dev, &hi);
                hires_scale = hires ? scale : 1;
                if (!hires) std::fprintf(stderr, "daytona: %dx super sampling unavailable: %s\n", scale, SDL_GetError());
            }
            if (scale > 1 && hires) {
                gpu.render(cmd, hires, screen_w, H, game->board().video(), scale);
                SDL_GenerateMipmapsForGPUTexture(cmd, hires);
                shown = hires, shown_scale = scale;
            } else {
                gpu.render(cmd, screen, screen_w, H, game->board().video());
                shown = screen, shown_scale = 1;
            }
            new_frame = false;
        }
        if (new_frame) {
            void *p = SDL_MapGPUTransferBuffer(dev, upload, true);
            screen_w = game->screen_width();
            std::memcpy(p, game->screen().data(), size_t(screen_w) * H * 4);
            SDL_UnmapGPUTransferBuffer(dev, upload);
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = upload;
            src.pixels_per_row = Uint32(screen_w);
            SDL_GPUTextureRegion dst{};
            dst.texture = screen;
            dst.w = Uint32(screen_w);
            dst.h = H;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, true);
            SDL_EndGPUCopyPass(copy);
            shown = screen, shown_scale = 1;
            new_frame = false;
        }

        ImDrawData *draw = nullptr;
        if (in_launcher
#ifdef M2_MOBILE
            || game != nullptr
#endif
        ) {
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            if (in_launcher) switch (launcher.draw(game != nullptr, devices)) {
            case app::Launcher::StartGame:
            case app::Launcher::Reset:
                if (start_game()) in_launcher = false, have_frame = false;
                break;
            case app::Launcher::Resume: in_launcher = false; break;
            case app::Launcher::Quit: running = false; break;
            default: break;
            }
#ifdef M2_MOBILE
            else app::draw_touch_controls(touch);
#endif
            sync_native_audio();
            ImGui::Render();
            draw = ImGui::GetDrawData();
            ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
        }

        SDL_GPUTexture *swap = nullptr;
        Uint32 sw = 0, sh = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh)) return fail("swapchain");
        if (swap) {
            if (have_frame) { // the game's screen at its own shape, letterboxed
                const double scale = std::min(double(sw) / screen_w, double(sh) / H);
                const Uint32 dw = Uint32(screen_w * scale), dh = Uint32(H * scale);
                // a higher resolution frame: from its smallest level still at least the window's size
                Uint32 level = 0;
                while (shown == hires && int(level) + 1 < hires_levels && Uint32((H * shown_scale) >> (level + 1)) >= dh)
                    ++level;
                SDL_GPUBlitInfo blit{};
                blit.source.texture = shown;
                blit.source.mip_level = level;
                blit.source.w = Uint32(screen_w * shown_scale) >> level;
                blit.source.h = Uint32(H * shown_scale) >> level;
                blit.destination.texture = swap;
                blit.destination.x = (sw - dw) / 2;
                blit.destination.y = (sh - dh) / 2;
                blit.destination.w = dw;
                blit.destination.h = dh;
                blit.load_op = SDL_GPU_LOADOP_CLEAR;
                blit.clear_color = SDL_FColor{0, 0, 0, 1};
                blit.filter = SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(cmd, &blit);
            }
            if (draw || !have_frame) { // the launcher on top (or a clear screen)
                SDL_GPUColorTargetInfo ct{};
                ct.texture = swap;
                ct.load_op = have_frame ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
                ct.store_op = SDL_GPU_STOREOP_STORE;
                ct.clear_color = SDL_FColor{0.05f, 0.05f, 0.08f, 1};
                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
                if (draw) ImGui_ImplSDLGPU3_RenderDrawData(draw, cmd, pass);
                SDL_EndGPURenderPass(pass);
            }
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    save_nv();
    cfg.save();
    if (game) std::printf("daytona: %llu frames\n", (unsigned long long)game->frames());
    if (native_audio.available()) {
        native_audio.pause();
        const auto stats = native_audio.stats();
        std::printf("daytona: native audio callbacks=%u frames=%u queued=%u overflows=%u failed=%u invalid=%u unsupported=%u\n",
            stats.callbacks, stats.frames, stats.queued, stats.overflows, stats.failed, stats.invalid, stats.unsupported);
    }
    native_audio.close();
    audio.close();
    SDL_WaitForGPUIdle(dev);
    gpu.shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseGPUTransferBuffer(dev, upload);
    SDL_ReleaseGPUTexture(dev, screen);
    if (hires) SDL_ReleaseGPUTexture(dev, hires);
    SDL_ReleaseWindowFromGPUDevice(dev, window);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(window);
    ffb.close();
    devices.close_all();
    SDL_Quit();
    return 0;
}
