#include "drive.h"
#include <stdio.h>
#include <stdlib.h>
#include "nm_mem.h"

_Static_assert(sizeof(nm_crash_row_t) == 12, "DND13 crash row layout");
_Static_assert(sizeof(nm_drive_params_t) == 284452, "DND27 payload layout");
_Static_assert(sizeof(nm_drive_terms_t) == 20, "drive output layout");

int nm_drive_params_valid(const nm_drive_params_t *p) {
    if (!p || p->guided_reserved || p->camera_reset_reserved || !isfinite(p->collision_speed_step) || !isfinite(p->speed_index_scale) || !isfinite(p->inhibit_speed_drop) ||
        !isfinite(p->throttle_scale) || !isfinite(p->throttle_offset) ||
        !isfinite(p->gravity) || !isfinite(p->brake[0]) || !isfinite(p->brake[1]) ||
        !isfinite(p->compensation_scale)) return 0;
    for (unsigned i = 0; i < 4; ++i) if (p->pivot_sides[i] > 1 || p->collision_corner[i] >= 4) return 0;
    unsigned corners = 0;
    for (unsigned i = 0; i < 4; ++i) corners |= 1u<<p->collision_corner[i];
    if (corners != 15) return 0;
    for (unsigned row = 0; row < 4; ++row) {
        unsigned mask = 0;
        for (unsigned i = 0; i < 14; ++i) {
            if (p->collision_schedule[row][i] >= 14) return 0;
            mask |= 1u<<p->collision_schedule[row][i];
        }
        if (mask != 16383) return 0;
    }
    for (unsigned body = 0; body < 2; ++body) for (unsigned side = 0; side < 4; ++side) {
        const uint32_t mask = p->collision_flags[body][side];
        if (!mask || (mask&(mask-1))) return 0;
    }
    if (!isfinite(p->axle.pitch) || !isfinite(p->axle.transfer)) return 0;
    if (!isfinite(p->acceleration_scale) || !isfinite(p->slip_threshold) || !isfinite(p->rpm_scale)) return 0;
    if (!isfinite(p->turn_scale) || !isfinite(p->tire_speed_scale) ||
        !isfinite(p->tire_response[0]) || !isfinite(p->tire_response[1])) return 0;
    for (unsigned a = 0; a < 2; ++a)
        if (!isfinite(p->turn_geometry[a]) || !isfinite(p->inertial_scale[a]) ||
            !isfinite(p->axle.bank[a]) || !isfinite(p->axle.scale[a]) ||
            !isfinite(p->axle.minimum[a]) || !isfinite(p->axle.maximum[a])) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(p->gear[i]) || !isfinite(p->slip_gear[i]) || !isfinite(p->gear_shift_scale[i]) ||
            p->gear_down[i] > p->gear_up[i] || p->gear_rpm_delta[i][0] > p->gear_rpm_delta[i][1]) return 0;
    for (unsigned i = 0; i < 8; ++i)
        if (p->gear_selector[i] < -1 || p->gear_selector[i] > 3) return 0;
    for (unsigned i = 0; i < 5; ++i) if (!isfinite(p->resistance[i])) return 0;
    for (unsigned i = 0; i < 32; ++i) if (!isfinite(p->spin_scale[i])) return 0;
    if (!isfinite(p->spin_minimum)) return 0;
    for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a)
        if (!isfinite(p->wheel_local[v][a])) return 0;
    if (!isfinite(p->guided_adjust_limit) || p->guided_adjust_limit < 0) return 0;
    for (unsigned i = 0; i < 256; ++i) if (!isfinite(p->guided_player_target[i])) return 0;
    for (unsigned i = 0; i < 127; ++i) if (!isfinite(p->guided_targets[i])) return 0;
    for (unsigned c = 0; c < 2; ++c) for (unsigned i = 0; i < 67; ++i)
        if (!isfinite(p->curve[c][i])) return 0;
    for (unsigned c = 0; c < 3; ++c) for (unsigned i = 0; i < 512; ++i)
        if (!isfinite(p->drag[c][i])) return 0;
    for (unsigned i = 0; i < 16384; ++i) {
        float value; nm_memcpy(&value,&p->sine[i],4);
        if (!isfinite(value) || value < 0 || value > 1) return 0;
        nm_memcpy(&value,&p->reciprocal[i],4); if (!isfinite(value)) return 0;
        nm_memcpy(&value,&p->inverse_sqrt[i],4); if (!isfinite(value)) return 0;
    }
    return 1;
}

nm_drive_params_t *nm_drive_load(const char *path) {
    const uint32_t endian = 1;
    if (!path || *(const uint8_t *)&endian != 1) return NULL;
    FILE *file = fopen(path,"rb");
    if (!file) return NULL;
    uint32_t header[4];
    nm_drive_params_t *p = NULL;
    int ok = !setvbuf(file,NULL,_IOFBF,64*1024) && fread(header,1,16,file) == 16 &&
        header[0] == 0x52444e44 && header[1] == 16+sizeof(*p) && !header[2] && !header[3];
    if (ok) {
        p = nm_alloc(sizeof(*p));
        ok = p && fread(p,1,sizeof(*p),file) == sizeof(*p) && fgetc(file) == EOF &&
            !ferror(file) && nm_drive_params_valid(p);
    }
    if (fclose(file)) ok = 0;
    if (!ok) { free(p); return NULL; }
    return p;
}
