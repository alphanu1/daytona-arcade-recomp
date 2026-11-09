// Local-ROM integration fixture: palette fades and renderer reuse without
// changing guest RAM or retaining screenshots. Run explicitly with the same
// cabinet snapshot and input replay as original_panorama_validate.py.
#include "app/gpu/gpu_renderer.h"
#include "runtime/game_loop.h"
#include "../tools/common/input_script.h"
#include "../tools/common/nvram.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

void check_play_panorama(SDL_GPUDevice *, app::GpuRenderer &, SDL_GPUTexture *, SDL_GPUTransferBuffer *,
                         const char *, const char *, int, char **);

int main(int argc, char **argv) {
    const bool play = argc > 3 && std::strcmp(argv[3], "--play") == 0;
    if (argc < (play ? 5 : 4)) {
        std::fprintf(stderr, "usage: m2panoramacheck IMAGES_DIR NVRAM_DIR [--play] INPUTS [INPUTS ...]\n");
        return 2;
    }
    auto require = [](bool ok, const char *message) { if (!ok) throw std::runtime_error(message); };
    SDL_GPUDevice *device = nullptr;
    SDL_GPUTexture *target = nullptr;
    SDL_GPUTransferBuffer *download = nullptr;
    app::GpuRenderer gpu;
    int status = 0;
    try {
        require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
                                     false, nullptr);
        require(device != nullptr, SDL_GetError());
        require(gpu.init(device, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM), gpu.error().c_str());
        constexpr int W = 1366, H = 384, margin = (W - 496) / 2;
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ti.width = W; ti.height = H; ti.layer_count_or_depth = ti.num_levels = 1;
        target = SDL_CreateGPUTexture(device, &ti);
        SDL_GPUTransferBufferCreateInfo bi{};
        bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        bi.size = W * H * 4;
        download = SDL_CreateGPUTransferBuffer(device, &bi);
        require(target && download, SDL_GetError());
        if (play) check_play_panorama(device, gpu, target, download, argv[1], argv[2], argc - 4, argv + 4);
        else for (unsigned instance = 0; instance < unsigned(argc - 3) * 2; ++instance) {
            tools::Script script;
            script.load(argv[3 + instance / 2]);
            rt::GameLoop game(argv[1]);
            tools::load_nvram(game, argv[2]);
            auto &video = game.board().video();
            video.panorama().original = video.panorama().only = true;
            video.set_external_3d(true, true);
            game.set_aspect(32.0 / 9.0);
            for (unsigned frame = 0; frame < 3600; ++frame) game.run_frame(script.at(game.board().frame()));
            require(video.panorama_active(), "fixture did not reach a supported panorama");
            const auto &polys = video.gpu_polys();
            const auto mem = video.gpu_mem();
            const int windows = video.gpu_windows();
            std::vector<uint8_t> palette(mem.palram, mem.palram + rt::Video::kGpuPens * 2);
            const auto original_palette = palette;
            std::vector<uint32_t> first;
            auto read_gpu = [&](int width) {
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
                const auto *data = static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(device, download, false));
                require(data != nullptr, SDL_GetError());
                std::vector<uint32_t> pixels(data, data + width * H);
                SDL_UnmapGPUTransferBuffer(device, download);
                return pixels;
            };
            for (unsigned level : {255u, 128u, 0u, 255u}) {
                // Change only Video's derived pens. Guest palette/translation
                // memory and game state are unchanged throughout the fixture.
                for (unsigned i = 0; i < rt::Video::kGpuPens; ++i) {
                    const unsigned value = original_palette[i * 2] | original_palette[i * 2 + 1] << 8;
                    unsigned faded = value & 0x8000;
                    for (unsigned shift : {0u, 5u, 10u}) faded |= (((value >> shift) & 31) * level / 255) << shift;
                    palette[i * 2] = uint8_t(faded); palette[i * 2 + 1] = uint8_t(faded >> 8);
                    video.palette_w(i, palette.data(), mem.colorxlat);
                }
                video.panorama().original = true;
                video.set_external_3d(true, true);
                video.screen_update(polys, windows, mem);
                require(video.panorama_active(), "fixture panorama became inactive");
                const auto hardware = read_gpu(W);
                video.set_external_3d(false);
                video.screen_update(polys, windows, mem);
                require(hardware == video.screen(), "palette fixture CPU/GPU mismatch");
                video.panorama().sweep = true; // same phase, original scalar compositor
                video.screen_update(polys, windows, mem);
                require(hardware == video.screen(), "faded margin/scalar panorama mismatch");
                video.panorama().sweep = false;
                // Compare widths on this frozen state; independent replays
                // can reach different guest camera phases at the same frame.
                struct View { double aspect; int width; };
                for (const auto &view : {View{16.0 / 10.0, 614}, View{16.0 / 9.0, 682}, View{21.0 / 9.0, 896}}) {
                    game.set_aspect(view.aspect);
                    require(game.screen_width() == view.width, "unexpected aspect width");
                    video.set_external_3d(false);
                    video.screen_update(polys, windows, mem);
                    const auto narrow = video.screen();
                    for (int y = 0; y < H; ++y)
                        require(!std::memcmp(narrow.data() + y * view.width,
                                            hardware.data() + y * W + (W - view.width) / 2,
                                            view.width * 4), "same-frame aspect crop changed source scale");
                    video.set_external_3d(true, true);
                    video.screen_update(polys, windows, mem);
                    require(read_gpu(view.width) == narrow, "narrow frozen frame CPU/GPU mismatch");
                }
                game.set_aspect(32.0 / 9.0);
                video.set_external_3d(false);
                video.panorama().original = false;
                video.screen_update(polys, windows, mem);
                for (int y = 0; y < H; ++y)
                    require(!std::memcmp(hardware.data() + y * W + margin,
                                        video.screen().data() + y * W + margin, 496 * 4), "faded centre changed");
                if (first.empty()) first = hardware;
                else if (level == 255) require(first == hardware, "restored palette did not restore the sky");
                else require(first != hardware, "palette did not affect cached sky");
                if (!level) for (uint32_t pixel : hardware) require(!(pixel & 0xffffff), "zero palette did not fade to black");
                require(!std::memcmp(mem.palram, original_palette.data(), original_palette.size()), "guest palette was changed");
            }
            require(gpu.panorama_uploads() == instance + 1, "palette changes re-uploaded sky or instance reset reused stale cache");
            std::printf("instance %u, course %u, height %u: four palette stages match CPU/GPU and legacy centre; one upload\n",
                        instance + 1, unsigned(video.panorama().cached_course), video.panorama().source_height());
        }
        if (!play) std::puts("PASS: fades, same-frame aspect crops, CPU/GPU agreement, cache reuse and new game instance");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "panorama check: %s\n", error.what());
        status = 1;
    }
    gpu.shutdown();
    if (download) SDL_ReleaseGPUTransferBuffer(device, download);
    if (target) SDL_ReleaseGPUTexture(device, target);
    if (device) SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return status;
}
