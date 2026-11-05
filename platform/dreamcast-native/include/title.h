#ifndef DAYTONA_NATIVE_TITLE_H
#define DAYTONA_NATIVE_TITLE_H
#include <stddef.h>
#include <stdint.h>

enum { TITLE_STATIC, TITLE_LOGO, TITLE_PROMPT, TITLE_FOOTER };
typedef struct { int logo_x, prompt, footer; } title_pose_t;

/* Reference blanking clock: 656 * 424 / 16 MHz. PCB timing remains unmeasured. */
static inline uint64_t title_tick(uint64_t elapsed_us) { return elapsed_us / 17384; }

/* Authored ROM motion is prepared offline; no machine state or frame images. */
/* ponytail: hold the no-credit title after the measured interval; wire its
 * exit/credit state when the native selection and attract scenes exist. */
static inline title_pose_t title_pose(const int16_t *motion, size_t count, uint64_t tick) {
    if (!count) return (title_pose_t){0, 0, 0};
    const uint64_t prompt_start = count + 30;
    return (title_pose_t){
        motion[tick < count ? (size_t)tick : count - 1],
        tick >= prompt_start && !((tick - prompt_start) & 8),
        tick >= count - 1
    };
}
#endif
