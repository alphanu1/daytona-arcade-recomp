#ifndef DAYTONA_NATIVE_HUD_H
#define DAYTONA_NATIVE_HUD_H
#include <stdint.h>
#include <string.h>
#include "force_math.h" /* TGP commands 3 (divide) and 41 (circle test) of the CONDITION panel. */
#include "nm_mem.h"

/* In-race HUD value rules of the original (daytona93, course 0, automatic).
 * Pure integer/float state, no renderer, assets or allocation: the same code
 * runs on the host against the reference (tests/check_hud.c).
 *
 * The original draws the HUD into tile layer 0 and never clears it between
 * updates, so every cell below PERSISTS until its owner rewrites it. Owners:
 *   3414, 35f4..37d8   remaining time (phase handler, 3x4-tile digits at 28,1)
 *   a6b4..a7bc         one-time labels: LAP, n/total, POSITION, /count, frames
 *   a96c..ab28         lap digit, lap numbers and lap times (rows 4.., 2 each)
 *   aef4..af7c         position and its suffix (table 231efc)
 *   afc0..b0b8         tachometer needle (polygon model 288b6a4)
 *   b0c0..b1a0         gear indicator (manual only): lever animation, then "(n)" and "SHIFT"
 *   b1a4..b25c         speed
 *   a818..a968         wrong-way message "逆走" (unit 1 on this board, 53e59a bit0)
 *   ae98..aef0         "TIME EXTENSION!!" after a time award (phase handler 3558)
 *   ab2c..ab74         "PIT WORK!" while the pit's stop count 501a84 is positive (blinks while the car stands)
 *   ad34..ae94         the lap's time after the lap line, blinking (nm_hud_overlay)
 *   ab78..ad30, d438..d5e0  the rolling start's overlay on course 0 (nm_hud_overlay)
 *   1299c..12c70       course map and its two markers (polygons)
 *   12c98..13208       CONDITION panel: the cars near the player (polygons, with the map)
 * The HUD object a7c0 runs only while flags 501090 select phase 20 or 22; the
 * map object while they select 20, 22, 24 or 26.
 *
 * Order inside one original frame: the phase handler (timer digits, clock),
 * then the objects; the HUD cells show the state at the end of the tick. The
 * timer digits are the exception: 134f8 adds a time extension after the
 * handler drew them, so they show it one tick later. The polygons (needle,
 * map markers, the CONDITION panel) reach the screen one frame after the
 * tiles, with the rest of the 3D scene (measured: they fit the reference to
 * 0.01 px only against the previous tick's state). The native renderer draws the 3D scene of the same
 * tick, so nm_hud_tick uses this tick's state for everything; call it once
 * per gameplay tick, after the tick.
 *
 * Not here yet: the 501a80
 * time-attack variants (12f20: record times in place of the CONDITION
 * panel's cars) and the mph branch of b1a4 (53e59a == 0). */

enum { NM_HUD_BLANK=255, NM_HUD_MINUTE=10, NM_HUD_SECOND=11 };
/* 231efc glyphs after the rank: 11 ST, 12 ND, 13 RD, 14 TH. */

typedef struct {
    uint8_t active_phase, phase; /* Latched flags 501090 as a phase, next phase 5010a4. */
    int32_t remaining;           /* 5010d0 after this tick's handler. */
    uint32_t clock;              /* 5010cc after this tick's handler. */
    uint32_t counter;            /* 5010b0 of this tick; a818 reads bit3. */
    uint8_t reverse;             /* HUD object flags bit28 after this tick's a818 (race scene wrong_way). */
    /* Events of this tick's player update: the lap line (13410: lap message ticks 5010dc = 128) and a
     * time award (13534: 5010d4 = 128, the native award.display; award.counter counts them). */
    uint8_t lap_line, award;
    uint32_t hold;               /* 501a84 after this tick (the pit's stop count, race_field.h timer_hold). */
    int16_t stage;               /* 501850 after this tick (-1 off course 0): the rolling start's sequence. */
    uint16_t accel;              /* 501054 of this tick: d480 chooses the hint by it. */
    uint16_t lap_last;           /* +66 after this tick: the ticks of the lap just ended. */
    uint32_t finish_flags;       /* 5010d8 after this tick: bit1 (the last lap's line) hides the lap message. */
    uint32_t held;               /* 53e120 of this tick: the chord of 129d0 (set by the caller after nm_hud_observe). */
} nm_hud_race_t;

/* CONDITION panel inputs: a car's position and its drawn heading +58 + +d6. */
enum { NM_HUD_RADAR_CARS=43 };
typedef struct { float x, z; uint16_t angle; uint8_t id; } nm_hud_radar_car_t;

typedef struct {
    uint32_t start_flags;        /* 501854: bit0 while the rolling start runs. */
    uint8_t completed, total_laps, rank, count; /* +6d, 5010ec, +6a, 501080. */
    uint16_t best_ticks, start_ticks;           /* +64, +68. */
    int32_t speed_index, filtered_rpm;          /* +18, +154. */
    float x, z; uint16_t yaw;                   /* +1c, +24, +58. */
    /* 12b34: the opponent whose rank is 0, or 1 when the player leads. */
    float leader_x, leader_z; uint16_t leader_yaw; uint8_t leader_valid, leader_second;
    uint32_t lap_times[8];       /* 5015a0[0..7] (50159c[1..8]), written by 13430. */
    /* 12c98..13208: the player's +58 + +d6 and every opponent of the list 501224, in list order. */
    uint16_t angle;
    uint8_t others, award_counter; /* award_counter: the scene's award.counter seen last (hud_scene.h). */
    nm_hud_radar_car_t other[NM_HUD_RADAR_CARS];
    /* TGP tables (commands 3 and 41) for the panel's radius and circle test, exact to the original;
     * NULL: IEEE single precision (host checks without an asset set). Not touched by reset or observe. */
    const nm_drive_params_t *math;
    /* b0c0 (last: the fields before keep their offsets). gear: +148, 0..3 for gears 1..4; manual: 501464 of
     * this race, set by the race owner, not touched by reset or observe. */
    uint32_t gear;
    uint8_t manual;
} nm_hud_cars_t;

typedef struct { uint8_t number[2], time[8], best; } nm_hud_lap_row_t;

typedef struct {
    uint32_t clock;
    int32_t remaining;           /* End of the previous tick. */
    int8_t blink_ticks;          /* 501880 */
    uint8_t blink_phase;         /* 501884: also selects the beep 231e60[]. */
    uint8_t beep;                /* One-tick cue for a future sound owner: 1 + blink_phase. */
    /* Drawn cells. */
    uint8_t timer[2];
    uint8_t lap_current, lap_total, rows;
    nm_hud_lap_row_t lap[8];
    uint8_t rank[2], suffix, rank_drawn, count[2];
    uint8_t speed[3], speed_drawn;
    uint8_t reverse;             /* a818's block 236830[1] (2876914, 16x6 tiles at 25,21) drawn. */
    uint8_t extension;           /* ae98's block 2872c42 drawn: bit0 at 1000626 (row 12), bit1 at 1000826 (row 16). */
    int32_t extension_ticks;     /* 5010d4 */
    int32_t lap_ticks;           /* 5010dc: the lap time message runs; ae98 then uses row 12. */
    uint8_t pit;                 /* ab2c's block 2876860 ("PIT WORK!", 21x4 tiles at 20,20) drawn. */
    /* Polygons, drawn only on the ticks their object runs. */
    uint8_t needle, map;
    uint8_t course;              /* 501460 of the race (set by the caller after nm_hud_reset): the map art's course. */
    uint16_t needle_angle;
    float player_x, player_z, leader_x, leader_z;
    uint16_t player_yaw, leader_yaw;
    uint8_t leader, leader_second;
    /* CONDITION panel, drawn with the map: zoom index +53 and smoothed zoom +34 of the map task, the
     * player's heading, and the opponents inside the circle in list order (offsets from the player). */
    uint8_t radar_index, radar_count;
    float zoom;
    uint16_t radar_angle;
    struct { float dx, dz; uint16_t angle; uint8_t id; } radar[NM_HUD_RADAR_CARS];
    /* b0c0 (nm_hud_gear; last: the fields before keep their offsets): the lever block 23263c[gear_lever] (3x3 tiles at 34,9) or NM_HUD_BLANK, and the
     * digit gear_digit (1..4, 0 none) between the brackets 2872a0a/2872a1a with the label 28729de below. */
    uint8_t gear_lever, gear_digit;
    uint8_t gear_last, gear_from; /* +14c of the player (the gear seen last), and the gear the lever leaves. */
    int8_t gear_ticks;            /* The HUD object's +6: 31 ticks of lever animation after a change. */
    /* The rolling start's overlay (nm_hud_overlay; last: the fields before keep their offsets). hint: 0 none, 1 "ACCEL ON!",
     * 2 "AUTO DRIVE"; digit: 1..3, 0 none; scroll: 4 * 5010b0 & 1023, how far the banner has run left. */
    uint8_t roll_banner, roll_hint, roll_digit, roll_go;
    uint8_t lap_label;            /* ad34: 0 not drawn, 1 "LAP TIME", 2 "BEST LAP" */
    uint8_t overlay;              /* Something of nm_hud_overlay is drawn. */
    uint8_t map_index;            /* The map task's +52 (129d0): its size, 1 at the start. */
    uint16_t roll_scroll;
    uint16_t lap_time;            /* ad34: the ticks it shows. */
    float map_scale;              /* The map task's +38, 0 until its first tick. */
    uint8_t map_apart;            /* The map goes through nm_hud_map_apart: another size, another course's, the "2" marker. */
} nm_hud_t;

/* 234dd8: zoom by index (START + VR2/VR3/VR4 select 0, 1, 2; 1 at the map object's start, 12898..128ac).
 * tools/prepare_hud.py checks the table against the ROM. */
static const uint32_t nm_hud_radar_zooms[3]={0x3c03126fu,0x3ba3d70au,0x3b23d70au}; /* 0.008, 0.005, 0.0025 */
/* 234de8[course*4 + index]: the map's scale by the course slot and the index +52 (12a4c); the same check. */
static const uint32_t nm_hud_map_scales[3][3]={{0x339652e8u,0x3356bf95u,0x331652e8u},{0x32e7eda1u,0x32bcfa83u,0x32920765u},
    {0x332bcc77u,0x3300d959u,0x32abcc77u}};

/* 187c0, 18a08, 1891c, 18b44: right-aligned decimal, blank cells left of it. */
static inline void nm_hud_number(uint32_t value, uint8_t *cells, unsigned count) {
    unsigned i=count;
    while(i) {
        cells[--i]=(uint8_t)(value%10u);value/=10u;
        if(!value) break;
    }
    while(i) cells[--i]=NM_HUD_BLANK;
}

/* 186cc: ticks*100>>6 hundredths as M'SS"HH, tens of minutes only when nonzero. */
static inline void nm_hud_time(uint32_t ticks, uint8_t cells[8]) {
    uint32_t v=(uint32_t)(ticks*100u)>>6;
    cells[7]=(uint8_t)(v%10u);v/=10u;
    cells[6]=(uint8_t)(v%10u);v/=10u;
    cells[5]=NM_HUD_SECOND;
    cells[4]=(uint8_t)(v%10u);v/=10u;
    cells[3]=(uint8_t)(v%6u);v/=6u;
    cells[2]=NM_HUD_MINUTE;
    cells[1]=(uint8_t)(v%10u);v/=10u;
    cells[0]=v ? (uint8_t)(v%10u) : NM_HUD_BLANK;
}

/* af44..af78 for the displayed rank (1-based). */
static inline uint8_t nm_hud_suffix(unsigned place) {
    static const uint8_t glyph[10]={14,11,12,13,14,14,14,14,14,14};
    return glyph[place/10u==1 ? 0 : place%10u];
}

/* cvtri in the default mode: round to nearest, ties to even. */
static inline int32_t nm_hud_round(float value) {
    const float magnitude=value<0 ? -value : value;
    int32_t whole=(int32_t)magnitude;
    const float fraction=magnitude-(float)whole;
    if(fraction>0.5f || (fraction==0.5f && (whole&1))) ++whole;
    return value<0 ? -whole : whole;
}

/* afc0..b09c: angle of the needle model, 65536 per turn. */
static inline uint16_t nm_hud_needle_angle(int32_t filtered_rpm) {
    if(filtered_rpm>0x2134) filtered_rpm=0x2134;
    return (uint16_t)(0x6a00+nm_hud_round((float)filtered_rpm*1.18f));
}

/* b0c0: the gear indicator, manual transmission only. On a change of the gear +148 against +14c the lever
 * animates for 31 ticks: frame ticks>>1 of the row 23253a[old][new] (16 bytes, read from frame 15 down),
 * each a block of 23263c: 0..3 the lever in gear 1..4 (1 and 2 left, 3 and 4 right of the H), 4 the middle of
 * the gate, 5 and 6 the neutral of the left and the right column, 7 a blank block (the last tick erases the
 * lever). Then the digit gear+1 between brackets, colour 0x84 (501334), and the label below. The row as a
 * rule: the old gear, the neutral of its column, through the middle and the other column's neutral when the
 * columns differ, then the new gear; tools/prepare_hud.py checks the rule against all 16 rows of the ROM.
 * The cells persist while a7c0 does not run (the tile layer is never cleared).
 * Build options (Dreamcast additions, off: the original):
 *   NM_HUD_GEAR_GATE  the lever stays in place of "(n) SHIFT": the H-gate with the current gear, always;
 *   NM_HUD_GEAR_DIGIT with the gate: 1 the digit gear+1 of "(n)" as well, to the right of the gate, from the
 *                     change on (the lever still animates; render/ui/hud_draw.h nm_hud_gear_place); 0 the
 *                     gate alone;
 *   NM_HUD_GEAR_AUTO  the indicator runs in an automatic race too (on the automatic gear). */
#ifndef NM_HUD_GEAR_GATE
#define NM_HUD_GEAR_GATE 0
#endif
#ifndef NM_HUD_GEAR_DIGIT
#define NM_HUD_GEAR_DIGIT 0
#endif
#if NM_HUD_GEAR_DIGIT && !NM_HUD_GEAR_GATE
#error "NM_HUD_GEAR_DIGIT needs NM_HUD_GEAR_GATE"
#endif
#ifndef NM_HUD_GEAR_AUTO
#define NM_HUD_GEAR_AUTO 0
#endif
enum { NM_HUD_GEAR_TICKS=31, NM_HUD_GEAR_MIDDLE=4, NM_HUD_GEAR_BLANK_FRAME=7 };
static inline uint8_t nm_hud_gear_frame(unsigned from, unsigned to, unsigned index) {
    if(!index) return NM_HUD_GEAR_BLANK_FRAME;
    const unsigned step=15u-index,neutral_from=5u+(from>>1),neutral_to=5u+(to>>1);
    if(from==to || step>=4u) return (uint8_t)to;
    const uint8_t path[4]={(uint8_t)from,(uint8_t)neutral_from,NM_HUD_GEAR_MIDDLE,(uint8_t)neutral_to};
    const unsigned length=(from>>1)==(to>>1) ? 2u : 4u;
    return step<length ? path[step] : (uint8_t)to;
}
static __attribute__((noinline, unused)) void nm_hud_gear(nm_hud_t *hud, const nm_hud_cars_t *c) {
    const unsigned gear=c->gear&3u;
    if(gear!=hud->gear_last) { /* b0d4..b0fc */
        hud->gear_from=hud->gear_last;hud->gear_last=(uint8_t)gear;hud->gear_ticks=NM_HUD_GEAR_TICKS;
    }
    if(hud->gear_ticks>0) { /* b100..b140: the label erased, the lever drawn over the digit's cells */
        hud->gear_lever=nm_hud_gear_frame(hud->gear_from,gear,(unsigned)hud->gear_ticks>>1);
        /* The blank block; the gate keeps the lever in the gear. */
        if(hud->gear_lever==NM_HUD_GEAR_BLANK_FRAME) hud->gear_lever=NM_HUD_GEAR_GATE ? (uint8_t)gear : NM_HUD_BLANK;
        --hud->gear_ticks;hud->gear_digit=NM_HUD_GEAR_DIGIT ? (uint8_t)(gear+1u) : 0;
    } else if(NM_HUD_GEAR_GATE) {
        hud->gear_lever=(uint8_t)gear;hud->gear_digit=NM_HUD_GEAR_DIGIT ? (uint8_t)(gear+1u) : 0;
    } else { /* b148..b19c */
        hud->gear_lever=NM_HUD_BLANK;hud->gear_digit=(uint8_t)(gear+1u);
    }
}

/* 6-bit seconds of the remaining time, 231e48: ticks between timer blinks. */
static inline int8_t nm_hud_blink_period(unsigned seconds) {
    static const int8_t period[8]={4,4,5,6,8,12,16,20};
    return period[seconds&7u];
}

/* Scene entry: a6b4 labels and 3414 timer. `cars` is the state after the reset. */
static inline void nm_hud_reset(nm_hud_t *hud, const nm_hud_race_t *race, const nm_hud_cars_t *cars) {
    nm_memset(hud,0,sizeof(*hud));
    hud->clock=race->clock;hud->remaining=race->remaining;
    nm_hud_number((uint32_t)(race->remaining>0 ? race->remaining : 0)>>6,hud->timer,2);
    hud->lap_current=(uint8_t)(cars->completed+1u);hud->lap_total=cars->total_laps;
    nm_hud_number(cars->count,hud->count,2);
    nm_memset(hud->rank,NM_HUD_BLANK,sizeof(hud->rank));
    nm_memset(hud->speed,NM_HUD_BLANK,sizeof(hud->speed));
    /* ponytail: +14c is the player's and only b0c0 writes it; the original keeps it across races (a race
     * that starts in another gear than the last one ended in animates once). Here: the gear at entry. */
    hud->gear_lever=NM_HUD_BLANK;hud->gear_last=hud->gear_from=(uint8_t)(cars->gear&3u);
    /* 12898..128ac: zoom index 1 and its zoom; 340c: 5010d4 = 0 (5010dc is cleared by phase 21). */
    hud->radar_index=1;hud->zoom=nm_force_float(nm_hud_radar_zooms[1]);hud->map_index=1;
}

/* 12d4c..13208 (the map task, after its markers): the zoom follows its target by a fifth a tick (TGP
 * commands 1, 2, 0: z + 0.2*(target - z)), the circle around the player has the radius 0.21 / zoom
 * (command 3), and an opponent is drawn when command 41's distance from its centre minus the radius
 * has the sign bit set, with 501a80 clear (no time attack here): at T(x - x0, -0.01, z - z0) Ry(+58 + +d6)
 * in the panel turned by Ry(-(+58 + +d6)) of the player. */
static inline void nm_hud_radar(nm_hud_t *hud, const nm_hud_cars_t *c) {
    const float target=nm_force_float(nm_hud_radar_zooms[hud->radar_index<3 ? hud->radar_index : 1]);
    const float step=target-hud->zoom,fifth=nm_force_float(0x3e4ccccdu)*step;
    hud->zoom=fifth+hud->zoom;
    const float radius=c->math ? nm_force_divide(c->math,nm_force_float(0x3e570a3du),hud->zoom) :
        nm_force_float(0x3e570a3du)/hud->zoom;
    hud->radar_angle=c->angle;hud->radar_count=0;
    for(unsigned i=0;i<c->others && i<NM_HUD_RADAR_CARS;++i) {
        const nm_hud_radar_car_t *o=c->other+i;
        const float cx=c->x-o->x,cz=c->z-o->z;
        const float distance=c->math ? nm_force_length(c->math,cx,cz) : sqrtf(cx*cx+cz*cz);
        if(!(nm_force_bits(distance-radius)>>31)) continue;
        hud->radar[hud->radar_count].dx=o->x-c->x;hud->radar[hud->radar_count].dz=o->z-c->z;
        hud->radar[hud->radar_count].angle=o->angle;hud->radar[hud->radar_count++].id=o->id;
    }
}

/* The rolling start's overlay, course 0, tiles (never in the cells of the rules above). What the original draws and
 * erases tick by tick comes down to a state of this tick's stage 501850, 5010b0 and 501854:
 *  - the player's rolling update d328 (from the tick after its init, so not in phase 19): d448..d46c draw the banner
 *    28829f0 (57x7 tiles at 7,23) and the car 2882d1a (7x5 at 0,24) on its first two ticks, d56c..d5a4 give lines
 *    184..239 the scroll -4 * 5010b0 & 1023 in the layer pair's 1024-pixel ring, the other half blank; d5a8 erases
 *    both when the player passes the line (+128 = 0, 501854 bit0 clears);
 *  - d470..d534, stage 0: while 5010b0 & 31 < 15 "ACCEL ON!" 28766c8 at 19,19, with the accelerator 501054 above 128
 *    "AUTO DRIVE" 287679c at 20,19; erased otherwise. ab9c..abc8 erases both every tick of stage 0 in phase 22, so the
 *    tick that leaves stage 0 leaves them erased. (A pedal crossing 128 inside the 15 ticks leaves one column of
 *    "ACCEL ON!" at column 43 until the next erase: d4d8 erases with the width of the shorter block. Not reproduced.)
 *  - ab78, stages 0..3 (the player 29, 19, 9 profiles before the line): "3", "2", "1" (2882e9c, 2882e24, 2882dac, 9x6
 *    at 27,16) drawn with the stage's step, kept until GO's first erase;
 *  - ac04.. from the line on, stage 5..64: GO! 2882f14 (20x12 at 21,14) drawn while stage & 15 > 7, erased otherwise.
 * The lap message ad34 (the phase handler, after ae98, once the rolling start is over and a lap is done): while 5010dc
 * runs, unless 5010d8 bit1 (the last lap): drawn when its bit2 is set and it is not 1, else erased, and it stays as
 * it is once 5010dc is out. "BEST LAP" 28829a0 when +66 equals the best +64, else "LAP TIME" 28829c8 (14x1 at 24,15),
 * over the time +66 (18544, as nm_hud_time, 2x3 tiles a glyph at 22,16). The call stands before the player's update
 * and a96c's count, as the handler. ae24..ae90 (" PIT WORK" at 21,22 while +204 is nonzero) never runs: the program's
 * only store to +204 is e45c's zero. */
static __attribute__((noinline, unused)) void nm_hud_overlay(nm_hud_t *hud, const nm_hud_race_t *race, const nm_hud_cars_t *c) {
    const int stage=race->stage;
    /* 129d0..12a04, the map task's chord, while START 53e120 bit 4 is held: VR2, VR3, VR4 alone choose the CONDITION
     * panel's zoom +53 = 0, 1, 2, and with VR1 the map's size +52 = 0, 1, 2 (table 13278, the bits VR4 VR3 VR2 VR1).
     * 12a38..12aa8: +38 follows the course's 234de8[+52] by a fifth a tick, as the zoom. The map and its markers'
     * places scale with it; the markers keep their size (12b04: 6e-8 / +38 under the scale +38). */
    if(race->active_phase==20 || race->active_phase==22 || race->active_phase==24 || race->active_phase==26) {
        if(race->held&16u) {
            const unsigned vr=((race->held>>5)&7u)|((race->held>>13)&8u);
            const unsigned choice=vr&14u,index=choice==2u ? 0u : choice==4u ? 1u : choice==8u ? 2u : 3u;
            if(index<3u && !(vr&1u)) hud->radar_index=(uint8_t)index;
            else if(index<3u) hud->map_index=(uint8_t)index;
        }
        const uint32_t *table=nm_hud_map_scales[hud->course<3u ? hud->course : 0u];
        const float standard=nm_force_float(table[1]);
        if(hud->map_scale==0.0f) hud->map_scale=standard;
        hud->map_scale=nm_force_float(0x3e4ccccdu)*(nm_force_float(table[hud->map_index])-hud->map_scale)+hud->map_scale;
        hud->map_apart=hud->map_scale!=standard || hud->course || c->leader_second;
    }
    if(race->active_phase==22 && !(c->start_flags&1u) && c->completed && hud->lap_ticks>0 && !(race->finish_flags&2u)) {
        hud->lap_label=0;
        if(hud->lap_ticks!=1 && (hud->lap_ticks&4)) {
            hud->lap_label=race->lap_last==c->best_ticks ? 2u : 1u;hud->lap_time=race->lap_last;
        }
    }
    hud->roll_banner=(c->start_flags&1u) && race->active_phase!=19;
    hud->roll_scroll=(uint16_t)((race->counter<<2)&1023u);
    hud->roll_hint=hud->roll_banner && !stage && (race->counter&31u)<15u ? (race->accel>128u ? 2u : 1u) : 0u;
    hud->roll_digit=stage>=1 && stage<=3 ? (uint8_t)(4-stage) : 0u;
    hud->roll_go=stage>3 && (stage&15)>7;
    hud->overlay=hud->roll_banner || hud->roll_digit || hud->roll_go || hud->lap_label || hud->map_apart;
}

/* One gameplay tick. `race` and `cars` are the state after the tick. */
static __attribute__((noinline, unused)) void nm_hud_tick(nm_hud_t *hud, const nm_hud_race_t *race, const nm_hud_cars_t *c) {
    hud->beep=0;
    if(race->active_phase==21) hud->lap_ticks=0; /* 34a8..34b0 */
    if(race->active_phase==22 && !(c->start_flags&1u) && hud->extension_ticks>0) {
        /* 3558 -> ae98 (the phase handler, once the rolling start is over): the place is row 12 while the
         * lap message runs, else row 16; the counter counts down, the block is drawn while its bit2 is
         * set and erased otherwise and at 0, at the place of this tick (the other place keeps its cells). */
        const uint8_t place=hud->lap_ticks>0 ? 1u : 2u;
        --hud->extension_ticks;
        if(hud->extension_ticks && (hud->extension_ticks&4)) hud->extension|=place;
        else hud->extension&=(uint8_t)~place;
    }
    nm_hud_overlay(hud,race,c);
    /* The player's update: 13410 and 13534. */
    if(race->lap_line) hud->lap_ticks=128;
    if(race->award) hud->extension_ticks=128;
    if(race->active_phase==22) {
        if(race->phase==23) { /* 371c: finish */
            nm_hud_number((uint32_t)(race->remaining>0 ? race->remaining : 0)>>6,hud->timer,2);
        } else if(race->phase==25) { /* 37c0: time up */
            nm_hud_number(0,hud->timer,2);
        } else if(race->clock!=hud->clock) { /* 35e0: the clock ran */
            /* The handler's value: an extension awarded later in this tick is not in it. */
            const int32_t left=race->remaining>hud->remaining ? hud->remaining-1 : race->remaining;
            if(left<=0) nm_hud_number(0,hud->timer,2);
            else if(left==640 || left==385) { /* 36fc/370c: warning voices, no redraw */ }
            else if(left>=384) nm_hud_number((uint32_t)left>>6,hud->timer,2);
            else if(--hud->blink_ticks<0) { /* 364c..36e0: one blank tick and a beep */
                hud->blink_ticks=nm_hud_blink_period((unsigned)left>>6);
                hud->blink_phase=(uint8_t)((hud->blink_phase+1u)&1u);
                hud->beep=(uint8_t)(1u+hud->blink_phase);
                hud->timer[0]=hud->timer[1]=NM_HUD_BLANK;
            } else nm_hud_number((uint32_t)left>>6,hud->timer,2);
        }
    }
    hud->clock=race->clock;hud->remaining=race->remaining;
    hud->needle=0;
    if(race->active_phase==20 || race->active_phase==22) {
        /* a96c: lap digit, completed laps, running lap, lap numbers. */
        const unsigned done=c->completed,total=c->total_laps;
        if(done+1u<=total) hud->lap_current=(uint8_t)(done+1u);
        for(unsigned i=0;i<done && i<8;++i) {
            nm_hud_time(c->lap_times[i],hud->lap[i].time);
            hud->lap[i].best=c->lap_times[i]==c->best_ticks;
        }
        if(done!=total && done<8) {
            nm_hud_time(race->clock-c->start_ticks,hud->lap[done].time);
            hud->lap[done].best=0;
            for(unsigned i=0;i<=done;++i) nm_hud_number(i+1u,hud->lap[i].number,2);
            if(hud->rows<done+1u) hud->rows=(uint8_t)(done+1u);
        }
        /* aef4: position, unless the rolling start still runs. */
        if(!(c->start_flags&1u) && c->rank<c->count) {
            nm_hud_number(c->rank+1u,hud->rank,2);
            hud->suffix=nm_hud_suffix(c->rank+1u);hud->rank_drawn=1;
        }
        /* afc0: needle. b1a4 with 53e59a set: the speed index is km/h. */
        hud->needle=1;hud->needle_angle=nm_hud_needle_angle(c->filtered_rpm);
        if(c->manual || NM_HUD_GEAR_AUTO) nm_hud_gear(hud,c); /* b0c0 (b0c8: 501464) */
        nm_hud_number((uint32_t)c->speed_index,hud->speed,3);hud->speed_drawn=1;
        /* a818, after the rule set or cleared the HUD object's bit28: while it is
         * set the block is drawn when 5010b0 bit3 is clear (a878..a8bc) and erased
         * when set (a8c4..a8fc), 8 ticks each; a910..a964 erase it with the bit.
         * The cell persists through the phases a7c0 does not run. */
        hud->reverse=race->reverse && !(race->counter&8u);
        /* ab2c (a7c0): while 501a84 > 0 the block is drawn when its low four bits are 7 or more and erased
         * otherwise (640 while the car rolls in: erased; 9 ticks on, 7 off while it stands; 1 at the release:
         * erased); at 0 the cell keeps what it has. */
        if((int32_t)race->hold>0) hud->pit=(race->hold&15u)>=7u;
        /* ab10..ab20, the end of a96c: the lap message counts down. */
        if(hud->lap_ticks>0) --hud->lap_ticks;
    }
    hud->map=race->active_phase==20 || race->active_phase==22 || race->active_phase==24 || race->active_phase==26;
    if(hud->map) {
        hud->player_x=c->x;hud->player_z=c->z;hud->player_yaw=c->yaw;
        hud->leader=c->leader_valid;hud->leader_second=c->leader_second;
        hud->leader_x=c->leader_x;hud->leader_z=c->leader_z;hud->leader_yaw=c->leader_yaw;
        nm_hud_radar(hud,c);
    }
}
#endif
