/* Host only. The tight HUD geometry (render/ui/hud_draw.h, nm_hud_tight_pieces)
 * must show the picture of the whole quads it replaces, pixel for pixel.
 *
 *   check_hud_tight HUD_PACK_DIR HUD_ROWS [HUD_ROWS ...]
 *
 * HUD_PACK_DIR: tools/prepare_hud.py output (hud/sheet-N.dt, hud/hud.dp); the
 *               program is compiled against that pack's hud_data.h.
 * HUD_ROWS:     `trace_race_native ... --hud FILE`: one row per race tick with
 *               everything the HUD shows (tests/hud_dump.h).
 *
 * First the table itself (nm_hud_tight_valid, the console loader's check).
 * Then, for every row, three pictures of 640x480 output pixels are drawn by
 * one triangle rasteriser and compared at every pixel any of them touches:
 *   before   the whole quads as hud_render.c submitted them: punch-through,
 *            in paint order, no depth test, texel alpha;
 *   depth    the rectangles as submitted during the race: cores on the opaque
 *            list WITHOUT an alpha test (a transparent texel under a core
 *            would show its palette entry and fail here), pieces
 *            punch-through, greater-or-equal depth test with write;
 *   flat     the same rectangles punch-through in paint order, no depth test.
 * Rasteriser: pixel centres, top-left fill rule, u and v linear over each
 * triangle from its float vertices in double arithmetic, nearest texel; a
 * sample within 1e-6 texel below a texel boundary takes the texel above the
 * boundary (rounding down at the exact boundary, as tools/check_hud.py). The
 * whole quads put every fifth row centre exactly on such a boundary; the
 * rectangles do not (NM_HUD_ROW_NUDGE).
 * Sprites on whole arcade pixels are rasterised once per place and form and
 * composed from that; the markers and the needle are rasterised every row. */
#include "render/ui/hud_draw.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W=640, H=480, NOTHING=255 };
static uint8_t *sheet_texels[NM_HUD_SHEET_COUNT]; /* Palette index per texel. */
static uint32_t palette[NM_HUD_PALETTE_COLORS];

typedef struct { uint32_t pixel; uint8_t colour, core; } fragment_t;
typedef struct { fragment_t *list; unsigned count, capacity; } fragments_t;
static void fragment_add(fragments_t *f,uint32_t pixel,unsigned colour,unsigned core) {
    if(f->count==f->capacity) {
        f->capacity=f->capacity ? f->capacity*2 : 256;f->list=realloc(f->list,f->capacity*sizeof(*f->list));
        if(!f->list) { fputs("out of memory\n",stderr);exit(2); }
    }
    f->list[f->count++]=(fragment_t){pixel,(uint8_t)colour,(uint8_t)core};
}

/* One quad in strip order as its two triangles. alpha_test: keep only opaque
 * texels. Returns the pixels covered, transparent ones included. */
static unsigned raster(const float xy[4][2],const float uv[4][2],unsigned sheet,int alpha_test,unsigned core,fragments_t *out) {
    static const unsigned corners[2][3]={{0,1,2},{2,1,3}};
    const unsigned sw=nm_hud_sheets[sheet][0],sh=nm_hud_sheets[sheet][1];
    unsigned covered=0;
    for(unsigned t=0;t<2;++t) {
        double x[3],y[3],u[3],v[3];
        for(unsigned k=0;k<3;++k) {
            x[k]=xy[corners[t][k]][0];y[k]=xy[corners[t][k]][1];u[k]=uv[corners[t][k]][0];v[k]=uv[corners[t][k]][1];
        }
        double area=(x[1]-x[0])*(y[2]-y[0])-(y[1]-y[0])*(x[2]-x[0]);
        if(area==0) continue;
        if(area<0) { /* The other winding: swap two corners, the same triangle. */
            double s;
            s=x[1];x[1]=x[2];x[2]=s;s=y[1];y[1]=y[2];y[2]=s;s=u[1];u[1]=u[2];u[2]=s;s=v[1];v[1]=v[2];v[2]=s;area=-area;
        }
        double low_x=x[0],high_x=x[0],low_y=y[0],high_y=y[0];
        for(unsigned k=1;k<3;++k) {
            if(x[k]<low_x) low_x=x[k];
            if(x[k]>high_x) high_x=x[k];
            if(y[k]<low_y) low_y=y[k];
            if(y[k]>high_y) high_y=y[k];
        }
        int x0=(int)floor(low_x),x1=(int)ceil(high_x),y0=(int)floor(low_y),y1=(int)ceil(high_y);
        if(x0<0) x0=0;
        if(y0<0) y0=0;
        if(x1>W-1) x1=W-1;
        if(y1>H-1) y1=H-1;
        for(int py=y0;py<=y1;++py) for(int px=x0;px<=x1;++px) {
            const double cx=px+0.5,cy=py+0.5;
            double weight[3];int inside=1;
            for(unsigned k=0;k<3 && inside;++k) {
                /* Edge from corner k to k+1; its value weighs the corner opposite, k+2. */
                const unsigned a=k,b=(k+1)%3;
                const double dx=x[b]-x[a],dy=y[b]-y[a],e=dx*(cy-y[a])-dy*(cx-x[a]);
                if(e<0 || (e==0 && !((dy==0 && dx>0) || dy<0))) inside=0; /* Top and left edges belong to the triangle. */
                weight[(k+2)%3]=e/area;
            }
            if(!inside) continue;
            ++covered;
            const double tu=weight[0]*u[0]+weight[1]*u[1]+weight[2]*u[2],tv=weight[0]*v[0]+weight[1]*v[1]+weight[2]*v[2];
            long tx=(long)floor(tu*sw+1e-6),ty=(long)floor(tv*sh+1e-6);
            if(tx<0) tx=0;
            if(ty<0) ty=0;
            if(tx>(long)sw-1) tx=(long)sw-1;
            if(ty>(long)sh-1) ty=(long)sh-1;
            const unsigned index=sheet_texels[sheet][(unsigned)ty*sw+(unsigned)tx];
            if(alpha_test && palette[16*NM_HUD_SHEET_BANK(sheet)+index]>>24<128u) continue;
            fragment_add(out,(uint32_t)(py*W+px),16*NM_HUD_SHEET_BANK(sheet)+index,core);
        }
    }
    return covered;
}

/* A sprite at a place in one form: what the whole quad draws, what its rectangles draw. */
typedef struct { uint32_t key[4]; int used; fragments_t before,after; unsigned before_px,piece_px,core_px,hidden_px; } entry_t;
enum { ENTRIES=1u<<16 };
static entry_t *entries;
static unsigned entry_count;
static entry_t *entry_find(const uint32_t key[4]) {
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<16;++i) hash=(hash^((const uint8_t *)key)[i])*16777619u;
    for(unsigned at=hash%ENTRIES;;at=(at+1)%ENTRIES) {
        entry_t *e=entries+at;
        if(!e->used) {
            if(++entry_count>ENTRIES/2) { fputs("too many distinct sprite places\n",stderr);exit(2); }
            memcpy(e->key,key,16);return e;
        }
        if(!memcmp(e->key,key,16)) return e;
    }
}
static uint8_t *core_mark;
static void entry_draw(entry_t *e,const nm_hud_quad_t *q,const nm_hud_piece_t *pieces,unsigned count,float sx,float sy) {
    float xy[4][2];
    for(unsigned corner=0;corner<4;++corner) { xy[corner][0]=q->xy[corner][0]*sx;xy[corner][1]=q->xy[corner][1]*sy; }
    e->before.count=e->after.count=0;e->piece_px=e->core_px=e->hidden_px=0;
    e->before_px=raster(xy,q->uv,q->sheet,1,0,&e->before);
    for(unsigned k=0;k<count;++k) if(pieces[k].opaque) e->core_px+=raster(pieces[k].xy,pieces[k].uv,pieces[k].sheet,0,1,&e->after);
    const unsigned cores=e->after.count;
    for(unsigned i=0;i<cores;++i) core_mark[e->after.list[i].pixel]=1;
    for(unsigned k=0;k<count;++k) if(!pieces[k].opaque) {
        /* Count what the piece has textured: every pixel of it that no core of the sprite hides. */
        static fragments_t all;
        all.count=0;
        e->piece_px+=raster(pieces[k].xy,pieces[k].uv,pieces[k].sheet,0,0,&all);
        for(unsigned i=0;i<all.count;++i) {
            const fragment_t *f=all.list+i;
            if(core_mark[f->pixel]) { ++e->hidden_px;continue; }
            if(palette[f->colour]>>24>=128u) fragment_add(&e->after,f->pixel,f->colour,0);
        }
    }
    for(unsigned i=0;i<cores;++i) core_mark[e->after.list[i].pixel]=0;
    e->used=1;
}

static const char *field(const char *line,const char *key) {
    char pattern[32];snprintf(pattern,sizeof(pattern)," %s=",key);
    const char *at=strstr(line,pattern);
    if(!at) { fprintf(stderr,"missing field %s\n",key);exit(2); }
    return at+strlen(pattern);
}
static void cells(const char *text,uint8_t *out,unsigned count) {
    for(unsigned i=0;i<count;++i) {
        const char c=text[i];
        if(c>='0' && c<='9') out[i]=(uint8_t)(c-'0');
        else if(c=='m') out[i]=NM_HUD_MINUTE;
        else if(c=='s') out[i]=NM_HUD_SECOND;
        else if(c=='_') out[i]=NM_HUD_BLANK;
        else { fprintf(stderr,"unknown HUD cell '%c'\n",c);exit(2); }
    }
}
static float bits_float(const char *text,char **end) {
    const uint32_t bits=(uint32_t)strtoul(text,end,16);float value;memcpy(&value,&bits,4);return value;
}
/* The inverse of nm_hud_dump for everything nm_hud_quads reads. */
static void row_parse(const char *line,nm_hud_t *h) {
    char *end;
    memset(h,0,sizeof(*h));
    cells(field(line,"timer"),h->timer,2);
    const char *lap=field(line,"lap");
    h->lap_current=(uint8_t)strtoul(lap,&end,10);h->lap_total=(uint8_t)strtoul(end+1,NULL,10);
    h->rows=(uint8_t)strtoul(field(line,"rows"),NULL,10);
    for(unsigned row=0;row<h->rows && row<8;++row) {
        char key[8];snprintf(key,sizeof(key),"r%u",row);
        const char *text=field(line,key);
        cells(text,h->lap[row].number,2);cells(text+3,h->lap[row].time,8);h->lap[row].best=text[12]=='1';
    }
    const char *rank=field(line,"rank");
    h->rank_drawn=rank[2]!='_';
    if(h->rank_drawn) {
        cells(rank,h->rank,2);
        if(rank[2]<'a' || rank[2]>'d') { fputs("unknown rank suffix\n",stderr);exit(2); }
        h->suffix=(uint8_t)(11+rank[2]-'a');
    } else memset(h->rank,NM_HUD_BLANK,2);
    cells(field(line,"count"),h->count,2);
    cells(field(line,"speed"),h->speed,3);
    h->speed_drawn=h->speed[2]!=NM_HUD_BLANK;
    const char *reverse=strstr(line," reverse="); /* a818's message; rows made before the field have none. */
    h->reverse=reverse ? (uint8_t)strtoul(reverse+9,NULL,10) : 0;
    const char *extension=strstr(line," extension="); /* ae98's message; rows made before the field have none. */
    h->extension=extension ? (uint8_t)strtoul(extension+11,NULL,10) : 0;
    const char *gear=strstr(line," gear="); /* b0c0's lever frame and digit, '_' none; rows made before have none. */
    h->gear_lever=gear && gear[6]>='0' && gear[6]<='6' ? (uint8_t)(gear[6]-'0') : NM_HUD_BLANK;
    h->gear_digit=gear && gear[7]>='1' && gear[7]<='4' ? (uint8_t)(gear[7]-'0') : 0;
    const char *roll=strstr(line," roll="); /* The rolling start's overlay; rows made before the field have none. */
    if(roll) {
        h->roll_hint=(uint8_t)(roll[6]-'0');h->roll_digit=(uint8_t)(roll[7]-'0');h->roll_go=(uint8_t)(roll[8]-'0');
        h->roll_banner=(uint8_t)(roll[9]-'0');h->roll_scroll=(uint16_t)strtoul(field(line,"scroll"),NULL,10);
    }
    const char *course=strstr(line," course=");
    h->course=course ? (uint8_t)strtoul(course+8,NULL,10) : 0;
    const char *size=strstr(line," mapsize="); /* The map's size (the chord); rows made before the field have none. */
    if(size) {
        h->map_index=(uint8_t)strtoul(size+9,&end,10);h->map_scale=bits_float(end+1,NULL);
        h->map_apart=h->map_scale!=nm_force_float(nm_hud_map_scales[h->course][1]) || h->course || h->leader_second;
    }
    const char *message=strstr(line," message="); /* ad34; the time back from its cells to ticks at nm_hud_time's rate. */
    if(message) {
        h->lap_label=(uint8_t)(message[9]-'0');
        if(h->lap_label) {
            uint8_t t[8];cells(message+11,t,8);
            const uint32_t hundredths=((t[0]==NM_HUD_BLANK ? 0u : t[0]*10u)+t[1])*6000u+(t[3]*10u+t[4])*100u+t[6]*10u+t[7];
            uint32_t ticks=hundredths*64u/100u;
            while((ticks*100u)>>6<hundredths) ++ticks;
            h->lap_time=(uint16_t)ticks;
        }
    }
    h->overlay=h->roll_banner || h->roll_digit || h->roll_go || h->lap_label || h->map_apart;
    const char *needle=field(line,"needle");
    h->needle=(uint8_t)strtoul(needle,&end,10);h->needle_angle=(uint16_t)strtoul(end+1,NULL,10);
    h->map=(uint8_t)strtoul(field(line,"map"),NULL,10);
    const char *player=field(line,"player");
    h->player_x=bits_float(player,&end);h->player_z=bits_float(end+1,&end);h->player_yaw=(uint16_t)strtoul(end+1,NULL,10);
    const char *leader=field(line,"leader");
    h->leader=(uint8_t)strtoul(leader,&end,10);
    h->leader_x=bits_float(end+1,&end);h->leader_z=bits_float(end+1,&end);h->leader_yaw=(uint16_t)strtoul(end+1,NULL,10);
    const char *second=strstr(line," second="); /* The "2" marker; rows made before the field have none. */
    h->leader_second=second && second[8]=='1';
    /* The CONDITION panel's icons (rows of map ticks; rows made before the fields have none). */
    const char *zoom=strstr(line," zoom=");
    if(h->map && zoom) {
        h->radar_index=(uint8_t)strtoul(zoom+6,&end,10);h->zoom=bits_float(end+1,NULL);
        h->radar_angle=(uint16_t)strtoul(field(line,"ra"),NULL,10);
        h->radar_count=(uint8_t)strtoul(field(line,"radar"),&end,10);
        for(unsigned i=0;i<h->radar_count && i<NM_HUD_RADAR_CARS;++i) {
            h->radar[i].id=(uint8_t)strtoul(end+1,&end,10);
            h->radar[i].dx=bits_float(end+1,&end);h->radar[i].dz=bits_float(end+1,&end);
            h->radar[i].angle=(uint16_t)strtoul(end+1,&end,10);
        }
    } else h->zoom=NM_HUD_RADAR_ZOOM;
}
static void host_sincos(uint16_t angle,float *sine,float *cosine) {
    const double radians=angle*(2.0*3.14159265358979323846/65536.0);
    *sine=(float)sin(radians);*cosine=(float)cos(radians);
}

int main(int argc,char **argv) {
    if(argc<3) { fputs("usage: check_hud_tight HUD_PACK_DIR HUD_ROWS [HUD_ROWS ...]\n",stderr);return 2; }
    char path[1024];
    snprintf(path,sizeof(path),"%s/hud/hud.dp",argv[1]);
    FILE *in=fopen(path,"rb");uint32_t head[8];
    if(!in || fread(head,4,2,in)!=2 || head[0]!=0x4c415044u || head[1]!=NM_HUD_PALETTE_COLORS ||
       fread(palette,4,NM_HUD_PALETTE_COLORS,in)!=NM_HUD_PALETTE_COLORS) { fprintf(stderr,"cannot read %s\n",path);return 2; }
    fclose(in);
    uint8_t *marks=malloc(NM_HUD_MARK_BYTES);
    unsigned table_failures=0;
    for(unsigned sheet=0;sheet<NM_HUD_SHEET_COUNT;++sheet) {
        const unsigned sw=nm_hud_sheets[sheet][0],sh=nm_hud_sheets[sheet][1];
        uint8_t *payload=malloc(sw*sh/2);
        snprintf(path,sizeof(path),"%s/hud/sheet-%u.dt",argv[1],sheet);
        if(!(in=fopen(path,"rb")) || fread(head,4,8,in)!=8 || head[0]!=0x78546344u || head[3]!=(sw|(uint32_t)sh<<16) ||
           fread(payload,1,sw*sh/2,in)!=sw*sh/2 || fgetc(in)!=EOF) { fprintf(stderr,"cannot read %s\n",path);return 2; }
        fclose(in);
        sheet_texels[sheet]=malloc(sw*sh);
        for(unsigned y=0;y<sh;++y) for(unsigned x=0;x<sw;++x) sheet_texels[sheet][y*sw+x]=(uint8_t)nm_hud_sheet_texel(payload,sw,sh,x,y);
        if(!nm_hud_tight_valid(sheet,payload,palette+16*NM_HUD_SHEET_BANK(sheet),marks)) { printf("FAIL tight table does not fit sheet %u\n",sheet);++table_failures; }
        free(payload);
    }
    unsigned rectangles=0,cores=0,single=0;
    for(unsigned sprite=0;sprite<NM_HUD_SPRITE_COUNT;++sprite) {
        const nm_hud_tight_t *t=NM_HUD_TIGHT_OF(sprite);
        rectangles+=(unsigned)t->cores+t->pieces;cores+=t->cores;
        single+=t->cores+t->pieces==1;
    }
    printf("tight table: %u sprites, %u rectangles (%u opaque cores, %u punch-through pieces), %u sprites of one rectangle, "
        "cost model %u ns a TA record, %u records a rectangle; %s\n",
        (unsigned)NM_HUD_SPRITE_COUNT,rectangles,cores,rectangles-cores,single,(unsigned)NM_HUD_TIGHT_LINE_NS,(unsigned)NM_HUD_TIGHT_LINES,
        table_failures ? "FAIL" : "every opaque texel held, no transparent texel in a core");
    /* The console submits sprites with 16-bit u/v (code/render/ui/hud_render.c). */
    if(!nm_hud_uv16_valid()) { puts("FAIL a texture coordinate does not fit 16-bit sprite u/v");++table_failures; }
    else puts("sprite u/v: every texel boundary and the needle's texel centre exact in 16 bits");
    if(table_failures) return 1;

    entries=calloc(ENTRIES,sizeof(*entries));core_mark=calloc(W*H,1);
    uint8_t *canvas[3];uint16_t *depth=calloc(W*H,2);uint8_t *touched_mark=calloc(W*H,1);
    uint32_t *touched=malloc(W*H*4);
    for(unsigned c=0;c<3;++c) { canvas[c]=malloc(W*H);memset(canvas[c],NOTHING,W*H); }
    const float sx=640.0f/496.0f,sy=480.0f/384.0f;
    static nm_hud_quad_t quads[NM_HUD_MAX_QUADS];
    static nm_hud_piece_t pieces[NM_HUD_MAX_PIECES];
    static entry_t loose; /* Markers and needle: drawn anew every row. */
    static char line[8192];
    uint64_t failures=0,ties=0;
    for(int file=2;file<argc;++file) {
        if(!(in=fopen(argv[file],"r"))) { perror(argv[file]);return 2; }
        uint64_t rows=0,sum_quads=0,sum_pieces=0,sum_cores=0,sum_before=0,sum_piece_px=0,sum_core_px=0,sum_hidden=0,
            differing=0,whole_rows=0,ink=0;
        unsigned peak_quads=0,peak_pieces=0;long first_bad=-1;
        while(fgets(line,sizeof(line),in)) {
            if(line[0]!='D') continue;
            const long frame=strtol(field(line,"frame"),NULL,10);
            nm_hud_t hud;row_parse(line,&hud);
            const unsigned count=nm_hud_quads(&hud,quads,host_sincos);
            const unsigned total=nm_hud_tight_pieces(quads,count,pieces,sx,sy);
            unsigned tight=0,at=0,rank=0,n_touched=0,row_cores=0;
            for(unsigned i=0;i<count;++i) tight+=nm_hud_quad_pieces(quads+i,0);
            whole_rows+=tight!=total;
            for(unsigned sheet=0;sheet<NM_HUD_SHEET_COUNT;++sheet) for(unsigned i=0;i<count;++i) {
                const nm_hud_quad_t *q=quads+i;
                if(q->sheet!=sheet) continue;
                const unsigned first=at;
                while(at<total && (unsigned)((pieces[at].z-NM_HUD_DEPTH)*0.5f)==rank) {
                    if(pieces[at].z!=NM_HUD_DEPTH+2.0f*(float)rank+(float)pieces[at].opaque || pieces[at].sheet!=sheet) {
                        if(failures<8) printf("FAIL frame %ld: rectangle %u has depth %g on sheet %u\n",frame,at,(double)pieces[at].z,pieces[at].sheet);++failures;
                    }
                    row_cores+=pieces[at].opaque;++at;
                }
                if(at==first) {
                    if(failures<8) printf("FAIL frame %ld: quad %u has no rectangle\n",frame,i);
                    ++failures;++rank;continue;
                }
                entry_t *e=&loose;
                if(nm_hud_quad_fixed(q)) {
                    uint32_t key[4]={q->sprite,0,0,at-first};
                    memcpy(key+1,q->at,8);
                    e=entry_find(key);
                    if(!e->used) entry_draw(e,q,pieces+first,at-first,sx,sy);
                } else entry_draw(e,q,pieces+first,at-first,sx,sy);
                sum_before+=e->before_px;sum_piece_px+=e->piece_px-e->hidden_px;sum_core_px+=e->core_px;sum_hidden+=e->hidden_px;
#define TOUCH(p) do { if(!touched_mark[p]) { touched_mark[p]=1;touched[n_touched++]=(p); } } while(0)
                for(unsigned k=0;k<e->before.count;++k) { const fragment_t *f=e->before.list+k;TOUCH(f->pixel);canvas[0][f->pixel]=f->colour; }
                for(unsigned k=0;k<e->after.count;++k) {
                    const fragment_t *f=e->after.list+k;const uint16_t z=(uint16_t)(1u+2u*rank+f->core);
                    TOUCH(f->pixel);
                    /* depth: cores unconditionally solid, pieces by their opaque texels; nearer or equal wins. */
                    if(z==depth[f->pixel]) ++ties; /* Two rectangles of one sprite and one list over a pixel. */
                    if(z>=depth[f->pixel]) { depth[f->pixel]=z;canvas[1][f->pixel]=f->colour; }
                    /* flat: paint order, alpha test for every rectangle. */
                    if(palette[f->colour]>>24>=128u) canvas[2][f->pixel]=f->colour;
                }
                ++rank;
            }
            if(at!=total) {
                if(failures<8) printf("FAIL frame %ld: %u rectangles belong to no quad\n",frame,total-at);
                ++failures;
            }
            unsigned bad=0;
            for(unsigned k=0;k<n_touched;++k) {
                const uint32_t p=touched[k];
                if(canvas[0][p]!=canvas[1][p] || canvas[0][p]!=canvas[2][p]) {
                    if(!bad && failures<8) printf("FAIL frame %ld pixel %u,%u: before %u depth %u flat %u (16*sheet+palette index, %u nothing)\n",
                        frame,p%W,p/W,canvas[0][p],canvas[1][p],canvas[2][p],(unsigned)NOTHING);
                    ++bad;
                }
                ink+=canvas[0][p]!=NOTHING;
                canvas[0][p]=canvas[1][p]=canvas[2][p]=NOTHING;depth[p]=0;touched_mark[p]=0;
            }
            if(bad) { ++failures;differing+=bad;if(first_bad<0) first_bad=frame; }
            ++rows;sum_quads+=count;sum_pieces+=total;sum_cores+=row_cores;
            if(count>peak_quads) peak_quads=count;
            if(total>peak_pieces) peak_pieces=total;
        }
        fclose(in);
        if(!rows) { printf("FAIL %s: no HUD rows\n",argv[file]);++failures;continue; }
        printf("%s: %" PRIu64 " rows, %" PRIu64 " pixels differ%s\n",argv[file],rows,differing,differing ? " FAIL" : "");
        if(first_bad>=0) printf("  first differing frame %ld\n",first_bad);
        printf("  per row, before -> after: quads %.1f (peak %u) -> rectangles %.1f (peak %u; %.1f opaque, %.1f punch-through); "
            "TA records of 32 bytes %.0f -> %.0f (4 a quad, %u a rectangle; headers aside); rows over the budget of %u rectangles: %" PRIu64 "\n",
            (double)sum_quads/rows,peak_quads,(double)sum_pieces/rows,peak_pieces,(double)sum_cores/rows,
            (double)(sum_pieces-sum_cores)/rows,4.0*sum_quads/rows,(double)NM_HUD_TIGHT_LINES*sum_pieces/rows,(unsigned)NM_HUD_TIGHT_LINES,
            (unsigned)NM_HUD_MAX_PIECES,whole_rows);
        printf("  pixels per row: punch-through %.0f -> %.0f; on the opaque list 0 -> %.0f; piece pixels behind a core of their sprite %.0f; "
            "shown (opaque texels) %.0f\n",(double)sum_before/rows,(double)sum_piece_px/rows,(double)sum_core_px/rows,(double)sum_hidden/rows,(double)ink/rows);
    }
    if(ties) { printf("FAIL %" PRIu64 " pixels lie under two rectangles of one sprite on the same list\n",ties);++failures; }
    printf("distinct places of fixed sprites rasterised: %u\n",entry_count);
    if(failures) { puts("HUD tight check FAIL");return 1; }
    puts("HUD tight check PASS: the rectangles draw the whole quads' pixels and colours in every row, by depth and flat");
    return 0;
}
