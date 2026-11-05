#ifndef DAYTONA_NATIVE_GUIDED_CHECK_H
#define DAYTONA_NATIVE_GUIDED_CHECK_H
#include "../include/guided.h"
#include <stddef.h>
#define NM_GUIDED_CHECK_CASES 8192u
static inline uint32_t nm_guided_expected_hash(unsigned slot, unsigned route) {
    static const uint32_t hashes[3][8] = {
        {0x4f7918d2,0xc7180bb5,0x1dd29256,0xc7180bb5,0x4f7918d2,0x95811c0b,0x4f7918d2,0x18886748},
        {0xa0648aa4,0xdea91a2b,0x983631df,0xdea91a2b,0xed92d9f7,0xfb2293f9,0x67d8238f,0xfb2293f9},
        {0xbd2df966,0xd171642e,0x690a46e7,0x89504cf7,0x534f4a92,0x424696ff,0xe966bab7,0x0d56179a}
    };
    return slot < 3 && route < 8 ? hashes[slot][route] : 0;
}
static inline void nm_guided_test_input(const nm_path_pack_t *path, unsigned i, nm_road_profile_t *profile,
                                        nm_guided_player_input_t *in, nm_guided_player_state_t *s) {
    memset(profile,0,sizeof(*profile)); memset(in,0,sizeof(*in)); memset(s,0,sizeof(*s));
    const unsigned current = i%(path->count+1);
    const nm_path_row_t *row = path->rows+current+1;
    memcpy(s->position,row->xyz,12);
    s->position[0] += (float)((int)(i%7)-3); s->position[1] += (float)((int)(i%11)-5);
    s->position[2] += (float)((int)(i%13)-6);
    profile->first_lane = 1; profile->last_lane = 6;
    for (unsigned lane = 0; lane < 8; ++lane) {
        for (unsigned a = 0; a < 3; ++a) profile->lanes[lane].xyz[a] = row->xyz[a]+(float)((int)lane-4)*0.25f;
        for (unsigned a = 0; a < 3; ++a) profile->lanes[lane].angles[a] = nm_yaw_angle(i*31+lane*113+a*17);
    }
    s->cursor.offset = (uint16_t)(current*28); s->cursor.road = 0xabcd1234;
    s->lateral.current = (int16_t)(i%2048); s->lateral.target = (int16_t)((i*73)%2048);
    const int16_t controls[] = {-32768,-1,0,1,224,225,32767,2};
    s->lateral.control = controls[(i/256)%8]; s->lateral.step = (int16_t)((int)(i%35)-17);
    s->heading = nm_yaw_angle(i*97); s->tire_angle = nm_yaw_angle(i*227);
    s->pitch = nm_yaw_angle(i*5); s->roll = nm_yaw_angle(i*11); s->road_yaw = nm_yaw_angle(i*59);
    s->profile_updates = (uint8_t)i;
    s->adjustment = i&1 ? 0.25f : -0.25f;
    s->adjustment_target = i%3 == 0 ? s->adjustment : i%3 == 1 ? -1.0f : 1.0f;
    const float speeds[] = {0,0.125f,1,8,-0.0f,-1,0.01f,2};
    in->speed = speeds[(i/32)%8]; in->scale = 0.5f;
    in->speed_index = (int32_t)(i%1024)-128;
    in->flags = i&8 ? 0x10000000u : 0;
    in->path_flags = i&16 ? 1024 : 0; in->surface = i&64 ? 33 : 2;
    in->condition = (int8_t)((int)((i/128)%3)-1); in->mode = (uint8_t)i;
}
static inline uint32_t nm_guided_hash_state(uint32_t hash, const nm_guided_player_state_t *s) {
    const unsigned char *bytes = (const unsigned char *)s;
    for (unsigned i = 0; i <= offsetof(nm_guided_player_state_t,profile_updates); ++i) hash = (hash^bytes[i])*16777619u;
    return hash;
}
static inline uint32_t nm_guided_test_hash(const nm_path_pack_t *path, const nm_drive_params_t *math) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < NM_GUIDED_CHECK_CASES; ++i) {
        nm_road_profile_t profile; nm_guided_player_input_t in; nm_guided_player_state_t s;
        nm_guided_test_input(path,i,&profile,&in,&s);
        const unsigned accepted = nm_guided_player_step(path,math,&profile,&in,&s);
        hash = (hash^accepted)*16777619u;
        hash = nm_guided_hash_state(hash,&s);
    }
    return hash;
}
#endif
