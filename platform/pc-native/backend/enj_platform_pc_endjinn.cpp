/* Daytona USA PC: the KOS PVR/video entry points the game and enDjinn call,
 * on the Vulkan backend (pc_endjinn_vulkan.cpp) and the TA decoder
 * (host_pvr.cpp).  Derived from enDjinn's enj_platform_pc_endjinn.cpp (MIT). */
#include <kos.h>

#include "host_pvr.h"
#include "pc_endjinn_vulkan.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
/* The arcade and the Dreamcast port both run one simulation tick per
 * 59.94 Hz field; frame_clock.c counts the fields a frame missed. */
constexpr double kFieldHz = 60000.0 / 1001.0;
uint64_t g_clock_origin_ns = 0u;
size_t g_frames = 0u;

size_t field_count() {
  if (g_clock_origin_ns == 0u) return 0u;
  const double seconds = static_cast<double>(SDL_GetTicksNS() - g_clock_origin_ns) * 1e-9;
  return static_cast<size_t>(seconds * kFieldHz);
}

/* Sleep to the next field boundary (the console's vertical blank). */
void wait_next_field() {
  if (g_clock_origin_ns == 0u) {
    g_clock_origin_ns = SDL_GetTicksNS();
    return;
  }
  const size_t next = field_count() + 1u;
  const uint64_t target = g_clock_origin_ns + static_cast<uint64_t>(std::llround(next * 1e9 / kFieldHz));
  for (;;) {
    const uint64_t now = SDL_GetTicksNS();
    if (now >= target) break;
    const uint64_t left = target - now;
    if (left > 2000000u) {
      SDL_DelayNS(left - 1500000u);
    } else {
      SDL_DelayPrecise(left);
      break;
    }
  }
}
}  // namespace

extern "C" {

#if !defined(__GLIBC__)
/* KOS's memalign for C libraries without one; the result stays free()-able. */
void *memalign(size_t alignment, size_t size) {
#if defined(_WIN32)
  /* UCRT has no free()-compatible aligned allocation; its malloc is 16-byte
   * aligned, which covers what the game asks memalign for on PC. */
  (void)alignment;
  return malloc(size);
#else
  void *ptr = nullptr;
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  return posix_memalign(&ptr, alignment, size) == 0 ? ptr : nullptr;
#endif
}
#endif

vid_mode_t *vid_mode = pc_endjinn::video_mode();

uint64_t timer_ns_gettime64(void) { return pc_endjinn::timer_ns_gettime64(); }
uint64_t timer_us_gettime64(void) { return pc_endjinn::timer_ns_gettime64() / 1000u; }
uint64_t timer_ms_gettime64(void) { return pc_endjinn::timer_ns_gettime64() / 1000000u; }

void vid_border_color(uint8_t r, uint8_t g, uint8_t b) { pc_endjinn::vid_border_color(r, g, b); }

void vid_set_mode(vid_display_mode_generic_t display_mode, vid_pixel_mode_t pixel_mode) {
  pc_endjinn::vid_set_mode(display_mode, pixel_mode);
}

void vid_set_dithering(bool) {}

int pvr_init(const pvr_init_params_t *params) {
  (void)enj_host_pvr::vram_base();
  pc_endjinn::pvr_init(params);
  return 0;
}

int pvr_shutdown(void) {
  pc_endjinn::pvr_shutdown();
  return 0;
}

void pvr_set_bg_color(float r, float g, float b) { pc_endjinn::pvr_set_bg_color(r, g, b); }

/* The Dreamcast build wraps pvr_wait_ready (code/audio/idle_wait.c) to spend
 * the frame's idle time on the world's stream record cache and on scene
 * loads.  On PC scene loads run at once (scene_loader.c's host path) and
 * there is CPU to spare: the cache gets a fixed budget each frame. */
unsigned nm_render_world_stream_idle(unsigned budget_us) __attribute__((weak));
int nm_loader_busy(void) __attribute__((weak));

int pvr_wait_ready(void) {
  if (nm_render_world_stream_idle != nullptr && (nm_loader_busy == nullptr || !nm_loader_busy())) {
    (void)nm_render_world_stream_idle(4000u);
  }
  pc_endjinn::pvr_wait_ready();
  return 0;
}

void pvr_scene_begin(void) { pc_endjinn::pvr_scene_begin(); }

int pvr_scene_finish(void) {
  pc_endjinn::pvr_scene_finish();
  ++g_frames;
  wait_next_field();
  return 0;
}

/* enj_render_hold_frame(): no scene_begin, so the last scene's primitives
 * are still queued; present them again. */
void enj_host_scene_repeat(void) {
  pc_endjinn::pvr_scene_finish();
  ++g_frames;
  wait_next_field();
}

uint64_t enj_pc_presented_frame_count(void) { return pc_endjinn::pvr_presented_frame_count(); }
bool enj_pc_capture_next_frame(const char *path) { return pc_endjinn::capture_next_frame(path); }

int pvr_list_begin(pvr_list_t list) {
  pc_endjinn::pvr_list_begin(list);
  return 0;
}

int pvr_list_finish(void) {
  pc_endjinn::pvr_list_finish();
  return 0;
}

int pvr_wait_render_done(void) {
  pc_endjinn::pvr_wait_render_done();
  return 0;
}

int pvr_get_stats(pvr_stats_t *stat) {
  if (stat == nullptr) return -1;
  std::memset(stat, 0, sizeof *stat);
  stat->frame_count = g_frames;
  stat->vbl_count = field_count();
  stat->frame_rate = static_cast<float>(kFieldHz);
  stat->frame_last_time = static_cast<uint64_t>(1e9 / kFieldHz);
  return 0;
}

int pvr_get_vbl_count(void) { return static_cast<int>(field_count()); }

void pvr_set_pal_format(pvr_palfmt_t fmt) { PVR_SET(PVR_PALETTE_CFG, static_cast<uint32_t>(fmt)); }

void pvr_set_shadow_scale(bool enable, float scale_value) {
  const float s = scale_value < 0.f ? 0.f : (scale_value > 1.f ? 1.f : scale_value);
  const uint32_t scale = static_cast<uint32_t>(s * 256.0f) > 255u ? 255u : static_cast<uint32_t>(s * 256.0f);
  PVR_SET(PVR_CHEAP_SHADOW, (enable ? 0x100u : 0u) | scale);
}

void pvr_set_zclip(float) {}
void pvr_set_presort_mode(bool) {}

pvr_ptr_t pvr_mem_malloc(size_t size) { return enj_host_pvr::texture_alloc(size); }
void pvr_mem_free(pvr_ptr_t ptr) { enj_host_pvr::texture_free(ptr); }
size_t pvr_mem_available(void) { return enj_host_pvr::texture_available(); }

void pvr_txr_load(const void *src, pvr_ptr_t dst, size_t count) { enj_host_pvr::texture_load(src, dst, count); }

void pvr_txr_load_ex(const void *src, pvr_ptr_t dst, uint32_t width, uint32_t height, uint32_t flags) {
  enj_host_pvr::texture_load_ex(src, dst, width, height, flags);
}

void *pvr_sq_load(void *dest, const void *src, size_t n, pvr_dma_type_t) {
  enj_host_pvr::texture_load(src, dest, n);
  return dest;
}

void pvr_fog_table_color(float a, float r, float g, float b) { pc_endjinn::pvr_fog_table_color(a, r, g, b); }
void pvr_fog_table_linear(float start, float end) { pc_endjinn::pvr_fog_table_linear(start, end); }

void *pc_pvr_dr_target(void) { return enj_host_pvr::dr_target(); }
void pc_pvr_dr_commit(void *ptr) { enj_host_pvr::dr_commit(ptr); }

}  // extern "C"
