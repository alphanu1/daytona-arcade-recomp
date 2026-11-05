/* Host-only failure diagnostics, never gameplay assets. Outputs must be new. */
#include "../tests/contact_check.h"
#include "../tests/boundary_check.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    const uint32_t endian=1;
    if(argc!=5 || *(const unsigned char *)&endian!=1) return 1;
    nm_drive_params_t *math=nm_drive_load(argv[1]);nm_contact_pack_t pack={0};
    if(!math || !nm_contact_load(&pack,argv[2])) {free(math);return 1;}
    int ok=nm_contact_test_hash(&pack,math)==nm_contact_expected_hash(pack.slot) &&
        nm_boundary_test_hash(math)==NM_BOUNDARY_EXPECTED_HASH;
    for(unsigned boundary=0;ok && boundary<2;++boundary) {
        FILE *f=fopen(argv[3+boundary],"wbx");if(!f) {ok=0;break;}
        const unsigned count=boundary ? 65536 : nm_contact_test_count(&pack),words=boundary ? 12 : 8;
        const uint32_t header[]={UINT32_C(0x31445046),count,words};
        ok=fwrite(header,4,3,f)==3;
        for(unsigned i=0;ok && i<count;++i) {
            uint32_t row[12];
            if(boundary) {
                float points[8];nm_boundary_test_input(i,points);memcpy(row,points,32);
                nm_boundary_hit_t result={{17,-19},0};
                row[8]=(uint32_t)nm_boundary_intersection(math,points,&result);
                row[9]=nm_force_bits(result.point[0]);row[10]=nm_force_bits(result.point[1]);row[11]=(uint32_t)result.hit;
            } else {
                float input[3];nm_contact_test_input(&pack,i,input);memcpy(row,input,12);
                nm_contact_result_t result={input[2],0x1234,0x5678,0xabcdef};
                row[3]=(uint32_t)nm_contact_query(&pack,math,input[0],input[1],&result);memcpy(row+4,&result,16);
            }
            ok=fwrite(row,4,words,f)==words;
        }
        if(fclose(f)) ok=0;
    }
    nm_contact_unload(&pack);free(math);return ok ? 0 : 1;
}
