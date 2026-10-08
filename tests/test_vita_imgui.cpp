#include "platform/vita/imgui_vita.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

static std::vector<unsigned char> pixels;
alignas(16) static unsigned char pool[8 * 1024 * 1024];
static unsigned used = 0, draws = 0, waits = 0;
static bool clipping = false;
vita2d_texture *vita2d_create_empty_texture_format(unsigned w, unsigned h, SceGxmTextureFormat) {
    pixels.resize(w * h * 4);
    auto *texture = new vita2d_texture;
    texture->gxm_tex.data = pixels.data(); texture->gxm_tex.stride = w * 4;
    return texture;
}
void vita2d_free_texture(vita2d_texture *t) { assert(waits); delete t; }
void *vita2d_texture_get_datap(const vita2d_texture *t) { return t->gxm_tex.data; }
unsigned vita2d_texture_get_stride(const vita2d_texture *t) { return t->gxm_tex.stride; }
unsigned vita2d_pool_free_space() { return sizeof(pool) - used; }
void *vita2d_pool_memalign(unsigned n, unsigned) { assert(n <= vita2d_pool_free_space()); void *p = pool + used; used += n; return p; }
void vita2d_wait_rendering_done() { ++waits; }
void vita2d_enable_clipping() { clipping = true; }
void vita2d_disable_clipping() { clipping = false; }
void vita2d_set_clip_rectangle(int x, int y, int r, int b) { assert(x >= 0 && y >= 0 && r <= 960 && b <= 544); }
void vita2d_draw_array_textured(const vita2d_texture *t, int, const vita2d_texture_vertex *v, unsigned count, uint32_t) {
    assert(t && clipping && count % 3 == 0 && count > 0);
    for (unsigned i = 0; i < count; ++i) assert(std::isfinite(v[i].x) && std::isfinite(v[i].u));
    ++draws;
}
int main() {
    vita::ImGuiVita ui;
    const bool initialized = ui.init(); // (outside assert: NDEBUG builds must still initialize)
    assert(initialized); (void)initialized;
    for (int frame = 0; frame < 3; ++frame) {
        used = frame == 2 ? sizeof(pool) : 0;
        ui.frame(1.f / 60);
        ImGui::SetNextWindowPos({12, 12}); ImGui::SetNextWindowSize({936, 520});
        ImGui::Begin("Daytona USA"); ImGui::TextUnformatted("Vita menu"); ImGui::Button("Start"); ImGui::End();
        ui.render(); assert(!clipping);
    }
    assert(draws && pixels.size()); ui.shutdown();
    std::cout << "Vita ImGui atlas, clipped triangles, pool exhaustion and shutdown passed\n";
}
