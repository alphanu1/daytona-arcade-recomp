#ifndef DAYTONA_NATIVE_SKY_CASES_H
#define DAYTONA_NATIVE_SKY_CASES_H
#include "../include/pose.h"
#define NM_SKY_TEST_HASH 0x05d8948du
/* Source-oracle fixtures shared by host and SH4 startup, never gameplay data. */
static inline void nm_sky_test_case(unsigned i,nm_player_t *car) {
    memset(car,0,sizeof(*car));
    car->boundary.ground.pitch=nm_yaw_angle(i*8192u);
    car->boundary.ground.position[1]=(float)((int)i-4)*61.25f;
}
static inline uint32_t nm_sky_test_hash(const nm_drive_params_t *p) {
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<8;++i) {
        nm_player_t car;uint16_t scroll;
        nm_sky_test_case(i,&car);
        if(!nm_follow_sky_scroll(p,&car,&scroll)) return 0;
        hash=(hash^scroll)*16777619u;
    }
    return hash;
}
#endif
