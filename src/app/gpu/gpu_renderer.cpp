#include "app/gpu/gpu_renderer.h"

#include "app/gpu/shaders_gen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace app {

namespace {

inline uint16_t le16(const uint8_t *base, uint32_t index) { return uint16_t(base[index * 2] | base[index * 2 + 1] << 8); }

constexpr uint32_t kSheetWords = 0x80000;                 // one texture sheet (VideoMem tex0, tex1)
constexpr uint32_t kTexramBytes = kSheetWords * 4 * 2;    // both sheets
constexpr uint32_t kLumaBytes = 0x20000;                  // luma RAM (one entry per dword)
constexpr uint32_t kXlatEntries = 0x6000;                 // colour translation (0xc000 bytes of u16)
constexpr uint32_t kTilePixBytes = 4 * 512 * 512 * 2;     // the four tilemap pixmaps, a u16 per pixel
constexpr uint32_t kTileHeader = 16;                      // m2.hlsl tiledata: header, pens, tile words
constexpr uint32_t kTileDataWords = kTileHeader + rt::Video::kGpuPens + rt::Video::kGpuTileWords / 2;

} // namespace

SDL_GPUShader *GpuRenderer::shader(const unsigned char *spv, size_t spv_len, const unsigned char *dxil, size_t dxil_len,
                                   const char *msl, const char *dxil_entry, SDL_GPUShaderStage stage, int uniforms,
                                   int samplers, int storage_buffers) {
    SDL_GPUShaderCreateInfo si{};
    si.stage = stage;
    si.num_uniform_buffers = Uint32(uniforms);
    si.num_samplers = Uint32(samplers);
    si.num_storage_buffers = Uint32(storage_buffers);
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev_);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        si.format = SDL_GPU_SHADERFORMAT_SPIRV, si.code = spv, si.code_size = spv_len, si.entrypoint = "main";
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        si.format = SDL_GPU_SHADERFORMAT_DXIL, si.code = dxil, si.code_size = dxil_len, si.entrypoint = dxil_entry;
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        si.format = SDL_GPU_SHADERFORMAT_MSL, si.code = reinterpret_cast<const Uint8 *>(msl),
        si.code_size = std::strlen(msl), si.entrypoint = "main0";
    } else {
        error_ = "no supported shader format (SPIR-V, DXIL or MSL)";
        return nullptr;
    }
    SDL_GPUShader *s = SDL_CreateGPUShader(dev_, &si);
    if (!s) error_ = std::string("SDL_CreateGPUShader: ") + SDL_GetError();
    return s;
}

bool GpuRenderer::init(SDL_GPUDevice *dev, SDL_GPUTextureFormat format) {
    dev_ = dev;
    for (int i = 0; i < 256; i++) // the rasterizer's gamma (MAME video_start)
        gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));

    SDL_GPUShader *vs_poly = shader(k_vs_poly_spv, sizeof k_vs_poly_spv, k_vs_poly_dxil, sizeof k_vs_poly_dxil,
                                    k_vs_poly_msl, "vs_poly", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader *ps_poly = shader(k_ps_poly_spv, sizeof k_ps_poly_spv, k_ps_poly_dxil, sizeof k_ps_poly_dxil,
                                    k_ps_poly_msl, "ps_poly", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 3);
    SDL_GPUShader *vs_quad = shader(k_vs_quad_spv, sizeof k_vs_quad_spv, k_vs_quad_dxil, sizeof k_vs_quad_dxil,
                                    k_vs_quad_msl, "vs_quad", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader *ps_quad = shader(k_ps_quad_spv, sizeof k_ps_quad_spv, k_ps_quad_dxil, sizeof k_ps_quad_dxil,
                                    k_ps_quad_msl, "ps_quad", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    SDL_GPUShader *ps_back = shader(k_ps_tiles_back_spv, sizeof k_ps_tiles_back_spv, k_ps_tiles_back_dxil,
                                    sizeof k_ps_tiles_back_dxil, k_ps_tiles_back_msl, "ps_tiles_back",
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
    SDL_GPUShader *ps_front = shader(k_ps_tiles_front_spv, sizeof k_ps_tiles_front_spv, k_ps_tiles_front_dxil,
                                     sizeof k_ps_tiles_front_dxil, k_ps_tiles_front_msl, "ps_tiles_front",
                                     SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
    if (!vs_poly || !ps_poly || !vs_quad || !ps_quad || !ps_back || !ps_front) {
        for (SDL_GPUShader *s : {vs_poly, ps_poly, vs_quad, ps_quad, ps_back, ps_front})
            if (s) SDL_ReleaseGPUShader(dev_, s);
        return false;
    }

    SDL_GPUColorTargetDescription color{};
    color.format = format;

    // Polygons: position, depth (draw order), 1/z u/z v/z, colour, texture state.
    SDL_GPUVertexBufferDescription pvb{};
    pvb.slot = 0;
    pvb.pitch = sizeof(PolyVertex);
    pvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute pattr[6] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(PolyVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, depth))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, Uint32(offsetof(PolyVertex, ooz))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, Uint32(offsetof(PolyVertex, r))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, Uint32(offsetof(PolyVertex, info))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, Uint32(offsetof(PolyVertex, more))},
    };
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs_poly;
    pi.fragment_shader = ps_poly;
    pi.vertex_input_state.vertex_buffer_descriptions = &pvb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = pattr;
    pi.vertex_input_state.num_vertex_attributes = 6;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = true;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS; // earlier in the order wins
    pi.target_info.color_target_descriptions = &color;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    pi.target_info.has_depth_stencil_target = true;
    poly_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);

    // Layers: a textured quad; no depth test.
    SDL_GPUVertexBufferDescription qvb{};
    qvb.slot = 0;
    qvb.pitch = sizeof(QuadVertex);
    qvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute qattr[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, u))},
    };
    SDL_GPUGraphicsPipelineCreateInfo qi{};
    qi.vertex_shader = vs_quad;
    qi.fragment_shader = ps_quad;
    qi.vertex_input_state.vertex_buffer_descriptions = &qvb;
    qi.vertex_input_state.num_vertex_buffers = 1;
    qi.vertex_input_state.vertex_attributes = qattr;
    qi.vertex_input_state.num_vertex_attributes = 2;
    qi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    qi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    qi.target_info.color_target_descriptions = &color;
    qi.target_info.num_color_targets = 1;
    qi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    qi.target_info.has_depth_stencil_target = true; // same pass as the polygons
    quad_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    // Tilemap layers: the same quad, composed per pixel.
    qi.fragment_shader = ps_back;
    back_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    qi.fragment_shader = ps_front;
    front_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);

    for (SDL_GPUShader *s : {vs_poly, ps_poly, vs_quad, ps_quad, ps_back, ps_front}) SDL_ReleaseGPUShader(dev_, s);
    if (!poly_pipe_ || !quad_pipe_ || !back_pipe_ || !front_pipe_) {
        error_ = std::string("SDL_CreateGPUGraphicsPipeline: ") + SDL_GetError();
        shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo smp{}; // layers are drawn 1:1: nearest
    smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_NEAREST;
    smp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(dev_, &smp);

    SDL_GPUBufferCreateInfo qb{};
    qb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    qb.size = sizeof(QuadVertex) * 6;
    qbuf_ = SDL_CreateGPUBuffer(dev_, &qb);
    SDL_GPUBufferCreateInfo sb{};
    sb.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    sb.size = kTexramBytes;
    texram_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kLumaBytes;
    luma_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kXlatEntries * 4;
    xlat_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kTilePixBytes;
    tilepix_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kTileDataWords * 4;
    tiledata_ = SDL_CreateGPUBuffer(dev_, &sb);
    if (!sampler_ || !qbuf_ || !texram_ || !luma_ || !xlat_ || !tilepix_ || !tiledata_) {
        error_ = std::string("SDL_GPU setup: ") + SDL_GetError();
        shutdown();
        return false;
    }
    return true;
}

void GpuRenderer::shutdown() {
    if (!dev_) return;
    if (poly_pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, poly_pipe_), poly_pipe_ = nullptr;
    for (SDL_GPUGraphicsPipeline **p : {&quad_pipe_, &back_pipe_, &front_pipe_})
        if (*p) SDL_ReleaseGPUGraphicsPipeline(dev_, *p), *p = nullptr;
    if (sampler_) SDL_ReleaseGPUSampler(dev_, sampler_), sampler_ = nullptr;
    panorama_uploads_ = 0;
    original_panorama_instance_ = 0;
    original_panorama_course_ = 255;
    tilepix_panorama_bytes_ = 0;
    if (front_) SDL_ReleaseGPUTexture(dev_, front_), front_ = nullptr;
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_), depth_ = nullptr;
    if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_), vbuf_ = nullptr;
    if (qbuf_) SDL_ReleaseGPUBuffer(dev_, qbuf_), qbuf_ = nullptr;
    for (SDL_GPUBuffer **b : {&texram_, &luma_, &xlat_, &tilepix_, &tiledata_})
        if (*b) SDL_ReleaseGPUBuffer(dev_, *b), *b = nullptr;
    texram_generation_ = ~0ull;
    tile_instance_ = 0;
    if (upload_) SDL_ReleaseGPUTransferBuffer(dev_, upload_), upload_ = nullptr;
    vbuf_size_ = upload_size_ = 0;
    depth_w_ = depth_h_ = front_w_ = front_h_ = 0;
}

// Textures and buffers big enough for this frame.
bool GpuRenderer::ensure(int w, int h, int scale, uint32_t vert_bytes) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    if (w != front_w_ || h != front_h_) {
        if (front_) SDL_ReleaseGPUTexture(dev_, front_), front_ = nullptr;
        ti.width = Uint32(w);
        ti.height = Uint32(h);
        ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the layers' 0xAARRGGBB words
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        front_ = SDL_CreateGPUTexture(dev_, &ti);
        if (!front_) return false;
        front_w_ = w, front_h_ = h;
    }
    if (w * scale != depth_w_ || h * scale != depth_h_) {
        if (depth_) SDL_ReleaseGPUTexture(dev_, depth_), depth_ = nullptr;
        ti.width = Uint32(w * scale);
        ti.height = Uint32(h * scale);
        ti.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        depth_ = SDL_CreateGPUTexture(dev_, &ti);
        if (!depth_) return false;
        depth_w_ = w * scale, depth_h_ = h * scale;
    }
    if (vert_bytes > vbuf_size_) {
        if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_);
        vbuf_size_ = std::max<uint32_t>(vert_bytes, 1u << 20) * 2;
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = vbuf_size_;
        vbuf_ = SDL_CreateGPUBuffer(dev_, &bi);
        if (!vbuf_) return false;
    }
    const uint32_t need = uint32_t(w) * uint32_t(h) * 4 + uint32_t(sizeof(QuadVertex) * 6) + vert_bytes +
                          kTexramBytes + kLumaBytes + kXlatEntries * 4 + kTilePixBytes + kTileDataWords * 4;
    if (need > upload_size_) {
        if (upload_) SDL_ReleaseGPUTransferBuffer(dev_, upload_);
        upload_size_ = need * 2;
        SDL_GPUTransferBufferCreateInfo tb{};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = upload_size_;
        upload_ = SDL_CreateGPUTransferBuffer(dev_, &tb);
        if (!upload_) return false;
    }
    return true;
}

// The polygons as triangles, in the rasterizer's order, batched by clip rectangle.
void GpuRenderer::build(const rt::Video &video, int w, int h, int scale) {
    verts_.clear();
    batches_.clear();
    const std::vector<rt::GeoPoly> &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    const int windows = video.gpu_windows();
    // widescreen: the screen is `margin` wider on each side; x shifts by it
    const int margin = (w - rt::Video::W) / 2;
    const int crtc_x = video.crtc_x() + margin, crtc_y = video.crtc_y(), render_x = video.render_x() + margin,
              render_y = video.render_y();
    // HUD at the edges: the condition panel's own quads (its z, inside its box) move
    const int hud_dx = video.gpu_hud_shift();
    const float *hud_box = video.raster().hud_box();
    const uint16_t hud_z = video.raster().hud_z();
    if (polys.empty() || !mem.palram || !mem.colorxlat) return;
    // internal resolution: the mip level the rasterizer picks from z, less
    // log2(scale) (its levels are 128 units of fast_log2)
    const int lod_bias = int(std::lround(128.0 * std::log2(double(scale))));

    // Raster::render: windows from the last down to 0, low z first, newest first within z.
    order_.resize(polys.size());
    std::iota(order_.begin(), order_.end(), size_t(0));
    std::sort(order_.begin(), order_.end(), [&](size_t a, size_t b) {
        if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
        if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
        return a > b;
    });
    const float depth_step = 1.0f / float(order_.size() + 2);
    uint32_t drawn = 0;
    for (size_t i : order_) {
        const rt::GeoPoly &poly = polys[i];
        if (poly.window > windows || poly.num_vertices < 3) continue;
        const int renderer = (poly.texheader[0] >> 13) & 3;
        if (renderer == 1) continue; // translucent solid: the rasterizer draws nothing

        // HUD overlay quad? (Raster::render_one's test, on the projected outline)
        int dx = 0;
        if (hud_dx && poly.z == hud_z) {
            float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
            for (int k = 0; k < poly.num_vertices; k++) {
                const rt::GeoVertex &g0 = poly.v[k];
                const float z = g0.p[0] + std::numeric_limits<float>::min();
                const float px = float(crtc_x + poly.center[0]) + g0.x / z - float(margin);
                const float py = float((384 - poly.center[1]) + crtc_y) - g0.y / z;
                bx0 = std::min(bx0, px), bx1 = std::max(bx1, px), by0 = std::min(by0, py), by1 = std::max(by1, py);
            }
            constexpr float kTol = 1.5f;
            if (bx0 >= hud_box[0] - kTol && bx1 <= hud_box[1] + kTol && by0 >= hud_box[2] - kTol && by1 <= hud_box[3] + kTol)
                dx = hud_dx;
        }
        // clip rectangle (Raster::render_one), in the renderer's offsets, within the screen;
        // a viewport spanning the screen extends into the widescreen margins
        const int wide = margin && poly.viewport[0] <= 0 && poly.viewport[2] >= 495 ? margin : 0;
        const int x0 = std::max(poly.viewport[0] - wide + render_x + dx, 0),
                  x1 = std::min(poly.viewport[2] + wide + render_x + dx, w - 1);
        const int y0 = std::max((384 - poly.viewport[3]) + render_y, 0), y1 = std::min((384 - poly.viewport[1]) + render_y, h - 1);
        if (x0 > x1 || y0 > y1) continue;
        const SDL_Rect clip{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        if (batches_.empty() || std::memcmp(&batches_.back().clip, &clip, sizeof clip) != 0)
            batches_.push_back({uint32_t(verts_.size()), 0, clip});

        // colour: the solid renderer's (palette entry, luma translation, gamma)
        const uint32_t colorbase = (poly.texheader[3] >> 6) & 0x3ff;
        const uint32_t luma = poly.luma >> 2;
        const uint32_t color = le16(mem.palram, colorbase + 0x1000);
        const float r = gamma_[le16(mem.colorxlat, 0x0000 / 2 + (((color >> 0) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float depth = float(++drawn) * depth_step;

        // texture state, decoded as render_one (m2.hlsl PolyIn)
        const uint32_t th0 = poly.texheader[0], th1 = poly.texheader[1], th2 = poly.texheader[2];
        const uint32_t mirx = (th0 >> 8) & 1, miry = (th0 >> 9) & 1;
        const uint32_t wlog = th0 & 7, hlog = (th0 >> 3) & 7;
        // the rasterizer's 30 - countl_zero(min(width, height)): log2(min) - 1, width = 32 << wlog
        const uint32_t max_level = 4 + std::min(wlog, hlog);
        uint32_t info[4], more[2];
        info[0] = uint32_t(renderer) | ((th0 >> 15) & 1) << 2 | (((th0 >> 6) & 1) & ~mirx) << 3 |
                  (((th0 >> 7) & 1) & ~miry) << 4 | mirx << 5 | miry << 6 | ((th0 >> 12) & 1) << 7 |
                  ((th0 >> 10) & 3) << 8 | ((th2 >> 12) & 1) << 10 | max_level << 12 | wlog << 17 | hlog << 20;
        info[1] = (32u * (th2 & 0x3f)) | (32u * ((th2 >> 6) & 0x1f)) << 16;
        info[2] = (((th2 >> 13) & 1) * 128) | (((th2 >> 14) & 3) * 128) << 16;
        info[3] = ((th1 & 0xff) << 7) | uint32_t(poly.luma) << 16;
        more[0] = uint32_t(poly.texlod + lod_bias);
        more[1] = le16(mem.palram, colorbase + 0x1000) & 0x7fff;

        // model2_3d_project, then a fan from vertex 0 (the polygon is convex)
        PolyVertex v[8];
        for (int k = 0; k < poly.num_vertices; k++) {
            const rt::GeoVertex &g0 = poly.v[k];
            const float z = g0.p[0] + std::numeric_limits<float>::min();
            const float ooz = 1.0f / z; // render_one, textured: p[0] = 1/z, p[1..2] scaled by it / 8
            v[k] = {float(crtc_x + dx + poly.center[0]) + g0.x / z, float((384 - poly.center[1]) + crtc_y) - g0.y / z, depth,
                    ooz, g0.p[1] * ooz * (1.0f / 8.0f), g0.p[2] * ooz * (1.0f / 8.0f),
                    r, g, b, 1.0f, {info[0], info[1], info[2], info[3]}, {more[0], more[1]}};
        }
        for (int k = 1; k + 1 < poly.num_vertices; k++) {
            verts_.push_back(v[0]);
            verts_.push_back(v[k]);
            verts_.push_back(v[k + 1]);
        }
        batches_.back().count = uint32_t(verts_.size()) - batches_.back().first;
    }
}

void GpuRenderer::prepare_panorama(SDL_GPUCommandBuffer *cmd, const rt::Video &video) {
    if (video.panorama().original) {
        if (!video.panorama_active() || (original_panorama_instance_ == video.instance() &&
            original_panorama_course_ == video.panorama().cached_course)) return;
        const uint32_t bytes = uint32_t(video.panorama().indices.size() * sizeof(uint16_t));
        if (tilepix_panorama_bytes_ < bytes) {
            SDL_GPUBufferCreateInfo info{};
            info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
            info.size = kTilePixBytes + bytes;
            SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(dev_, &info);
            if (!buffer) throw std::runtime_error(std::string("panorama buffer: ") + SDL_GetError());
            SDL_ReleaseGPUBuffer(dev_, tilepix_);
            tilepix_ = buffer;
            tilepix_panorama_bytes_ = bytes;
            tile_instance_ = 0; // the replacement needs all four ordinary layers too
        }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = bytes;
        auto *upload = SDL_CreateGPUTransferBuffer(dev_, &info);
        void *data = upload ? SDL_MapGPUTransferBuffer(dev_, upload, false) : nullptr;
        if (!data) {
            if (upload) SDL_ReleaseGPUTransferBuffer(dev_, upload);
            throw std::runtime_error(std::string("original panorama upload: ") + SDL_GetError());
        }
        std::memcpy(data, video.panorama().indices.data(), bytes);
        SDL_UnmapGPUTransferBuffer(dev_, upload);
        auto *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation source{upload, 0};
        SDL_GPUBufferRegion destination{tilepix_, kTilePixBytes, bytes};
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(dev_, upload);
        original_panorama_instance_ = video.instance();
        original_panorama_course_ = video.panorama().cached_course;
        ++panorama_uploads_;
        return;
    }
}

void GpuRenderer::render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h, const rt::Video &video,
                         int scale) {
    if (!ok()) return;
    scale = std::clamp(scale, 1, 4);
    build(video, w, h, scale);
    const uint32_t vert_bytes = uint32_t(verts_.size() * sizeof(PolyVertex));
    if (!ensure(w, h, scale, vert_bytes)) return;
    prepare_panorama(cmd, video);

    // The tilemap pixmaps' rows to upload, per layer: the span of the tile
    // rows changed since tilepix_ was filled (everything for a new Video).
    const uint64_t tile_generation = video.system24_texture_generation();
    const bool all_tiles = video.instance() != tile_instance_ || tile_generation < tile_generation_;
    for (int l = 0; l < 4; l++) {
        int first = 64, last = -1;
        for (unsigned r = 0; r < 64; r++) {
            bool changed = all_tiles;
            for (unsigned t = r * 64; !changed && t < r * 64 + 64; t++)
                changed = video.system24_tile_generation(l, t) > tile_generation_;
            if (changed) first = std::min(first, int(r)), last = int(r);
        }
        tile_rows_[l] = last < 0 ? Rows{0, 0} : Rows{uint32_t(first) * 8, uint32_t(last - first + 1) * 8};
    }
    tile_instance_ = video.instance();
    tile_generation_ = tile_generation;

    // upload: the CPU front layers (HUD at the edges only), the layer quad,
    // the polygon vertices, the storage data
    const bool cpu_front = video.cpu_front();
    const uint32_t layer_bytes = uint32_t(w) * uint32_t(h) * 4;
    auto *p = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev_, upload_, true));
    uint32_t at = 0;
    if (cpu_front) {
        const std::vector<uint32_t> &front = video.foreground_layer();
        std::memcpy(p, front.data(), std::min(front.size() * 4, size_t(layer_bytes)));
        at = layer_bytes;
    }
    const float fw = float(w), fh = float(h);
    const QuadVertex quad[6] = {{0, 0, 0, 0}, {fw, 0, 1, 0}, {0, fh, 0, 1}, {fw, 0, 1, 0}, {fw, fh, 1, 1}, {0, fh, 0, 1}};
    const uint32_t quad_at = at;
    std::memcpy(p + quad_at, quad, sizeof quad);
    const uint32_t verts_at = quad_at + uint32_t(sizeof quad);
    if (vert_bytes) std::memcpy(p + verts_at, verts_.data(), vert_bytes);
    at = verts_at + vert_bytes;
    // luma RAM and the gamma'd colour translation every frame, texture RAM when it changed
    const rt::VideoMem &mem = video.gpu_mem();
    const uint32_t data_at = at;
    const bool have_mem = mem.lumaram && mem.colorxlat && mem.tex0 && mem.tex1;
    bool tex_changed = false;
    if (have_mem) {
        std::memcpy(p + data_at, mem.lumaram, kLumaBytes);
        xlat_table_.resize(kXlatEntries);
        for (uint32_t i = 0; i < kXlatEntries; i++) xlat_table_[i] = gamma_[le16(mem.colorxlat, i) & 0xff];
        std::memcpy(p + data_at + kLumaBytes, xlat_table_.data(), kXlatEntries * 4);
        at += kLumaBytes + kXlatEntries * 4;
        tex_changed = mem.tex_generation != texram_generation_; // the board counts texture RAM writes
        if (tex_changed) {
            texram_generation_ = mem.tex_generation;
            std::memcpy(p + at, mem.tex0, kSheetWords * 4);
            std::memcpy(p + at + kSheetWords * 4, mem.tex1, kSheetWords * 4);
            at += kTexramBytes;
        }
    }
    // tilemaps: the changed pixmap rows, packed as m2.hlsl reads them (pen, category in bit 15)
    uint32_t rows_at[4];
    for (int l = 0; l < 4; l++) {
        rows_at[l] = at;
        const uint32_t n = tile_rows_[l].count * 512;
        if (!n) continue;
        const uint16_t *pixels = video.system24_pixels(l) + size_t(tile_rows_[l].first) * 512;
        const uint8_t *flags = video.system24_flags(l) + size_t(tile_rows_[l].first) * 512;
        uint16_t *dst = reinterpret_cast<uint16_t *>(p + at);
        for (uint32_t i = 0; i < n; i++) dst[i] = uint16_t(pixels[i] | (flags[i] & 1) << 15);
        at += n * 2;
    }
    // the header (widescreen margin and its fill), pens, tile words
    tiledata_words_.assign(kTileDataWords, 0);
    tiledata_words_[0] = uint32_t(video.margin());
    tiledata_words_[1] = uint32_t(video.backdrop()); // Edges 0, Sky 1, Stretch 2, as m2.hlsl
    tiledata_words_[2] = uint32_t(scale);
    tiledata_words_[3] = uint32_t(video.panorama_active() && video.panorama().original);
    tiledata_words_[4] = uint32_t(video.panorama().scroll_x());
    tiledata_words_[5] = rt::Panorama::SourceY;
    tiledata_words_[6] = video.panorama().source_height();
    std::memcpy(&tiledata_words_[kTileHeader], video.gpu_pens(), rt::Video::kGpuPens * 4);
    std::memcpy(&tiledata_words_[kTileHeader + rt::Video::kGpuPens], video.gpu_tile_words(), rt::Video::kGpuTileWords * 2);
    const uint32_t tiledata_at = at;
    std::memcpy(p + tiledata_at, tiledata_words_.data(), kTileDataWords * 4);
    SDL_UnmapGPUTransferBuffer(dev_, upload_);

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    // cycle: the whole buffer is replaced, so a buffer still in use may be swapped for a fresh one
    auto upload_buffer = [&](uint32_t from, SDL_GPUBuffer *buffer, uint32_t offset, uint32_t size, bool cycle = true) {
        SDL_GPUTransferBufferLocation src{upload_, from};
        SDL_GPUBufferRegion dst{buffer, offset, size};
        SDL_UploadToGPUBuffer(copy, &src, &dst, cycle);
    };
    if (cpu_front) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = upload_;
        src.pixels_per_row = Uint32(w);
        SDL_GPUTextureRegion dst{};
        dst.texture = front_;
        dst.w = Uint32(w);
        dst.h = Uint32(h);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
    }
    upload_buffer(quad_at, qbuf_, 0, sizeof quad);
    if (vert_bytes) upload_buffer(verts_at, vbuf_, 0, vert_bytes);
    if (have_mem) {
        upload_buffer(data_at, luma_, 0, kLumaBytes);
        upload_buffer(data_at + kLumaBytes, xlat_, 0, kXlatEntries * 4);
        if (tex_changed) upload_buffer(data_at + kLumaBytes + kXlatEntries * 4, texram_, 0, kTexramBytes);
    }
    for (int l = 0; l < 4; l++)
        if (tile_rows_[l].count)
            upload_buffer(rows_at[l], tilepix_, (uint32_t(l) * 512 * 512 + tile_rows_[l].first * 512) * 2,
                          tile_rows_[l].count * 512 * 2, false); // rows: the rest must stay
    upload_buffer(tiledata_at, tiledata_, 0, kTileDataWords * 4);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = SDL_FColor{0, 0, 0, 1};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth_;
    dt.clear_depth = 1.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    // the target may be bigger than this frame; positions are in original
    // pixels (screen), the viewport and clip rectangles in the target's
    const SDL_GPUViewport viewport{0, 0, float(w * scale), float(h * scale), 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    const float screen[4] = {fw, fh, 0, 0};
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    const SDL_Rect full{0, 0, w * scale, h * scale};

    // a full-frame quad through `pipe`: the tilemap layers, or the CPU front layers
    auto draw_quad = [&](SDL_GPUGraphicsPipeline *pipe) {
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        SDL_SetGPUScissor(pass, &full);
        SDL_GPUBufferBinding qb{qbuf_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &qb, 1);
        if (pipe == quad_pipe_) {
            SDL_GPUTextureSamplerBinding tsb{front_, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        } else {
            SDL_GPUBuffer *storage[2] = {tilepix_, tiledata_};
            SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 2);
        }
        SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    };
    draw_quad(back_pipe_);
    const bool panorama_only = video.panorama().only;
    if (!verts_.empty() && !panorama_only) {
        SDL_BindGPUGraphicsPipeline(pass, poly_pipe_);
        SDL_GPUBufferBinding vb{vbuf_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUBuffer *storage[3] = {texram_, luma_, xlat_};
        SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 3);
        for (const Batch &b : batches_) {
            if (!b.count) continue;
            const SDL_Rect clip{b.clip.x * scale, b.clip.y * scale, b.clip.w * scale, b.clip.h * scale};
            SDL_SetGPUScissor(pass, &clip);
            SDL_DrawGPUPrimitives(pass, b.count, 1, b.first, 0);
        }
    }
    if (!panorama_only) draw_quad(cpu_front ? quad_pipe_ : front_pipe_);
    SDL_EndGPURenderPass(pass);
}

} // namespace app
