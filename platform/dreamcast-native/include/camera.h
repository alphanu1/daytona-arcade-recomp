#ifndef DAYTONA_NATIVE_CAMERA_H
#define DAYTONA_NATIVE_CAMERA_H
#include "geometry.h"

/* ponytail: translation-only inspection of the sampled scene, in its initial
 * reference-view basis. Replace the +/-4 source-unit box with course/collision
 * bounds when world placement and gameplay camera semantics are calibrated. */
#define NM_CAMERA_LIMIT 4.0f

static inline float nm_camera_axis(int value) {
    if (value < -128) value = -128;
    if (value > 127) value = 127;
    if (value < -16) return (value + 16) / 112.0f;
    if (value > 16) return (value - 16) / 111.0f;
    return 0;
}

/* Motion is normalized; integration uses seconds, not number of renders.
 * Reject invalid input without changing position; don't jump after a stall.
 * Reset is explicit, not a side effect of releasing the controller. */
static inline int nm_camera_update(float position[3], const float motion[3], float seconds, int reset) {
    if (!nm_finite_float(seconds) || seconds < 0) return 0;
    for (unsigned a = 0; a < 3; ++a)
        if (!nm_finite_float(position[a]) || position[a] < -NM_CAMERA_LIMIT || position[a] > NM_CAMERA_LIMIT ||
            !nm_finite_float(motion[a]) || motion[a] < -1 || motion[a] > 1) return 0;
    if (seconds > 0.05f) seconds = 0.05f;
    for (unsigned a = 0; a < 3; ++a) {
        float next = reset ? 0 : position[a] + motion[a] * (3.0f * seconds);
        if (next < -NM_CAMERA_LIMIT) next = -NM_CAMERA_LIMIT;
        if (next > NM_CAMERA_LIMIT) next = NM_CAMERA_LIMIT;
        position[a] = next;
    }
    return 1;
}
#endif
