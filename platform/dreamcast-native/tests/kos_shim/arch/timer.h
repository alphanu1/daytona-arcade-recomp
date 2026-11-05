/* Host shim of KOS for tests/test_scene_loader.c: a simulated clock (us) the test advances. */
#pragma once
#include <stdint.h>
extern uint64_t shim_clock_us;
static inline uint64_t timer_us_gettime64(void) { return shim_clock_us; }
