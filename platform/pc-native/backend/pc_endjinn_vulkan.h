#ifndef PC_ENDJINN_VULKAN_H
#define PC_ENDJINN_VULKAN_H

#include <kos.h>

namespace pc_endjinn {

vid_mode_t *video_mode();
uint64_t timer_ns_gettime64();
void vid_border_color(uint8_t r, uint8_t g, uint8_t b);
void vid_set_mode(vid_display_mode_generic_t display_mode,
                  vid_pixel_mode_t pixel_mode);
void pvr_init(const pvr_init_params_t *params);
void pvr_shutdown();
void pvr_set_bg_color(float r, float g, float b);
void pvr_wait_ready();
void pvr_scene_begin();
void pvr_scene_finish();
/* Host-only observation hook.  This deliberately reports the count of fully
 * presented PVR scenes and has no effect on collection or rendering. */
uint64_t pvr_presented_frame_count();
bool capture_next_frame(const char *path);
void pvr_list_begin(pvr_list_t list);
void pvr_list_finish();
void pvr_wait_render_done();
void pvr_set_pal_format(pvr_palfmt_t mode);
void pvr_fog_table_color(float a, float r, float g, float b);
void pvr_fog_table_linear(float start, float end);
void *pvr_dr_target();
void pvr_dr_commit(void *ptr);

}  // namespace pc_endjinn

#endif
