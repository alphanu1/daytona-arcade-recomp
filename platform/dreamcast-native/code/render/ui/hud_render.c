#include "render/ui/hud_render.h"
#include "render/materials/palette_ram.h"
#include "nm_mem.h"
#ifndef NATIVE_HUD
/* Built without NATIVE_HUD_DIR: no HUD assets; the race keeps its debug text. */
int nm_hud_render_load(const char *root) { (void)root;return 1; }
void nm_hud_render_unload(void) {}
void nm_hud_render_reset(const nm_hud_t *hud) { (void)hud; }
void nm_hud_render_tick(const nm_hud_t *hud) { (void)hud; }
int nm_hud_render_enqueue(int flat) { (void)flat;return 0; }
#else
#include "render/ui/hud_draw.h"
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

/* The arcade's race HUD (include/hud.h) from resident PAL4 sheets. The value
 * rules run once per gameplay tick; the first frame that shows a new state
 * places the sprites (render/ui/hud_draw.h, nm_hud_place) and writes their
 * rectangles as finished TA records, and the list callbacks only copy those
 * through the store queues. What does not change in a race is computed once
 * at load: every rectangle's edges relative to its element origin and its
 * packed texture coordinates (spans below), the headers.
 *
 * A rectangle is one PVR sprite: two 32-byte records (NATIVE_HUD_STRIPS=1:
 * a strip of four vertices, four records, as until 14 October). Its corners
 * and texel coordinates are those of render/ui/hud_draw.h, nm_hud_tight_pieces
 * (what tests/check_hud_tight.c compares with the whole quads); sprite u and v
 * are the upper halves of those floats, exact for every HUD coordinate
 * (nm_hud_uv16_valid, checked at load).
 *
 * Two forms of the same rectangles:
 * - by depth (the race): cores on the opaque list, pieces on the
 *   punch-through list, each sprite at the depth of its paint rank in front
 *   of the 3D picture, depth test GEQUAL with write. No header has a shadow
 *   bit, so the cars' modifier volumes do not darken a core; a core also
 *   keeps the 3D picture behind it from being textured.
 * - flat: all of them on the punch-through list in paint order, depth test
 *   ALWAYS without write, as the whole quads were drawn. For frames with
 *   another overlay of that kind, which would otherwise paint over the cores:
 *   the finish actor (phases 23 and 24), and the debug text of
 *   NATIVE_HUD_DEBUG_TEXT. They write their paint-rank depth all the same,
 *   for the panel below.
 * The CONDITION panel's background (a stipple in the original) is one
 * translucent sprite on the translucent list, behind every HUD sprite and in
 * front of the 3D picture (depth NM_HUD_DEPTH-1, GEQUAL, no write): what a
 * HUD sprite covers stays as it is, the picture behind it (and the finish
 * actor) is darkened as the stipple did. Not with NATIVE_DIAG_LISTS, which
 * closes that list. Off by default (project decision, 16 October 2026: about
 * 100x102 blended pixels a frame on the translucent list are not worth the
 * fill rate); make NATIVE_HUD_RADAR_PANEL=1 draws it. The icons are drawn
 * either way. The rolling start's banner has a backdrop of the same kind
 * (hud_data.h nm_hud_roll_panel: a stipple in the original too), under the same
 * option: about 25,500 arcade pixels while it is on the screen, where the
 * stipple as punch-through texels would cost 1.2 ms a frame (28.4 ns a textured
 * pixel) and alias at 640x480. */
#ifndef NM_HUD_RADAR_PANEL
#define NM_HUD_RADAR_PANEL 0
#endif
typedef union { pvr_vertex_t vertex; pvr_poly_hdr_t header; uint32_t word[8]; float f[8]; } hud_record_t;
_Static_assert(sizeof(hud_record_t)==32,"one store-queue line per TA record");
enum { HUD_OPAQUE, HUD_PUNCH, HUD_FLAT, HUD_HEADERS };
#ifdef NATIVE_HUD_STRIPS
enum { HUD_LINES=4 };
#else
enum { HUD_LINES=2 };
#endif
/* Per list: every rectangle, and a header where the sheet changes (sheet by sheet: once each). */
enum { HUD_RECORDS=NM_HUD_MAX_PIECES*HUD_LINES+NM_HUD_SHEET_COUNT };
/* A rectangle as the console places it: edges from nm_hud_span, u/v packed for a sprite (A, B, C). */
typedef union { uint32_t u; float f; } hud_word_t;
#ifdef NATIVE_HUD_STRIPS
typedef nm_hud_span_t hud_span_t;
#else
/* Edges as nm_hud_span_t, then the packed u/v of C, B, A (put_rectangles reads it in this order). */
typedef struct { float x[2], y[2]; hud_word_t uv[4]; } hud_span_t;
_Static_assert(sizeof(hud_span_t)==32,"put_rectangles steps 32 bytes a span");
#endif
static const hud_word_t hud_eol={PVR_CMD_VERTEX_EOL};
static enj_texture_info_t textures[NM_HUD_SHEET_COUNT];
alignas(32) static pvr_poly_hdr_t headers[HUD_HEADERS][NM_HUD_SHEET_COUNT];
alignas(32) static hud_record_t panel[5]; /* Header and one sprite (NATIVE_HUD_STRIPS: a strip of four) for the translucent list. */
static unsigned panel_records;
/* The same for the rolling start's banner: a header and its rectangles, built on the ticks it shows. */
alignas(32) static hud_record_t roll_panel[1+4*NM_HUD_ROLL_PANEL_RECTS];
static unsigned roll_panel_records;
alignas(32) static nm_hud_quad_t quads[NM_HUD_MAX_QUADS];
alignas(32) static hud_record_t records[2][HUD_RECORDS]; /* Opaque list, punch-through list. */
alignas(32) static hud_span_t spans[NM_HUD_RECTANGLE_COUNT],whole_spans[NM_HUD_SPRITE_COUNT];
static const nm_hud_t *hud; /* The race owner's state (code/practice.c). */
static unsigned loaded,ready,count,frames,ticks;
static unsigned piece_count,record_count[2],records_flat,records_stale;
static unsigned flat_frames,piece_peak,whole_frames,builds;
static uint64_t lines_drawn;

void nm_hud_render_unload(void) {
    ready=0;
    if(!loaded) return;
    printf("Native HUD: frames=%u ticks=%u builds=%u; cached headers, direct SQ, startup-only uploads\n",frames,ticks,builds);
    printf("Native HUD tight: rectangles peak=%u of %u, frames over the budget=%u, flat frames=%u; %s, TA records a frame avg=%llu "
        "(cost model %u ns a TA record)\n",piece_peak,(unsigned)NM_HUD_MAX_PIECES,whole_frames,flat_frames,
        HUD_LINES==2 ? "sprites" : "strips",frames ? (unsigned long long)(lines_drawn/frames) : 0ull,(unsigned)NM_HUD_TIGHT_LINE_NS);
    if(pvr_wait_ready() || pvr_wait_render_done()) { enj_state_flag_shutdown(NULL);return; }
    while(loaded) enj_texture_unload(textures+--loaded);
    nm_memset(textures,0,sizeof(textures));count=frames=ticks=builds=0;lines_drawn=0;
    piece_count=record_count[0]=record_count[1]=flat_frames=piece_peak=whole_frames=0;
}

static int read_exact(const char *path,void *data,size_t size) {
    FILE *file=fopen(path,"rb");if(!file) return 0;
    const int ok=fread(data,1,size,file)==size && fgetc(file)==EOF;
    fclose(file);return ok;
}

/* Once for the race: every rectangle's edges and packed u/v (put_rectangles).
 * Sprite u/v are 16 bits: 0 if a coordinate would not fit (nm_hud_uv16_valid). */
static int spans_prepare(void) {
    if(HUD_LINES==2 && !nm_hud_uv16_valid()) return 0;
    for(unsigned k=0;k<NM_HUD_RECTANGLE_COUNT+NM_HUD_SPRITE_COUNT;++k) {
        unsigned sprite=k-NM_HUD_RECTANGLE_COUNT;
        if(k<NM_HUD_RECTANGLE_COUNT) for(sprite=0;sprite+1<NM_HUD_SPRITE_COUNT && NM_HUD_TIGHT_OF(sprite+1)->first<=k;++sprite) {}
        const nm_hud_sprite_t *p=nm_hud_sprites+sprite;
        const uint8_t full[4]={0,0,p->width,p->height};
        hud_span_t *o=k<NM_HUD_RECTANGLE_COUNT ? spans+k : whole_spans+sprite;
        const nm_hud_span_t span=nm_hud_span(sprite,k<NM_HUD_RECTANGLE_COUNT ? nm_hud_rectangles[k] : full);
#ifdef NATIVE_HUD_STRIPS
        *o=span;
#else
        /* A = left bottom, B = left top, C = right top (put_rectangles). */
        for(unsigned e=0;e<2;++e) { o->x[e]=span.x[e];o->y[e]=span.y[e]; }
        o->uv[0].u=PVR_PACK_16BIT_UV(span.u[1],span.v[0]);
        o->uv[1].u=PVR_PACK_16BIT_UV(span.u[0],span.v[0]);
        o->uv[2].u=PVR_PACK_16BIT_UV(span.u[0],span.v[1]);
        o->uv[3].u=0;
#endif
    }
    return 1;
}

int nm_hud_render_load(const char *root) {
    if(!root) return 0;
    /* A new race reloads. ponytail: nothing unloads the sheets when the race scene is
     * released; nm_practice_render_release should call nm_hud_render_unload. */
    if(loaded) nm_hud_render_unload();
    /* Race palette plan: banks 0/1 finish actor, 2..59 cars, 60..63 the HUD, in 60/61 the result labels once the HUD
     * is gone: the HUD is drawn in phases 20..24 only, the labels are written at the phase-25 hand-off, and the next
     * scene load writes the HUD's palettes again. */
    _Static_assert(NM_HUD_BANK_FIRST>=60 && NM_HUD_BANK_FIRST*16+NM_HUD_PALETTE_COLORS<=1024,
        "HUD palette must stay in banks 60..63");
    const unsigned before=pvr_mem_available();
    unsigned bytes=0,largest=0;
    for(unsigned i=0;i<NM_HUD_SHEET_COUNT;++i) {
        const unsigned w=nm_hud_sheets[i][0],h=nm_hud_sheets[i][1],size=32+w*h/2;
        if(w<8 || w>256 || h<8 || h>256 || (w&(w-1)) || (h&(h-1))) return 0;
        bytes+=size;if(size>largest) largest=size;
    }
    if(before<bytes+256*1024) return 0;
    if(!spans_prepare()) { puts("FAIL HUD texture coordinates do not fit 16-bit sprite u/v");return 0; }
    char path[512];
    uint32_t *blob=nm_alloc(largest+NM_HUD_MARK_BYTES); /* Load time only; freed before returning. */
    if(!blob) return 0;
    alignas(32) uint32_t palette[2+NM_HUD_PALETTE_COLORS];
    if(snprintf(path,sizeof(path),"%shud/hud.dp",root)>=(int)sizeof(path) ||
       !read_exact(path,palette,sizeof(palette)) || palette[0]!=0x4c415044 || palette[1]!=NM_HUD_PALETTE_COLORS) goto fail;
    for(unsigned i=0;i<NM_HUD_SHEET_COUNT;++i) {
        const unsigned w=nm_hud_sheets[i][0],h=nm_hud_sheets[i][1],size=32+w*h/2;
        const uint32_t expected[8]={0x78546344,size,0x0f000000,w|((uint32_t)h<<16),
            0x28000000|((__builtin_ctz(w)-3)<<3)|(__builtin_ctz(h)-3),0,0,0};
        if(snprintf(path,sizeof(path),"%shud/sheet-%u.dt",root,i)>=(int)sizeof(path) ||
           !read_exact(path,blob,size) || memcmp(blob,expected,32) || !enj_texture_load_blob(blob,textures+i)) goto fail;
        ++loaded;
        const volatile uint32_t *vram=(const volatile uint32_t *)textures[i].ptr;
        for(unsigned word=0;word<(size-32)/4;++word) if(vram[word]!=blob[word+8]) goto fail;
        /* The header's rectangles against these texels: a core over a transparent texel would be drawn solid. */
        if(NM_HUD_SHEET_BANK(i)*16u>=NM_HUD_PALETTE_COLORS) goto fail;
        if(!nm_hud_tight_valid(i,(const uint8_t *)(blob+8),palette+2+16*NM_HUD_SHEET_BANK(i),(uint8_t *)blob+largest)) {
            printf("FAIL HUD tight geometry does not fit sheet %u (hud_data.h and hud/ from different packs?)\n",i);goto fail;
        }
        for(unsigned form=0;form<HUD_HEADERS;++form) {
            const pvr_list_t list=form==HUD_OPAQUE ? PVR_LIST_OP_POLY : PVR_LIST_PT_POLY;
            const int format=textures[i].pvrformat|PVR_TXRFMT_4BPP_PAL(NM_HUD_BANK_FIRST+NM_HUD_SHEET_BANK(i));
            const pvr_depthcmp_mode_t depth=form==HUD_FLAT ? PVR_DEPTHCMP_ALWAYS : PVR_DEPTHCMP_GEQUAL;
            const bool write=PVR_DEPTHWRITE_ENABLE; /* Flat as well: the panel must not cover a sprite. */
#ifdef NATIVE_HUD_STRIPS
            pvr_poly_cxt_t c;
            pvr_poly_cxt_txr(&c,list,format,w,h,textures[i].ptr,PVR_FILTER_NONE);
            c.gen.culling=PVR_CULLING_NONE;c.depth.comparison=depth;c.depth.write=write;
            pvr_poly_compile(headers[form]+i,&c);nm_memset((char *)(headers[form]+i)+16,0,16);
#else
            /* Sprite header: base colour white, no offset colour (words 4, 5), as the vertices of the strips were. */
            pvr_sprite_cxt_t c;
            pvr_sprite_cxt_txr(&c,list,format,w,h,textures[i].ptr,PVR_FILTER_NONE);
            c.gen.culling=PVR_CULLING_NONE;c.depth.comparison=depth;c.depth.write=write;
            pvr_sprite_compile(headers[form]+i,&c);
            uint32_t *word=(uint32_t *)(headers[form]+i);
            word[4]=0xffffffffu;word[5]=word[6]=word[7]=0;
#endif
        }
    }
    {   /* The CONDITION panel and the banner's: an untextured translucent sprite, its colour in the header. */
#ifdef NATIVE_HUD_STRIPS
        pvr_poly_cxt_t c;
        pvr_poly_cxt_col(&c,PVR_LIST_TR_POLY);
        c.gen.culling=PVR_CULLING_NONE;c.depth.comparison=PVR_DEPTHCMP_GEQUAL;c.depth.write=PVR_DEPTHWRITE_DISABLE;
        c.gen.shading=PVR_SHADE_FLAT;
        pvr_poly_compile(&panel[0].header,&c);
        roll_panel[0].header=panel[0].header;
#else
        pvr_sprite_cxt_t c;
        pvr_sprite_cxt_col(&c,PVR_LIST_TR_POLY);
        c.gen.culling=PVR_CULLING_NONE;c.depth.comparison=PVR_DEPTHCMP_GEQUAL;c.depth.write=PVR_DEPTHWRITE_DISABLE;
        pvr_sprite_compile((pvr_sprite_hdr_t *)&panel[0].header,&c);
        roll_panel[0].header=panel[0].header;
        panel[0].word[4]=NM_HUD_RADAR_PANEL_ARGB;panel[0].word[5]=panel[0].word[6]=panel[0].word[7]=0;
        roll_panel[0].word[4]=NM_HUD_ROLL_PANEL_ARGB;roll_panel[0].word[5]=roll_panel[0].word[6]=roll_panel[0].word[7]=0;
#endif
    }
    /* In the program's palette format (palette_ram.h), read back. */
    if(!nm_palette_ram_write_dpal(palette,NM_HUD_BANK_FIRST*16,"HUD palette")) goto fail;
    free(blob);
    ready=1;
    printf("Native HUD resident PASS: sheets=%u allocated=%u free=%u palette banks=%u..%u; VRAM/palette readback\n",
        loaded,before-(unsigned)pvr_mem_available(),(unsigned)pvr_mem_available(),NM_HUD_BANK_FIRST,
        NM_HUD_BANK_FIRST+NM_HUD_PALETTE_COLORS/16-1);
    return 1;
fail:
    free(blob);
    puts("FAIL HUD startup");nm_hud_render_unload();return 0;
}

static void hud_sincos(uint16_t angle,float *sine,float *cosine) {
    const shz_sincos_t pair=shz_sincosu16(angle);
    *sine=pair.sin;*cosine=pair.cos;
}

/* The rectangles s[0..n) of a fixed quad at element origin `at`, depth z, as
 * sprites; edges (at + x)*sx, (at + y)*sy - NM_HUD_ROW_NUDGE, the arithmetic
 * of nm_hud_span_place. Sprite corners A = left bottom, B = left top, C =
 * right top, D = right bottom (enDjinn's sprites go round so too). The SH4
 * has no float store with a displacement but one with pre-decrement, and a
 * load with post-increment: the loop below reads a span front to back and
 * writes the sprite's 16 words last to first, 40 instructions a rectangle,
 * every value in a register. */
static inline hud_record_t *put_rectangles(hud_record_t *r,const hud_span_t *s,unsigned n,const float at[2],float sx,float sy,float z) {
#ifdef NATIVE_HUD_STRIPS
    for(;n;--n,++s) {
        float e[4];nm_hud_span_place(s,at,sx,sy,e);
        for(unsigned corner=0;corner<4;++corner,++r) {
            pvr_vertex_t *v=&r->vertex;
            v->flags=corner==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v->x=e[corner>>1];v->y=e[2+(corner&1)];v->z=z;
            v->u=s->u[corner>>1];v->v=s->v[corner&1];v->argb=0xffffffff;v->oargb=0;
        }
    }
    return r;
#else
    if(!n) return r;
    float x0,x1,y0,y1,t;
    hud_record_t *end;
    const float ax=at[0],ay=at[1],nudge=NM_HUD_ROW_NUDGE,zero=0.0f,eol=hud_eol.f;
    __asm__ volatile(
        "1:\n\t"
        "fmov.s @%[s]+,%[x0]\n\tfmov.s @%[s]+,%[x1]\n\tfmov.s @%[s]+,%[y0]\n\tfmov.s @%[s]+,%[y1]\n\t"
        "fadd %[ax],%[x0]\n\tfadd %[ax],%[x1]\n\tfadd %[ay],%[y0]\n\tfadd %[ay],%[y1]\n\t"
        "fmul %[sx],%[x0]\n\tfmul %[sx],%[x1]\n\tfmul %[sy],%[y0]\n\tfmul %[sy],%[y1]\n\t"
        "fsub %[nudge],%[y0]\n\tfsub %[nudge],%[y1]\n\t"
        "add #64,%[r]\n\tmov %[r],%[end]\n\t"
        "fmov.s @%[s]+,%[t]\n\tfmov.s %[t],@-%[end]\n\t"    /* cuv */
        "fmov.s @%[s]+,%[t]\n\tfmov.s %[t],@-%[end]\n\t"    /* buv */
        "fmov.s @%[s]+,%[t]\n\tfmov.s %[t],@-%[end]\n\t"    /* auv */
        "add #4,%[s]\n\t"
        "fmov.s %[zero],@-%[end]\n\tfmov.s %[y1],@-%[end]\n\tfmov.s %[x1],@-%[end]\n\tfmov.s %[z],@-%[end]\n\t"  /* -, dy, dx, cz */
        "fmov.s %[y0],@-%[end]\n\tfmov.s %[x1],@-%[end]\n\tfmov.s %[z],@-%[end]\n\tfmov.s %[y0],@-%[end]\n\t"   /* cy, cx, bz, by */
        "fmov.s %[x0],@-%[end]\n\tfmov.s %[z],@-%[end]\n\tfmov.s %[y1],@-%[end]\n\tfmov.s %[x0],@-%[end]\n\t"   /* bx, az, ay, ax */
        "dt %[n]\n\t"
        "bf.s 1b\n\t"
        "fmov.s %[eol],@-%[end]\n\t"                                                                            /* command */
        : [r]"+r"(r),[s]"+r"(s),[n]"+r"(n),[end]"=&r"(end),[x0]"=&f"(x0),[x1]"=&f"(x1),[y0]"=&f"(y0),[y1]"=&f"(y1),[t]"=&f"(t)
        : [ax]"f"(ax),[ay]"f"(ay),[sx]"f"(sx),[sy]"f"(sy),[nudge]"f"(nudge),[z]"f"(z),[zero]"f"(zero),[eol]"f"(eol)
        : "t","memory");
    return r;
#endif
}
/* A quad that turns (markers, needle): corners in strip order from nm_hud_quads. */
static inline hud_record_t *put_quad(hud_record_t *r,const nm_hud_quad_t *q,float sx,float sy,float z) {
#ifdef NATIVE_HUD_STRIPS
    for(unsigned corner=0;corner<4;++corner,++r) {
        pvr_vertex_t *v=&r->vertex;
        v->flags=corner==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x=q->xy[corner][0]*sx;v->y=q->xy[corner][1]*sy;v->z=z;
        v->u=q->uv[corner][0];v->v=q->uv[corner][1];v->argb=0xffffffff;v->oargb=0;
    }
    return r;
#else
    /* B is strip corner 0; A and C its neighbours 1 and 2, in the order that
     * goes round as the fixed sprites do (the needle's model winds the other way). */
    unsigned a=1,c=2;
    const float ux=q->xy[0][0]-q->xy[1][0],uy=q->xy[0][1]-q->xy[1][1],vx=q->xy[2][0]-q->xy[0][0],vy=q->xy[2][1]-q->xy[0][1];
    if(ux*vy-uy*vx<0.0f) { a=2;c=1; }
    const unsigned order[4]={a,0,c,3};
    float x[4],y[4];
    for(unsigned k=0;k<4;++k) { x[k]=q->xy[order[k]][0]*sx;y[k]=q->xy[order[k]][1]*sy; }
    r[0].word[0]=PVR_CMD_VERTEX_EOL;
    r[0].f[1]=x[0];r[0].f[2]=y[0];r[0].f[3]=z;
    r[0].f[4]=x[1];r[0].f[5]=y[1];r[0].f[6]=z;
    r[0].f[7]=x[2];r[1].f[0]=y[2];r[1].f[1]=z;
    r[1].f[2]=x[3];r[1].f[3]=y[3];r[1].word[4]=0;
    for(unsigned k=0;k<3;++k) r[1].word[5+k]=PVR_PACK_16BIT_UV(q->uv[order[k]][0],q->uv[order[k]][1]);
    return r+2;
#endif
}

/* The sprites of the HUD's current state as TA records. By depth: records[0]
 * for the opaque list, records[1] for the punch-through list. Flat: all in
 * records[1]. Paint order as nm_hud_tight_pieces: sheet by sheet, a sprite's
 * cores before its pieces, rank r at NM_HUD_DEPTH+2r and its cores one nearer;
 * a header where a list's sheet changes. A frame that would need more than
 * NM_HUD_MAX_PIECES rectangles draws the sprites that gain least whole. */
/* The CONDITION panel's sprite (records panel[1..]) when the map shows; 0 records otherwise. */
static void panel_build(float sx,float sy) {
    float rect[4];
    panel_records=0;
    if(!nm_hud_radar_panel_rect(hud,rect)) return;
    const float x0=rect[0]*sx,y0=rect[1]*sy,x1=rect[2]*sx,y1=rect[3]*sy,z=NM_HUD_DEPTH-1.0f;
#ifdef NATIVE_HUD_STRIPS
    const float corner[4][2]={{x0,y1},{x0,y0},{x1,y1},{x1,y0}};
    for(unsigned k=0;k<4;++k) {
        pvr_vertex_t *v=&panel[1+k].vertex;
        v->flags=k==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x=corner[k][0];v->y=corner[k][1];v->z=z;v->u=v->v=0.0f;v->argb=NM_HUD_RADAR_PANEL_ARGB;v->oargb=0;
    }
    panel_records=5;
#else
    hud_record_t *r=panel+1;
    r[0].word[0]=PVR_CMD_VERTEX_EOL;
    r[0].f[1]=x0;r[0].f[2]=y1;r[0].f[3]=z; /* A: left bottom */
    r[0].f[4]=x0;r[0].f[5]=y0;r[0].f[6]=z; /* B: left top */
    r[0].f[7]=x1;r[1].f[0]=y0;r[1].f[1]=z; /* C: right top */
    r[1].f[2]=x1;r[1].f[3]=y1;              /* D: right bottom */
    for(unsigned k=4;k<8;++k) r[1].word[k]=0;
    panel_records=3;
#endif
}
/* The banner's backdrop (a stipple in the original), under the option of the CONDITION panel: the rectangles on the
 * screen, by the depth of the panel. */
static void roll_panel_build(float sx,float sy) {
    roll_panel_records=0;
    if(!hud->roll_banner) return;
    const float ax=(float)(nm_hud_roll_x(hud)+NM_HUD_ROLL_BANNER_0_X),ay=(float)NM_HUD_ROLL_BANNER_0_Y,z=NM_HUD_DEPTH-1.0f;
    hud_record_t *r=roll_panel+1;
    for(unsigned i=0;i<NM_HUD_ROLL_PANEL_RECTS;++i) {
        const uint16_t *rect=nm_hud_roll_panel[i];
        const float x0=(ax+(float)rect[0])*sx,y0=(ay+(float)rect[1])*sy,x1=(ax+(float)rect[2])*sx,y1=(ay+(float)rect[3])*sy;
        if(x1<=0.0f || x0>=496.0f*sx) continue;
#ifdef NATIVE_HUD_STRIPS
        const float corner[4][2]={{x0,y1},{x0,y0},{x1,y1},{x1,y0}};
        for(unsigned k=0;k<4;++k,++r) {
            pvr_vertex_t *v=&r->vertex;
            v->flags=k==3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v->x=corner[k][0];v->y=corner[k][1];v->z=z;v->u=v->v=0.0f;v->argb=NM_HUD_ROLL_PANEL_ARGB;v->oargb=0;
        }
#else
        r[0].word[0]=PVR_CMD_VERTEX_EOL;
        r[0].f[1]=x0;r[0].f[2]=y1;r[0].f[3]=z;
        r[0].f[4]=x0;r[0].f[5]=y0;r[0].f[6]=z;
        r[0].f[7]=x1;r[1].f[0]=y0;r[1].f[1]=z;
        r[1].f[2]=x1;r[1].f[3]=y1;
        for(unsigned k=4;k<8;++k) r[1].word[k]=0;
        r+=2;
#endif
    }
    if(r>roll_panel+1) roll_panel_records=(unsigned)(r-roll_panel);
}
static __attribute__((noinline)) void records_build(int flat) {
    const float sx=(640.0f/496.0f)*ENJ_XSCALE,sy=480.0f/384.0f;
    count=nm_hud_place(hud,quads,hud_sincos,0);
#if NM_HUD_RADAR_PANEL
    panel_build(sx,sy);
    roll_panel_build(sx,sy);
#else
    (void)panel_build;(void)roll_panel_build;panel_records=0;
#endif
    /* The table's rectangle counts bound nm_hud_quad_pieces(q,0) from above. */
    unsigned need=0,level=0;
    for(unsigned i=0;i<count;++i) { const nm_hud_tight_t *t=NM_HUD_TIGHT_OF(quads[i].sprite);need+=(unsigned)t->cores+t->pieces; }
    while(need>NM_HUD_MAX_PIECES) {
        need=0;
        for(unsigned i=0;i<count;++i) need+=nm_hud_quad_pieces(quads+i,level);
        if(need<=NM_HUD_MAX_PIECES) break;
        level+=8; /* 256: every sprite whole, at most NM_HUD_MAX_QUADS. */
    }
    whole_frames+=level!=0;
    hud_record_t *op=records[0],*pt=records[1];
    unsigned sheet_op=NM_HUD_SHEET_COUNT,sheet_pt=NM_HUD_SHEET_COUNT,rectangles=0;
    float depth=NM_HUD_DEPTH; /* NM_HUD_DEPTH+2*rank, exact; an unsigned to float conversion costs the SH4 two FPSCR switches. */
    const unsigned form_op=flat ? HUD_FLAT : HUD_OPAQUE,form_pt=flat ? HUD_FLAT : HUD_PUNCH;
#define HUD_HEADER(list) do { if(sheet_##list!=sheet) { sheet_##list=sheet;(list++)->header=headers[form_##list][sheet]; } } while(0)
    for(unsigned sheet=0;sheet<NM_HUD_SHEET_COUNT;++sheet) for(unsigned i=0;i<count;++i) {
        const nm_hud_quad_t *q=quads+i;
        if(q->sheet!=sheet) continue;
        const nm_hud_tight_t *t=NM_HUD_TIGHT_OF(q->sprite);
        const float z=depth; /* Flat too (depth ALWAYS): the rank keeps the panel off every sprite. */
        depth+=2.0f;
        if(!nm_hud_quad_fixed(q)) { /* One quad as it was; opaque when the sprite has no transparent texel. */
            ++rectangles;
            if(t->cores==1 && !t->pieces && !flat) { HUD_HEADER(op);op=put_quad(op,q,sx,sy,z+1.0f); }
            else { HUD_HEADER(pt);pt=put_quad(pt,q,sx,sy,z); }
            continue;
        }
        const unsigned whole=t->priority<level,cores=whole ? 0u : t->cores,pieces=whole ? 1u : t->pieces;
        const hud_span_t *s=whole ? whole_spans+q->sprite : spans+t->first;
        rectangles+=cores+pieces;
        if(cores) {
            if(flat) { HUD_HEADER(pt);pt=put_rectangles(pt,s,cores,q->at,sx,sy,z); }
            else { HUD_HEADER(op);op=put_rectangles(op,s,cores,q->at,sx,sy,z+1.0f); }
        }
        if(pieces) { HUD_HEADER(pt);pt=put_rectangles(pt,s+cores,pieces,q->at,sx,sy,z); }
    }
#undef HUD_HEADER
    piece_count=rectangles;
    if(piece_count>piece_peak) piece_peak=piece_count;
    record_count[0]=(unsigned)(op-records[0]);record_count[1]=(unsigned)(pt-records[1]);
    records_flat=(unsigned)flat;records_stale=0;++builds;
}

void nm_hud_render_reset(const nm_hud_t *state) {
    hud=state;records_stale=1;
}

void nm_hud_render_tick(const nm_hud_t *state) {
    hud=state;records_stale=1;++ticks;
}

/* data: 0 the opaque list's records, 1 the punch-through list's. */
static void render_panel(void *data) {
    (void)data;
    for(unsigned n=0;n<panel_records;++n) {
        hud_record_t *out=pvr_dr_target();*out=panel[n];pvr_dr_commit(out);
    }
    for(unsigned n=0;n<roll_panel_records;++n) {
        hud_record_t *out=pvr_dr_target();*out=roll_panel[n];pvr_dr_commit(out);
    }
}
static void render(void *data) {
    if(!ready) return;
    const unsigned list=(unsigned)(uintptr_t)data;
    const hud_record_t *r=records[list];
    for(unsigned n=record_count[list];n;--n,++r) {
        hud_record_t *out=pvr_dr_target();*out=*r;pvr_dr_commit(out);
    }
    if(list) { ++frames;lines_drawn+=record_count[0]+record_count[1]; }
}

int nm_hud_render_enqueue(int flat) {
    if(!ready || !hud) return 0;
    flat=flat!=0;
    if(records_stale || records_flat!=(unsigned)flat) records_build(flat);
    flat_frames+=(unsigned)flat;
    if(record_count[0]) enj_render_list_add(PVR_LIST_OP_POLY,render,(void *)(uintptr_t)0);
    enj_render_list_add(PVR_LIST_PT_POLY,render,(void *)(uintptr_t)1);
#ifndef NM_DIAG_LISTS
    if(panel_records || roll_panel_records) enj_render_list_add(PVR_LIST_TR_POLY,render_panel,NULL);
#endif
    return 1;
}
#endif
