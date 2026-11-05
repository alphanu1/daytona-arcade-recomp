#ifndef DAYTONA_NATIVE_ATTRACT_2D_H
#define DAYTONA_NATIVE_ATTRACT_2D_H
/* The attract's 2D layer: what the original writes to tile layer A (1000000: 64 tile words a row of 128 bytes,
 * 8x8 characters; the screen shows columns 0..61 of rows 0..47) and to its line table (100c000: 8 bytes a scanline,
 * all ones hide the line) over the cinematic, the ranking and the V.R. demo. Ports of the original's writers; the
 * words are the original's (tools/prepare_attract_2d.py makes the atlas their characters and pens are drawn from:
 * code/render/ui/attract_2d_render.c). Destinations are byte offsets into the layer RAM, as the original's g1:
 *   18d74  the layer filled with 8020 (phase 2: 2688)        1a240  the layer filled with 0020 (phase 6)
 *   18e2c  an authored block: words base + value, row by row; leaves g1 = destination + height rows
 *   18dec  a block's area filled with 0020; leaves g1 as 18e2c
 *   18394  a string, 18414 one character (2x3 characters, (c - 32) * 6 + 501338 << 7 | 50133c)
 *   18478  a number, right-aligned in N cells, blank cells (the font's first word) to the left
 *   18ad0  a large digit (3x4 characters from 8500), 18c4c a time m'ss"cc (1/64 s units, cells to the left)
 * The objects: the footer 5674 (the SEGA and copyright blocks, CREDIT and the credit count), the line of 5588
 * (blinking with 5010b0 & 8), the AM2 block of phase 2, the ranking 2266f8 (226764, 226864: the records' lines
 * revealed one a tick) and the demo's 224e64/224ef4. Checked against the original's tile RAM through a loop
 * (tests/test_attract_2d.c). Presentation only: nothing here is read by the game. */
#include <stdint.h>
#include <string.h>
#include "attract_2d_data.h"
#include "race_records.h"
#include "nm_mem.h"

enum { NM_A2D_COLUMNS = 64, NM_A2D_ROWS = 64, NM_A2D_LINES = 384 };
/* The credit display of the factory settings: no coin taken, 53e110 = 3 (the native game has no coins). */
enum { NM_A2D_CREDITS = 0, NM_A2D_COIN_SETTING = 3 };

typedef struct {
    uint16_t tiles[NM_A2D_COLUMNS*NM_A2D_ROWS]; /* layer A */
    uint16_t hidden_first, hidden_end;            /* line table: scanlines [first, end) hidden (else none) */
    uint8_t ranking, demo;                        /* the objects' states: 0 off, then their next routine */
    uint16_t ranking_counter, demo_counter;       /* 2266f8's +6 (the next line revealed), 224e64's +6 */
    uint8_t view;                                 /* 501710 as the demo object writes it */
    uint8_t country;                              /* 53e59a & 1 */
    const nm_race_record_t *records;              /* 50138c: the course's records (the first five drawn) */
    uint8_t course;                               /* 501460 */
} nm_a2d_t;

static inline uint16_t *nm_a2d_cell(nm_a2d_t *s, uint32_t at) {
    at = (at-0x1000000u)&0x1ffeu;                 /* 8 KB, word aligned (the layer's 64 x 64 words) */
    return s->tiles+at/2u;
}
static inline const nm_a2d_block_t *nm_a2d_block_find(uint32_t address) {
    for(unsigned i=0;i<NM_A2D_BLOCKS;++i) if(nm_a2d_blocks[i].address==address) return nm_a2d_blocks+i;
    return NULL;
}
/* 18d74 (8020) and 1a240 (0020). */
static inline void nm_a2d_fill(nm_a2d_t *s, uint16_t word) {
    for(unsigned i=0;i<NM_A2D_COLUMNS*NM_A2D_ROWS;++i) s->tiles[i]=word;
}
/* 18e2c; returns g1 after it (0 for an unknown block: nothing written). */
static inline uint32_t nm_a2d_block(nm_a2d_t *s, uint32_t address, uint32_t at) {
    const nm_a2d_block_t *b=nm_a2d_block_find(address);
    if(!b) return 0;
    for(unsigned y=0;y<b->height;++y,at+=128u)
        for(unsigned x=0;x<b->width;++x) *nm_a2d_cell(s,at+2u*x)=(uint16_t)(b->base+nm_a2d_values[b->first+y*b->width+x]);
    return at;
}
/* 18dec. */
static inline uint32_t nm_a2d_block_erase(nm_a2d_t *s, uint32_t address, uint32_t at) {
    const nm_a2d_block_t *b=nm_a2d_block_find(address);
    if(!b) return 0;
    for(unsigned y=0;y<b->height;++y,at+=128u)
        for(unsigned x=0;x<b->width;++x) *nm_a2d_cell(s,at+2u*x)=0x0020u;
    return at;
}
static inline void nm_a2d_glyph_at(nm_a2d_t *s, uint32_t at, uint16_t first) {
    static const uint16_t offset[6]={0,2,128,130,256,258};
    for(unsigned k=0;k<6;++k) *nm_a2d_cell(s,at+offset[k])=(uint16_t)(first+k);
}
/* 18414: c may be any word (the time's separators pass 8022 and 8027: the product wraps to 16 bits). */
static inline void nm_a2d_char(nm_a2d_t *s, uint32_t c, uint32_t at) {
    nm_a2d_glyph_at(s,at,(uint16_t)((c-32u)*6u+NM_A2D_FONT_BASE));
}
/* 18394. */
static inline void nm_a2d_string(nm_a2d_t *s, const char *text, uint32_t at) {
    for(;*text;++text,at+=4u) nm_a2d_char(s,(uint8_t)*text,at);
}
/* 18478: VALUE right-aligned in DIGITS cells from AT. */
static inline void nm_a2d_number(nm_a2d_t *s, uint32_t value, uint32_t at, unsigned digits) {
    digits&=15u;
    if(!digits) return;
    uint32_t cell=at+4u*digits;
    for(;;) {
        cell-=4u;
        nm_a2d_glyph_at(s,cell,(uint16_t)((value%10u+16u)*6u+NM_A2D_FONT_BASE));
        value/=10u;
        if(!value) break;
        if(digits--<=1u) return;
    }
    while(digits-->1u) {
        cell-=4u;
        static const uint16_t offset[6]={0,2,128,130,256,258};
        for(unsigned k=0;k<6;++k) *nm_a2d_cell(s,cell+offset[k])=(uint16_t)NM_A2D_FONT_BASE;
    }
}
/* 18ad0. */
static inline void nm_a2d_digit(nm_a2d_t *s, unsigned digit, uint32_t at) {
    uint16_t word=(uint16_t)(NM_A2D_DIGITS_BASE+12u*digit);
    for(unsigned y=0;y<4;++y) for(unsigned x=0;x<3;++x) *nm_a2d_cell(s,at+128u*y+2u*x)=word++;
}
/* 18c4c: TIME in 1/64 s, its last cell left of AT + 44. */
static inline void nm_a2d_time(nm_a2d_t *s, uint32_t time, uint32_t at) {
    uint32_t cell=at+44u,value=time*100u>>6;
    cell-=6u;nm_a2d_digit(s,value%10u,cell);value/=10u;
    cell-=6u;nm_a2d_digit(s,value%10u,cell);value/=10u;
    cell-=4u;nm_a2d_char(s,0x8022u,cell);
    cell-=6u;nm_a2d_digit(s,value%10u,cell);value/=10u;
    cell-=6u;nm_a2d_digit(s,value%6u,cell);value/=6u;
    cell-=4u;nm_a2d_char(s,0x8027u,cell);
    cell-=6u;nm_a2d_digit(s,value%10u,cell);value/=10u;
    cell-=6u;
    if(value) nm_a2d_digit(s,value%10u,cell);
    else for(unsigned y=0;y<4;++y) for(unsigned x=0;x<3;++x) *nm_a2d_cell(s,cell+128u*y+2u*x)=0x8280u;
}

/* 5674: the footer. */
static inline void nm_a2d_footer(nm_a2d_t *s) {
    (void)nm_a2d_block(s,0x2872a2au,0x10014d8u);
    (void)nm_a2d_block(s,0x2876a84u,0x100163cu);
    const unsigned credits=NM_A2D_CREDITS,coins=NM_A2D_COIN_SETTING;
    if(!coins) { nm_a2d_string(s,nm_a2d_text_free_play,0x1001608u);return; }
    nm_a2d_string(s,credits<2u ? nm_a2d_text_credit : nm_a2d_text_credits,0x1001608u);
    nm_a2d_number(s,credits,0x1001628u,2);
    nm_a2d_char(s,62u,0x1001630u);
    nm_a2d_number(s,coins,0x1001634u,1);
}
/* 5588: the footer, then the line: blank while 5010b0 & 8 is clear, else INSERT COIN(S) without a credit. */
static inline void nm_a2d_insert_coin(nm_a2d_t *s, uint32_t counter) {
    nm_a2d_footer(s);
    const char *line=!(counter&8u) ? nm_a2d_text_blank_line : !NM_A2D_CREDITS ? nm_a2d_text_insert_coin :
        NM_A2D_CREDITS<NM_A2D_COIN_SETTING ? nm_a2d_text_insert_more : nm_a2d_text_push_start;
    nm_a2d_string(s,line,0x1001318u);
}

/* 2267b8: one record (its rank picks the rows). */
static inline void nm_a2d_record(nm_a2d_t *s, const nm_race_record_t *r) {
    uint32_t at=0x100069cu+(r->rank-1u)*640u;
    nm_a2d_number(s,r->rank,at,1);
    at+=4u;nm_a2d_char(s,nm_a2d_suffixes[r->rank<12u ? r->rank : 11u],at);
    char name[5];nm_memcpy(name,&r->name,4);name[4]=0;
    at+=8u;nm_a2d_string(s,name,at);
    at-=116u;nm_a2d_time(s,r->time_copy,at);
    at+=48u;at=(r->flags&1u) ? nm_a2d_block(s,0x2876130u,at) : nm_a2d_block_erase(s,0x2876130u,at);
    at-=0x178u;(void)((r->flags&2u) ? nm_a2d_block(s,0x2876112u,at) : nm_a2d_block_erase(s,0x2876112u,at));
}
/* The ranking object's iteration (its task runs after the phase handler). */
static inline void nm_a2d_ranking_tick(nm_a2d_t *s) {
    switch(s->ranking) {
    case 1: /* 2266f8 */
        (void)nm_a2d_block(s,0x2875eaeu,0x10000a4u);
        (void)nm_a2d_block(s,nm_a2d_course_blocks[s->course&3u],
            0x1000000u+2u*nm_a2d_course_places[s->course&3u][0]+128u*nm_a2d_course_places[s->course&3u][1]);
        s->ranking=2;break;
    case 2: /* 226764: lines 96..287 hidden, the five records */
        s->ranking_counter=96;s->hidden_first=96;s->hidden_end=288;
        if(s->records) for(unsigned i=0;i<5;++i) nm_a2d_record(s,s->records+i);
        s->ranking=3;break;
    case 3: /* 226864: one more line shown, up to 288 */
        if(s->ranking_counter<=288u) {
            s->hidden_first=(uint16_t)(s->ranking_counter+1u);++s->ranking_counter;
            if(s->hidden_first>=s->hidden_end) s->hidden_first=s->hidden_end=0;
        }
        break;
    default: break;
    }
}
/* The demo object's iteration (224e64, then 224ef4 each tick). */
static inline void nm_a2d_demo_tick(nm_a2d_t *s) {
    if(s->demo==1) {
        s->demo_counter=0;
        for(unsigned t=0;t<2;++t) {
            const nm_a2d_place_t *p=&nm_a2d_demo_text[t][s->country&1u];
            (void)nm_a2d_block(s,p->block,0x1000000u+2u*p->column+128u*p->row);
        }
        s->demo=2;return;
    }
    if(s->demo!=2) return;
    ++s->demo_counter;
    const unsigned old=s->view;
    s->view=(uint8_t)((s->demo_counter>>7)&3u);
    const uint32_t block=(s->demo_counter&8u) ? nm_a2d_demo_boxes[old&3u] : nm_a2d_demo_boxes_dim;
    (void)nm_a2d_block(s,block,0x1000000u+2u*nm_a2d_demo_box_places[s->country&1u][0]+
        128u*nm_a2d_demo_box_places[s->country&1u][1]);
}

/* The attract's iterations, as the phase handlers order them (the objects after the handler):
 *   phase 2 (197c): 2688 (8020), the footer, the AM2 block;
 *   a cinematic tick in phase 3 or 5 (1d24, 1dfc): 5588; then the ranking object (phase 5);
 *   phase 4 (1d8c): the footer, the ranking object switched on (it runs in this iteration);
 *   phase 6 (1e5c): 1a240 (0020), the footer, the demo object switched on (it runs in this iteration);
 *   phase 7 (2028): 5588, then the demo object.
 * COUNTER: 5010b0 of the iteration. */
static inline void nm_a2d_phase2(nm_a2d_t *s, unsigned course, const nm_race_record_t *records, unsigned country,
        unsigned view) {
    nm_memset(s,0,sizeof(*s));
    s->course=(uint8_t)course;s->records=records;s->country=(uint8_t)(country&1u);s->view=(uint8_t)(view&3u);
    nm_a2d_fill(s,0x8020u);
    nm_a2d_footer(s);
    (void)nm_a2d_block(s,0x287659cu,0x10000dcu);
}
/* LAST: phase 5's last iteration (1dfc switches the ranking object off before it would run). */
static inline void nm_a2d_cinematic(nm_a2d_t *s, unsigned phase, uint32_t counter, int last) {
    if(phase==4) { nm_a2d_footer(s);s->ranking=1; }
    else nm_a2d_insert_coin(s,counter);
    if(phase==5 && last) s->ranking=0;
    nm_a2d_ranking_tick(s);
}
static inline void nm_a2d_demo(nm_a2d_t *s, unsigned phase, uint32_t counter) {
    if(phase==6) {
        s->ranking=0;s->hidden_first=s->hidden_end=0;
        nm_a2d_fill(s,0x0020u);
        nm_a2d_footer(s);s->demo=1;
    } else nm_a2d_insert_coin(s,counter);
    nm_a2d_demo_tick(s);
}

/* The atlas slot of a word, or -1 (a word without ink). */
static inline int nm_a2d_slot(uint16_t word) {
    const uint16_t key=word&0x7fffu;
    unsigned low=0,high=NM_A2D_TILES;
    while(low<high) {
        const unsigned mid=(low+high)/2u;
        if(nm_a2d_keys[mid]<key) low=mid+1u; else high=mid;
    }
    return low<NM_A2D_TILES && nm_a2d_keys[low]==key ? (int)low : -1;
}
#endif
