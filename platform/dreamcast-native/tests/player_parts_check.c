/* Host check of the player's part rules (include/player_parts.h) with damage and views.
 *
 * usage: player_parts_check                       the rules on chosen states (exit 0 on success)
 *        player_parts_check DRIVE.dnd COURSE.dnr  then, per stdin line "v mode eye damage" (hex words: transmission
 *                                                 501464, view mode 5014a4, eye word 501728, damage +208), one line
 *                                                 "show hood list": nm_player_show, nm_player_hood and the body list
 *                                                 +14 the rule gives (crash_model[v][nm_player_damage_body] of
 *                                                 drive.dnd, or the clean list road->start->body_model[v]).
 * tests/test_damage_render.py feeds it every race frame of the original's display-list logs. */
#undef NDEBUG
#include "player_parts.h"
#include "road.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t bits(float value) { uint32_t out;memcpy(&out,&value,4);return out; }

static void rules(void) {
    /* 16bd4/15cdc: bit 8 wins, then the corners; wheel bits 4-7 alone keep the clean body. */
    assert(nm_player_damage_body(0)==-1 && nm_player_damage_body(0xf0)==-1);
    assert(nm_player_damage_body(0x44)==4 && nm_player_damage_body(0xcc)==12 && nm_player_damage_body(0x11)==1);
    assert(nm_player_damage_body(0x100)==0 && nm_player_damage_body(0x1dd)==0);
    /* 231ea0: front corners bit 2 (wheel 1) -> 1, bit 3 (wheel 0) -> 2, both -> 3; crashed -> 3. */
    assert(nm_player_hood(0)==0 && nm_player_hood(0x3)==0 && nm_player_hood(0x44)==1 && nm_player_hood(0x88)==2);
    assert(nm_player_hood(0xcc)==3 && nm_player_hood(0x100)==3 && nm_player_hood(0x111)==3);
    const unsigned all=NM_PLAYER_SHOW_BODY|NM_PLAYER_SHOW_WHEELS;
    /* Modes 0 and 1 (follow presets 2/3, keyed cameras) draw the body whatever the eye word. */
    assert(nm_player_show(0,bits(2.15f))==all && nm_player_show(1,bits(0.2f))==all && nm_player_show(0,0)==all);
    /* VR1 (2.15) and anything above 2.1: nothing; 2.1 itself and VR2 (0.2): hood, no wheels. */
    assert(nm_player_show(2,bits(2.15f))==0 && nm_player_show(2,0x40066667u)==0);
    assert(nm_player_show(2,0x40066666u)==NM_PLAYER_SHOW_HOOD && nm_player_show(2,bits(0.2f))==NM_PLAYER_SHOW_HOOD);
    /* Negative eye words: sign set, never the driver's view; below -0.45 the body, -0.45 hood with wheels. */
    assert(nm_player_show(2,bits(-3.98f))==all && nm_player_show(2,bits(-0.45f))==(NM_PLAYER_SHOW_HOOD|NM_PLAYER_SHOW_WHEELS));
    assert(nm_player_show(2,bits(-0.449f))==NM_PLAYER_SHOW_HOOD);
    assert(nm_player_part_mask(all)==0xffu);
    assert(nm_player_part_mask(NM_PLAYER_SHOW_HOOD)==(1u<<NM_PLAYER_PART_BODY|1u<<NM_PLAYER_PART_SHADOW));
    assert(nm_player_part_mask(0)==1u<<NM_PLAYER_PART_SHADOW);
}

int main(int argc,char **argv) {
    rules();
    if(argc==1) { puts("player_parts_check: rules PASS");return 0; }
    if(argc!=3) { fputs("usage: player_parts_check [DRIVE.dnd COURSE.dnr] < states\n",stderr);return 2; }
    const nm_drive_params_t *p=nm_drive_load(argv[1]);
    nm_road_pack_t road;
    if(!p || !nm_road_load(&road,argv[2]) || !road.start) { fputs("cannot load drive.dnd/course0.dnr\n",stderr);return 2; }
    char line[256];unsigned rows=0;
    while(fgets(line,sizeof(line),stdin)) {
        unsigned v;uint32_t mode,eye,damage;
        if(sscanf(line,"%x %" SCNx32 " %" SCNx32 " %" SCNx32,&v,&mode,&eye,&damage)!=4 || v>1) {
            fputs("bad state line\n",stderr);return 2;
        }
        const int choice=nm_player_damage_body(damage);
        const uint32_t list=choice<0 ? road.start->body_model[v] : p->crash_model[v][choice];
        printf("%u %u %" PRIx32 "\n",nm_player_show(mode,eye),nm_player_hood(damage),list);
        ++rows;
    }
    fprintf(stderr,"player_parts_check: %u states\n",rows);
    return 0;
}
