#pragma once

// vitaGL backend for the Vita GPU path (build option DAYTONA_VITA_GPU_GL).
// Alternative to gpu_fast.cpp (libvita2d); both initialise sceGxm, so only one can be linked.
// GpuGlRenderer mirrors GpuFastRenderer's public interface so main_gpu.cpp needs few #ifdefs.

#include "runtime/video.h"
#include "flat_index.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

// vitaGL.h does not include the sceGxm headers it needs: they must come first.
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

// Same packing as libvita2d's RGBA8: bytes in memory are r, g, b, a.
#ifndef RGBA8
#define RGBA8(r, g, b, a) ((((a) & 0xFF) << 24) | (((b) & 0xFF) << 16) | (((g) & 0xFF) << 8) | (((r) & 0xFF) << 0))
#endif

namespace vita {

// ---- Frame level API (replaces the vita2d_* calls in main_gpu.cpp) ----------
bool gl_init();              // init vitaGL + shaders; false on failure (see gl_error())
const char *gl_error();      // last init error, empty when none
void gl_begin_frame();       // clear the screen and the recorded draw list
void gl_end_frame();         // draw everything recorded since gl_begin_frame() and swap
void gl_fini();              // release shaders/buffers
void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba); // display pixels (960x544)

// ImGui menu (imgui_vita.h): an RGBA texture (linear, clamped) and textured triangles in
// display pixels, vertex colour bytes r, g, b, a, alpha blended, drawn in recording order
// inside the scissor rectangle [x0, x1) x [y0, y1). Recorded like everything else and run
// by gl_end_frame().
struct GlUiVertex { float x, y, u, v; uint32_t rgba; };
uint32_t gl_create_ui_texture(int w, int h, const void *rgba); // GLuint, 0 on failure
void gl_delete_ui_texture(uint32_t texture);
void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1);

// What the main loop did during one iteration (one presented frame), in microseconds.
// main_gpu.cpp fills it and calls gl_profile_loop() once per iteration; gl.log prints
// averages next to the renderer's own timings, so a whole frame is accounted for.
struct GlLoopProfile {
    bool menu = false;
    unsigned board_frames = 0;   // emulated Model 2 frames (normally 0 or 1 per present)
    uint64_t board = 0;          // whole emulated frame (GameLoop::run_frame_sound_packet)
    uint64_t board_core = 0;     //   i960 main CPU + synchronous TGP + scheduling
    uint64_t board_geometry = 0; //   vblank start: geometry, polygon list for the GPU
    uint64_t board_video = 0;    //   vblank end: software video (System 24 layers, CPU raster)
    uint64_t sound_wait = 0;     // main thread blocked waiting for the sound worker
    uint64_t sound_worker = 0;   // sound board time on the worker thread (runs in parallel)
    uint64_t prepare = 0;        // GpuGlRenderer::prepare_frame (texture cache resets)
};
void gl_profile_loop(const GlLoopProfile &profile);
// Time the last gl_end_frame() spent writing gl.log (diagnostics builds), in microseconds:
// perf.log counts it as logging, not as frame end.
uint64_t gl_last_diagnostic_us();

class GpuGlRenderer {
public:
    GpuGlRenderer();
    ~GpuGlRenderer(); // stops the 2D worker; main_gpu calls shutdown() before gl_fini()
    GpuGlRenderer(const GpuGlRenderer &) = delete;
    GpuGlRenderer &operator=(const GpuGlRenderer &) = delete;

    bool ok() const { return ok_; }
    void reset_materials();      // drop every cached texture and palette (mode change, new game)
    void prepare_frame();        // call before gl_begin_frame(), also for menus
    void shutdown();
    void draw(rt::Video &video);       // System 24 layers + Model 2 polygons (the only Vita GPU case)

    // ---- Diagnostics read by main_gpu.cpp (same names as GpuFastRenderer) ----
    double last_gpu_ms() const { return last_gpu_ms_; }
    uint64_t last_sort_us() const { return last_sort_us_; } // priority sort feeding the depth ranks
    uint64_t last_texture_us() const { return last_texture_us_; } // index texture builds (inside polygons)
    uint64_t last_vertex_us() const { return last_vertex_us_; }   // vertex writing (inside polygons)
    uint64_t last_polygon_us() const { return last_polygon_us_; }
    uint64_t last_tile_us() const { return last_tile_us_; }
    uint64_t last_upload_us() const { return last_upload_us_; }  // main core waiting for the 2D worker
    uint64_t last_2d_worker_us() const { return last_2d_worker_us_; } // 2D worker job (core 2, in parallel)
    bool worker_2d_threaded() const { return s24_thread_ >= 0; }
    int worker_2d_result() const { return s24_create_result_; }
    static int worker_2d_core();
    std::size_t cached_bytes() const { return cached_bytes_; }
    std::size_t cached_materials() const { return palette_index_.size(); } // palette rows
    std::size_t cached_sources() const { return sources_.size(); }         // index textures
    std::size_t reserved_bytes() const { return 0; }
    unsigned cache_resets() const { return cache_resets_; }
    unsigned material_drops() const { return material_drops_; }
    unsigned material_builds() const { return material_builds_; }
    unsigned material_defers() const { return material_defers_; }
    std::size_t submitted_vertices() const { return submitted_vertices_; }
    unsigned system24_quads() const { return system24_quads_; }
    unsigned system24_uploaded_tiles() const { return system24_uploaded_tiles_; }
    unsigned textured_polys() const { return textured_polys_; }
    unsigned solid_polys() const { return solid_polys_; }
    unsigned checker_polys() const { return checker_polys_; }
    unsigned textured_checker_polys() const { return textured_checker_polys_; }
    unsigned clip_changes() const { return clip_changes_; }
    // Not tracked by this backend.
    unsigned pool_drops() const { return 0; }
    unsigned subdivided_polys() const { return 0; }
    unsigned min_pool_free() const { return 0; }
    unsigned textured_draws() const { return 0; }
    unsigned solid_draws() const { return 0; }
    unsigned shader_setups() const { return 0; }
    unsigned state_reuses() const { return 0; }
    unsigned draw_errors() const { return 0; }

private:
    // Model 2 textures are 4-bit texel indices; the colour comes from a palette chosen per
    // polygon. As on the real board, the GPU filters the INDEX, then looks the colour up:
    //   * Source  = the indices of one texture region, as a GL_LUMINANCE_ALPHA texture
    //               (L = index * 16, A = 0 for a transparent texel). Shared by all palettes.
    //   * Palette = one 128-entry row of the palette texture per (luma base, colour, luma):
    //               entry k = colour of luminance step k (index * 8 + filtered fraction).
    struct SourceKey {
        uint16_t h0 = 0, h2 = 0; // texheader[0] size/mirror/transparent, texheader[2] position
        bool operator==(const SourceKey &o) const { return h0 == o.h0 && h2 == o.h2; }
    };
    struct SourceKeyHash {
        std::size_t operator()(const SourceKey &k) const { return std::hash<uint32_t>()(k.h0 | uint32_t(k.h2) << 16); }
    };
    struct Source {
        uint32_t texture = 0;                // GLuint
        uint32_t source_w = 1, source_h = 1; // size in Model 2 texels
        bool transparent = false;            // texel 15 is see-through: alpha-test shader
        std::size_t bytes = 0;
    };

    static constexpr std::size_t kSourceCacheBytes = 24u * 1024u * 1024u;
    // New index textures per frame: at most kSourceBuildBudget, and at most
    // kSourceTexelBudget texels decoded (the first build of a frame always runs).
    static constexpr unsigned kSourceBuildBudget = 32;
    static constexpr uint32_t kSourceTexelBudget = 128u * 1024u; // e.g. two 256x256 or 128 32x32
    static constexpr uint32_t kTextureLimit = 512;     // max GL texture side
    static constexpr uint32_t kPaletteWidth = 128;     // luminance steps per palette
    static constexpr uint32_t kPaletteRows = 2048;     // palettes in the palette texture
    static constexpr uint32_t kPaletteFrameRows = 512; // rows kept free for one frame

    // Textures
    uint32_t make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t, uint32_t format,
                          const void *pixels);
    static void *texture_memory(uint32_t texture); // CPU pointer to a texture's pixels (linear rows)
    const Source *source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    int palette_row(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    uint32_t solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const;
    void clear_cache();
    // 2D layers (System 24). Drawn one frame late, in phase with the pipelined 3D: draw()
    // shows the slot prepared during the previous frame while the 2D worker thread (core 2,
    // the sound core) prepares this frame's into the other slot: tile and palette uploads,
    // then the layer rectangles. The worker makes no GL call; draw() waits for it before
    // returning, so the board never runs while it reads Video, and gl_begin_frame() has
    // waited for the GPU before the slot it writes (shown one frame earlier) is rewritten.
    struct S24Rect { int x0, x1, y0, y1, h, v; }; // native pixels; h = horizontal scroll, v = source line of y0
    struct S24Run { int layer; uint32_t first, count; };  // consecutive rects of one source layer
    struct S24Slot {
        std::array<uint32_t, 8> textures{};  // GLuint: 0-3 background layers, 4-7 foreground layers (pen numbers)
        std::array<void *, 8> texels{};      // their pixels (fixed while the textures live)
        uint32_t palette_texture = 0;        // GLuint, 128x64 RGBA: the 8192 System 24 pens
        uint32_t *palette = nullptr;
        uint64_t generation = UINT64_MAX;    // Video::system24_texture_generation() of the tiles
        uint64_t palette_generation = UINT64_MAX;
        bool valid = false;                  // prepared since the last reset
        uint32_t backdrop = 0;               // pen 0, RGBA
        std::vector<S24Rect> rects;          // same rectangles for the background and foreground passes
        std::vector<S24Run> runs;            // in drawing order (layer 3 to 0)
        std::array<std::vector<S24Rect>, 2> split; // scratch of the split modes
        unsigned uploaded_tiles = 0;
    };
    static void s24_prepare(S24Slot &slot, const rt::Video &video); // the worker's job (no GL)
    static int s24_thread_entry(SceSize args, void *argp);
    bool s24_start_thread();
    void s24_stop_thread();
    void s24_kick(const rt::Video &video); // starts preparing the back slot (inline without thread)
    void s24_join();
    void s24_emit(const S24Slot &slot, bool foreground);
    // Polygons
    void draw_polygons(rt::Video &video);

    bool ok_ = false, shutdown_ = false;
    std::array<S24Slot, 2> s24_{};
    unsigned s24_back_ = 0;                        // slot prepared this frame; the other one is shown
    int s24_thread_ = -1, s24_start_ = -1, s24_done_ = -1, s24_create_result_ = 0; // SceUID
    std::atomic<bool> s24_stop_{false};
    bool s24_busy_ = false;                        // a job was started and not joined
    const rt::Video *s24_video_ = nullptr;         // the job: Video -> slot (set before the start signal)
    S24Slot *s24_job_ = nullptr;
    uint64_t s24_job_us_ = 0;                      // written by the worker, read after the join
    std::unordered_map<SourceKey, Source, SourceKeyHash> sources_; // storage (stable addresses)
    FlatIndex<uint32_t, const Source *, 12> source_cache_;         // fast lookup in front of sources_
    std::vector<uint8_t> index_scratch_;
    std::size_t cached_bytes_ = 0;
    bool cache_reset_pending_ = false;
    // Palettes: rows of the palette texture, filled in order of first use.
    uint32_t palette_texture_ = 0;     // GLuint, kPaletteWidth x kPaletteRows RGBA
    uint32_t *palette_data_ = nullptr; // its pixels, written directly
    FlatIndex<uint32_t, uint32_t, 12> palette_index_; // palette key -> row (at most kPaletteRows)
    uint32_t palette_used_ = 0;        // rows
    uint8_t gamma_[256]{};

    // Diagnostics
    double last_gpu_ms_ = 0.0;
    uint64_t last_polygon_us_ = 0, last_tile_us_ = 0, last_upload_us_ = 0, last_sort_us_ = 0, last_texture_us_ = 0,
             last_2d_worker_us_ = 0, last_vertex_us_ = 0;
    unsigned cache_resets_ = 0, material_drops_ = 0, material_builds_ = 0, material_defers_ = 0,
             system24_quads_ = 0, system24_uploaded_tiles_ = 0, clip_changes_ = 0, textured_polys_ = 0,
             solid_polys_ = 0, checker_polys_ = 0, textured_checker_polys_ = 0;
    std::size_t submitted_vertices_ = 0;
};

} // namespace vita