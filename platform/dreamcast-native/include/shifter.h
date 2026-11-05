#ifndef DAYTONA_NATIVE_SHIFTER_H
#define DAYTONA_NATIVE_SHIFTER_H
#include <stdint.h>

/* The cabinet's four-speed H shifter as a pad's two shift buttons.
 *
 * The original reads the shifter as three bits of the input byte at 1c00012,
 * inverted into 53e120 bits 20-22 by the input routine 1c020 every frame; the
 * manual gear rule 11e80 maps selector = 53e120>>20&7 through table 234abc
 * {-1,2,3,-1,-1,0,1,-1} (drive.dnd gear_selector) and keeps the gear on -1.
 * Gears 1..4 are the values 5,6,1,2 (MAME's gear values 2,1,6,5 inverted),
 * neutral is 7. Nothing else in the race reads those bits (the 8d38 reader
 * sits behind the debug gate 501090&0x700000), so the selector stream is the
 * whole gameplay input of the shifter.
 *
 * Sequential mapping: a press asks for one gear above or below the car's
 * current gear (+148) for one tick; every other tick the lever rests in
 * neutral, which keeps the gear. Any such stream is one a cabinet player can
 * produce, so the gear rule needs nothing beyond the original's. */
enum { NM_SHIFTER_NEUTRAL=7, NM_SHIFTER_GEARS=4 };
static const uint8_t nm_shifter_gear_values[NM_SHIFTER_GEARS]={5,6,1,2};

/* request: +1 shift up, -1 shift down, 0 none. gear: the car's gear index before the tick. */
static inline uint8_t nm_shifter_sequential(uint32_t gear,int request) {
    if(gear>=NM_SHIFTER_GEARS || !request) return NM_SHIFTER_NEUTRAL;
    if(request>0) return gear+1<NM_SHIFTER_GEARS ? nm_shifter_gear_values[gear+1] : NM_SHIFTER_NEUTRAL;
    return gear ? nm_shifter_gear_values[gear-1] : NM_SHIFTER_NEUTRAL;
}

/* The selector table of the loaded drive pack must name gear g for value
 * nm_shifter_gear_values[g] and keep the gear in neutral. */
static inline int nm_shifter_check(const int8_t table[8]) {
    if(!table || table[NM_SHIFTER_NEUTRAL]!=-1) return 0;
    for(unsigned g=0;g<NM_SHIFTER_GEARS;++g) if(table[nm_shifter_gear_values[g]]!=(int8_t)g) return 0;
    return 1;
}
#endif
