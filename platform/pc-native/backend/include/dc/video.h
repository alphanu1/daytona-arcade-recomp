#ifndef PC_ENDJINN_DC_VIDEO_H
#define PC_ENDJINN_DC_VIDEO_H

#include <pc_endjinn/types.h>

typedef enum vid_pixel_mode { PM_RGB565 = 0, PM_RGB888P = 1 } vid_pixel_mode_t;
typedef enum vid_display_mode_generic { DM_640x480 = 0 }
    vid_display_mode_generic_t;

typedef struct vid_mode {
  int width;
  int height;
} vid_mode_t;

PC_ENDJINN_BEGIN_DECLS
extern vid_mode_t *vid_mode;
void vid_border_color(uint8_t r, uint8_t g, uint8_t b);
void vid_set_mode(vid_display_mode_generic_t mode,
                  vid_pixel_mode_t pixel_mode);
PC_ENDJINN_END_DECLS

#endif
