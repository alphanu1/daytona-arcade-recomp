#ifndef PC_ENDJINN_DC_FMATH_H
#define PC_ENDJINN_DC_FMATH_H

#include <math.h>

#ifndef F_PI
#define F_PI 3.14159265358979323846f
#endif

static inline void fsincosr(float angle, float *sine, float *cosine) {
  *sine = sinf(angle);
  *cosine = cosf(angle);
}

static inline float fsqrt(float value) { return sqrtf(value); }

#endif
