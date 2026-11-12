#include "pc_endjinn_frame.h"

#include "host_pvr.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace pc_endjinn {
namespace {

using enj_host_pvr::QueuedPrimitive;

PcVertex make_vertex(const vid_mode_t &video_mode, bool fsaa_enabled,
                     float x, float y, float z, uint32_t argb, float u,
                     float v)
{
    const float half_width = static_cast<float>(video_mode.width) *
        (fsaa_enabled ? 1.0f : 0.5f);
    const float half_height = static_cast<float>(video_mode.height) * 0.5f;
    PcVertex vertex{};
    vertex.position[0] = x / half_width - 1.0f;
    vertex.position[1] = y / half_height - 1.0f;
    vertex.position[2] = z;
    vertex.color[0] = static_cast<float>((argb >> 16u) & 0xffu) / 255.0f;
    vertex.color[1] = static_cast<float>((argb >> 8u) & 0xffu) / 255.0f;
    vertex.color[2] = static_cast<float>(argb & 0xffu) / 255.0f;
    vertex.color[3] = static_cast<float>((argb >> 24u) & 0xffu) / 255.0f;
    vertex.uv[0] = u;
    vertex.uv[1] = v;
    return vertex;
}

bool triangle_is_culled(const QueuedPrimitive &primitive, uint32_t a,
                        uint32_t b, uint32_t c)
{
    if (primitive.culling != PVR_CULLING_CCW &&
        primitive.culling != PVR_CULLING_CW) {
        return false;
    }
    const float signed_area =
        (primitive.x[b] - primitive.x[a]) *
            (primitive.y[c] - primitive.y[a]) -
        (primitive.y[b] - primitive.y[a]) *
            (primitive.x[c] - primitive.x[a]);
    /* PVR framebuffer coordinates have a downward-growing Y axis. */
    return primitive.culling == PVR_CULLING_CCW ? signed_area < 0.0f
                                                : signed_area > 0.0f;
}

void emit_primitive(std::vector<PcVertex> &vertices,
                    const QueuedPrimitive &primitive,
                    const vid_mode_t &video_mode, bool fsaa_enabled)
{
    const auto emit = [&](uint32_t index) {
        vertices.push_back(make_vertex(
            video_mode, fsaa_enabled, primitive.x[index], primitive.y[index],
            primitive.z[index], primitive.color[index], primitive.u[index],
            primitive.v[index]));
    };
    if (primitive.count == 3u) {
        if (!triangle_is_culled(primitive, 0u, 1u, 2u)) {
            emit(0u);
            emit(1u);
            emit(2u);
        }
    } else if (primitive.count == 4u) {
        if (!triangle_is_culled(primitive, 0u, 1u, 2u)) {
            emit(0u);
            emit(1u);
            emit(2u);
        }
        if (!triangle_is_culled(primitive, 0u, 2u, 3u)) {
            emit(0u);
            emit(2u);
            emit(3u);
        }
    }
}

}  // namespace

FrameDrawData build_frame_draw_data(const vid_mode_t &video_mode,
                                    bool fsaa_enabled,
                                    bool translucent_autosort)
{
    const std::vector<QueuedPrimitive> &queued = enj_host_pvr::primitives();
    FrameDrawData frame;
    frame.vertices.reserve(queued.size() * 6u);
    frame.translucent_modifiers.reserve(queued.size());

    for (const QueuedPrimitive &primitive : queued) {
        if (primitive.list != PVR_LIST_TR_MOD || !primitive.modifier_volume ||
            primitive.count != 3u ||
            triangle_is_culled(primitive, 0u, 1u, 2u)) {
            continue;
        }
        const PcVertex a = make_vertex(
            video_mode, fsaa_enabled, primitive.x[0], primitive.y[0],
            primitive.z[0], 0u, 0.0f, 0.0f);
        const PcVertex b = make_vertex(
            video_mode, fsaa_enabled, primitive.x[1], primitive.y[1],
            primitive.z[1], 0u, 0.0f, 0.0f);
        const PcVertex c = make_vertex(
            video_mode, fsaa_enabled, primitive.x[2], primitive.y[2],
            primitive.z[2], 0u, 0.0f, 0.0f);
        GpuModifierTriangle event{};
        std::memcpy(event.a, a.position, sizeof(a.position));
        std::memcpy(event.b, b.position, sizeof(b.position));
        std::memcpy(event.c, c.position, sizeof(c.position));
        event.state[0] = primitive.modifier_volume_last ? 1u : 0u;
        event.state[1] = !primitive.modifier_volume_last &&
                primitive.modifier_mode != PVR_MODIFIER_OTHER_POLY
            ? 1u
            : 0u;
        event.state[2] = primitive.modifier_mode;
        frame.translucent_modifiers.push_back(event);
    }

    const auto append_list = [&](pvr_list_t list, bool sort_back_to_front,
                                 bool modifier_volume, int modifier_filter) {
        std::vector<const QueuedPrimitive *> primitives;
        primitives.reserve(queued.size());
        for (const QueuedPrimitive &primitive : queued) {
            if (primitive.list == list &&
                primitive.modifier_volume == modifier_volume &&
                (modifier_filter < 0 ||
                 primitive.modifier == (modifier_filter != 0))) {
                primitives.push_back(&primitive);
            }
        }
        if (sort_back_to_front) {
            const auto diagnostics = enj_host_translucent_sort::sort(primitives);
            if (list == PVR_LIST_TR_POLY) {
                frame.translucent_sort = diagnostics;
            }
        }
        for (const QueuedPrimitive *primitive : primitives) {
            const bool same_batch = !frame.batches.empty() &&
                frame.batches.back().list == list &&
                frame.batches.back().depth_test == primitive->depth_test &&
                frame.batches.back().depth_write == primitive->depth_write &&
                frame.batches.back().textured == primitive->textured &&
                frame.batches.back().texture == primitive->texture &&
                frame.batches.back().texture_format == primitive->texture_format &&
                frame.batches.back().texture_width == primitive->texture_width &&
                frame.batches.back().texture_height == primitive->texture_height &&
                frame.batches.back().texture_filter == primitive->texture_filter &&
                frame.batches.back().modifier_receiver ==
                    primitive->modifier_receiver &&
                frame.batches.back().modifier == primitive->modifier &&
                frame.batches.back().modifier_volume ==
                    primitive->modifier_volume &&
                frame.batches.back().modifier_volume_last ==
                    primitive->modifier_volume_last &&
                frame.batches.back().modifier_mode == primitive->modifier_mode &&
                frame.batches.back().alpha_cutout == primitive->alpha_cutout &&
                frame.batches.back().depth_func == primitive->depth_func &&
                frame.batches.back().blend_src == primitive->blend_src &&
                frame.batches.back().blend_dst == primitive->blend_dst &&
                frame.batches.back().tex_env == primitive->tex_env &&
                frame.batches.back().use_alpha == primitive->use_alpha &&
                frame.batches.back().ignore_tex_alpha == primitive->ignore_tex_alpha &&
                frame.batches.back().uv_clamp == primitive->uv_clamp &&
                frame.batches.back().uv_flip == primitive->uv_flip &&
                frame.batches.back().mip_bias == primitive->mip_bias;
            if (!same_batch) {
                DrawBatch batch{};
                batch.list = list;
                batch.first_vertex =
                    static_cast<uint32_t>(frame.vertices.size());
                batch.depth_test = primitive->depth_test;
                batch.depth_write = primitive->depth_write;
                batch.textured = primitive->textured;
                batch.texture = primitive->texture;
                batch.texture_format = primitive->texture_format;
                batch.texture_width = primitive->texture_width;
                batch.texture_height = primitive->texture_height;
                batch.texture_filter = primitive->texture_filter;
                batch.modifier_receiver = primitive->modifier_receiver;
                batch.modifier = primitive->modifier;
                batch.modifier_volume = primitive->modifier_volume;
                batch.modifier_volume_last = primitive->modifier_volume_last;
                batch.modifier_mode = primitive->modifier_mode;
                batch.alpha_cutout = primitive->alpha_cutout;
                batch.depth_func = primitive->depth_func;
                batch.blend_src = primitive->blend_src;
                batch.blend_dst = primitive->blend_dst;
                batch.tex_env = primitive->tex_env;
                batch.use_alpha = primitive->use_alpha;
                batch.ignore_tex_alpha = primitive->ignore_tex_alpha;
                batch.uv_clamp = primitive->uv_clamp;
                batch.uv_flip = primitive->uv_flip;
                batch.mip_bias = primitive->mip_bias;
                const uint32_t pixel_format =
                    (primitive->texture_format >> 27u) & 7u;
                batch.palette_base = pixel_format == PVR_PIXEL_MODE_PAL_4BPP
                    ? ((primitive->texture_format >> 21u) & 0x3fu) * 16u
                    : ((primitive->texture_format >> 25u) & 0x03u) * 256u;
                frame.batches.push_back(batch);
            }
            const uint32_t before =
                static_cast<uint32_t>(frame.vertices.size());
            emit_primitive(frame.vertices, *primitive, video_mode, fsaa_enabled);
            frame.batches.back().vertex_count +=
                static_cast<uint32_t>(frame.vertices.size()) - before;
        }
    };

    /* Area 0 establishes receiver depth before volume classification. */
    append_list(PVR_LIST_OP_POLY, false, false, 0);
    append_list(PVR_LIST_OP_MOD, false, true, -1);
    append_list(PVR_LIST_OP_POLY, false, false, 1);
    append_list(PVR_LIST_PT_POLY, false, false, -1);
    append_list(PVR_LIST_TR_POLY, translucent_autosort, false, -1);
    return frame;
}

}  // namespace pc_endjinn
