#include "../include/damage.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    nm_crash_row_t rows[347]={0};
    for(unsigned i=0;i<347;++i) rows[i]=(nm_crash_row_t){1000+i,0,{11,22,33}};
    unsigned cases=0;
    for(unsigned counter=0;counter<256;++counter)
    for(unsigned trigger=0;trigger<2;++trigger)
    for(unsigned blocked=0;blocked<3;++blocked) {
        nm_damage_state_t s={.flags=0x80000000u|(trigger<<6),
            .sequence_counter=(uint8_t)counter,.overlay_model=7,
            .inhibit_ticks=blocked==2 ? -1 : 0};
        assert(nm_intro_crash_step(rows,347,1,blocked==1 ? -128 : 0,&s));
        assert(!(s.flags&(1u<<6)) && (s.flags&0x80000000u));
        if(trigger && !blocked) {
            assert(s.remaining==346 && s.row==1 && s.overlay_model==1000);
            assert(s.inhibit_ticks==218 && s.initial_inhibit==219);
            assert(s.override_count==63 && s.sound_ticks==-9999);
            assert(s.sequence_counter==(uint8_t)(counter+1) && s.orientation==(counter&3));
            assert(s.angle[0]==((counter&1) ? -11 : 11));
            assert(s.angle[1]==(((counter^(counter>>1))&1) ? -22 : 22));
            assert(s.angle[2]==((counter&2) ? -33 : 33));
            while(s.remaining) assert(nm_intro_crash_step(rows,347,1,0,&s));
            assert(nm_intro_crash_step(rows,347,1,0,&s) && !s.overlay_model);
        } else assert(!s.remaining && !s.row && !s.overlay_model && s.sequence_counter==counter);
        assert(!s.sound_requests && !s.auxiliary_request);++cases;
    }
    nm_damage_state_t s={.flags=64,.remaining=1,.row=347,.inhibit_ticks=1},before=s;
    assert(!nm_intro_crash_step(rows,347,1,0,&s) && !memcmp(&s,&before,sizeof(s)));
    assert(!nm_intro_crash_step(rows,2049,1,0,&s) && !memcmp(&s,&before,sizeof(s)));
    printf("PASS intro crash gate/start/advance: %u cases, signed gates, counter wrap, silent, atomic rejection\n",cases);
}
