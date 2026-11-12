#ifndef PC_ENDJINN_FRAME_H
#define PC_ENDJINN_FRAME_H

#include "host_translucent_sort.h"

#include <kos.h>

#include <cstdint>
#include <vector>

namespace pc_endjinn {

struct PcVertex {
    float position[3];
    float color[4];
    float uv[2];
};

struct alignas(16) GpuModifierTriangle {
    float a[4];
    float b[4];
    float c[4];
    uint32_t state[4];
};

static_assert(sizeof(GpuModifierTriangle) == 64u);

struct DrawBatch {
    pvr_list_t list;
    uint32_t first_vertex;
    uint32_t vertex_count;
    bool depth_test;
    bool depth_write;
    bool textured;
    pvr_ptr_t texture;
    uint32_t texture_format;
    uint32_t texture_width;
    uint32_t texture_height;
    pvr_filter_mode_t texture_filter;
    uint32_t palette_base;
    bool modifier_receiver;
    bool modifier;
    bool modifier_volume;
    bool modifier_volume_last;
    uint32_t modifier_mode;
    bool alpha_cutout;
    /* PVR render state (host_pvr.h QueuedPrimitive). */
    uint8_t depth_func;
    uint8_t blend_src;
    uint8_t blend_dst;
    uint8_t tex_env;
    bool use_alpha;
    bool ignore_tex_alpha;
    uint8_t uv_clamp;
    uint8_t uv_flip;
    uint8_t mip_bias;
};

struct FrameDrawData {
    std::vector<PcVertex> vertices;
    std::vector<DrawBatch> batches;
    std::vector<GpuModifierTriangle> translucent_modifiers;
    enj_host_translucent_sort::Diagnostics translucent_sort;
};

FrameDrawData build_frame_draw_data(const vid_mode_t &video_mode,
                                    bool fsaa_enabled,
                                    bool translucent_autosort);

}  // namespace pc_endjinn

#endif
