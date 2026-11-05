#ifndef DAYTONA_NATIVE_STEER_CURVE_H
#define DAYTONA_NATIVE_STEER_CURVE_H
/* The pad's analogue stick to the arcade wheel's ADC byte (nm_controls_decode's steer, include/vehicle.h).
 * Pad only: the decoder and everything after it stay the arcade's; a scripted race injects its steer values and
 * never comes through here.
 *
 * The decoder reads an ADC magnitude of 5..80 as the wheel's 0..75 (a 5-count dead band, a clamp at 80) and scales
 * that to the steer of -127..127. This table spans exactly that band: a stick inside the dead zone gives the centre
 * (128); outside it the remaining travel is rescaled to 0..1, raised to NM_STEER_CURVE and mapped to the wheel's
 * 0..75, so the mapping is continuous at the dead-zone edge (wheel 0) and the end of the stick's travel (-128 is
 * clamped to -127, the two sides are mirror images) is exactly the wheel's full lock (ADC 128+-80, steer +-127).
 * Exponent 1 is linear with full lock; above 1 the centre is finer and the outer travel does the big turns.
 * Built once (256 entries); the frame does one table read. NATIVE_STEER_CURVE / NATIVE_STEER_DEADZONE (local.cfg.mk). */
#include <math.h>
#include <stdint.h>

#ifndef NM_STEER_CURVE
#define NM_STEER_CURVE 2.0
#endif
#ifndef NM_STEER_DEADZONE
#define NM_STEER_DEADZONE 12
#endif
#define NM_STEER_ADC_CENTRE 128
#define NM_STEER_ADC_BAND 5    /* the decoder's dead band: magnitudes <= 5 are wheel 0 */
#define NM_STEER_WHEEL_LOCK 75 /* the decoder's wheel magnitude at full lock (ADC magnitude 80) */
#define NM_STEER_STICK_END 127 /* the stick's range used: -127..127 (-128 clamped) */

/* table[joyx+128] = the ADC byte for that stick position. deadzone: stick counts (0..126) that read as centre;
 * exponent > 0. Monotonic in joyx and an exact mirror about 128. */
static inline void nm_steer_table_build(uint8_t table[256], double exponent, int deadzone) {
    if(deadzone<0) deadzone=0;
    if(deadzone>NM_STEER_STICK_END-1) deadzone=NM_STEER_STICK_END-1;
    if(!(exponent>0.0)) exponent=1.0;
    for(int raw=-128;raw<=127;++raw) {
        const int stick=raw<-NM_STEER_STICK_END ? -NM_STEER_STICK_END : raw;
        const int magnitude=stick<0 ? -stick : stick;
        int wheel=-1; /* -1: the centre byte itself, not the dead band's edge */
        if(magnitude>deadzone) {
            const double travel=(double)(magnitude-deadzone)/(double)(NM_STEER_STICK_END-deadzone);
            const double curved=pow(travel,exponent);
            wheel=(int)(curved*NM_STEER_WHEEL_LOCK+0.5);
            if(wheel>NM_STEER_WHEEL_LOCK) wheel=NM_STEER_WHEEL_LOCK;
            if(wheel<0) wheel=0;
        }
        const int adc=wheel<0 ? 0 : NM_STEER_ADC_BAND+wheel;
        table[raw+128]=(uint8_t)(stick<0 ? NM_STEER_ADC_CENTRE-adc : NM_STEER_ADC_CENTRE+adc);
    }
}
static inline uint8_t nm_steer_table_adc(const uint8_t table[256], int8_t joyx) { return table[(int)joyx+128]; }
#endif
