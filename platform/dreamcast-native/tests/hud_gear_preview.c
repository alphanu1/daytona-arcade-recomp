/* Host only. Quad lists of the race HUD with the gear indicator in each gear, for
 * tools/hud_gear_preview.py, which compiles this once per gear build option
 * (NM_HUD_GEAR_GATE, NM_HUD_GEAR_DIGIT) against a HUD pack and draws the lists.
 *
 *   hud_gear_preview OUT_PREFIX RPM
 *
 * Writes OUT_PREFIX-gear1..4 (the indicator settled in gears 1..4) and
 * OUT_PREFIX-extension (gear 3 with "TIME EXTENSION!!" at row 12), OUT_PREFIX-shift-NN
 * (NN 30..0: the ticks of a shift from gear 2 to gear 3),
 * in the format of tests/check_hud.c's quad files. The other cells hold fixed
 * sample values; the needle stands at the filtered rpm RPM (+154). */
#include "render/ui/hud_draw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void host_sincos(uint16_t angle,float *sine,float *cosine) {
    const double radians=angle*(2.0*3.14159265358979323846/65536.0);
    *sine=(float)sin(radians);*cosine=(float)cos(radians);
}
static void write_quads(const char *prefix,const char *name,const nm_hud_t *hud) {
    static nm_hud_quad_t quads[NM_HUD_MAX_QUADS];
    char path[512];
    snprintf(path,sizeof(path),"%s-%s",prefix,name);
    const unsigned count=nm_hud_quads(hud,quads,host_sincos);
    FILE *out=fopen(path,"w");
    if(!out) { perror(path);exit(2); }
    fputs("# sheet then four x,y,u,v corners in strip order; arcade 496x384 pixels, normalized uv\n",out);
    /* Paint order (nm_hud_tight_pieces): sheet by sheet, the list's order within a sheet. */
    for(unsigned sheet=0;sheet<NM_HUD_SHEET_COUNT;++sheet) for(unsigned i=0;i<count;++i) {
        if(quads[i].sheet!=sheet) continue;
        fprintf(out,"%u",quads[i].sheet);
        for(unsigned c=0;c<4;++c) fprintf(out," %.6f,%.6f,%.8f,%.8f",(double)quads[i].xy[c][0],(double)quads[i].xy[c][1],
            (double)quads[i].uv[c][0],(double)quads[i].uv[c][1]);
        fputc('\n',out);
    }
    fclose(out);
}
static void sample(nm_hud_t *hud,int32_t rpm) {
    hud->timer[0]=4;hud->timer[1]=2;
    hud->lap_current=2;hud->lap_total=8;hud->rows=1;
    const uint8_t number[2]={NM_HUD_BLANK,1},time[8]={NM_HUD_BLANK,0,NM_HUD_MINUTE,3,8,NM_HUD_SECOND,4,1};
    memcpy(hud->lap[0].number,number,2);memcpy(hud->lap[0].time,time,8);
    hud->rank[0]=NM_HUD_BLANK;hud->rank[1]=3;hud->suffix=nm_hud_suffix(3);hud->rank_drawn=1;
    hud->count[0]=4;hud->count[1]=0;
    hud->speed[0]=2;hud->speed[1]=1;hud->speed[2]=7;hud->speed_drawn=1;
    hud->needle=1;hud->needle_angle=nm_hud_needle_angle(rpm);
}
int main(int argc,char **argv) {
    if(argc<3) { fputs("usage: hud_gear_preview OUT_PREFIX RPM\n",stderr);return 2; }
    const int32_t rpm=(int32_t)strtol(argv[2],NULL,0);
    static nm_hud_t hud;
    static const nm_hud_race_t race;
    static nm_hud_cars_t cars;
    char name[32];
    cars.manual=1;
    for(unsigned gear=0;gear<4;++gear) { /* Settled: entry in this gear, past any animation. */
        cars.gear=gear;nm_hud_reset(&hud,&race,&cars);sample(&hud,rpm);
        for(unsigned t=0;t<NM_HUD_GEAR_TICKS+2;++t) nm_hud_gear(&hud,&cars);
        snprintf(name,sizeof(name),"gear%u",gear+1u);write_quads(argv[1],name,&hud);
        if(gear==2) { hud.extension=1;write_quads(argv[1],"extension",&hud); } /* ae98's "TIME EXTENSION!!" at row 12 */
    }
    cars.gear=1;nm_hud_reset(&hud,&race,&cars);sample(&hud,rpm);
    nm_hud_gear(&hud,&cars);
    cars.gear=2;
    for(int t=NM_HUD_GEAR_TICKS-1;t>=0;--t) {
        nm_hud_gear(&hud,&cars);
        snprintf(name,sizeof(name),"shift-%02d",t);write_quads(argv[1],name,&hud);
    }
    return 0;
}
