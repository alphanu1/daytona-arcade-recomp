#ifndef DAYTONA_NATIVE_RACE_RECORDS_H
#define DAYTONA_NATIVE_RACE_RECORDS_H
#include <stdint.h>

/* Phase22,3520..3554: sample the current view BEFORE movement/camera updates.
 * Rolling-start bit0 suppresses counting, not the other flag bits. */
static inline void nm_race_view_tick(uint32_t counts[4],uint32_t start_flags,uint8_t view) {
    if(!(start_flags&1u)) ++counts[view&3u];
}

/* 2268dc..226a58. A course owns24 slots, including the candidate at23.
 * Rank labels stay attached to slots; ONLY the five payload words move.
 * Caller selects the course and owns persistence/name entry separately. */
typedef struct {
    uint32_t rank, time, view, flags, time_copy, name;
} nm_race_record_t;

/* Offline-expanded defaults from2265d0..226634. Reset leaves view untouched;
 * cold-start memory policy belongs to the owner, not this reset operation. */
typedef struct { uint32_t rank, time, flags, name; } nm_race_record_seed_t;
static inline void nm_race_records_reset(nm_race_record_t table[24],
        const nm_race_record_seed_t seed[24]) {
    for(unsigned i=0;i<24;++i) {
        table[i].rank=seed[i].rank;
        table[i].time=table[i].time_copy=seed[i].time;
        table[i].flags=seed[i].flags;table[i].name=seed[i].name;
    }
}

/* Returns0 invalid,1 ineligible/slower (table and selected unchanged),2 inserted.
 * The notification is cleared even when no record is inserted, like501398.
 * Equal times insert BEFORE the old equal entry. View uses the GREATEST
 * of501700..50170c's camera counters (3538 increments501710&3's counter),
 * retaining the earliest view on a tie.
 * Pure integer work: no allocation, guest pointers, sound or storage I/O. */
static inline int nm_race_record_update(nm_race_record_t table[24],uint32_t eligible,
        uint32_t time,const uint32_t view_ticks[4],uint32_t mode,uint32_t alternate,
        unsigned *selected,uint32_t *notification) {
    if(!table || !view_ticks || !selected || !notification) return 0;
    *notification=0;
    if(!(eligible&2u) || time>table[23].time) return 1;
    unsigned view=0;
    for(unsigned i=1;i<4;++i) if(view_ticks[i]>view_ticks[view]) view=i;
    table[23].time=time;table[23].view=view;
    table[23].flags=mode|(alternate<<1);
    table[23].time_copy=time;table[23].name=0x00202020;
    unsigned at=23;
    while(at && table[at].time<=table[at-1].time) {
        const nm_race_record_t old=table[at-1];
        const uint32_t rank=table[at].rank;
        table[at-1]=table[at];table[at-1].rank=old.rank;
        table[at]=old;table[at].rank=rank;
        --at;
    }
    *selected=at;
    *notification=table[at].rank<=5;
    return 2;
}
#endif
