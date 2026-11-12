#ifndef PC_ENDJINN_DC_MAPLE_VMU_H
#define PC_ENDJINN_DC_MAPLE_VMU_H

#include <dc/maple.h>

#define VMU_SCREEN_WIDTH 48
#define VMU_SCREEN_HEIGHT 32

typedef struct vmufb {
  uint32_t data[VMU_SCREEN_WIDTH];
} vmufb_t;

typedef struct vmufb_font {
  unsigned int id;
  unsigned int w;
  unsigned int h;
  size_t stride;
  const uint8_t *fontdata;
} vmufb_font_t;

PC_ENDJINN_BEGIN_DECLS
void vmufb_paint_area(vmufb_t *fb, unsigned int x, unsigned int y,
                      unsigned int w, unsigned int h, const uint8_t *data);
void vmufb_clear(vmufb_t *fb);
void vmufb_present(const vmufb_t *fb, maple_device_t *device);
void vmufb_print_string_into(vmufb_t *fb, const vmufb_font_t *font,
                             unsigned int x, unsigned int y, unsigned int w,
                             unsigned int h, unsigned int line_spacing,
                             const char *text);
PC_ENDJINN_END_DECLS

#endif
