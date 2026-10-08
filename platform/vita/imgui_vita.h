#pragma once
#include "imgui.h"
#include <psp2/touch.h>
// Two renderers, the same menu: libvita2d (GPU_FAST builds) or vitaGL (GPU_GL builds,
// gpu_gl.h: the menu's triangles join the frame's draw list, blended).
#ifndef DAYTONA_VITA_GPU_GL
#define DAYTONA_VITA_GPU_GL 0
#endif
#if DAYTONA_VITA_GPU_GL
#include "gpu_gl.h"
#include <vector>
#else
#include <vita2d.h>
#endif
#include <algorithm>
#include <cstring>
#include <cstdint>

namespace vita {
// Menu-only ImGui renderer on the existing GXM context. No SDL video device,
// extra display buffers or game shader changes are needed.
class ImGuiVita {
#if DAYTONA_VITA_GPU_GL
    uint32_t atlas_ = 0;              // GLuint
    std::vector<GlUiVertex> vertices_; // one command's triangles, reused
#else
    vita2d_texture *atlas_ = nullptr;
#endif
public:
    bool init() {
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendRendererName = DAYTONA_VITA_GPU_GL ? "daytona_vitagl" : "daytona_vita2d";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        ImFontConfig font; font.SizePixels = 20;
        io.Fonts->AddFontDefault(&font);
        unsigned char *pixels; int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
#if DAYTONA_VITA_GPU_GL
        atlas_ = gl_create_ui_texture(w, h, pixels);
        if (!atlas_) { ImGui::DestroyContext(); return false; }
        io.Fonts->SetTexID(ImTextureID(uintptr_t(atlas_)));
#else
        atlas_ = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        if (!atlas_) { ImGui::DestroyContext(); return false; }
        auto *dst = static_cast<unsigned char *>(vita2d_texture_get_datap(atlas_));
        const auto stride = vita2d_texture_get_stride(atlas_);
        for (int y = 0; y < h; ++y) std::memcpy(dst + y * stride, pixels + y * w * 4, w * 4);
        io.Fonts->SetTexID(ImTextureID(reinterpret_cast<uintptr_t>(atlas_)));
#endif
        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.FramePadding = {10, 7}; style.ItemSpacing = {10, 8};
        style.ScrollbarSize = 24;
#if !DAYTONA_VITA_GPU_GL // (vitaGL: one colour per vertex, antialiased edges kept)
        // vita2d's texture shader supplies one tint per triangle, not per vertex.
        // Disable vertex-alpha fringes; glyph antialiasing remains in the atlas.
        style.AntiAliasedFill = false; style.AntiAliasedLines = false;
#endif
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        return true;
    }
    void frame(float dt) {
        auto &io = ImGui::GetIO(); io.DisplaySize = {960, 544};
        io.DeltaTime = std::clamp(dt, .001f, .1f);
        SceTouchData touch{};
        if (sceTouchPeek( SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum) {
            io.AddMousePosEvent(touch.report[0].x * .5f, touch.report[0].y * .5f);
            io.AddMouseButtonEvent(0, true);
        } else io.AddMouseButtonEvent(0, false);
        ImGui::NewFrame();
    }
#if DAYTONA_VITA_GPU_GL
    // Between gl_begin_frame() and gl_end_frame(), like the game image.
    void render() {
        ImGui::Render();
        const auto *data = ImGui::GetDrawData();
        for (const auto *list : data->CmdLists) for (const auto &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            const int x0 = int(cmd.ClipRect.x), y0 = int(cmd.ClipRect.y);
            const int x1 = int(cmd.ClipRect.z), y1 = int(cmd.ClipRect.w);
            if (x1 <= x0 || y1 <= y0 || !cmd.ElemCount) continue;
            vertices_.resize(cmd.ElemCount);
            for (unsigned i = 0; i < cmd.ElemCount; ++i) {
                const auto &in = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i]];
                vertices_[i] = {in.pos.x, in.pos.y, in.uv.x, in.uv.y, in.col}; // IM_COL32: bytes r, g, b, a
            }
            gl_ui_triangles(uint32_t(uintptr_t(cmd.GetTexID())), vertices_.data(), vertices_.size(), x0, y0, x1, y1);
        }
    }
    void shutdown() {
        gl_delete_ui_texture(atlas_);
        atlas_ = 0; ImGui::DestroyContext();
    }
#else
    void render() {
        ImGui::Render();
        const auto *data = ImGui::GetDrawData();
        vita2d_enable_clipping();
        for (const auto *list : data->CmdLists) for (const auto &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            int x0 = std::clamp(int(cmd.ClipRect.x), 0, 960), y0 = std::clamp(int(cmd.ClipRect.y), 0, 544);
            int x1 = std::clamp(int(cmd.ClipRect.z), 0, 960), y1 = std::clamp(int(cmd.ClipRect.w), 0, 544);
            if (x1 <= x0 || y1 <= y0 || !cmd.ElemCount) continue;
            const size_t bytes = cmd.ElemCount * sizeof(vita2d_texture_vertex);
            if (vita2d_pool_free_space() < bytes + 4096) break;
            vita2d_set_clip_rectangle(x0, y0, x1, y1);
            auto *v = static_cast<vita2d_texture_vertex *>(vita2d_pool_memalign(bytes, 4));
            if (!v) break;
            auto *texture = reinterpret_cast<vita2d_texture *>(uintptr_t(cmd.GetTexID()));
            for (unsigned i = 0; i < cmd.ElemCount; ++i) {
                const auto &in = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i]];
                v[i] = {in.pos.x, in.pos.y, .5f, in.uv.x, in.uv.y};
            }
            // Most widgets/text are long runs of identical tint; batch those.
            for (unsigned first = 0; first + 2 < cmd.ElemCount;) {
                auto color = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + first]].col;
                unsigned end = first + 3;
                while (end + 2 < cmd.ElemCount && list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + end]].col == color) end += 3;
                vita2d_draw_array_textured(texture, SCE_GXM_PRIMITIVE_TRIANGLES, v + first, end - first, color);
                first = end;
            }
        }
        vita2d_disable_clipping();
    }
    void shutdown() {
        vita2d_wait_rendering_done();
        if (atlas_) vita2d_free_texture(atlas_);
        atlas_ = nullptr; ImGui::DestroyContext();
    }
#endif
};
} // namespace vita
