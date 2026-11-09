// Hardware renderer (SDL_GPU): draws a frame on the GPU instead of the CPU
// rasterizer (src/runtime/raster.cpp, the exact reference) and tilemap
// drawing (Video::draw): the back tilemap layers, the Model 2 3D layer, the
// front tilemap layers (Video's external-3D desktop mode).
//
// The tilemap layers are composed per pixel (m2.hlsl ps_tiles_back,
// ps_tiles_front) from the pixmaps the CPU decodes, uploaded by the rows of
// tiles that changed, and the frame's tile registers and pens. With the HUD
// moved to the widescreen edges the front layers come from the CPU instead.
//
// Polygons are projected exactly as model2_3d_project, drawn as a fan from
// their first vertex in the rasterizer's order (window, then z, newest first)
// and clipped to their window; the depth test stands in for the rasterizer's
// "first polygon to fill a pixel wins". Solid polygons take their palette
// colour from the CPU; textured ones are shaded per pixel by a port of the
// rasterizer's draw_tex_span (m2.hlsl) reading texture RAM, the luma RAM and
// the colour translation from storage buffers.
#pragma once

#include "runtime/video.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace app {

class GpuRenderer {
public:
    // Creates the pipelines for drawing into textures of `format`. False (with
    // error()) if the GPU or its shader format is not usable.
    bool init(SDL_GPUDevice *dev, SDL_GPUTextureFormat format);
    void shutdown();
    bool ok() const { return poly_pipe_ != nullptr; }
    const std::string &error() const { return error_; }

    // Draws the frame into `target` (at least w x h times `scale`, created
    // with COLOR_TARGET usage): the back tilemap layers, the 3D polygons, the
    // front tilemap layers, all from `video` in external-3D mode. scale > 1
    // (the internal resolution enhancement): the 3D drawn at w x h times
    // scale, textures a mip level finer per doubling, the tilemaps' pixels
    // repeated.
    void render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h, const rt::Video &video, int scale = 1);
    uint64_t panorama_uploads() const { return panorama_uploads_; }

private:
    struct PolyVertex {
        float x, y, depth;
        float ooz, uoz, voz; // render_one's p[0..2]: 1/z, u/z, v/z
        float r, g, b, a;
        uint32_t info[4], more[2]; // texture state (m2.hlsl PolyIn)
    };
    struct QuadVertex {
        float x, y, u, v;
    };
    struct Batch { // consecutive polygons sharing one clip rectangle
        uint32_t first, count;
        SDL_Rect clip;
    };

    SDL_GPUDevice *dev_ = nullptr;
    SDL_GPUGraphicsPipeline *poly_pipe_ = nullptr, *quad_pipe_ = nullptr, *back_pipe_ = nullptr, *front_pipe_ = nullptr;
    SDL_GPUSampler *sampler_ = nullptr;
    SDL_GPUTexture *front_ = nullptr; // the front layers from the CPU (HUD at the edges)
    uint64_t panorama_uploads_ = 0;
    uint64_t original_panorama_instance_ = 0;
    uint8_t original_panorama_course_ = 255;
    uint32_t tilepix_panorama_bytes_ = 0;
    SDL_GPUTexture *depth_ = nullptr;
    int depth_w_ = 0, depth_h_ = 0, front_w_ = 0, front_h_ = 0;
    SDL_GPUBuffer *vbuf_ = nullptr, *qbuf_ = nullptr;
    // storage buffers: texture RAM (both sheets), luma RAM, colour translation with gamma
    SDL_GPUBuffer *texram_ = nullptr, *luma_ = nullptr, *xlat_ = nullptr;
    uint64_t texram_generation_ = ~0ull; // the texture RAM generation texram_ holds
    // the tilemaps: pixmaps (4 x 512 x 512 u16), header + pens + tile words
    SDL_GPUBuffer *tilepix_ = nullptr, *tiledata_ = nullptr;
    uint64_t tile_instance_ = 0, tile_generation_ = 0; // the Video and its tile generation tilepix_ holds
    struct Rows { uint32_t first, count; }; // pixmap rows (of pixels) to upload, per layer
    Rows tile_rows_[4];
    std::vector<uint32_t> tiledata_words_;
    std::vector<uint32_t> xlat_table_;
    uint32_t vbuf_size_ = 0;
    SDL_GPUTransferBuffer *upload_ = nullptr;
    uint32_t upload_size_ = 0;
    std::vector<PolyVertex> verts_;
    std::vector<Batch> batches_;
    std::vector<size_t> order_;
    uint8_t gamma_[256];
    std::string error_;

    SDL_GPUShader *shader(const unsigned char *spv, size_t spv_len, const unsigned char *dxil, size_t dxil_len,
                          const char *msl, const char *dxil_entry, SDL_GPUShaderStage stage, int uniforms, int samplers,
                          int storage_buffers = 0);
    bool ensure(int w, int h, int scale, uint32_t vert_bytes);
    void build(const rt::Video &video, int w, int h, int scale);
    void prepare_panorama(SDL_GPUCommandBuffer *cmd, const rt::Video &video);
};

} // namespace app
