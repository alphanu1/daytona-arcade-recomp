#ifndef DAYTONA_NATIVE_CONTACT_CLEAR_H
#define DAYTONA_NATIVE_CONTACT_CLEAR_H
#include "player.h"

/* Contact-timer clear of the spark routine 9148..9480 (daytona93). The camera
 * object's updater 6510 (installed by 6484, pool row 60 in the race) draws the
 * car it follows (5012f0, the player) through 6b64 -> 6c0c on every tick it
 * runs, whatever the view: 6c18 calls the timer decrement 9014
 * (nm_player_effect_timers), then both paths (crash 6da4, normal 6f00) call the
 * contact effects 9088, which call 9148 while +98 (90d4) or +99 (90e0) is
 * nonzero. 9148 draws the sparks (presentation), but first, when the spin
 * angle +d6 (ldis, 9158) is below -0x1000 or above 0x1000 (9164/916c, so
 * +-0x1000 itself keeps them), it clears both timers and returns (9474..947c).
 * The camera runs after the player (row 12), the opponents (13-55) and the
 * interactions (row 56, which arm +98 at 14ca8), before the HUD (row 61):
 * call right after nm_player_effect_timers, before the wrong-way rule.
 * Gameplay: +98 (response_ticks) gates the next car-car response (14ca8,
 * collision.h) and the pit entry (d998); +99 (boundary.condition) gates the
 * next wall response (166f4: speed x 0.8, shake, the timer itself,
 * boundary.h), the steering filter (f70c) and the pit entry (d98c). The attract
 * and the podium run the same camera task, but in the attract neither timer is
 * ever set (5000 logged frames: 4394 calls of 9088, +98 = +99 = 0), and the
 * native intro scene has no 9014 either. Returns 1 when it cleared. */
static inline int nm_player_contact_clear(nm_player_t *s) {
    if(!s->response_ticks && !s->boundary.condition) return 0;           /* 9148..9154: ldib +98, else ldib +99 */
    if(s->inhibit_angle>=-0x1000 && s->inhibit_angle<=0x1000) return 0;  /* 9158..916c: cmpibl / cmpibg to 9474 */
    s->response_ticks=0;s->boundary.condition=0;                         /* 9474..947c: stib 0 to +98, +99 */
    return 1;
}
#endif
