#ifndef DAYTONA_NATIVE_SLOT_MACHINE_H
#define DAYTONA_NATIVE_SLOT_MACHINE_H
#include <stdint.h>

/* Slot machine actor, course0 handler 2204fc (init 2204fc..220530, per-tick
 * routine 220534..2209e0). Actor record 2370a0, cell 151 (16e10 at 220514).
 * Gameplay: a paying stop adds to the remaining time 5010d0. No RNG, no
 * position or phase test: only the previous pass's draw bitmap, the parity
 * of the frame counter 5010b0 and the START edge (53e584 bit4) decide. */
typedef struct {
    int16_t reel[3];   /* task +34, +38, +3c (ldis/stis), 0..64; +3c always even */
    int16_t state;     /* +40: 0 all spin, 1 reel1 stopped, 2 reels1-2, 3 all + evaluate */
    int32_t awarded;   /* +44: bonuses paid this scene (0 or 1) */
} nm_slot_machine_t;   /* All zero == 220518..22052c, the scene's actor creation. */

#define NM_SLOT_MACHINE_CELL 151u          /* +2e: 7 + 16*9 */
/* 5010b0 on the phase20 tick of every reference race (even). The original's
 * value depends on everything since power-on; only its bit0 is read. */
#define NM_SLOT_MACHINE_COUNTER_BASE 2368u

/* Bit 151 of a draw bitmap (501520 word4 bit23). */
static inline int nm_slot_machine_visible(const uint32_t bitmaps[16]) {
    return (int)((bitmaps[NM_SLOT_MACHINE_CELL>>5]>>(NM_SLOT_MACHINE_CELL&31u))&1u);
}

/* 2207ac/220838/2208c4: divi then muli; reel values are never negative. */
static inline int16_t nm_slot_machine_round8(int16_t v) { return (int16_t)(v/8*8); }

/* One call of 220534. visible: cell bit of the PREVIOUS pass's draw bitmap
 * (scenery 5890 runs after the actors); counter: 5010b0 of this loop
 * iteration (bit0 only); start_edge: 53e584 bit4; flags: 501090; remaining:
 * 5010d0 (u32 add == i960 addi). Returns the 19bdc command (0, 0xae2003 or
 * 0xae2002); the remaining time is paid even when the sound is gated off. */
static inline uint32_t nm_slot_machine_tick(nm_slot_machine_t *s,int visible,uint32_t counter,
        int start_edge,uint32_t flags,uint32_t *remaining) {
    if(!visible) return 0;                                      /* 220550 bno 2205dc: edge lost */
    if(start_edge && ++s->state==4) s->state=0;                 /* 220554..220574 */
    const int st=s->state;                                      /* 220578..220594 switch */
    if(st==0) { if(counter&1u) { if(s->reel[0]==64) s->reel[0]=0; ++s->reel[0]; } } /* 2205e0 spin1 */
    else s->reel[0]=nm_slot_machine_round8(s->reel[0]);                             /* 2207ac stop1 */
    if(st<=1) { if(s->reel[1]==64) s->reel[1]=0; ++s->reel[1]; }                     /* 220684 spin2 */
    else s->reel[1]=nm_slot_machine_round8(s->reel[1]);                             /* 220838 stop2 */
    if(st<=2) { if(s->reel[2]==64) s->reel[2]=0; s->reel[2]+=2; }                   /* 220718 spin3 */
    else s->reel[2]=nm_slot_machine_round8(s->reel[2]);                             /* 2208c4 stop3 */
    /* 220950 evaluate, state3 only: 22095c..220960 once per scene, 220968/22096c
     * three equal; 64 draws the "7" list but compares as 64 and never pays. */
    if(st!=3 || s->awarded || s->reel[0]!=s->reel[1] || s->reel[1]!=s->reel[2] ||
       (s->reel[2]!=0 && s->reel[2]!=32)) return 0;
    ++s->awarded;
    *remaining+=s->reel[2]==0 ? 7u<<6 : 5u<<6;                  /* 22097c "7" / 2209b0 "BAR" */
    /* 19bdc queues only when 501090&0x500000 (phase20 or 22). */
    return (flags&0x500000u) ? (s->reel[2]==0 ? 0xae2003u : 0xae2002u) : 0u;
}
#endif
