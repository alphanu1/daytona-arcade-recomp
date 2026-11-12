#ifndef PC_ENDJINN_DC_MAPLE_PURUPURU_H
#define PC_ENDJINN_DC_MAPLE_PURUPURU_H

#include <dc/maple.h>

typedef union purupuru_effect {
  uint32_t raw;
  struct {
    uint32_t cont : 1, res : 3, motor : 4;
    uint32_t bpow : 3, div : 1, fpow : 3, conv : 1;
    uint32_t freq : 8, inc : 8;
  };
} purupuru_effect_t;

PC_ENDJINN_BEGIN_DECLS
void purupuru_rumble_raw(maple_device_t *device, uint32_t effect);
PC_ENDJINN_END_DECLS

#endif
