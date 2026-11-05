/* Presentation transform/source-data comparison; no gameplay math suite.
 * cc -std=c11 -Iinclude -I<name assets> test_name_exit_pose.c code/drive.c -lm
 * Run with drive.dnd and m2segment --export-name-exit output. */
#include "name_data.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv) {
    if(argc!=3) return 2;
    nm_drive_params_t *math=nm_drive_load(argv[1]);
    FILE *source=fopen(argv[2],"r");
    if(!math || !source) return 2;
    unsigned phase,glyph,tick,cases=0;float maximum=0;
    while(fscanf(source,"%u %u %u",&phase,&glyph,&tick)==3) {
        float expected[12],actual[12];
        for(unsigned i=0;i<12;++i) if(fscanf(source,"%f",expected+i)!=1) return 2;
        if(tick>UINT16_MAX || !nm_name_exit_pose(math,&nm_name_layout_data,glyph,phase,(uint16_t)tick,actual)) return 1;
        for(unsigned i=0;i<12;++i) {
            const float error=fabsf(expected[i]-actual[i]);
            if(error>maximum) maximum=error;
            if(!isfinite(expected[i]) || error>0.00002f) {
                fprintf(stderr,"phase=%u glyph=%u tick=%u component=%u source=%g native=%g\n",
                    phase,glyph,tick,i,(double)expected[i],(double)actual[i]);return 1;
            }
        }
        ++cases;
    }
    const int ok=feof(source) && !ferror(source) && cases==319;
    fclose(source);free(math);
    printf("name exit source poses: cases=%u max-error=%g\n",cases,(double)maximum);
    return !ok;
}
