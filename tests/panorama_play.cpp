// Local-ROM checks of the exact settings policy used by the playable app.
// A second game retains the diagnostic panorama opt-in throughout, providing
// a reference with uninterrupted scroll tracking across native-view intervals.
#include "app/gpu/gpu_renderer.h"
#include "app/video_settings.h"
#include "../tools/common/input_script.h"
#include "../tools/common/nvram.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

void check_play_panorama(SDL_GPUDevice *device, app::GpuRenderer &gpu, SDL_GPUTexture *target,
                         SDL_GPUTransferBuffer *download, const char *images, const char *nvram,
                         int count, char **inputs) {
    constexpr int H = rt::Video::H;
    unsigned comparisons = 0, active_frames = 0, fallback_frames = 0;
    unsigned uploads = 0;
    uint64_t uploaded_instance = 0;
    uint8_t uploaded_course = 255;
    for (int input = 0; input < count; ++input) for (unsigned restart = 0; restart < 2; ++restart) {
        tools::Script script;
        script.load(inputs[input]);
        rt::GameLoop game(images), reference(images);
        tools::load_nvram(game, nvram);
        tools::load_nvram(reference, nvram);
        app::Config cfg;
        cfg.renderer = "hardware";
        cfg.aspect = restart ? "32:9" : "";
        auto &video = game.board().video();
        auto &control = reference.board().video();
        video.panorama().only = control.panorama().only = true;
        unsigned last_change = 0, checked = 0;
        unsigned previous_course = 255, course_changes = 0;
        for (unsigned frame = 1; frame <= (restart ? 3600u : 18000u); ++frame) {
            auto require = [&](bool ok, const char *message) {
                if (!ok) throw std::runtime_error(std::string(inputs[input]) + ", restart " + std::to_string(restart) +
                    ", frame " + std::to_string(frame) + ", aspect " + cfg.aspect + ": " + message);
            };
            if (!restart) {
                struct Change { unsigned frame; const char *aspect; };
                for (const auto &change : {Change{2400, "32:9"}, {3000, ""}, {3200, "16:9"}, {3210, "32:9"},
                        {3220, ""}, {3230, "16:10"}, {3240, "21:9"}, {3250, "32:9"}, {3260, ""},
                        {3500, "32:9"}, {3510, "16:9"}, {3520, "32:9"}, {17000, ""}, {17020, "32:9"}}) {
                    if (frame == change.frame) { cfg.aspect = change.aspect; last_change = frame; }
                }
                if (frame == 3530 || frame == 3538) {
                    cfg.renderer = frame == 3530 ? "software" : "hardware";
                    last_change = frame;
                }
                if (frame == 3540 || frame == 3548 || frame == 3556) {
                    cfg.draw_mode = frame == 3540 ? 2 : frame == 3548 ? 1 : 0;
                    last_change = frame;
                }
            }
            app::apply_video_settings(game, cfg, true);
            app::apply_video_settings(reference, cfg, true);
            control.panorama().original = true; // existing diagnostic mode
            game.run_frame(script.at(game.board().frame()));
            reference.run_frame(script.at(reference.board().frame()));
            require(video.panorama_active() == control.panorama_active(), "automatic mode lost readiness versus explicit mode");
            if (frame < 2400 && !restart) require(video.panorama().indices.empty(), "native startup allocated panorama artwork");
            if (video.panorama_active()) {
                ++active_frames;
                if (previous_course != video.panorama().cached_course) {
                    previous_course = video.panorama().cached_course;
                    ++course_changes;
                }
            } else ++fallback_frames;
            // Check every frame around setting changes, sampled loading frames,
            // then the longer race/attract replay. Raw pixels stay in memory.
            if (frame % 300 && frame - last_change > 9 && !(frame >= 2400 && frame <= 3100 && frame % 5 == 0)) continue;
            const int width = game.screen_width(), margin = (width - rt::Video::W) / 2;
            const auto mem = video.gpu_mem(), control_mem = control.gpu_mem();
            const auto &polys = video.gpu_polys();
            const int windows = video.gpu_windows();
            control.set_external_3d(false);
            control.screen_update(control.gpu_polys(), control.gpu_windows(), control_mem);
            video.set_external_3d(false);
            video.screen_update(polys, windows, mem);
            require(video.screen() == control.screen(), "automatic/explicit software background mismatch");
            const auto software = video.screen();
            if (video.panorama_active()) {
                // Keep the live phase, but use the capture sweep's scalar,
                // full-width compositor as a reference for the margin path.
                video.panorama().sweep = true;
                video.screen_update(polys, windows, mem);
                require(video.screen() == software, "margin/scalar panorama mismatch");
                video.panorama().sweep = false;
                ++comparisons;
            }
            video.set_external_3d(true, true);
            video.screen_update(polys, windows, mem);
            if (video.panorama_active() && (uploaded_instance != video.instance() ||
                                           uploaded_course != video.panorama().cached_course)) {
                ++uploads;
                uploaded_instance = video.instance();
                uploaded_course = video.panorama().cached_course;
            }
            auto *command = SDL_AcquireGPUCommandBuffer(device);
            require(command != nullptr, SDL_GetError());
            gpu.render(command, target, width, H, video);
            auto *copy = SDL_BeginGPUCopyPass(command);
            SDL_GPUTextureRegion source{};
            source.texture = target; source.w = width; source.h = H; source.d = 1;
            SDL_GPUTextureTransferInfo destination{};
            destination.transfer_buffer = download; destination.pixels_per_row = width;
            SDL_DownloadFromGPUTexture(copy, &source, &destination);
            SDL_EndGPUCopyPass(copy);
            auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
            require(fence != nullptr, SDL_GetError());
            const bool waited = SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);
            require(waited, SDL_GetError());
            const auto *pixels = static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(device, download, false));
            require(pixels != nullptr, SDL_GetError());
            const bool same = !std::memcmp(pixels, software.data(), software.size() * sizeof(uint32_t));
            SDL_UnmapGPUTransferBuffer(device, download);
            require(same, "automatic CPU/GPU background mismatch");
            require(gpu.panorama_uploads() == uploads, "aspect/renderer switch re-uploaded sky or restart reused stale cache");
            video.panorama().original = false;
            video.set_external_3d(false);
            video.screen_update(polys, windows, mem);
            for (int y = 0; y < H; ++y)
                require(!std::memcmp(software.data() + y * width + margin,
                                    video.screen().data() + y * width + margin, rt::Video::W * 4), "original centre changed");
            video.panorama().original = width > rt::Video::W;
            ++checked;
            comparisons += 3;
        }
        std::printf("%s restart %u: %u frames compared on CPU/GPU and original centre, %u course activations\n",
                    inputs[input], restart, checked, course_changes);
    }
    std::printf("PASS: %u pixel comparisons, %u active / %u fallback frames, %u uploads; aspect changes, renderer changes and restarts\n",
                comparisons, active_frames, fallback_frames, uploads);
}
