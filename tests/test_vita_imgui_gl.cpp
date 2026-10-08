// The vitaGL branch of the Vita ImGui menu (imgui_vita.h, GPU_GL builds) against host
// mocks of gpu_gl.h's menu entry points: font atlas upload, triangle lists with their
// scissor rectangles and vertex colours, and texture release at shutdown.
#define DAYTONA_VITA_GPU_GL 1
#include "platform/vita/imgui_vita.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed: %s\n", #x); std::abort(); } } while (0)

namespace {
constexpr uint32_t kAtlas = 7;
unsigned created = 0, deleted = 0, batches = 0;
size_t triangles = 0;
std::vector<uint32_t> colours;
} // namespace

namespace vita {
uint32_t gl_create_ui_texture(int w, int h, const void *rgba) {
    CHECK(w > 0 && h > 0 && rgba);
    ++created;
    return kAtlas;
}
void gl_delete_ui_texture(uint32_t texture) {
    CHECK(texture == kAtlas);
    ++deleted;
}
void gl_ui_triangles(uint32_t texture, const GlUiVertex *v, size_t count, int x0, int y0, int x1, int y1) {
    CHECK(texture == kAtlas && count && count % 3 == 0);
    CHECK(x0 >= 0 && y0 >= 0 && x1 <= 960 && y1 <= 544 && x0 < x1 && y0 < y1);
    for (size_t i = 0; i < count; ++i) {
        CHECK(std::isfinite(v[i].x) && std::isfinite(v[i].y) && std::isfinite(v[i].u) && std::isfinite(v[i].v));
        colours.push_back(v[i].rgba);
    }
    ++batches;
    triangles += count / 3;
}
} // namespace vita

int main() {
    vita::ImGuiVita ui;
    const bool initialized = ui.init();
    CHECK(initialized && created == 1);
    for (int frame = 0; frame < 3; ++frame) {
        ui.frame(1.f / 60);
        ImGui::SetNextWindowPos({12, 12}); ImGui::SetNextWindowSize({936, 520});
        ImGui::Begin("Daytona USA"); ImGui::TextUnformatted("Vita menu"); ImGui::Button("Start"); ImGui::End();
        ui.render();
    }
    CHECK(batches && triangles);
    // Per-vertex colours reach the renderer as IM_COL32 (bytes r, g, b, a): not all one tint.
    bool varied = false;
    for (uint32_t c : colours) varied = varied || c != colours.front();
    CHECK(varied);
    ui.shutdown();
    CHECK(deleted == 1);
    std::printf("Vita ImGui (vitaGL): atlas texture, %zu triangles in %u scissored batches, per-vertex colours, shutdown passed\n",
                triangles, batches);
}
