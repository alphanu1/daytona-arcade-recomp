#ifndef DAYTONA_NATIVE_WRONG_WAY_H
#define DAYTONA_NATIVE_WRONG_WAY_H
#include <stdint.h>
#include "contact.h"

/* Wrong-way rule a818..a968 (daytona93), run by the HUD object's task a7c0
 * (a7d8: `call a818`) only while 501090 & 0x500000 (phase 20 or 22). The HUD
 * object is pool row 61 (501268, handler a674 set by the phase19 entry at
 * 3340), after the player (row 12), the opponents (13-55), row 56 and the
 * cameras (58, 60), before the actor creator (74) and the scenery (139): the
 * readers of bit 28 see this tick's value on the next tick. Gameplay: the
 * player's flags bit 28, read by the road cursor (109e8 -> 10bcc/10c50,
 * nm_road_select), the opponents (10ce0, guided.h) and the lap award (13590,
 * checkpoint.h). The message it draws is presentation (hud.h); its cue, the
 * intent lane 501a02 = 30 on every wrong-way tick (a900), is written by
 * nm_race_scene_wrong_way (race_intents.h: ae2111 and its repeats). */
#define NM_WRONG_WAY_FLAG (UINT32_C(1)<<28)

/* One call of a818. road: player +30 (TGP 30 heading of that road quad,
 * a820..a834); heading: player +48 (ldis, a83c); shown: the HUD object's own
 * flags bit 28 (cleared by a674 at a68c: the race scene start). Rejects a road
 * outside the contact pack and publishes nothing then. */
/* a840..a85c: the heading points against the road quad's (call with a road inside the pack). */
static inline int nm_wrong_way_facing(const nm_contact_pack_t *contact,uint32_t road,int16_t heading) {
    const uint32_t d=(contact->quads[road].heading-(uint32_t)(int32_t)heading)&0xffffu; /* a840..a84c subi, and 0xffff */
    return d>=0x4000u && d<=0xc000u;               /* a850..a85c: cmpobl 0x4000 / cmpobg 0xc000 to a910 */
}
static inline int nm_wrong_way_tick(const nm_contact_pack_t *contact,uint32_t road,int16_t heading,
        uint8_t *shown,uint32_t *player_flags) {
    if(!contact || !contact->quads || !shown || !player_flags || road>=contact->quad_count) return 0;
    if(nm_wrong_way_facing(contact,road,heading)) {
        *player_flags|=NM_WRONG_WAY_FLAG;*shown=1;  /* a860..a874 */
    } else if(*shown) {                            /* a910..a914: only after the HUD object set it */
        *shown=0;*player_flags&=~NM_WRONG_WAY_FLAG; /* a918..a928 */
    }
    return 1;
}
#endif
