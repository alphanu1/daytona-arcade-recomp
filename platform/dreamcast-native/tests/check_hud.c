/* Host only. Runs the native HUD value rules (include/hud.h) on the source
 * variables recorded by `m2segment --trace-hud` and writes what they display,
 * one row per reference frame, for tools/check_hud.py to compare with the
 * reference's own tile words and overlay polygons.
 *
 *   check_hud HUD_TRACE NEW_DISPLAY_FILE [--drive DRIVE_PACK] [--manual] [--course SLOT] [QUAD_FRAME ...]
 *
 * DRIVE_PACK (drive.dnd of an asset set): the TGP tables of the CONDITION
 * panel's radius and circle test, as the race uses them; without it IEEE
 * single precision (cars within a few ulps of the circle may differ).
 * Build with code/drive.c (nm_drive_load). --manual: the trace is of a manual race (501464 = 1; the
 * gear indicator b0c0 runs on the trace's gear +148).
 *
 * For each QUAD_FRAME it also writes NEW_DISPLAY_FILE.quads-FRAME: the quad
 * list the console renderer would submit (arcade 496x384 coordinates), which
 * tools/check_hud.py composes into a preview from the exported sheets. */
#include "hud_dump.h"
#include "render/ui/hud_draw.h"
#include <math.h>
#include <stdlib.h>

static const char *field(const char *line,const char *key) {
    char pattern[32];snprintf(pattern,sizeof(pattern)," %s=",key);
    const char *at=strstr(line,pattern);
    return at ? at+strlen(pattern) : NULL;
}
static long number(const char *line,const char *key,int base) {
    const char *at=field(line,key);
    if(!at) { fprintf(stderr,"missing field %s\n",key);exit(2); }
    return (long)strtoll(at,NULL,base);
}
static float real(const char *line,const char *key) {
    const uint32_t bits=(uint32_t)strtoul(field(line,key),NULL,16);float value;memcpy(&value,&bits,4);return value;
}
static void host_sincos(uint16_t angle,float *sine,float *cosine) {
    const double radians=angle*(2.0*3.14159265358979323846/65536.0);
    *sine=(float)sin(radians);*cosine=(float)cos(radians);
}
void nm_hud_write_quads(const char *path,const nm_hud_t *hud) {
    static nm_hud_quad_t quads[NM_HUD_MAX_QUADS];
    const unsigned count=nm_hud_quads(hud,quads,host_sincos);
    FILE *out=fopen(path,"w");
    if(!out) { perror(path);exit(2); }
    fputs("# sheet then four x,y,u,v corners in strip order; arcade 496x384 pixels, normalized uv\n",out);
    for(unsigned i=0;i<count;++i) {
        fprintf(out,"%u",quads[i].sheet);
        for(unsigned c=0;c<4;++c) fprintf(out," %.6f,%.6f,%.8f,%.8f",(double)quads[i].xy[c][0],(double)quads[i].xy[c][1],
            (double)quads[i].uv[c][0],(double)quads[i].uv[c][1]);
        fputc('\n',out);
    }
    fclose(out);
}
#ifndef NM_HUD_CHECK_NO_MAIN
int main(int argc,char **argv) {
    if(argc<3) { fputs("usage: check_hud HUD_TRACE NEW_DISPLAY_FILE [--drive DRIVE_PACK] [--manual] [QUAD_FRAME ...]\n",stderr);return 2; }
    uint8_t manual=0,course=0;
    for(int i=3;i<argc;++i) {
        if(!strcmp(argv[i],"--manual")) manual=1;
        else if(!strcmp(argv[i],"--course") && i+1<argc) course=(uint8_t)strtoul(argv[i+1],NULL,10);
    }
    const nm_drive_params_t *math=NULL;
    for(int i=3;i+1<argc;++i) if(!strcmp(argv[i],"--drive") && !(math=nm_drive_load(argv[i+1]))) {
        fprintf(stderr,"cannot load %s\n",argv[i+1]);return 2;
    }
    FILE *in=fopen(argv[1],"r"),*out=fopen(argv[2],"wx");
    if(!in || !out) { perror(in ? argv[2] : argv[1]);return 2; }
    static char line[8192];
    nm_hud_t hud;int started=0;long rows=0,lap_message=0;
    while(fgets(line,sizeof(line),in)) {
        if(line[0]!='H') continue;
        nm_hud_race_t race={.active_phase=(uint8_t)number(line,"active",10),.phase=(uint8_t)number(line,"phase",10),
            .remaining=(int32_t)number(line,"remaining",10),.clock=(uint32_t)number(line,"clock",10)};
        /* a818's inputs; traces made before these fields show no message. */
        if(field(line,"counter")) {
            race.counter=(uint32_t)number(line,"counter",10);
            race.reverse=(uint8_t)(((uint32_t)number(line,"hudflags",16)>>28)&1u);
        }
        nm_hud_cars_t cars={.start_flags=(uint32_t)number(line,"start",16),
            .completed=(uint8_t)number(line,"done",10),.total_laps=(uint8_t)number(line,"laps",10),
            .rank=(uint8_t)number(line,"rank",10),.count=(uint8_t)number(line,"count",10),
            .best_ticks=(uint16_t)number(line,"best",10),.start_ticks=(uint16_t)number(line,"lapstart",10),
            .speed_index=(int32_t)number(line,"si",10),.filtered_rpm=(int32_t)number(line,"rpm2",10),
            .gear=(uint32_t)number(line,"gear",10),.manual=manual,
            .x=real(line,"x"),.z=real(line,"z"),.yaw=(uint16_t)number(line,"yaw",10),
            .leader_x=real(line,"lx"),.leader_z=real(line,"lz"),.leader_yaw=(uint16_t)number(line,"lyaw",10),
            .leader_valid=number(line,"leader_id",10)!=255};
        cars.leader_second=cars.rank==0;
        /* ae98's inputs: this tick set 5010d4 (it reads 128 after the tick: ae98 counts down first) or
         * 5010dc (it rose: a96c counts it down after the player set it). */
        if(field(line,"ext")) {
            const long message=number(line,"lapmsg",10);
            race.award=number(line,"ext",10)==128;race.lap_line=message>lap_message;lap_message=message;
        }
        /* The rolling start's: traces made before these fields show no overlay. */
        if(field(line,"stage")) { race.stage=(int16_t)number(line,"stage",10);race.accel=(uint16_t)number(line,"accel",10); }
        /* ad34's: rows made before these fields show no message. */
        if(field(line,"lastlap")) { race.lap_last=(uint16_t)number(line,"lastlap",10);race.finish_flags=(uint32_t)number(line,"flags8",16); }
        /* The CONDITION panel's inputs. */
        cars.math=math;
        if(field(line,"cars")) {
            cars.angle=(uint16_t)(number(line,"yaw",10)+number(line,"pd6",10));
            const char *at=field(line,"cars");
            while(*at && *at!=' ' && *at!='\n' && cars.others<NM_HUD_RADAR_CARS) {
                char *end;nm_hud_radar_car_t *o=cars.other+cars.others++;
                o->id=(uint8_t)strtoul(at,&end,10);
                uint32_t bits=(uint32_t)strtoul(end+1,&end,16);memcpy(&o->x,&bits,4);
                bits=(uint32_t)strtoul(end+1,&end,16);memcpy(&o->z,&bits,4);
                const long yaw=strtol(end+1,&end,10);
                o->angle=(uint16_t)(yaw+strtol(end+1,&end,10));
                at=*end==';' ? end+1 : end;
            }
        }
        const char *laps=field(line,"laptimes");
        for(unsigned i=0;i<8;++i) { char *end;cars.lap_times[i]=(uint32_t)strtoul(laps,&end,10);laps=*end=='.' ? end+1 : end; }
        const long frame=number(line,"frame",10);
        if(!started) {
            /* The HUD exists from the phase-19 entry (a6b4, 3414). */
            if(race.active_phase<19) continue;
            nm_hud_reset(&hud,&race,&cars);hud.course=course;started=1;
        } else {
            /* 129d0..12a04: the chord from the reference's held inputs. Traces before the field: its index after this tick. */
            if(field(line,"held")) race.held=(uint32_t)number(line,"held",16);
            else if(field(line,"zix")) hud.radar_index=(uint8_t)number(line,"zix",10);
            nm_hud_tick(&hud,&race,&cars);
        }
        nm_hud_dump(out,frame,&hud);++rows;
        for(int i=3;i<argc;++i) if(strcmp(argv[i-1],"--drive") && strcmp(argv[i-1],"--course") && strtol(argv[i],NULL,10)==frame) {
            char path[1024];snprintf(path,sizeof(path),"%s.quads-%ld",argv[2],frame);
            nm_hud_write_quads(path,&hud);
        }
    }
    if(fclose(out)) { perror(argv[2]);return 2; }
    printf("check_hud: %ld rows from the reference's source variables\n",rows);
    return rows ? 0 : 1;
}
#endif
