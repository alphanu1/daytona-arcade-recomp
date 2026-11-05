#include "frame_clock.h"
#include <dc/pvr.h>

uint32_t nm_frame_ticks(nm_frame_clock_t *clock) {
    pvr_stats_t stats;
    if(pvr_get_stats(&stats)) return clock->primed ? 1u : 0u; /* no statistics: one tick a frame */
    const uint32_t missed=(uint32_t)(stats.vbl_count-stats.frame_count); /* vblanks that flipped nothing */
    if(!clock->primed) { clock->primed=1;clock->missed=missed;return 0; }
    const uint32_t late=missed-clock->missed;
    clock->missed=missed;
    return late<NM_FRAME_TICKS_LIMIT ? 1u+late : NM_FRAME_TICKS_LIMIT;
}
