#include "render/materials/result_materials.h"
#include "render/materials/palette_ram.h"
#include "result_pal4_data.h"
#include <stdio.h>
#include <string.h>
#include "nm_mem.h"

static enj_texture_info_t textures[NM_RESULT_TEXTURE_COUNT];
static unsigned loaded,ready,applied;
/* Banks 2..59 belong to the cars while cars are drawn (palette_banks.h): the
 * validated blob waits here until the result hand-off. */
alignas(32) static uint32_t palette[2+NM_RESULT_PALETTE_COLORS];

void nm_result_materials_unload(void) {
    ready=applied=0;
    if(!loaded) return;
    if(pvr_wait_ready() || pvr_wait_render_done()) { enj_state_flag_shutdown(NULL);return; }
    while(loaded) enj_texture_unload(textures+--loaded);
    nm_memset(textures,0,sizeof(textures));
}

const enj_texture_info_t *nm_result_material(const uint16_t header[4],unsigned luma,unsigned *bank) {
    if(!ready || !header || !bank || luma>255) return NULL;
    for(unsigned i=0;i<8;++i) if(!memcmp(header,nm_result_material_headers[i],8)) {
        const uint8_t *binding=nm_result_pal4_bindings[i][luma];
        if(binding[0]>=loaded || binding[1]<2 || binding[1]>=64) return NULL;
        *bank=binding[1];return textures+binding[0];
    }
    return NULL;
}

/* Result hand-off (phase 25): no car is drawn from here until the scene is
 * released. The last car frame may still be rendering and palette RAM is read
 * at render time, so let it finish first; a failed wait costs at most one
 * frame of wrong car colours and is not an error. */
int nm_result_materials_palettes_apply(void) {
    if(!ready) return 0;
    if(applied) return 1;
    if(pvr_wait_ready() || pvr_wait_render_done()) puts("Native result palettes: render fence timed out; writing anyway");
    uint32_t goal_colors[32];
    for(unsigned i=0;i<32;++i) goal_colors[i]=nm_palette_ram_read(i);
    /* In the program's palette format (palette_ram.h), read back. */
    if(!nm_palette_ram_write_dpal(palette,NM_RESULT_PALETTE_FIRST,"result palettes")) return 0;
    for(unsigned i=0;i<32;++i)
        if(nm_palette_ram_read(i)!=goal_colors[i]) return 0;
    applied=1;
    printf("Native result palettes: %u colours at %u written at the result hand-off (%s); readback PASS, GOAL banks preserved\n",
        NM_RESULT_PALETTE_COLORS,NM_RESULT_PALETTE_FIRST,nm_palette_ram_format_name());
    return 1;
}

static int read_exact(const char *path,void *data,size_t size) {
    FILE *file=fopen(path,"rb");if(!file) return 0;
    const int ok=fread(data,1,size,file)==size && fgetc(file)==EOF;
    fclose(file);return ok;
}

int nm_result_materials_load(const char *root) {
    if(!root || loaded || ready) return 0;
    _Static_assert(NM_RESULT_PALETTE_FIRST==32 && NM_RESULT_PALETTE_COLORS<=992,"reserve GOAL banks0/1");
    const unsigned before=pvr_mem_available();
    if(before<NM_RESULT_TEXTURE_COUNT*544+512*1024) return 0;
    uint32_t goal_colors[32];
    for(unsigned i=0;i<32;++i) goal_colors[i]=nm_palette_ram_read(i);
    char path[512];alignas(32) uint32_t blob[136];
    const uint32_t expected[8]={0x78546344,544,0x0f000000,0x00200020,0x28000012,0,0,0};
    for(unsigned i=0;i<NM_RESULT_TEXTURE_COUNT;++i) {
        if(snprintf(path,sizeof(path),"%sresult/texture_%u.dt",root,i)>=(int)sizeof(path) ||
           !read_exact(path,blob,sizeof(blob)) || memcmp(blob,expected,sizeof(expected)) ||
           !enj_texture_load_blob(blob,textures+i)) goto fail;
        ++loaded;
        const volatile uint32_t *vram=(const volatile uint32_t *)textures[i].ptr;
        for(unsigned w=0;w<128;++w) if(vram[w]!=blob[w+8]) goto fail;
    }
    if(snprintf(path,sizeof(path),"%sresult/palettes.dpal",root)>=(int)sizeof(path) ||
       !read_exact(path,palette,sizeof(palette)) || palette[0]!=0x4c415044 ||
       palette[1]!=NM_RESULT_PALETTE_COLORS) goto fail;
    for(unsigned i=0;i<32;++i)
        if(nm_palette_ram_read(i)!=goal_colors[i]) goto fail;
    if(pvr_mem_available()<512*1024) goto fail;
    ready=1;
    unsigned bindings=0;
    for(unsigned m=0;m<8;++m) for(unsigned luma=0;luma<256;++luma) {
        unsigned bank=UINT32_MAX;
        const enj_texture_info_t *texture=nm_result_material(nm_result_material_headers[m],luma,&bank);
        if(nm_result_pal4_bindings[m][luma][0]==255) {
            if(texture || bank!=UINT32_MAX) goto fail;
        } else { if(!texture || bank!=nm_result_pal4_bindings[m][luma][1]) goto fail; ++bindings; }
    }
    printf("Native result PAL4 resident PASS: textures=%u payload=%u allocated=%u free=%u palette-colors=%u bindings=%u; startup VRAM readback, palette RAM written at the result hand-off, no runtime texture uploads\n",
        loaded,loaded*512,before-pvr_mem_available(),pvr_mem_available(),NM_RESULT_PALETTE_COLORS,bindings);
    return 1;
fail:
    puts("FAIL result PAL4 startup assets");nm_result_materials_unload();return 0;
}
