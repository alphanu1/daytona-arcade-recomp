#ifndef DAYTONA_NATIVE_SKY_H
#define DAYTONA_NATIVE_SKY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { NM_SKY_TEXTURE_BYTES=67616, NM_SKY_PACK_BYTES=32+4*NM_SKY_TEXTURE_BYTES };
/* Match KOS's default background plane with GEQUAL, behind accepted geometry. */
#define NM_SKY_DEPTH 0.0001f
/* DNS1:32-byte LE header (magic,size,exact sky/ground ARGB, the panorama's content rows as a halfword at 16
 * (0: 392, course 0's), 14 zero bytes), then four unchanged full-codebook512x512 RGB565 VQ DcTx files. */
enum { NM_SKY_ROWS=392 };
static inline unsigned nm_sky_header_rows(const unsigned char *p) {
    const unsigned rows=(unsigned)p[16]|(unsigned)p[17]<<8;
    return rows ? rows : NM_SKY_ROWS;
}
static inline uint32_t nm_sky_word(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline int nm_sky_header_valid(const void *data,size_t bytes) {
    if(!data || bytes!=32) return 0;
    const unsigned char *p=(const unsigned char *)data;
    if(memcmp(p,"DNS1",4) || nm_sky_word(p+4)!=NM_SKY_PACK_BYTES || p[11]!=255 || p[15]!=255) return 0;
    const unsigned rows=nm_sky_header_rows(p);
    if(rows%8 || rows<8 || rows>512) return 0;
    for(unsigned i=18;i<32;++i) if(p[i]) return 0;
    return 1;
}
static inline int nm_sky_texture_valid(const void *data,size_t bytes) {
    static const unsigned char header[32]={'D','c','T','x',0x20,8,1,0,0,0,255,0,
        0,2,0,2,0x36,4,0,0x48};
    return data && bytes==NM_SKY_TEXTURE_BYTES && !memcmp(data,header,32);
}
int nm_sky_load(const char *path);
void nm_sky_unload(void);
int nm_sky_enqueue(uint16_t heading,uint16_t scroll_y);

enum { NM_SKY_BLUE=4, NM_SKY_GROUND=5, NM_SKY_RECTS=4 };
typedef struct { uint16_t x,y,width,height,u,v,page; } nm_sky_rect_t;

/* Authored panorama: four512-wide pages, ROWS content rows (8 per tile row of the course's sky sections: 392
 * on course 0, 344 on course 2) from layer row 48. Uniform fill completes the original1024-row vertical period.
 * Coordinates remain in the496x384 source viewport; presentation scales once when submitting.
 * Bounded native rectangles, no tile map, per-pixel loop or frame allocation. */
static inline unsigned nm_sky_rects_rows(uint16_t heading,uint16_t scroll_y,unsigned rows,
        nm_sky_rect_t out[NM_SKY_RECTS]) {
    if (!out || rows<8 || rows>624) return 0;
    unsigned count=0,y=0,v=scroll_y&1023;
    while(y<384) {
        unsigned end,page;
        if(v<48) { end=48;page=NM_SKY_BLUE; }
        else if(v<48+rows) { end=48+rows;page=0; }
        else if(v<672) { end=672;page=NM_SKY_GROUND; }
        else { end=1024;page=NM_SKY_BLUE; }
        const unsigned h=end-v<384-y ? end-v : 384-y;
        unsigned x=0,u=(0u-(heading>>5))&2047;
        while(x<496) {
            unsigned w=496-x;
            if(page<4 && w>512-(u&511)) w=512-(u&511);
            if(count==NM_SKY_RECTS) return 0;
            out[count++]=(nm_sky_rect_t){(uint16_t)x,(uint16_t)y,(uint16_t)w,(uint16_t)h,
                (uint16_t)(page<4 ? u&511 : 0),(uint16_t)(page<4 ? v-48 : 0),
                (uint16_t)(page<4 ? u>>9 : page)};
            x+=w;u=(u+w)&2047;
        }
        y+=h;v=(v+h)&1023;
    }
    return count;
}
static inline unsigned nm_sky_rects(uint16_t heading,uint16_t scroll_y,nm_sky_rect_t out[NM_SKY_RECTS]) {
    return nm_sky_rects_rows(heading,scroll_y,NM_SKY_ROWS,out);
}
#endif
