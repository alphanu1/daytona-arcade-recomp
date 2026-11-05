/* Phase27/28 control lifecycle only: no cinematic, assets or gameplay math. */
#undef NDEBUG
#include "podium.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    nm_podium_t s={.phase=27,.timer=-7};
    unsigned cinematic=0,cleanups=0,entries=0,ticks=0;
    while(s.phase!=25) {
        assert(nm_podium_step(&s));++ticks;
        assert(s.phase==28 || s.phase==25);
        entries+=s.entered;cleanups+=s.cleanup;cinematic+=s.cinematic;
        assert(s.entered==(ticks==1));           /* 27 falls into 28 in one pass. */
        assert(s.cleanup==(ticks==1216) && s.cleanup==(s.timer==384));
        assert(s.cinematic==(ticks<1216));       /* Tasks are off on the cleanup tick. */
        assert(s.timer==1600-(int32_t)ticks);
        assert(ticks<=1601);
    }
    /* Timer zero still holds phase28; only the signed-negative tick leaves. */
    assert(ticks==1601 && s.timer==-1 && entries==1 && cleanups==1 && cinematic==1215);
    const nm_podium_t done=s;
    assert(!nm_podium_step(&s) && !memcmp(&s,&done,sizeof(s)));
    assert(!nm_podium_step(NULL));
    for(unsigned phase=0;phase<256;++phase) if(phase!=27 && phase!=28) {
        nm_podium_t other={.timer=5,.phase=(uint8_t)phase,.cinematic=1},before=other;
        assert(!nm_podium_step(&other) && !memcmp(&other,&before,sizeof(other)));
    }
    puts("PASS: podium phase27/28 control: 1215 cinematic ticks, cleanup at384, phase25 after1601");
    return 0;
}
