#include "gameplay/name_scene.h"

int nm_name_scene_init(nm_name_scene_t *s,const nm_name_layout_t *layout) {
    if(!s || !layout || !isfinite(layout->radius[0])) return 0;
    *s=(nm_name_scene_t){.entry={.glyph='A'},.intro_remaining=32,.draw_intro=31,
        .remaining=5<<8,.draw_seconds=20,
        .marker={layout->radius[0]+0.008f,layout->angle[0]}};
    return isfinite(s->marker.radius);
}

int nm_name_scene_step(nm_name_scene_t *s,const nm_name_layout_t *layout,
                      int8_t steering,unsigned accept_edge) {
    if(!s || !layout || s->intro_remaining>32 || s->entry.index>=29 || s->entry.count>3) return 0;
    s->commands[0]=s->commands[1]=0;
    if(s->finish_requested) return 2;
    if(s->intro_remaining) {
        s->draw_intro=(int)--s->intro_remaining;return 1;
    }
    s->draw_intro=-1;s->draw_entry=s->entry;
    /*227660 displays the unsigned counter before227908 decrements it. */
    s->draw_seconds=(uint16_t)s->remaining>>6;
    if(!nm_name_marker_step(&s->marker,layout->radius[s->entry.index],layout->angle[s->entry.index]) ||
       !nm_name_select(&s->entry,steering,&s->commands[0])) return 0;
    const int accepted=nm_name_accept(&s->entry,accept_edge,&s->commands[1]);
    if(!accepted) return 0;
    /*226ac8 initializes1280. Zero still runs; negative requests commit. */
    --s->remaining;
    s->finish_requested=accepted==2 || s->remaining<0;
    return s->finish_requested ? 2 : 1;
}
