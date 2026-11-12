#ifndef PC_ENDJINN_DC_MAPLE_CONTROLLER_H
#define PC_ENDJINN_DC_MAPLE_CONTROLLER_H

#include <dc/maple.h>

#define CONT_C 0x0001u
#define CONT_B 0x0002u
#define CONT_A 0x0004u
#define CONT_START 0x0008u
#define CONT_DPAD_UP 0x0010u
#define CONT_DPAD_DOWN 0x0020u
#define CONT_DPAD_LEFT 0x0040u
#define CONT_DPAD_RIGHT 0x0080u
#define CONT_Z 0x0100u
#define CONT_Y 0x0200u
#define CONT_X 0x0400u
#define CONT_D 0x0800u
#define CONT_DPAD2_UP 0x1000u
#define CONT_DPAD2_DOWN 0x2000u
#define CONT_DPAD2_LEFT 0x4000u
#define CONT_DPAD2_RIGHT 0x8000u

typedef struct cont_state {
  union {
    uint32_t buttons;
    struct {
      uint32_t c : 1, b : 1, a : 1, start : 1;
      uint32_t dpad_up : 1, dpad_down : 1, dpad_left : 1, dpad_right : 1;
      uint32_t z : 1, y : 1, x : 1, d : 1;
      uint32_t dpad2_up : 1, dpad2_down : 1, dpad2_left : 1,
          dpad2_right : 1;
      uint32_t reserved : 16;
    };
  };
  int ltrig, rtrig;
  int joyx, joyy, joy2x, joy2y;
} cont_state_t;

#endif
