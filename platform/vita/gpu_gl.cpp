// vitaGL renderer for the Vita GPU path.
//
// One frame:
//   gl_begin_frame()       clear the screen and the depth buffer, empty the draw list
//   GpuGlRenderer::draw()  CPU only: fills the draw list (vertices + commands)
//   gl_end_frame()         uploads all the vertices to one VBO, runs the commands, swaps
//
// Polygons
//   * Visibility follows the Model 2 draw priority, as in the CPU reference renderer:
//     higher window first, then smaller z sort key (GeoPoly::z, float_to_zval of the
//     geometrizer), then newest polygon first; the first polygon to cover a pixel wins.
//     Each polygon gets a unique depth from its rank in that order, so the depth buffer
//     reproduces it whatever the submission order: polygons are grouped by (clip
//     rectangle, shader, texture) into one draw call per group.
//   * Projection is done by the GPU. Each vertex is sent in homogeneous form
//     (x * w, y * w, w), w being the vertex distance (pz). The GPU divides by w and
//     interpolates the texture coordinates with perspective correction on its own.
//   * Textures work like the real board: the GPU filters the 4-bit texel INDEX, then the
//     fragment shader looks its colour up in the polygon's palette (luma table + colour +
//     polygon luma). Texel indices are uploaded once per texture region; a palette is one
//     128-texel row of a shared palette texture, selected per vertex.
//   * Transparency: no blending, like the real board. Texel 15 can be transparent
//     (alpha test) and the Model 2 "checker" flag keeps one native pixel out of two.
//
// 2D layers (System 24 tilemaps) are alpha-tested textured quads placed by the depth buffer:
// foreground before the polygons, background after them (see k2DLayersByDepth). Their
// textures hold pen NUMBERS; the Layer shader looks the colour up in a 128x64 palette
// texture (unit 2). A palette change (fades, flashes) rewrites that 32 KB texture only,
// where colour textures had to rewrite all 16384 tiles (~67 ms on the Vita).
// The 2D is shown one frame late, like the 3D of the pipelined geometrizer, so both are
// in phase: two sets of layer textures; while the frame shows the set prepared during the
// previous frame, a worker thread on core 2 (the sound core, the least loaded) uploads
// this frame's tiles and palette into the other set and computes its layer rectangles.
//
// GPU memory written in place: gl_begin_frame() waits for the GPU (cheap: the previous
// frame finished long ago while the CPU emulated). After that, the frame's vertices and the
// changed texture pixels are written STRAIGHT into GPU-visible memory, like gpu_fast. glTexSubImage2D is avoided on purpose: on a texture
// used in the last frames, vitaGL first allocates a new copy and copies the old pixels
// back from GPU memory, which the CPU reads very slowly (~30 ms/frame for the System 24
// layers in the race).

#include "gpu_gl.h"
#include "core_policy.h"
#include "runtime/raster_texel.h"
#include "texel_index.h"
#include "system24_upload.h"
#include "polygon_order.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

// Waiting for the GPU (gl_begin_frame) uses vitaGL's internal 'gxm_context' symbol. Set to
// 0 if your libvitaGL does not export it (link error): glFinish() is used instead.
#ifndef DAYTONA_GL_GPU_TIMING
#define DAYTONA_GL_GPU_TIMING 1
#endif
#if DAYTONA_GL_GPU_TIMING
extern "C" SceGxmContext *gxm_context; // libvitaGL, gxm.c
#endif

// Set to 1 ONLY if libvitaGL was built with DRAW_SPEEDHACK=1: that build ignores the
// 'first' argument of glDrawArrays. A default vitaGL build needs 0. Not supported with
// the indexed quads (draw_quads after a draw_range would use moved attribute pointers):
// scripts/setup_vitagl.py refuses DRAW_SPEEDHACK=1.
#ifndef DAYTONA_GL_VGL_DRAW_SPEEDHACK
#define DAYTONA_GL_VGL_DRAW_SPEEDHACK 0
#endif

namespace vita {
namespace {

// ---- Screen ------------------------------------------------------------------------
// The 496x384 Model 2 picture is scaled to the 544 lines of the Vita and centred.
constexpr float kDisplayW = 960.0f;
constexpr float kDisplayH = 544.0f;
constexpr float kSourceW = float(rt::Video::W);
constexpr float kSourceH = float(rt::Video::H);
constexpr float kScale = kDisplayH / kSourceH;
constexpr float kOffsetX = (kDisplayW - kSourceW * kScale) * 0.5f;

inline float sx(float x) { return kOffsetX + x * kScale; } // native -> display pixels
inline float sy(float y) { return y * kScale; }

// Depth: the Model 2 has no depth buffer, it draws its polygons in priority order. The
// depth buffer only reproduces that order: one value per polygon (Vertex::d, the same on
// every vertex), d = 1 - (rank + 1) / (polygons + 1), rank in the Model 2 draw order above.
// Larger = in front (test GL_GEQUAL, cleared to 0, never cleared during the frame). The
// vertex shader outputs z = d * w, so it is d again after the division. A polygon is
// entirely in front of or behind another one: intersecting polygons do not cut, as on the
// arcade board.
//
// 2D layers sorted by the same depth buffer instead of by drawing order (every polygon
// depth is strictly inside (0, 1)). Drawing order:
//   1. foreground layers  depth test + write at kForegroundDepth (in front of every
//                         polygon): polygon pixels hidden by the HUD are rejected by the
//                         depth test before their fragment shader runs.
//   2. polygons           unchanged.
//   3. background         backdrop + layers, depth test only at kBackgroundDepth (behind
//                         every polygon, equal to the cleared depth): only the pixels no
//                         polygon covered are shaded, the rest is rejected before the shader.
// The image is the same as painting background, polygons, foreground in that order (no
// blending anywhere in the game image). false: plain painter order (background, polygons,
// foreground, 2D without depth test), same image, kept to compare on the console.
constexpr bool k2DLayersByDepth = true;

// Core of the 2D worker thread (System 24 uploads + layer rectangles): 2, the sound
// core, the least loaded one in the race (-1 = any application core).
#ifndef DAYTONA_VITA_2D_CORE
#define DAYTONA_VITA_2D_CORE 2
#endif
constexpr int kWorker2DCore = DAYTONA_VITA_2D_CORE;
constexpr float kForegroundDepth = 0.99999994f; // largest float below 1: z stays inside w
constexpr float kBackgroundDepth = 0.0f;        // glClear writes 0 (glClearDepthf(0))

// vitaGL's glDrawArrays reads a fixed index table of 0xC000 entries (MAX_IDX_NUMBER):
// never draw more vertices in one call. Multiple of 3.
constexpr size_t kMaxVerticesPerDraw = 49152;

// Indexed quads (polygons and System 24 rectangles). A static index buffer turns each group
// of 4 vertices {a, b, c, d} into the triangles (a, b, c) (a, c, d): a Model 2 quad needs 4
// vertices instead of 6 (95% of the polygons), and a clipped n-gon ceil((n-2)/2) quads
// instead of n-2 triangles (an odd fan ends with a degenerate quad, d = c, which draws
// nothing). Same triangles as the fan, so the same image, with a third less vertex data
// written to uncached GPU memory (perf.log: ~14,000 vertices = 435 KB per frame in a
// race, ~2.3 ms of the main core). Indices are absolute 16-bit vertex numbers, the vertex
// attributes stay at offset 0 (see set_attributes): batches start on a multiple of 4 and
// only the first kIndexedVertexLimit vertices of a frame are indexed; anything beyond, or
// a failed index buffer, falls back to plain triangles.
constexpr size_t kIndexedVertexLimit = 65536;              // 16-bit indices
constexpr size_t kQuadIndices = kIndexedVertexLimit / 4 * 6; // 98,304 indices, 192 KB

// Polygon luma kept in the palette key. 0xfc = 64 brightness levels per palette;
// 0 = always full brightness (fewest palettes).
constexpr uint8_t kLumaMask = 0xfc;

// ---- Log: ux0:data/<set>/gl.log, rewritten at every start -----------------------------
// Each line is appended then the file is closed, so the last line survives a crash.
#ifndef M2_ROMSET
#define M2_ROMSET "daytona93"
#endif
constexpr const char *kLogPath = "ux0:data/" M2_ROMSET "/gl.log";
constexpr unsigned kLogEvery = 300; // frames per report (averages over that period)
// No GPU time sampling: waiting for the GPU before and after a frame stalled the main core
// 30-50 ms on purpose, the only frame time spikes perf.log showed in the race. The gl.log
// write is reported to perf.log as logging, not as frame end (gl_last_diagnostic_us).

#ifndef DAYTONA_VITA_DIAGNOSTICS
#define DAYTONA_VITA_DIAGNOSTICS 0
#endif
// gl.log exists only in diagnostic builds (scripts/build_vita.py --diagnostics).
constexpr bool kGlLog = DAYTONA_VITA_DIAGNOSTICS != 0;

void gl_log(const char *fmt, ...) {
    if (!kGlLog) return;
    std::FILE *f = std::fopen(kLogPath, "a");
    if (!f) return;
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fclose(f);
}

inline uint64_t now_us() { return sceKernelGetProcessTimeWide(); }

// Blocks until the GPU has finished every frame submitted so far.
void wait_gpu() {
#if DAYTONA_GL_GPU_TIMING
    sceGxmFinish(gxm_context);
#else
    glFinish();
#endif
}

// ---- Shaders -------------------------------------------------------------------------
// One source for every program; #defines select the variant (see kPrograms).
//   TEXTURED    sample s_tex (unit 0)
//   MODEL       s_tex holds Model 2 indices; colour from s_palette (unit 1), row v_uvp.z
//   ALPHA_TEST  discard transparent texels
//   CHECKER     Model 2 checker: discard one native pixel out of two
//   PALETTE24   s_tex holds System 24 pen numbers (column r, row g); colour from s_pal24 (unit 2)
//   (TEXTURED alone: texture * vertex colour, the ImGui menu, blended)
const char *const kVertexShader = R"(
attribute vec3 a_pos;    // x * w, y * w (display pixels, top-left origin), w
attribute vec4 a_uvpd;   // texture u, v (1.0 = one texture width), palette row coordinate, depth
attribute vec4 a_color;
varying vec3 v_uvp;
varying vec4 v_color;
#ifdef CHECKER
varying vec3 v_pos;
#endif
void main() {
    float w = a_pos.z;
    gl_Position = vec4(a_pos.x * (2.0 / SCREEN_W) - w, w - a_pos.y * (2.0 / SCREEN_H), a_uvpd.w * w, w);
    v_uvp = a_uvpd.xyz;
    v_color = a_color;
#ifdef CHECKER
    v_pos = a_pos;
#endif
}
)";

const char *const kFragmentShader = R"(
varying vec3 v_uvp;
varying vec4 v_color;
#ifdef CHECKER
varying vec3 v_pos;
#endif
#ifdef TEXTURED
uniform sampler2D s_tex;
#endif
#ifdef MODEL
uniform sampler2D s_palette;
#endif
#ifdef PALETTE24
uniform sampler2D s_pal24;
#endif
void main() {
#ifdef CHECKER
    vec2 p = floor(v_pos.xy / v_pos.z);
    float s = p.x + p.y;
    if (s - 2.0 * floor(s * 0.5) < 0.5) { discard; }
#endif
#ifdef TEXTURED
    vec4 t = texture2D(s_tex, v_uvp.xy);
#ifdef ALPHA_TEST
    if (t.a < 0.5) { discard; }
#endif
#ifdef MODEL
    float k = floor(t.r * 127.5 + 0.01);
    gl_FragColor = texture2D(s_palette, vec2((k + 0.5) * (1.0 / 128.0), v_uvp.z)) * v_color;
#else
#ifdef PALETTE24
    float k = floor(t.r * 127.5 + 0.01);
    float j = floor(t.g * 63.75 + 0.01);
    gl_FragColor = texture2D(s_pal24, vec2((k + 0.5) * (1.0 / 128.0), (j + 0.5) * (1.0 / 64.0))) * v_color;
#else
    gl_FragColor = t * v_color;
#endif
#endif
#else
    gl_FragColor = v_color;
#endif
}
)";

// Opaque textures must not use an alpha-test variant: 'discard' disables the PowerVR
// hidden surface removal that makes the depth buffer cheap.
enum class Prog : uint8_t { Flat, Layer, Model, ModelAlpha, FlatChecker, ModelChecker, Ui, Count };
struct ProgramDef { const char *name; const char *defines; };
constexpr ProgramDef kPrograms[int(Prog::Count)] = {
    {"Flat", ""},
    {"Layer", "#define TEXTURED\n#define ALPHA_TEST\n#define PALETTE24\n"},
    {"Model", "#define TEXTURED\n#define MODEL\n"},
    {"ModelAlpha", "#define TEXTURED\n#define MODEL\n#define ALPHA_TEST\n"},
    {"FlatChecker", "#define CHECKER\n"},
    {"ModelChecker", "#define TEXTURED\n#define MODEL\n#define ALPHA_TEST\n#define CHECKER\n"},
    {"Ui", "#define TEXTURED\n"}, // ImGui menu (imgui_vita.h): RGBA texture * vertex colour, blended
};
inline bool textured(Prog p) { return p != Prog::Flat && p != Prog::FlatChecker; }

// ---- Draw list -----------------------------------------------------------------------
struct Vertex {
    float x, y, w;  // x * w, y * w in display pixels, w (1 for 2D quads)
    float u, v;     // texture coordinates
    float p;        // palette row coordinate (Model 2 textures only)
    float d;        // depth in [0, 1], larger = in front (Model 2 priority, see above)
    uint32_t color; // bytes r, g, b, a
};
static_assert(sizeof(Vertex) == 32, "attribute offsets below rely on this layout");

// Depth state of a command.
enum class Depth : uint8_t {
    Off,       // no depth test (menus, and the 2D layers when !k2DLayersByDepth)
    TestWrite, // test GL_GEQUAL + write (polygons, foreground layers)
    Test,      // test GL_GEQUAL, no write (background layers)
};

struct Cmd {
    Prog prog;
    GLuint texture;
    uint32_t first, count;    // vertex range in g.verts.data
    Depth depth;
    int clip;                 // scissor rectangle index in g.clips, -1 = none
    bool quads = false;       // indexed quads: first and count are multiples of 4
};

// A polygon waiting in its batch: everything draw_polygons() resolved for it (material and
// light); flush_batches() builds its vertices straight into the frame's GPU vertex memory.
struct PolyRef {
    const rt::GeoPoly *poly; // in Video::gpu_polys(), valid for the whole frame
    float base_x, base_y;    // projection centre in display pixels
    float u_scale, v_scale, palette;
    float depth;             // Model 2 priority depth
    uint32_t color;
};

// Polygons waiting to be drawn, grouped by (clip, program, texture). Model 2 windows need no
// separate pass: the window is part of the priority rank, hence of the depth.
struct Batch {
    int clip;
    Prog prog;
    GLuint texture;
    std::vector<PolyRef> poly_refs; // kept between frames to reuse the allocation
    // Filled by flush_batches(): what was actually written (invalid polygons are dropped).
    unsigned valid_polys = 0;
    size_t valid_vertices = 0;
};

struct FrameStats {
    unsigned polys_in = 0, polys_out = 0;
    unsigned skip_vcount = 0, skip_window = 0, skip_clip = 0, skip_invalid = 0, skip_texture = 0,
             skip_palette = 0, skip_renderer = 0;
    unsigned draw_calls = 0, batches = 0;
    unsigned new_sources = 0, new_palettes = 0, deferred_sources = 0;
    uint32_t new_texels = 0;       // texels decoded into new index textures this frame
    unsigned dropped_vertices = 0; // vertex memory exhausted (should stay 0)
};

// Times (microseconds) summed over one log period (kLogEvery frames).
struct Timing {
    // Renderer, measured here.
    unsigned frames = 0;
    uint64_t interval = 0, interval_max = 0; // gl_end_frame to gl_end_frame = one presented frame
    uint64_t record = 0;                     // GpuGlRenderer::draw (CPU)
    uint64_t rec_upload = 0;                 //   System 24 / layer texture uploads
    uint64_t rec_layers = 0;                 //   System 24 rectangles
    uint64_t rec_polygons = 0;               //   polygons (texture builds included)
    uint64_t rec_textures = 0;               //     of which: building index textures
    uint64_t gpu_wait = 0;                   // gl_begin_frame: GPU still busy with the previous frame
    uint64_t submit = 0;                     // execute(): GL calls
    uint64_t swap = 0;                       // vglSwapBuffers: waits for a free buffer / vsync
    // Main loop, reported by main_gpu.cpp through gl_profile_loop().
    unsigned loops = 0, menu_loops = 0, board_frames = 0;
    uint64_t board = 0, board_core = 0, board_geometry = 0, board_video = 0, board_max = 0;
    uint64_t sound_wait = 0, sound_worker = 0, prepare = 0;
};

// All the vertices of a frame, in GPU-visible memory. The CPU only writes it (never reads
// it back); the GPU draws it in place through the VBO (vglBufferData: no copy). Since
// gl_begin_frame waits for the GPU, the same memory is refilled every frame.
struct VertexArena {
    Vertex *data = nullptr;
    size_t size = 0, capacity = 0;

    Vertex *append(size_t n) {
        if (size + n > capacity && !grow(size + n)) return nullptr;
        Vertex *p = data + size;
        size += n;
        return p;
    }
    bool grow(size_t needed) {
        const size_t cap = std::max<size_t>(capacity ? capacity * 2 : size_t(1) << 16, needed);
        auto *bigger = static_cast<Vertex *>(vglForceAlloc(uint32_t(cap * sizeof(Vertex))));
        if (!bigger) return false;
        if (size) std::memcpy(bigger, data, size * sizeof(Vertex)); // rare
        if (data) vglFree(data); // the GPU is idle (gl_begin_frame) and the VBO is re-pointed in execute()
        data = bigger;
        capacity = cap;
        return true;
    }
    void release() {
        if (data) vglFree(data);
        data = nullptr;
        size = capacity = 0;
    }
};

struct State {
    bool ready = false;
    unsigned frame = 0;
    GLuint program[int(Prog::Count)] = {};
    GLuint vbo = 0;
    GLuint quad_ibo = 0;        // static index buffer of the indexed quads (0: plain triangles)
    GLuint palette_texture = 0; // bound on unit 1 for the MODEL shaders
    GLuint s24_palette_texture = 0; // bound on unit 2 for the Layer shader (System 24 pens)
    VertexArena verts;
    std::vector<Cmd> cmds;
    std::vector<std::array<int, 4>> clips; // x0, y0, x1, y1 display pixels, top-left origin, exclusive
    int clip = -1;                         // clip of the next pushed vertices
    std::vector<Batch> batches;
    size_t batch_count = 0;
    FlatIndex<uint64_t, uint32_t, 12> batch_index; // (clip, program, texture) -> batch, cleared per frame
    PolygonOrder order;        // Model 2 draw priority (same order as the CPU renderer)
    std::vector<float> depth;  // per polygon depth from that order
    FrameStats stats;
    Timing timing;
    uint64_t record_us = 0, last_end = 0;
    uint64_t diagnostic_us = 0; // last gl_end_frame: gl.log write
    bool skip_worst = false; // the next interval includes the log write
    char error[512] = {};
} g;

void set_error(const char *what, const char *log = nullptr) {
    std::snprintf(g.error, sizeof g.error, "%s%s%s", what, log && *log ? ": " : "", log ? log : "");
    gl_log("ERROR %s\n", g.error);
}

// Append vertices; merged into the previous command when it uses the same state.
void push(Prog prog, GLuint texture, const Vertex *vertices, size_t count, Depth depth = Depth::Off) {
    if (!count) return;
    const uint32_t first = uint32_t(g.verts.size);
    Vertex *dst = g.verts.append(count);
    if (!dst) { g.stats.dropped_vertices += unsigned(count); return; }
    std::memcpy(dst, vertices, count * sizeof(Vertex));
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == prog && last.texture == texture && last.depth == depth && last.clip == g.clip) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({prog, texture, first, uint32_t(count), depth, g.clip});
}

// Screen-space quad (w = 1), display pixels, at depth d (only used when depth != Off).
void push_quad(Prog prog, GLuint texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
               float v1, uint32_t color, Depth depth = Depth::Off, float d = 0.0f) {
    const Vertex q[6] = {
        {x0, y0, 1, u0, v0, 0, d, color}, {x1, y0, 1, u1, v0, 0, d, color}, {x1, y1, 1, u1, v1, 0, d, color},
        {x0, y0, 1, u0, v0, 0, d, color}, {x1, y1, 1, u1, v1, 0, d, color}, {x0, y1, 1, u0, v1, 0, d, color},
    };
    push(prog, texture, q, 6, depth);
}

// Index of a scissor rectangle in g.clips (added if new; there are only a few per frame).
int add_clip(const std::array<int, 4> &rect) {
    for (size_t i = 0; i < g.clips.size(); ++i)
        if (g.clips[i] == rect) return int(i);
    g.clips.push_back(rect);
    return int(g.clips.size() - 1);
}

// Index in g.batches of the polygon batch for this state, created on first use this frame.
uint32_t batch_for(int clip, Prog prog, GLuint texture) {
    const uint64_t key = uint64_t(clip & 0xffff) << 40 | uint64_t(prog) << 32 | texture;
    if (const uint32_t *found = g.batch_index.find(key)) return *found;
    if (g.batch_count == g.batches.size()) g.batches.emplace_back();
    Batch &b = g.batches[g.batch_count];
    b.clip = clip; b.prog = prog; b.texture = texture;
    b.poly_refs.clear();
    g.batch_index.insert(key, uint32_t(g.batch_count)); // full table: a second batch, same image
    return uint32_t(g.batch_count++);
}

// Vertex slots for `quads` indexed quads, starting on a multiple of 4 (the index buffer is
// built for groups of 4); nullptr when they would pass the 16-bit index limit (the caller
// then writes plain triangles) or memory is exhausted. The padding vertices are never drawn.
Vertex *append_quads(size_t quads) {
    if (!g.quad_ibo || !quads) return nullptr;
    const size_t pad = (4 - g.verts.size % 4) % 4;
    if (g.verts.size + pad + quads * 4 > kIndexedVertexLimit) return nullptr;
    if (!g.verts.append(pad + quads * 4)) return nullptr;
    return g.verts.data + (g.verts.size - quads * 4);
}

// Fan (v0, vi, vi+1) of a convex n-gon as quads (v0, v1+2k, v2+2k, v3+2k): the two fan
// triangles 1+2k and 2+2k. Odd fans end with (v0, vn-2, vn-1, vn-1), whose second triangle
// is degenerate. Returns the vertices written (4 per quad).
size_t write_fan_quads(Vertex *dst, const Vertex *v, int n) {
    size_t written = 0;
    for (int a = 1; a + 1 < n; a += 2) {
        *dst++ = v[0];
        *dst++ = v[a];
        *dst++ = v[a + 1];
        *dst++ = v[std::min(a + 2, n - 1)];
        written += 4;
    }
    return written;
}

// Moves the batches into the draw list, in order of first appearance. The order does not
// change the image: every polygon has its own priority depth and nothing is blended.
void flush_batches() {
    for (size_t bi = 0; bi < g.batch_count; ++bi) {
        Batch *b = &g.batches[bi];
        g.clip = b->clip;
        b->valid_polys = 0;
        b->valid_vertices = 0;
        if (b->poly_refs.empty()) continue;

        // Indexed quads when they fit under the 16-bit index limit (see kIndexedVertexLimit).
        size_t quads = 0;
        for (const PolyRef &ref : b->poly_refs) quads += size_t(ref.poly->num_vertices - 1) / 2;
        Vertex *quad_dst = append_quads(quads);
        const bool indexed = quad_dst != nullptr;
        const uint32_t first = indexed ? uint32_t(quad_dst - g.verts.data) : uint32_t(g.verts.size);
        uint32_t written = 0;
        for (const PolyRef &ref : b->poly_refs) {
            const rt::GeoPoly &poly = *ref.poly;
            const int n = poly.num_vertices;

            // 1. Vertices in homogeneous form; the GPU does the division by w.
            Vertex v[8];
            bool valid = true;
            for (int i = 0; i < n && valid; ++i) {
                const float w = poly.v[i].p[0];
                v[i] = {ref.base_x * w + poly.v[i].x * kScale, ref.base_y * w - poly.v[i].y * kScale, w,
                        poly.v[i].p[1] * ref.u_scale, poly.v[i].p[2] * ref.v_scale, ref.palette, ref.depth,
                        ref.color};
                valid = w > 0.0f && std::isfinite(w) && std::isfinite(v[i].x) && std::isfinite(v[i].y) &&
                        std::isfinite(v[i].u) && std::isfinite(v[i].v);
            }
            if (!valid) { ++g.stats.skip_invalid; continue; }

            // 2. Written once, straight into the frame's GPU vertex memory (vglForceAlloc:
            //    uncached RAM). The CPU only writes there, never reads back.
            if (indexed) { // quads in the slots reserved above (invalid polygons leave theirs unused)
                written += uint32_t(write_fan_quads(quad_dst + written, v, n));
                ++b->valid_polys;
                continue;
            }
            const size_t count = size_t(n - 2) * 3;
            Vertex *dst = g.verts.append(count);
            if (!dst) { g.stats.dropped_vertices += unsigned(count); continue; }
            for (int i = 1; i + 1 < n; ++i) {
                *dst++ = v[0];
                *dst++ = v[i];
                *dst++ = v[i + 1];
            }
            written += uint32_t(count);
            ++b->valid_polys;
        }
        b->valid_vertices = written;
        // Slots reserved for invalid polygons: give them back (they are at the end).
        if (indexed) g.verts.size = first + written;
        // One command per batch: batches have distinct states, so push() would not merge them.
        if (written) g.cmds.push_back({b->prog, b->texture, first, written, Depth::TestWrite, g.clip, indexed});
    }
    g.clip = -1;
    g.stats.batches = unsigned(g.batch_count);
}


// The 3 attributes are interleaved in the VBO. Their pointers stay at the small constant
// offsets 0/12/28 and each draw selects its vertices with glDrawArrays(first, ...).
// Never put a large byte offset in these pointers: vitaGL stores it in the 16-bit
// SceGxmVertexAttribute::offset (wrong vertices beyond 64 KB) and re-patches the vertex
// program for every new offset.
void set_attributes(size_t base) {
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(base + 0));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(base + 12));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void *>(base + 28));
}

void draw_range(uint32_t first, uint32_t count) {
    for (uint32_t done = 0; done < count;) {
        const uint32_t n = uint32_t(std::min<size_t>(kMaxVerticesPerDraw, count - done));
#if DAYTONA_GL_VGL_DRAW_SPEEDHACK
        set_attributes(size_t(first + done) * sizeof(Vertex)); // this build adds it to the stream address
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(n));
#else
        glDrawArrays(GL_TRIANGLES, GLint(first + done), GLsizei(n));
#endif
        ++g.stats.draw_calls;
        done += n;
    }
}

// Indexed quads (see kIndexedVertexLimit): count / 4 quads from vertex `first`, both
// multiples of 4, with the static index buffer bound.
void draw_quads(uint32_t first, uint32_t count) {
    const size_t offset = size_t(first / 4) * 6 * sizeof(uint16_t);
    glDrawElements(GL_TRIANGLES, GLsizei(count / 4 * 6), GL_UNSIGNED_SHORT, reinterpret_cast<const void *>(offset));
    ++g.stats.draw_calls;
}

// Runs the recorded draw list.
void execute() {
    if (g.cmds.empty()) return;
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    vglBufferData(GL_ARRAY_BUFFER, g.verts.data); // the VBO uses our GPU memory as is
    if (g.quad_ibo) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
    for (GLuint i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
    set_attributes(0);
    // Palette textures for the whole frame (unit 1 Model 2, unit 2 System 24); per-command
    // textures on unit 0.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g.palette_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g.s24_palette_texture);
    glActiveTexture(GL_TEXTURE0);

    // Current GL state, to skip redundant calls. -1 / 0 = unknown.
    // gl_begin_frame() left glDepthMask at GL_TRUE.
    GLuint program = 0, texture = 0;
    int blend = -1, depth_test = -1, depth_write = 1, clip = -2;
    for (const Cmd &c : g.cmds) {
        if (g.program[int(c.prog)] != program) {
            program = g.program[int(c.prog)];
            glUseProgram(program);
        }
        // Blending only for the menu / text rectangles; the game image is never blended.
        const int want_blend = ((c.prog == Prog::Flat || c.prog == Prog::Ui) && c.depth == Depth::Off) ? 1 : 0;
        if (want_blend != blend) {
            if (want_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            blend = want_blend;
        }
        const int want_test = c.depth != Depth::Off ? 1 : 0;
        if (want_test != depth_test) {
            if (want_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            depth_test = want_test;
        }
        const int want_write = c.depth != Depth::Test ? 1 : 0; // no effect while the test is off
        if (want_test && want_write != depth_write) {
            glDepthMask(want_write ? GL_TRUE : GL_FALSE);
            depth_write = want_write;
        }
        if (textured(c.prog) && c.texture != texture) {
            glBindTexture(GL_TEXTURE_2D, c.texture);
            texture = c.texture;
        }
        if (c.clip != clip) {
            if (c.clip < 0) {
                glDisable(GL_SCISSOR_TEST);
            } else {
                const std::array<int, 4> &r = g.clips[size_t(c.clip)];
                glEnable(GL_SCISSOR_TEST);
                glScissor(r[0], int(kDisplayH) - r[3], r[2] - r[0], r[3] - r[1]); // GL: bottom-left origin
            }
            clip = c.clip;
        }
        if (c.quads) draw_quads(c.first, c.count);
        else draw_range(c.first, c.count);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    if (depth_write != 1) glDepthMask(GL_TRUE);
    if (g.quad_ibo) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

GLuint compile(GLenum type, const char *common, const char *defines, const char *body, const char *name) {
    const GLuint shader = glCreateShader(type);
    const char *sources[] = {common, defines, body};
    glShaderSource(shader, 3, sources, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    char log[400] = {};
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    set_error(name, log);
    glDeleteShader(shader);
    return 0;
}

GLuint build_program(const ProgramDef &def) {
    // Constants shared by every shader.
    char common[320];
    std::snprintf(common, sizeof common,
                  "#version 140\nprecision lowp int;\nprecision highp float;\n"
                  "#define SCREEN_W %.1f\n#define SCREEN_H %.1f\n",
                  double(kDisplayW), double(kDisplayH));
    const GLuint vs = compile(GL_VERTEX_SHADER, common, def.defines, kVertexShader, def.name);
    const GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, common, def.defines, kFragmentShader, def.name) : 0;
    GLuint program = 0;
    if (vs && fs) {
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "a_pos");
        glBindAttribLocation(program, 1, "a_uvpd");
        glBindAttribLocation(program, 2, "a_color");
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok == GL_TRUE) {
            glUseProgram(program);
            if (GLint loc = glGetUniformLocation(program, "s_tex"); loc >= 0) glUniform1i(loc, 0);
            if (GLint loc = glGetUniformLocation(program, "s_palette"); loc >= 0) glUniform1i(loc, 1);
            if (GLint loc = glGetUniformLocation(program, "s_pal24"); loc >= 0) glUniform1i(loc, 2);
            glUseProgram(0);
        } else {
            char log[400] = {};
            glGetProgramInfoLog(program, sizeof log, nullptr, log);
            set_error(def.name, log);
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return program;
}

double ms(uint64_t us, unsigned n = 1) { return n ? double(us) / 1000.0 / n : 0.0; }

// Text built in memory and written with one gl_log(): opening ux0: once per line made the
// report itself take ~100 ms.
struct Report {
    std::string text;
    void add(const char *fmt, ...) {
        char line[320];
        std::va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof line, fmt, args);
        va_end(args);
        text += line;
    }
};

// One report per kLogEvery frames: where the frame time goes, and what the renderer did.
// All times are averages per presented frame, in ms.
void log_report() {
    const FrameStats &s = g.stats;
    const Timing &t = g.timing;
    const unsigned n = t.frames, l = t.loops;
    const double frame = ms(t.interval, n);
    const double board = ms(t.board, l), sound_wait = ms(t.sound_wait, l), prepare = ms(t.prepare, l);
    const double record = ms(t.record, n), submit = ms(t.submit, n), swap = ms(t.swap, n);
    const double gpu_wait = ms(t.gpu_wait, n);
    const double other = frame - board - sound_wait - prepare - record - gpu_wait - submit - swap;
    Report r;
    r.add("---- frame %u: %u frames (%u in menu), CPU %d MHz, GPU %d MHz ----\n", g.frame, n, t.menu_loops,
           scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency());
    r.add("  FRAME     %6.2f ms = %.1f fps shown, %.1f fps emulated (%.2f board frames per shown frame), worst %.2f ms\n",
           frame, frame > 0.0 ? 1000.0 / frame : 0.0, t.interval ? t.board_frames * 1e6 / double(t.interval) : 0.0,
           l ? double(t.board_frames) / l : 0.0, ms(t.interval_max));
    r.add("  emulation %6.2f ms  (i960+TGP core %.2f, geometry %.2f, 2D video %.2f; worst board frame %.2f)\n", board,
           ms(t.board_core, l), ms(t.board_geometry, l), ms(t.board_video, l), ms(t.board_max));
    r.add("  sound     %6.2f ms  waiting for the sound thread (it runs %.2f ms per frame in parallel)\n", sound_wait,
           ms(t.sound_worker, l));
    r.add("  renderer  %6.2f ms  CPU: 2D worker wait %.2f, 2D quads %.2f, polygons %.2f (texture builds %.2f) + prepare %.2f\n",
           record + prepare, ms(t.rec_upload, n), ms(t.rec_layers, n), ms(t.rec_polygons, n), ms(t.rec_textures, n),
           prepare);
    r.add("  GPU wait  %6.2f ms  at frame start, before rewriting vertices and textures in place\n", gpu_wait);
    r.add("  GL submit %6.2f ms\n", submit);
    r.add("  swap      %6.2f ms  waiting in vglSwapBuffers (GPU still busy with an older frame, or vsync)\n", swap);
    r.add("  other     %6.2f ms  input, menu, SDL_Delay, logs, everything not measured above\n", other);
    r.add("frame %u: polys in=%u out=%u skipped: vcount=%u window=%u clip=%u invalid=%u texture=%u palette=%u renderer=%u\n",
           g.frame, s.polys_in, s.polys_out, s.skip_vcount, s.skip_window, s.skip_clip, s.skip_invalid, s.skip_texture,
           s.skip_palette, s.skip_renderer);
    r.add("frame %u: batches=%u cmds=%u draw_calls=%u verts=%u (dropped %u) | new textures=%u (deferred %u)"
          " new palettes=%u | free vram %u KB\n",
          g.frame, s.batches, unsigned(g.cmds.size()), s.draw_calls, unsigned(g.verts.size),
          s.dropped_vertices, s.new_sources, s.deferred_sources, s.new_palettes, vglMemFree(VGL_MEM_VRAM) / 1024);
    gl_log("%s", r.text.c_str());
}

} // namespace

// ---- Frame level API ----------------------------------------------------------------
const char *gl_error() { return g.error; }

bool gl_init() {
    g.error[0] = 0;
    if (kGlLog) std::remove(kLogPath);
    gl_log("gl_init: runtime shader compiler\n");
    vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, GL_TRUE, GL_TRUE, GL_TRUE);
    // sceGxm buffer sizes, before vglInit*.
    constexpr uint32_t kParamBuffer = 16u * 1024u * 1024u;
    vglSetVDMBufferSize(2048u * 1024u);
    vglSetVertexBufferSize(8192u * 1024u);
    vglSetFragmentBufferSize(2048u * 1024u);
    vglSetUSSEBufferSize(128u * 1024u);
    vglSetParamBufferSize(kParamBuffer);
    const GLboolean fallback = vglInitExtended(0, int(kDisplayW), int(kDisplayH), kParamBuffer, SCE_GXM_MULTISAMPLE_NONE);
    gl_log("gl_init: vglInitExtended returned %d (1 = resolution fallback), free vram %u KB\n", int(fallback),
           vglMemFree(VGL_MEM_VRAM) / 1024);
    vglWaitVblankStart(GL_TRUE);

    glViewport(0, 0, int(kDisplayW), int(kDisplayH));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(0.0f);        // lowest priority = 0 (larger depth = in front)
    glDepthFunc(GL_GEQUAL);     // nearer = larger; on ties the later polygon wins
    glDepthRangef(-1.0f, 1.0f); // window depth = z / w as computed by the vertex shader
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);   // enabled per command (polygons only)
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int i = 0; i < int(Prog::Count); ++i) {
        g.program[i] = build_program(kPrograms[i]);
        gl_log("gl_init: program %s = %u\n", kPrograms[i].name, g.program[i]);
    }

    if (!g.program[int(Prog::Flat)] || !g.program[int(Prog::Layer)] || !g.program[int(Prog::Model)] ||
        !g.program[int(Prog::ModelAlpha)] || !g.program[int(Prog::Ui)])
        return false;
    // Checker programs are optional: without them checker polygons are drawn solid.
    if (!g.program[int(Prog::FlatChecker)] || !g.program[int(Prog::ModelChecker)]) {
        gl_log("gl_init: checker shaders unavailable, checker polygons drawn solid\n");
        g.error[0] = 0;
        if (g.program[int(Prog::FlatChecker)]) glDeleteProgram(g.program[int(Prog::FlatChecker)]);
        if (g.program[int(Prog::ModelChecker)]) glDeleteProgram(g.program[int(Prog::ModelChecker)]);
        g.program[int(Prog::FlatChecker)] = g.program[int(Prog::Flat)];
        g.program[int(Prog::ModelChecker)] = g.program[int(Prog::ModelAlpha)];
    }
    glGenBuffers(1, &g.vbo);
    if (!g.vbo || !g.verts.grow(size_t(1) << 16)) {
        set_error("vertex memory allocation failed");
        return false;
    }
    g.cmds.reserve(1024);
    {
        // Static index buffer of the indexed quads: {4q, 4q+1, 4q+2, 4q, 4q+2, 4q+3}.
        std::vector<uint16_t> indices(kQuadIndices);
        for (size_t q = 0; q < kIndexedVertexLimit / 4; ++q) {
            const uint16_t v = uint16_t(q * 4);
            uint16_t *i = &indices[q * 6];
            i[0] = v; i[1] = uint16_t(v + 1); i[2] = uint16_t(v + 2);
            i[3] = v; i[4] = uint16_t(v + 2); i[5] = uint16_t(v + 3);
        }
        glGenBuffers(1, &g.quad_ibo);
        if (g.quad_ibo) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * sizeof(uint16_t)), indices.data(),
                         GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        gl_log("gl_init: indexed quads %s\n", g.quad_ibo ? "on" : "unavailable, plain triangles");
    }
    g.ready = true;
    gl_log("gl_init: done\n");
    return true;
}

void gl_begin_frame() {
    ++g.frame;
    // The GPU may still read last frame's vertices and textures, rewritten in place below.
    const uint64_t wait_begin = now_us();
    wait_gpu();
    g.timing.gpu_wait += now_us() - wait_begin;
    g.verts.size = 0;
    g.cmds.clear();
    g.clips.clear();
    g.clip = -1;
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE); // glClear honours the mask
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void gl_end_frame() {
    Timing &t = g.timing;
    uint64_t diagnostic = 0; // gl.log write, reported apart (gl_last_diagnostic_us)
    const uint64_t t0 = now_us();
    execute();
    const uint64_t t1 = now_us();
    vglSwapBuffers(GL_FALSE);
    const uint64_t t2 = now_us();
    const uint64_t end = now_us();

    const bool exclude = g.skip_worst; // includes the log write
    g.skip_worst = false;
    if (g.last_end) {
        const uint64_t interval = end - g.last_end;
        ++t.frames;
        t.interval += interval;
        if (!exclude) t.interval_max = std::max(t.interval_max, interval);
        t.record += g.record_us;
        t.submit += t1 - t0;
        t.swap += t2 - t1;
    }
    g.last_end = end;
    g.record_us = 0;
    if (kGlLog && (g.frame <= 3 || g.frame % kLogEvery == 0)) {
        const uint64_t l0 = now_us();
        log_report();
        diagnostic += now_us() - l0;
        t = {};
        g.skip_worst = true;
    }
    g.diagnostic_us = diagnostic;
    g.stats = {};
}

uint64_t gl_last_diagnostic_us() { return g.diagnostic_us; }

void gl_profile_loop(const GlLoopProfile &p) {
    Timing &t = g.timing;
    ++t.loops;
    if (p.menu) ++t.menu_loops;
    t.board_frames += p.board_frames;
    t.board += p.board;
    t.board_core += p.board_core;
    t.board_geometry += p.board_geometry;
    t.board_video += p.board_video;
    t.board_max = std::max(t.board_max, p.board_frames ? p.board / p.board_frames : 0);
    t.sound_wait += p.sound_wait;
    t.sound_worker += p.sound_worker;
    t.prepare += p.prepare;
}

void gl_fini() {
    if (!g.ready) return;
    glFinish();
    for (int i = 0; i < int(Prog::Count); ++i) {
        bool shared = false; // checker fallbacks reuse other handles
        for (int j = 0; j < i; ++j) shared = shared || g.program[j] == g.program[i];
        if (g.program[i] && !shared) glDeleteProgram(g.program[i]);
        g.program[i] = 0;
    }
    if (g.vbo) {
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        vglBufferData(GL_ARRAY_BUFFER, nullptr); // our memory: vitaGL must not free it
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &g.vbo);
    }
    g.vbo = 0;
    if (g.quad_ibo) glDeleteBuffers(1, &g.quad_ibo);
    g.quad_ibo = 0;
    g.verts.release();
    g.ready = false;
}

void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba) {
    push_quad(Prog::Flat, 0, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 0.0f, rgba);
}

// ---- ImGui menu (imgui_vita.h) -----------------------------------------------------------
uint32_t gl_create_ui_texture(int w, int h, const void *rgba) {
    if (w <= 0 || h <= 0 || !rgba) return 0;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) return 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return texture;
}

void gl_delete_ui_texture(uint32_t texture) {
    if (!texture) return;
    glFinish(); // the GPU may still draw the last menu frame with it
    GLuint t = texture;
    glDeleteTextures(1, &t);
}

void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1) {
    count -= count % 3;
    if (!texture || !count) return;
    x0 = std::clamp(x0, 0, int(kDisplayW)); x1 = std::clamp(x1, 0, int(kDisplayW));
    y0 = std::clamp(y0, 0, int(kDisplayH)); y1 = std::clamp(y1, 0, int(kDisplayH));
    if (x1 <= x0 || y1 <= y0) return;
    Vertex *dst = g.verts.append(count);
    if (!dst) { g.stats.dropped_vertices += unsigned(count); return; }
    const uint32_t first = uint32_t(dst - g.verts.data);
    for (size_t i = 0; i < count; ++i) {
        const GlUiVertex &v = vertices[i];
        dst[i] = {v.x, v.y, 1.0f, v.u, v.v, 0.0f, 0.0f, v.rgba};
    }
    const int clip = add_clip({x0, y0, x1, y1});
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == Prog::Ui && last.texture == texture && last.clip == clip &&
            last.first + last.count == first) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({Prog::Ui, texture, first, uint32_t(count), Depth::Off, clip});
}

// ---- Renderer: setup -------------------------------------------------------------------
namespace {
uint16_t le16(const uint8_t *base, uint32_t index) { return uint16_t(base[index * 2] | base[index * 2 + 1] << 8); }
uint32_t argb_to_rgba(uint32_t argb) { return (argb & 0xff00ff00u) | ((argb >> 16) & 0xffu) | ((argb & 0xffu) << 16); }
} // namespace

GpuGlRenderer::GpuGlRenderer() {
    for (int i = 0; i < 256; ++i) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
    bool ok = true;
    for (S24Slot &slot : s24_) {
        for (size_t i = 0; i < slot.textures.size(); ++i) {
            // Point sampled and repeating: scroll offsets add up to 511 texels to the coordinates.
            slot.textures[i] = make_texture(512, 512, false, GL_REPEAT, GL_REPEAT, GL_RGBA, nullptr);
            slot.texels[i] = texture_memory(slot.textures[i]);
            ok = ok && slot.textures[i] && slot.texels[i];
        }
        // System 24 palette: 8192 pens, point sampled (system24_upload.h).
        slot.palette_texture = make_texture(kSystem24PaletteWidth, kSystem24PaletteHeight, false, GL_CLAMP_TO_EDGE,
                                            GL_CLAMP_TO_EDGE, GL_RGBA, nullptr);
        slot.palette = static_cast<uint32_t *>(texture_memory(slot.palette_texture));
        ok = ok && slot.palette_texture && slot.palette;
        // The worker should not allocate: room for the usual frames (more is allocated if needed).
        slot.rects.reserve(4096);
        slot.runs.reserve(64);
        for (auto &split : slot.split) split.reserve(1024);
    }
    // Palette texture: point sampled, one row per palette.
    palette_texture_ = make_texture(kPaletteWidth, kPaletteRows, false, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_RGBA, nullptr);
    palette_data_ = static_cast<uint32_t *>(texture_memory(palette_texture_));
    g.palette_texture = palette_texture_;
    ok = ok && palette_texture_ && palette_data_;
    sources_.reserve(1024);
    ok_ = g.ready && ok;
    if (ok_ && !s24_start_thread()) s24_stop_thread(); // 2D prepared inline on the main core
}

GpuGlRenderer::~GpuGlRenderer() { s24_stop_thread(); }

int GpuGlRenderer::worker_2d_core() { return kWorker2DCore; }

uint32_t GpuGlRenderer::make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t,
                                     uint32_t format, const void *pixels) {
    std::vector<uint8_t> zero;
    if (!pixels) {
        zero.assign(size_t(w) * h * 4u, 0);
        pixels = zero.data();
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) return 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLint(wrap_s));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLint(wrap_t));
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(format), GLsizei(w), GLsizei(h), 0, GLenum(format), GL_UNSIGNED_BYTE, pixels);
    return texture;
}

// vitaGL textures are linear: rows of VGL_ALIGN(width, 8) pixels. The pointer stays valid
// as long as glTexImage2D / glTexSubImage2D are not called again on the texture.
void *GpuGlRenderer::texture_memory(uint32_t texture) {
    if (!texture) return nullptr;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    return vglGetTexDataPointer(GL_TEXTURE_2D);
}

void GpuGlRenderer::clear_cache() {
    for (auto &entry : sources_) {
        GLuint texture = entry.second.texture;
        if (texture) glDeleteTextures(1, &texture);
    }
    sources_.clear();
    source_cache_.clear();
    cached_bytes_ = 0;
    cache_reset_pending_ = false;
    palette_index_.clear();
    palette_used_ = 0;
}

void GpuGlRenderer::reset_materials() {
    s24_join();
    glFinish();
    clear_cache();
    for (S24Slot &slot : s24_) {
        slot.valid = false; // nothing shown until a frame of the new state was prepared
        slot.generation = slot.palette_generation = UINT64_MAX;
    }
}

void GpuGlRenderer::prepare_frame() {
    // (gl_begin_frame waits for the GPU before anything is rewritten in GPU memory.)
    if (cache_reset_pending_) {
        clear_cache();
        ++cache_resets_;
    }
}

void GpuGlRenderer::shutdown() {
    if (shutdown_) return;
    glFinish();
    clear_cache();
    if (palette_texture_) {
        GLuint texture = palette_texture_;
        glDeleteTextures(1, &texture);
    }
    palette_texture_ = 0;
    palette_data_ = nullptr;
    g.palette_texture = 0;
    s24_stop_thread();
    g.s24_palette_texture = 0;
    for (S24Slot &slot : s24_) {
        for (uint32_t &texture : slot.textures) {
            GLuint t = texture;
            if (t) glDeleteTextures(1, &t);
            texture = 0;
        }
        GLuint t = slot.palette_texture;
        if (t) glDeleteTextures(1, &t);
        slot.palette_texture = 0;
        slot.palette = nullptr;
        slot.texels.fill(nullptr);
        slot.valid = false;
    }
    shutdown_ = true;
    ok_ = false;
}

// ---- Renderer: Model 2 textures and palettes -------------------------------------------
// Index texture of one texture region (texheader[0] size/mirror/transparent, texheader[2]
// position). L = index * 16 so that the GPU's bilinear filter interpolates the index.
// A = 0 marks a transparent texel (index 15 when the texture is transparent); its L is
// copied from an opaque neighbour so the filter does not pull index 15 into the edges.
const GpuGlRenderer::Source *GpuGlRenderer::source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const SourceKey key{uint16_t(poly.texheader[0] & 0x23ff), uint16_t(poly.texheader[2] & 0x1fff)};
    const uint32_t key32 = uint32_t(key.h0) | uint32_t(key.h2) << 16;
    if (const Source *const *cached = source_cache_.find(key32)) return *cached;
    auto found = sources_.find(key);
    if (found != sources_.end()) {
        source_cache_.insert(key32, &found->second); // full: stays in sources_ only
        return &found->second;
    }
    if (cached_bytes_ >= kSourceCacheBytes) {
        cache_reset_pending_ = true; // emptied by the next prepare_frame()
        ++material_drops_;
        return nullptr;
    }
    Source s;
    s.source_w = 32u << (poly.texheader[0] & 7);
    s.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
    // Spread a cold cache over several frames, by texels decoded (cost grows with the
    // size; a count let one frame take 32 large textures, ~88 ms). The first build of a
    // frame is always allowed, so even the largest texture is never deferred forever.
    const uint32_t texels = std::min(s.source_w, kTextureLimit) * std::min(s.source_h, kTextureLimit);
    if (g.stats.new_sources > 0 &&
        (g.stats.new_sources >= kSourceBuildBudget || g.stats.new_texels + texels > kSourceTexelBudget)) {
        ++material_defers_;
        ++g.stats.deferred_sources;
        return nullptr;
    }

    const uint64_t build_begin = now_us();
    s.transparent = (poly.texheader[0] >> 13) & 1;
    // Above kTextureLimit, keep one texel every 'step' (GL texture side limit).
    const uint32_t step_x = std::max(1u, (s.source_w + kTextureLimit - 1) / kTextureLimit);
    const uint32_t step_y = std::max(1u, (s.source_h + kTextureLimit - 1) / kTextureLimit);
    const uint32_t w = (s.source_w + step_x - 1) / step_x;
    const uint32_t h = (s.source_h + step_y - 1) / step_y;
    const uint32_t bx = 32u * (poly.texheader[2] & 0x3f);
    const uint32_t by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
    const uint32_t *sheet = (poly.texheader[2] & 0x1000) ? mem.tex1 : mem.tex0;
    std::vector<uint8_t> &la = index_scratch_; // L, A pairs
    la.resize(size_t(w) * h * 2u);
    const uint8_t transparent_alpha = s.transparent ? 0 : 255; // alpha of index 15
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t ty = std::min(y * step_y, s.source_h - 1);
        uint8_t *row = &la[size_t(y) * w * 2u];
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t tx = x * step_x; // < source_w: w = ceil(source_w / step_x)
            const uint32_t index = texel_index(bx, by, tx, ty, sheet);
            row[x * 2u] = uint8_t(index * 16);
            row[x * 2u + 1] = index == 15 ? transparent_alpha : 255;
        }
    }
    if (s.transparent) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t(y) * w + x) * 2u;
                if (la[i + 1]) continue;
                const size_t n[4] = {(size_t(y) * w + (x + 1) % w) * 2u, (size_t(y) * w + (x + w - 1) % w) * 2u,
                                     (size_t((y + 1) % h) * w + x) * 2u, (size_t((y + h - 1) % h) * w + x) * 2u};
                for (size_t j : n)
                    if (la[j + 1]) { la[i] = la[j]; break; }
            }
    }
    // Mirror flags map to GL mirrored repeat, otherwise plain repeat (road textures repeat).
    s.texture = make_texture(w, h, true, ((poly.texheader[0] >> 8) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT,
                             ((poly.texheader[0] >> 9) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT, GL_LUMINANCE_ALPHA,
                             la.data());
    if (!s.texture) { ++material_drops_; return nullptr; }
    s.bytes = la.size();
    cached_bytes_ += s.bytes;
    ++g.stats.new_sources;
    g.stats.new_texels += w * h;
    g.timing.rec_textures += now_us() - build_begin;
    last_texture_us_ += now_us() - build_begin;
    const Source *built = &sources_.emplace(key, s).first->second;
    source_cache_.insert(key32, built);
    return built;
}

// Row of the palette texture for this polygon (created on first use), -1 when full.
// Entry k = colour of luminance step k, same maths as the CPU renderer:
//   luma = lumaram[luma base + k] * polygon luma / 256, then colour table + gamma.
int GpuGlRenderer::palette_row(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const uint32_t lumabase = poly.texheader[1] & 0xff;
    const uint32_t color_index = (poly.texheader[3] >> 6) & 0x3ff;
    const uint32_t luma_scale = kLumaMask ? (poly.luma & kLumaMask) : 0xff;
    const uint32_t key = lumabase | color_index << 8 | luma_scale << 18;
    if (const uint32_t *found = palette_index_.find(key)) return int(*found);
    if (palette_used_ >= kPaletteRows) { ++material_drops_; return -1; }

    const uint32_t row = palette_used_++;
    const uint32_t color = le16(mem.palram, color_index + 0x1000) & 0x7fff;
    uint32_t *out = &palette_data_[size_t(row) * kPaletteWidth]; // texture memory, 128-pixel rows
    for (uint32_t k = 0; k < kPaletteWidth; ++k) {
        const uint32_t luma = std::min(uint32_t(mem.lumaram[((lumabase << 7) + k) * 4]) * luma_scale / 256, 0x3fu);
        const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t gg = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
        out[k] = RGBA8(r, gg, b, 255);
    }
    palette_index_.insert(key, row); // kPaletteRows < capacity: always room
    ++material_builds_;
    ++g.stats.new_palettes;
    return int(row);
}

uint32_t GpuGlRenderer::solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
    const uint32_t color = le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
    const uint32_t luma = poly.luma >> 2;
    const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t gg = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
    return RGBA8(r, gg, b, 255);
}

// ---- Renderer: Model 2 polygons --------------------------------------------------------
void GpuGlRenderer::draw_polygons(rt::Video &video) {
    const auto &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    FrameStats &st = g.stats;
    st.polys_in = unsigned(polys.size());
    material_drops_ = material_builds_ = material_defers_ = 0;
    textured_polys_ = solid_polys_ = checker_polys_ = textured_checker_polys_ = 0;
    submitted_vertices_ = 0;
    g.batch_count = 0;
    g.batch_index.clear();
    // Palette texture almost full: start it again (gl_begin_frame waited for the GPU, so no
    // frame in flight still reads it). Rows are cheap to rebuild.
    if (palette_used_ > kPaletteRows - kPaletteFrameRows) {
        palette_index_.clear();
        palette_used_ = 0;
        ++cache_resets_;
    }

    // Per-polygon state, cached on two levels and reused while the following polygons match:
    //   * Material: everything that depends on the header (texheader, viewport, window):
    //     clip, shader, texture, batch, UV scale. Neighbouring polygons nearly always belong
    //     to the same object and material.
    //   * Light: what depends on the polygon luma: the palette row (textured) or the flat
    //     colour. The geometrizer lights each face (normal . light), so the luma changes
    //     from one polygon to the next; only this part is redone when it is the only change.
    enum class Skip : uint8_t { None, Window, Clip, Texture, Renderer };

    struct Material {
        bool cached = false;
        // key
        uint16_t th[4] = {};
        int viewport[4] = {}, window = 0;
        // result
        Skip skip = Skip::None;
        bool textured = false, checker = false;
        int clip = -1;
        Prog prog = Prog::Flat;
        GLuint texture = 0;
        bool has_batch = false; // batch created when the first polygon gets a palette row
        uint32_t batch = 0;
        float u_scale = 0, v_scale = 0;
    } mat;
    struct Light {
        bool cached = false;
        uint8_t key = 0;     // the luma bits the result depends on (see luma_key)
        bool ok = false;     // false: palette texture full, polygon skipped
        float palette = 0;
        uint32_t color = 0xffffffffu;
    } light;

    const int windows = video.gpu_windows(), rx = video.render_x(), ry = video.render_y();
    const int crtc_x = video.crtc_x(), crtc_y = video.crtc_y();

    auto same_material = [&](const rt::GeoPoly &p) {
        return mat.cached && p.texheader[0] == mat.th[0] && p.texheader[1] == mat.th[1] &&
               p.texheader[2] == mat.th[2] && p.texheader[3] == mat.th[3] && p.window == mat.window &&
               p.viewport[0] == mat.viewport[0] && p.viewport[1] == mat.viewport[1] &&
               p.viewport[2] == mat.viewport[2] && p.viewport[3] == mat.viewport[3];
    };
    auto set_material = [&](const rt::GeoPoly &p) {
        mat = Material{};
        light.cached = false; // the palette also depends on texheader (luma base, colour)
        mat.cached = true;
        for (int i = 0; i < 4; ++i) { mat.th[i] = p.texheader[i]; mat.viewport[i] = p.viewport[i]; }
        mat.window = p.window;
        if (p.window > windows) { mat.skip = Skip::Window; return; }

        // Clip rectangle (the polygon's viewport), as a scissor in display pixels.
        const int l = std::max<int>(p.viewport[0] + rx, 0);
        const int r = std::min<int>(p.viewport[2] + rx, rt::Video::W - 1);
        const int t = std::max<int>(384 - p.viewport[3] + ry, 0);
        const int b = std::min<int>(384 - p.viewport[1] + ry, rt::Video::H - 1);
        if (l > r || t > b) { mat.skip = Skip::Clip; return; }
        mat.clip = add_clip({int(sx(float(l))), int(sy(float(t))), int(sx(float(r + 1))), int(sy(float(b + 1)))});

        // Shader and texture.
        const int renderer = (p.texheader[0] >> 13) & 3; // 0 flat, 1 unsupported, 2-3 textured
        mat.checker = (p.texheader[0] & 0x8000) != 0;
        if (renderer & 2) {
            const Source *src = source_for(p, mem);
            if (!src) { mat.skip = Skip::Texture; return; }
            mat.prog = mat.checker ? Prog::ModelChecker : src->transparent ? Prog::ModelAlpha : Prog::Model;
            mat.texture = src->texture;
            mat.textured = true;
            mat.u_scale = 1.0f / (8.0f * float(src->source_w)); // Model 2 texel units are x8
            mat.v_scale = 1.0f / (8.0f * float(src->source_h));
        } else if (renderer == 0) {
            mat.prog = mat.checker ? Prog::FlatChecker : Prog::Flat;
        } else {
            mat.skip = Skip::Renderer;
        }
    };
    // Luma bits that change the light result: the palette key keeps luma & kLumaMask
    // (palette_row), the flat colour uses luma >> 2 (solid_color).
    auto luma_key = [&](const rt::GeoPoly &p) -> uint8_t {
        if (mat.textured) return kLumaMask ? uint8_t(p.luma & kLumaMask) : uint8_t(0);
        return uint8_t(p.luma & 0xfc);
    };
    auto set_light = [&](const rt::GeoPoly &p) {
        light = Light{};
        light.cached = true;
        light.key = luma_key(p);
        if (mat.textured) {
            const int row = palette_row(p, mem);
            if (row < 0) return; // light.ok stays false
            light.palette = (float(row) + 0.5f) / float(kPaletteRows);
        } else {
            light.color = solid_color(p, mem);
        }
        light.ok = true;
    };

    // Model 2 draw priority -> one depth per polygon (see the Depth comment at the top).
    const uint64_t sort_begin = now_us();
    if (!polys.empty()) {
        const auto &order = g.order.sort(polys); // first = drawn first = wins
        g.depth.resize(polys.size());
        const float step = 1.0f / float(polys.size() + 1);
        for (size_t rank = 0; rank < order.size(); ++rank) g.depth[order[rank].index] = 1.0f - float(rank + 1) * step;
    }
    last_sort_us_ = now_us() - sort_begin;

    for (size_t pi = 0; pi < polys.size(); ++pi) {
        const rt::GeoPoly &poly = polys[pi];
        const int n = poly.num_vertices;
        if (n < 3 || n > 8) { ++st.skip_vcount; continue; }

        // 1. Material: clip, shader, texture (reused from the previous polygon if possible).
        if (!same_material(poly)) set_material(poly);
        switch (mat.skip) {
        case Skip::None: break;
        case Skip::Window: ++st.skip_window; continue;
        case Skip::Clip: ++st.skip_clip; continue;
        case Skip::Texture: ++st.skip_texture; continue;
        case Skip::Renderer: ++st.skip_renderer; continue;
        }
        // 2. Light: palette row or flat colour, redone only when the useful luma bits change.
        if (!light.cached || luma_key(poly) != light.key) set_light(poly);
        if (!light.ok) { ++st.skip_palette; continue; }
        // 3. Batch: created at the first polygon that gets this far. Batches mix Model 2
        //    windows: the window is part of the priority rank, hence of the depth.
        if (!mat.has_batch) {
            mat.batch = batch_for(mat.clip, mat.prog, mat.texture);
            mat.has_batch = true;
        }

        // 4. Reference only: the vertices are built by flush_batches(), straight into the
        //    frame's GPU vertex memory. x * w = (crtc_x + center_x) * w + vx (GPU divides by w).
        const float base_x = sx(float(crtc_x + poly.center[0]));
        const float base_y = sy(float(384 - poly.center[1] + crtc_y));
        g.batches[mat.batch].poly_refs.push_back({
            &poly, base_x, base_y, mat.u_scale, mat.v_scale, light.palette, g.depth[pi], light.color
        });
    }
    clip_changes_ = unsigned(g.clips.size());
    const uint64_t vertex_begin = now_us();
    flush_batches();
    last_vertex_us_ = now_us() - vertex_begin;

    // Polygon counters, from what flush_batches() actually wrote (it drops invalid polygons).
    for (size_t i = 0; i < g.batch_count; ++i) {
        const Batch &b = g.batches[i];
        st.polys_out += b.valid_polys;
        submitted_vertices_ += b.valid_vertices;
        const bool checker = b.prog == Prog::FlatChecker || b.prog == Prog::ModelChecker;
        if (textured(b.prog)) {
            textured_polys_ += b.valid_polys;
            if (checker) textured_checker_polys_ += b.valid_polys;
        } else {
            solid_polys_ += b.valid_polys;
            if (checker) checker_polys_ += b.valid_polys;
        }
    }
}

// ---- Renderer: 2D layers -----------------------------------------------------------------

// Worker job (core 2, or inline without the thread): no GL call, reads only Video's
// System 24 state, writes only the slot (its texture pixels: the GPU is done with them).
void GpuGlRenderer::s24_prepare(S24Slot &slot, const rt::Video &video) {
    slot.uploaded_tiles = 0;
    // Palette: 8192 pens, rewritten when a pen changed since this slot's last frame.
    if (slot.palette_generation == UINT64_MAX || slot.palette_generation != video.system24_palette_generation()) {
        upload_system24_palette(video, slot.palette, kSystem24PaletteWidth);
        slot.palette_generation = video.system24_palette_generation();
    }
    // Tiles whose pixels or categories changed since this slot's last frame (two frames
    // ago: tile generations only grow), straight into the textures, as pen numbers.
    if (slot.generation != video.system24_texture_generation()) {
        constexpr size_t kStride = 512u * sizeof(uint32_t);
        for (int layer = 0; layer < 4; ++layer)
            slot.uploaded_tiles += upload_system24_layer_indices(video, layer, slot.generation,
                                                                 slot.texels[size_t(layer)], kStride,
                                                                 slot.texels[size_t(layer + 4)], kStride);
        slot.generation = video.system24_texture_generation();
    }
    slot.backdrop = argb_to_rgba(video.system24_pen(0));

    // Layer rectangles, the same for the background and foreground passes. Same
    // layer/window/split logic as GpuFastRenderer::draw_system24.
    using Rect = S24Rect;
    auto &rects = slot.rects;
    auto &runs = slot.runs;
    rects.clear();
    runs.clear();
    auto add_run = [&](int layer, const std::vector<Rect> &list) {
        if (list.empty()) return;
        runs.push_back({layer, uint32_t(rects.size()), uint32_t(list.size())});
        rects.insert(rects.end(), list.begin(), list.end());
    };
    for (int layer = 3; layer >= 0; --layer) {
        const uint16_t hreg = video.system24_word(0x5000u + unsigned(layer));
        const uint16_t vreg = video.system24_word(0x5004u + unsigned(layer));
        if (vreg & 0x8000) continue; // layer disabled
        const uint16_t ctrl = video.system24_word(0x5004u + unsigned(layer & 2));
        const int split_mode = (ctrl >> 13) & 3;
        const bool line_scroll = (hreg & 0x8000) != 0;
        const uint32_t line_base = 0x4000u + 0x200u * unsigned(layer);

        if (split_mode) {
            // The even layer draws itself and the following odd layer; the odd one is skipped.
            if (layer & 1) continue;
            auto &split_rects = slot.split;
            split_rects[0].clear();
            split_rects[1].clear();
            const int source_y = vreg & 511;
            auto add = [&](int source_layer, int x0, int x1, int y0, int y1, int scroll) {
                x0 = std::clamp(x0, 0, rt::Video::W);
                x1 = std::clamp(x1, 0, rt::Video::W);
                y0 = std::clamp(y0, 0, rt::Video::H);
                y1 = std::clamp(y1, 0, rt::Video::H);
                if (x0 < x1 && y0 < y1) split_rects[size_t(source_layer - layer)].push_back({x0, x1, y0, y1, scroll, source_y + y0});
            };
            if (split_mode == 1) { // horizontal split line
                const int neg_v = (-int(vreg)) & 0x3ff;
                const int cut = neg_v & 511;
                int first_layer = layer;
                if (!(neg_v & 0x200)) first_layer ^= 1;
                if (line_scroll) {
                    for (int y = 0; y < rt::Video::H;) {
                        const int scroll = (-int(video.system24_word(line_base + unsigned(y)))) & 511;
                        const int source_layer = y >= cut ? first_layer ^ 1 : first_layer;
                        int y1 = y + 1;
                        while (y1 < rt::Video::H &&
                               ((-int(video.system24_word(line_base + unsigned(y1)))) & 511) == scroll &&
                               (y1 >= cut ? first_layer ^ 1 : first_layer) == source_layer)
                            ++y1;
                        add(source_layer, 0, rt::Video::W, y, y1, scroll);
                        y = y1;
                    }
                } else {
                    const int scroll = (-int(hreg)) & 511;
                    add(first_layer, 0, rt::Video::W, 0, cut, scroll);
                    add(first_layer ^ 1, 0, rt::Video::W, cut, rt::Video::H, scroll);
                }
            } else { // vertical split column
                if (line_scroll) {
                    for (int y = 0; y < rt::Video::H; ++y) {
                        const int raw = video.system24_word(line_base + unsigned(y));
                        const int cut = raw & 511;
                        const int first_layer = (raw & 0x200) ? layer : layer ^ 1;
                        const int scroll = (-cut) & 511;
                        add(first_layer, 0, cut, y, y + 1, scroll);
                        add(first_layer ^ 1, cut, rt::Video::W, y, y + 1, scroll);
                    }
                } else {
                    const int cut = hreg & 511;
                    const int first_layer = (hreg & 0x200) ? layer : layer ^ 1;
                    const int scroll = (-cut) & 511;
                    add(first_layer, 0, cut, 0, rt::Video::H, scroll);
                    add(first_layer ^ 1, cut, rt::Video::W, 0, rt::Video::H, scroll);
                }
            }
            add_run(layer, split_rects[0]);
            add_run(layer + 1, split_rects[1]);
            continue;
        }

        // Normal mode: each line has a 4x16-bit mask of visible 8-pixel blocks ("windows").
        // Lines with the same scroll and mask are merged into one rectangle per visible run.
        const bool win = (layer & 1) != 0;
        const uint32_t mask_base = layer >= 2 ? 0x6800u : 0x6000u;
        auto line_scroll_of = [&](int y) {
            return line_scroll ? ((-int(video.system24_word(line_base + unsigned(y)))) & 511) : ((-int(hreg)) & 511);
        };
        const uint32_t run_first = uint32_t(rects.size());
        for (int y = 0; y < rt::Video::H;) {
            const int h = line_scroll_of(y);
            std::array<uint16_t, 4> masks{};
            for (int w = 0; w < 4; ++w) masks[size_t(w)] = video.system24_word(mask_base + unsigned(y * 4 + w));
            int y1 = y + 1;
            for (; y1 < rt::Video::H && line_scroll_of(y1) == h; ++y1) {
                bool same = true;
                for (int w = 0; same && w < 4; ++w) same = video.system24_word(mask_base + unsigned(y1 * 4 + w)) == masks[size_t(w)];
                if (!same) break;
            }
            auto visible = [&](int block) { return ((masks[size_t(block / 16)] & (0x8000u >> (block & 15))) != 0) == win; };
            for (int block = 0; block * 8 < rt::Video::W;) {
                while (block * 8 < rt::Video::W && !visible(block)) ++block;
                const int first = block;
                while (block * 8 < rt::Video::W && visible(block)) ++block;
                if (first < block) rects.push_back({first * 8, std::min(block * 8, rt::Video::W), y, y1, h, (vreg & 511) + y});
            }
            y = y1;
        }
        if (rects.size() > run_first) runs.push_back({layer, run_first, uint32_t(rects.size()) - run_first});
    }
    slot.valid = true;
}

int GpuGlRenderer::s24_thread_entry(SceSize, void *argp) {
    GpuGlRenderer *self = *static_cast<GpuGlRenderer **>(argp);
    sceKernelSignalSema(self->s24_done_, 1); // started
    int applied_core_mask = 0;
    for (;;) {
        sceKernelWaitSema(self->s24_start_, 1, nullptr);
        if (self->s24_stop_.load()) break;
        if (kWorker2DCore < 0) apply_core_policy(applied_core_mask); // --free-core: the fourth-core policy
        const uint64_t begin = now_us();
        S24Slot &slot = *self->s24_job_;
        try {
            s24_prepare(slot, *self->s24_video_);
        } catch (...) { // out of memory: this frame shows no 2D, the next one uploads everything
            slot.valid = false;
            slot.generation = slot.palette_generation = UINT64_MAX;
        }
        self->s24_job_us_ = now_us() - begin;
#if defined(__arm__)
        // The texels go to GPU-visible memory: drain this core's writes before the main core submits.
        __asm__ volatile("dsb" ::: "memory");
#endif
        sceKernelSignalSema(self->s24_done_, 1);
    }
    return 0;
}

bool GpuGlRenderer::s24_start_thread() {
    s24_stop_.store(false);
    s24_start_ = sceKernelCreateSema("daytona_2d_start", 0, 0, 1, nullptr);
    s24_done_ = sceKernelCreateSema("daytona_2d_done", 0, 0, 1, nullptr);
    if (s24_start_ < 0 || s24_done_ < 0) {
        s24_create_result_ = s24_start_ < 0 ? s24_start_ : s24_done_;
        return false;
    }
    // One step above the main and sound threads (both at the default user priority): the
    // job is short (~2-3 ms) and the main core waits for it at the end of draw(), so it
    // preempts the sound board, which has room to spare in its frame.
    int priority = sceKernelGetThreadCurrentPriority();
    priority = priority > 64 && priority <= 191 ? priority - 1 : 159;
    const int mask = kWorker2DCore >= 0 && kWorker2DCore <= 2 ? SCE_KERNEL_CPU_MASK_USER_0 << kWorker2DCore
                                                              : SCE_KERNEL_CPU_MASK_USER_ALL;
    s24_thread_ = sceKernelCreateThread("daytona_2d", reinterpret_cast<SceKernelThreadEntry>(&s24_thread_entry),
                                        priority, 128 * 1024, 0, mask, nullptr);
    s24_create_result_ = s24_thread_;
    if (s24_thread_ < 0) { s24_thread_ = -1; return false; }
    GpuGlRenderer *self = this;
    const int started = sceKernelStartThread(s24_thread_, sizeof(self), &self);
    if (started < 0) {
        s24_create_result_ = started;
        sceKernelDeleteThread(s24_thread_);
        s24_thread_ = -1;
        return false;
    }
    sceKernelWaitSema(s24_done_, 1, nullptr);
    return true;
}

void GpuGlRenderer::s24_stop_thread() {
    s24_join();
    if (s24_thread_ >= 0) {
        s24_stop_.store(true);
        sceKernelSignalSema(s24_start_, 1);
        sceKernelWaitThreadEnd(s24_thread_, nullptr, nullptr);
        sceKernelDeleteThread(s24_thread_);
        s24_thread_ = -1;
    }
    if (s24_start_ >= 0) sceKernelDeleteSema(s24_start_);
    if (s24_done_ >= 0) sceKernelDeleteSema(s24_done_);
    s24_start_ = s24_done_ = -1;
}

void GpuGlRenderer::s24_kick(const rt::Video &video) {
    S24Slot &slot = s24_[s24_back_];
    if (s24_thread_ < 0) {
        const uint64_t begin = now_us();
        try {
            s24_prepare(slot, video);
        } catch (...) {
            slot.valid = false;
            slot.generation = slot.palette_generation = UINT64_MAX;
        }
        s24_job_us_ = now_us() - begin;
        return;
    }
    s24_video_ = &video;
    s24_job_ = &slot;
    s24_busy_ = true;
    sceKernelSignalSema(s24_start_, 1);
}

void GpuGlRenderer::s24_join() {
    if (!s24_busy_) return;
    sceKernelWaitSema(s24_done_, 1, nullptr);
    s24_busy_ = false;
}

// Quads of a prepared slot, written once, straight into the frame's GPU vertex memory.
// With k2DLayersByDepth the foreground is drawn before the polygons and the background
// after them; the depth buffer puts each one in its place (see k2DLayersByDepth).
void GpuGlRenderer::s24_emit(const S24Slot &slot, bool foreground) {
    const Depth depth = !k2DLayersByDepth ? Depth::Off : foreground ? Depth::TestWrite : Depth::Test;
    const float d = !k2DLayersByDepth ? 0.0f : foreground ? kForegroundDepth : kBackgroundDepth;
    if (!foreground) {
        // Backdrop (pen 0) under the background layers. Opaque when sorted by depth: alpha
        // is 255 once the palette is written, and black is black either way before that.
        push_quad(Prog::Flat, 0, kOffsetX, 0.0f, kOffsetX + kSourceW * kScale, kDisplayH, 0.0f, 0.0f, 0.0f, 0.0f,
                  slot.backdrop, depth, d);
    }
    for (const S24Run &run : slot.runs) {
        const GLuint texture = slot.textures[size_t((foreground ? 4 : 0) + run.layer)];
        // Indexed quads (4 vertices per rectangle) when they fit, else two triangles (6).
        Vertex *dst = append_quads(run.count);
        const bool indexed = dst != nullptr;
        const size_t count = size_t(run.count) * (indexed ? 4u : 6u);
        if (!indexed) dst = g.verts.append(count);
        if (!dst) { g.stats.dropped_vertices += unsigned(count); continue; }
        const uint32_t first = uint32_t(dst - g.verts.data);
        for (uint32_t i = run.first; i < run.first + run.count; ++i) {
            const S24Rect &r = slot.rects[i];
            const float u0 = float(r.x0 + r.h) / 512.0f, u1 = float(r.x1 + r.h) / 512.0f;
            const float v0 = float(r.v) / 512.0f, v1 = float(r.v + (r.y1 - r.y0)) / 512.0f;
            const float x0 = sx(float(r.x0)), x1 = sx(float(r.x1)), y0 = sy(float(r.y0)), y1 = sy(float(r.y1));
            // Triangles (x0y0, x1y0, x1y1) (x0y0, x1y1, x0y1) in both forms.
            *dst++ = {x0, y0, 1, u0, v0, 0, d, 0xffffffffu};
            *dst++ = {x1, y0, 1, u1, v0, 0, d, 0xffffffffu};
            *dst++ = {x1, y1, 1, u1, v1, 0, d, 0xffffffffu};
            if (!indexed) {
                *dst++ = {x0, y0, 1, u0, v0, 0, d, 0xffffffffu};
                *dst++ = {x1, y1, 1, u1, v1, 0, d, 0xffffffffu};
            }
            *dst++ = {x0, y1, 1, u0, v1, 0, d, 0xffffffffu};
        }
        // One command per layer texture (consecutive runs never share a texture).
        g.cmds.push_back({Prog::Layer, texture, first, uint32_t(count), depth, g.clip, indexed});
        system24_quads_ += run.count;
    }
}

// ---- Renderer: frame entry points ------------------------------------------------------
void GpuGlRenderer::draw(rt::Video &video) {
    const uint64_t begin = now_us();
    last_polygon_us_ = last_tile_us_ = last_upload_us_ = last_sort_us_ = last_texture_us_ = 0;
    // The Vita GPU always draws the System 24 layers itself (Video::system24_gpu_compatible()
    // is unconditionally true there): background tiles, Model 2 polygons, foreground tiles.
    // With k2DLayersByDepth the foreground goes first and the background last; the depth
    // buffer keeps the same image (see k2DLayersByDepth).
    // The polygons drawn here are the previous frame's (pipelined geometrizer), so the 2D
    // shown is the previous frame's too (the front slot) while the worker prepares this
    // frame's into the back slot, in parallel with the recording below.
    const S24Slot &front = s24_[s24_back_ ^ 1u];
    s24_kick(video);
    const uint64_t t0 = now_us();
    system24_quads_ = 0;
    g.s24_palette_texture = front.palette_texture;
    if (front.valid) s24_emit(front, k2DLayersByDepth); // first pass: foreground (by depth) or background
    const uint64_t t1 = now_us();
    draw_polygons(video);
    const uint64_t t2 = now_us();
    if (front.valid) s24_emit(front, !k2DLayersByDepth); // last pass: background (by depth) or foreground
    const uint64_t t3 = now_us();
    s24_join(); // the board must not run while the worker reads Video
    const uint64_t t4 = now_us();
    system24_uploaded_tiles_ = s24_[s24_back_].uploaded_tiles;
    s24_back_ ^= 1u;
    last_2d_worker_us_ = s24_job_us_;
    last_upload_us_ = (t0 - begin) + (t4 - t3); // inline job (no thread) or waiting for the worker
    last_polygon_us_ = t2 - t1;
    last_tile_us_ = (t1 - t0) + (t3 - t2);
    g.record_us += t4 - begin;
    g.timing.rec_upload += last_upload_us_;
    g.timing.rec_layers += last_tile_us_;
    g.timing.rec_polygons += last_polygon_us_;
    last_gpu_ms_ = double(t4 - begin) / 1000.0;
}

} // namespace vita