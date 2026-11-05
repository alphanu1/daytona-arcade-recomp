/* cc -std=c11 -fsanitize=address,undefined tests/check_sky_pack.c -o check-sky
 * ./check-sky sky.dns */
#include "../include/render/scene/sky.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb");assert(f);
    unsigned char header[32],*page=malloc(NM_SKY_TEXTURE_BYTES);assert(page);
    assert(fread(header,1,32,f)==32 && nm_sky_header_valid(header,32));
    assert(!nm_sky_header_valid(NULL,32) && !nm_sky_header_valid(header,31));
    /* Byte 17 is the high byte of the content rows (sky.h): a flip there may name another valid height. */
    for(unsigned i=0;i<32;++i) if(i<8 || i==11 || i==15 || (i>=16 && i!=17)) {
        header[i]^=1;assert(!nm_sky_header_valid(header,32));header[i]^=1;
    }
    for(unsigned p=0;p<4;++p) {
        assert(fread(page,1,NM_SKY_TEXTURE_BYTES,f)==NM_SKY_TEXTURE_BYTES);
        assert(nm_sky_texture_valid(page,NM_SKY_TEXTURE_BYTES));
        assert(!nm_sky_texture_valid(NULL,NM_SKY_TEXTURE_BYTES));
        assert(!nm_sky_texture_valid(page,NM_SKY_TEXTURE_BYTES-1));
        for(unsigned i=0;i<32;++i) {page[i]^=1;assert(!nm_sky_texture_valid(page,NM_SKY_TEXTURE_BYTES));page[i]^=1;}
    }
    assert(fgetc(f)==EOF && !ferror(f));assert(!fclose(f));free(page);
    puts("PASS DNS1 real pack, truncation/null/header mutations; fixed bounded VQ16 layout");
}
