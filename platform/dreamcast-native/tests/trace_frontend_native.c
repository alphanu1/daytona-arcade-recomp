/* Host-only lockstep of the native course/transmission select (include/select.h) against the original's
 * front-end trace (m2segment --trace-frontend, tools/trace_frontend.hpp): from the iteration of phase 17
 * (its fall-through into phase 18) to the race entry, each F row's inputs (decoded steering, its previous
 * value, the accelerator edge, START held) feed one native iteration, and the select's state after it is
 * compared field by field with the row: phase-18 substate and timer, the select object's state and
 * timers, the slot, course, transmission and time-attack words, and the iteration count (5010b0).
 *
 * usage: trace_frontend_native REFERENCE_TRACE
 * Prints one summary line; exit 0 when every row agrees, 1 on the first difference (named), 2 on a
 * malformed trace. The trace is ROM-run output under build/, never committed. */
#undef NDEBUG
#include "select.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int field(const char *line,const char *key,long long *out) {
    char pattern[32];snprintf(pattern,sizeof(pattern)," %s=",key);
    const char *at=strstr(line,pattern);
    if(!at) return 0;
    *out=strtoll(at+strlen(pattern),NULL,strcmp(key,"held") ? 10 : 16);return 1;
}
static long long need(const char *line,const char *key) {
    long long v;
    if(!field(line,key,&v)) { fprintf(stderr,"trace row lacks %s: %s",key,line);exit(2); }
    return v;
}

int main(int argc,char **argv) {
    if(argc!=2) { fputs("usage: trace_frontend_native REFERENCE_TRACE\n",stderr);return 2; }
    FILE *in=fopen(argv[1],"r");
    if(!in) { fprintf(stderr,"cannot open %s\n",argv[1]);return 2; }
    static char line[4096];
    nm_select_t s;nm_select_output_t out={{0},0};
    int entered=0,done=0;long long counter=0,rows=0,first_b0=-1,last_b0=-1;
    unsigned sounds=0;
    while(fgets(line,sizeof(line),in)) {
        if(line[0]!='F' || line[1]!=' ') continue;
        const long long active=need(line,"active"),b0=need(line,"b0");
        if(!entered) {
            if(active!=17) continue;
            nm_select_enter(&s);entered=1;counter=b0;first_b0=b0;
        } else {
            if(active==19) { done=1;last_b0=b0;break; }
            if(active!=18) { fprintf(stderr,"row b0=%lld: active %lld inside the select\n",b0,active);return 1; }
            if(b0!=++counter) { fprintf(stderr,"row b0=%lld: iteration count %lld expected\n",b0,counter);return 1; }
            const nm_select_input_t input={(int32_t)need(line,"steer"),(int32_t)need(line,"psteer"),
                (uint8_t)need(line,"edge"),(uint8_t)((need(line,"held")>>4)&1)};
            if(!nm_select_tick(&s,&input,(uint32_t)b0,&out)) {
                fprintf(stderr,"row b0=%lld: native select rejected the iteration (state %u substate %u)\n",b0,s.state,s.substate);
                return 1;
            }
            sounds+=out.sounds;
        }
        ++rows;
        const struct { const char *key; long long native; } check[]={
            {"phase",s.phase},{"sub",s.substate},{"timer",s.phase_timer},{"selon",s.enabled},
            {"sel",s.state},{"t56",s.timer},{"t58",s.blink},{"t54",s.fade},{"t5f",s.countdown},
            {"chosen",s.chosen},{"slot",s.slot},{"course",s.course},{"trans",s.transmission},{"attack",s.time_attack}};
        for(unsigned i=0;i<sizeof(check)/sizeof(check[0]);++i) {
            /* Once the object is off its fields are no longer the select's (the reference keeps the last). */
            if(!s.enabled && i>=4 && i<=10) continue;
            const long long reference=need(line,check[i].key);
            if(reference!=check[i].native) {
                fprintf(stderr,"FAIL row b0=%lld: %s native %lld reference %lld (state %u)\n",b0,check[i].key,
                    check[i].native,reference,s.state);
                return 1;
            }
        }
    }
    fclose(in);
    if(!entered || !done) { fprintf(stderr,"trace has no complete select (phase 17 .. 19)\n");return 2; }
    printf("PASS select lockstep: %lld iterations (5010b0 %lld..%lld), course %u transmission %u, %u sound commands; "
        "race entry at 5010b0 %lld, phase-20 tick %lld\n",rows,first_b0,last_b0,s.course,s.transmission,sounds,last_b0,last_b0+1);
    return 0;
}
