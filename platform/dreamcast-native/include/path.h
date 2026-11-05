#ifndef DAYTONA_NATIVE_PATH_H
#define DAYTONA_NATIVE_PATH_H
#include "force_math.h"

typedef struct {
    float xyz[3], speed;
    int16_t pitch, yaw, roll, tire, lateral;
    uint16_t road;
} nm_path_row_t;
typedef struct {
    uint32_t slot, route, count, records;
    nm_path_row_t *rows;
} nm_path_pack_t;
typedef struct { uint16_t offset, matched; uint32_t road; } nm_path_cursor_t;
#ifdef __cplusplus
extern "C" {
#endif
int nm_path_load(nm_path_pack_t *p, const char *file);
void nm_path_unload(nm_path_pack_t *p);
#ifdef __cplusplus
}
#endif

/* Complete10bcc/10c50. Source stores BYTE offsets, not row indices. matched
 * reports the original row used by the caller, even when offset advances.
 * Reverse's offset0 uses the authored terminal row, not count-1.
 * Neither routine checks the offset: 10bcc reads the row at base+offset. The
 * finish entry da58 sets (profile+5)*28 and db68 (profile+3)*28, so a race
 * finished on profile 194/195 (the last lap ending backwards, or the line crossed
 * at the course's last profiles: the fuzz test's finishes reach it) reads rows
 * count..count+4: the eight authored guard records after the course's rows
 * (tools/prepare_path.py) hold them. Only an offset past those rows rejects. */
static inline int nm_path_cursor_step(const nm_path_pack_t *p, const nm_drive_params_t *math,
                                      float x, float z, int reverse, nm_path_cursor_t *s) {
    if (!p || !p->rows || !math || !s || !p->count || p->count > 1162 ||
        p->records != p->count+8 || s->offset%28 || s->offset/28 >= p->records ||
        !isfinite(x) || !isfinite(z)) return 0;
    const unsigned current = s->offset/28;
    const unsigned test = reverse ? (current == 0 ? p->count : current-1) : current;
    const nm_path_row_t *row = p->rows+test;
    const float dx = reverse ? row->xyz[0]-x : x-row->xyz[0];
    const float dz = reverse ? z-row->xyz[2] : row->xyz[2]-z;
    if (!isfinite(dx) || !isfinite(dz)) return 0;
    const int32_t angle = nm_force_angle(math,dz,dx);
    if (angle == INT32_MIN) return 0;
    const uint32_t side = (uint32_t)(angle-(int32_t)row->yaw*4+16384);
    nm_path_cursor_t next = *s;
    next.matched = (uint16_t)current;
    if (side&32768) {
        unsigned offset = reverse ? test*28 : s->offset+28u;
        if (!reverse && offset >= p->count*28) offset -= p->count*28;
        next.offset = (uint16_t)offset;
    }
    next.road = (s->road&0xffff0000u) | p->rows[current].road;
    *s = next;
    return 1;
}
#endif
