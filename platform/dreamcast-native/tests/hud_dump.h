/* Host only: one text row per tick of what the native HUD rules display, in
 * the form tools/check_hud.py decodes from the reference tile words. */
#ifndef DAYTONA_NATIVE_HUD_DUMP_H
#define DAYTONA_NATIVE_HUD_DUMP_H
#include "hud.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static inline char nm_hud_dump_cell(uint8_t cell) {
    return cell<10 ? (char)('0'+cell) : cell==NM_HUD_MINUTE ? 'm' : cell==NM_HUD_SECOND ? 's' :
        cell==NM_HUD_BLANK ? '_' : '?';
}
static inline void nm_hud_dump_cells(FILE *out,const uint8_t *cells,unsigned count) {
    for(unsigned i=0;i<count;++i) fputc(nm_hud_dump_cell(cells[i]),out);
}
static inline uint32_t nm_hud_dump_bits(float value) { uint32_t b;memcpy(&b,&value,4);return b; }
static inline void nm_hud_dump(FILE *out,long frame,const nm_hud_t *h) {
    fprintf(out,"D frame=%ld timer=",frame);nm_hud_dump_cells(out,h->timer,2);
    fprintf(out," lap=%u/%u rows=%u",h->lap_current,h->lap_total,h->rows);
    for(unsigned row=0;row<h->rows && row<8;++row) {
        fprintf(out," r%u=",row);nm_hud_dump_cells(out,h->lap[row].number,2);fputc('|',out);
        nm_hud_dump_cells(out,h->lap[row].time,8);fprintf(out,"|%u",h->lap[row].best);
    }
    fputs(" rank=",out);
    if(h->rank_drawn) { /* Suffix glyphs 11..14 of the large font: a=ST b=ND c=RD d=TH. */
        nm_hud_dump_cells(out,h->rank,2);fputc(h->suffix>=11 && h->suffix<=14 ? 'a'+h->suffix-11 : '?',out);
    }
    else fputs("___",out);
    fputs(" count=",out);nm_hud_dump_cells(out,h->count,2);
    fputs(" speed=",out);nm_hud_dump_cells(out,h->speed,3);
    fprintf(out," reverse=%u extension=%u pit=%u",h->reverse,h->extension,h->pit);
    /* b0c0: the lever frame (0..6) and the digit (1..4), '_' when not drawn. */
    fprintf(out," gear=%c%c",h->gear_lever<=6 ? '0'+h->gear_lever : '_',h->gear_digit ? '0'+h->gear_digit : '_');
    /* The rolling start's overlay: hint, digit, GO, banner, the banner's scroll. */
    fprintf(out," roll=%u%u%u%u scroll=%u",h->roll_hint,h->roll_digit,h->roll_go,h->roll_banner,h->roll_scroll);
    /* ad34: the label (0 none), then the time's cells. */
    fprintf(out," message=%u:",h->lap_label);
    { uint8_t cells[8];nm_hud_time(h->lap_time,cells);nm_hud_dump_cells(out,cells,8); }
    fprintf(out," needle=%u:%u map=%u player=%08" PRIx32 ",%08" PRIx32 ",%u leader=%u:%08" PRIx32 ",%08" PRIx32 ",%u beep=%u",
        h->needle,h->needle_angle,h->map,nm_hud_dump_bits(h->player_x),nm_hud_dump_bits(h->player_z),h->player_yaw,
        h->leader,nm_hud_dump_bits(h->leader_x),nm_hud_dump_bits(h->leader_z),h->leader_yaw,h->beep);
    fprintf(out," course=%u mapsize=%u:%08" PRIx32 " second=%u",h->course,h->map_index,nm_hud_dump_bits(h->map_scale),h->leader_second);
    /* Last on the row: the CONDITION panel (zoom index and zoom, the player's heading, then id:dx:dz:angle
     * of each icon, dx and dz as float bits); only the rows of ticks the map object ran. */
    if(h->map) {
        fprintf(out," zoom=%u:%08" PRIx32 " ra=%u radar=%u",h->radar_index,nm_hud_dump_bits(h->zoom),h->radar_angle,h->radar_count);
        for(unsigned i=0;i<h->radar_count && i<NM_HUD_RADAR_CARS;++i)
            fprintf(out,"%c%u:%08" PRIx32 ":%08" PRIx32 ":%u",i ? ';' : ':',h->radar[i].id,nm_hud_dump_bits(h->radar[i].dx),
                nm_hud_dump_bits(h->radar[i].dz),h->radar[i].angle);
    }
    fputc('\n',out);
}
#endif
