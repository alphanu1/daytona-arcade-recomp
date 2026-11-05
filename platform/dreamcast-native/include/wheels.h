#ifndef DAYTONA_NATIVE_WHEELS_H
#define DAYTONA_NATIVE_WHEELS_H
#include "nm_mem.h"
#include "contact.h"
#include "yaw.h"

/* Body-ground state, not a replacement for the complete vehicle state.
 * Heights/surfaces use the same order as the four authored local wheel points. */
typedef struct {
    float position[3], world[4][3], height[4], road_frame[6];
    uint32_t road, surface[4], selected_surface;
    uint16_t mask[4], combined_mask;
    int16_t yaw, roll, pitch, road_heading;
} nm_wheels_state_t;

/* 11bd4: translation, yaw, roll; pitch is deliberately NOT in this transform.
 * Retain separate operations, including zero terms, until SH4 math is verified. */
static inline int nm_wheels_transform(const nm_drive_params_t *p, const float position[3],
                                      int16_t yaw, int16_t roll, float out[4][3]) {
    if (!p || !position || !out) return 0;
    for (unsigned a = 0; a < 3; ++a) if (!isfinite(position[a])) return 0;
    const float cy = nm_drive_sine(p,(uint16_t)((int)yaw+16384));
    const float sy = nm_drive_sine(p,(uint16_t)yaw);
    const float cr = nm_drive_sine(p,(uint16_t)((int)roll+16384));
    const float sr = nm_drive_sine(p,(uint16_t)roll);
    const float identity[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    float matrix[3][3], origin[3], result[4][3];
    for (unsigned a = 0; a < 3; ++a) {
        const float first = cy*identity[0][a] + sy*identity[2][a];
        matrix[2][a] = cy*identity[2][a] - sy*identity[0][a];
        matrix[0][a] = cr*first - sr*identity[1][a];
        matrix[1][a] = cr*identity[1][a] + sr*first;
        origin[a] = 0.0f + position[0]*identity[0][a];
        origin[a] += position[1]*identity[1][a]; origin[a] += position[2]*identity[2][a];
    }
    for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a) {
        float value = origin[a] + matrix[0][a]*p->wheel_local[v][0];
        value += matrix[1][a]*p->wheel_local[v][1]; value += matrix[2][a]*p->wheel_local[v][2];
        if (!isfinite(value)) return 0;
        result[v][a] = value;
    }
    nm_memcpy(out,result,sizeof(result));
    return 1;
}

/* Shared command2a/3030 road frame, also used by the center-only grid reset. */
static inline int nm_wheels_road_frame(const nm_contact_pack_t *pack, const nm_drive_params_t *p,
                                      nm_wheels_state_t *state) {
    if (!pack || !p || !state || state->road >= pack->quad_count) return 0;
    nm_wheels_state_t next; NM_COPY(&next,state);
    const nm_contact_quad_t *q = pack->quads+next.road;
    const float s = nm_drive_sine(p,(uint16_t)q->heading);
    const float c = nm_drive_sine(p,(uint16_t)(q->heading+16384));
    const float ns = nm_drive_sine(p,(uint16_t)(q->heading+32768));
    for (unsigned v = 0; v < 2; ++v) {
        next.road_frame[v] = s*(q->xyz[v][2]-next.position[2]) + c*(q->xyz[v][0]-next.position[0]);
        next.road_frame[3+v] = ns*(q->xyz[v+1][0]-next.position[0]) + c*(q->xyz[v+1][2]-next.position[2]);
    }
    next.road_frame[2] = ((next.road_frame[0]+next.road_frame[1])*-1.0f)*0.5f;
    next.road_frame[5] = ((next.road_frame[3]+next.road_frame[4])*-1.0f)*0.5f;
    for (unsigned i = 0; i < 6; ++i) if (!isfinite(next.road_frame[i])) return 0;
    next.road_heading = nm_yaw_angle(q->heading);
    NM_COPY(state,&next);
    return 1;
}

/* 116d0: four wheel probes, body attitude/height, road frame and direction.
 * road_frame follows command2a output order: 1c8,1c4,1cc,1d0,1d4,1d8.
 * No board state, matrix stack or FIFO; publish only a complete valid step. */
static inline int nm_wheels_ground(const nm_contact_pack_t *pack, const nm_drive_params_t *p,
                                   nm_wheels_state_t *state) {
    if (!pack || !p || !state) return 0;
    nm_wheels_state_t next; NM_COPY(&next,state);
    if (!nm_wheels_transform(p,state->position,state->yaw,state->roll,next.world)) return 0;
    uint32_t front_road[2];
    for (unsigned v = 0; v < 4; ++v) {
        nm_contact_result_t contact = {state->height[v],next.road,state->surface[v],0};
        if (!nm_contact_query(pack,p,next.world[v][0],next.world[v][2],&contact)) return 0;
        next.height[v] = contact.height; next.surface[v] = contact.surface; next.mask[v] = (uint16_t)contact.mask;
        if (v < 2) { next.road = contact.road; front_road[v] = contact.road; }
    }
    unsigned a = ((next.mask[0]|next.mask[1])&33) ? 2 : 0;
    const float width = p->wheel_local[a+1][0]-p->wheel_local[a][0];
    const float bank_height = next.height[a]-next.height[a+1];
    const int32_t bank = nm_force_angle(p,width,bank_height);
    a = ((next.mask[1]|next.mask[3])&33) ? 0 : 1;
    const float length = p->wheel_local[a][2]-p->wheel_local[a+2][2];
    const float pitch_height = next.height[a]-next.height[a+2];
    const int32_t pitch = nm_force_angle(p,length,pitch_height);
    if (bank == INT32_MIN || pitch == INT32_MIN) return 0;
    const int db = bank-state->roll, dp = pitch-state->pitch;
    next.roll = nm_yaw_angle((uint32_t)(state->roll + (db < 0 ? -((-db+3)/4) : db/4)));
    next.pitch = nm_yaw_angle((uint32_t)(state->pitch + (dp < 0 ? -((-dp+3)/4) : dp/4)));
    next.position[1] = ((next.height[0]+next.height[1])+next.height[2])+next.height[3];
    next.position[1] *= 0.25f;
    if (!isfinite(next.position[1])) return 0;
    int32_t s0, s1; nm_memcpy(&s0,&next.surface[0],4); nm_memcpy(&s1,&next.surface[1],4);
    const unsigned front = s0 < s1 ? 0 : 1;
    next.selected_surface = next.surface[front]; next.road = front_road[front];
    next.combined_mask = next.mask[0]|next.mask[1]|next.mask[2]|next.mask[3];
    if (!nm_wheels_road_frame(pack,p,&next)) return 0;
    NM_COPY(state,&next);
    return 1;
}
#endif
