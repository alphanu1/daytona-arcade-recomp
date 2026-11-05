/* The save data and its VMS file (include/save_data.h). */
#include "save_data.h"
#include <string.h>
#include "nm_mem.h"

/* 226654: the time-attack bests the reset writes (shlo immediates: 5<<11, 15<<10, 5<<8, 15<<8, 15<<9), by word of
 * 1d00b60.. (include/save_data.h); words 3 and 7 (course 3) are not written. Automatic and manual alike. */
static const uint32_t time_attack_reset[8]={
    UINT32_C(1280),UINT32_C(3840),UINT32_C(7680),UINT32_C(0xffffffff),     /* best lap: courses 0, 2, 1, 3 */
    UINT32_C(10240),UINT32_C(15360),UINT32_C(15360),UINT32_C(0xffffffff),  /* best race */
};

void nm_save_defaults(nm_save_t *save,const nm_race_record_seed_t seeds[NM_SAVE_TABLES][NM_SAVE_SLOTS]) {
    nm_memset(save,0xff,sizeof(*save));
    save->settings=nm_settings_factory();
    nm_memset(save->zero,0,sizeof(save->zero));
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) {
        nm_race_records_reset(save->records[t],seeds[t]);
        save->selected[t]=NM_SAVE_NO_SLOT;
    }
    for(unsigned i=0;i<NM_SAVE_TIME_ATTACK_WORDS;++i) save->time_attack[i]=time_attack_reset[i&7u];
}

int nm_save_valid(const nm_save_t *save) {
    if(!save || !nm_settings_valid(&save->settings)) return 0;
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) {
        if(save->selected[t]>=NM_SAVE_SLOTS && save->selected[t]!=NM_SAVE_NO_SLOT) return 0;
        for(unsigned i=0;i<NM_SAVE_SLOTS;++i) if(save->records[t][i].rank!=i+1) return 0;
    }
    return 1;
}

static uint16_t crc16_more(uint16_t crc,const uint8_t *data,size_t size) {
    while(size--) {
        crc^=(uint16_t)(*data++<<8);
        for(unsigned bit=0;bit<8;++bit) crc=(uint16_t)(crc&0x8000u ? (crc<<1)^0x1021u : crc<<1);
    }
    return crc;
}
uint16_t nm_save_crc16(const uint8_t *data,size_t size) { return crc16_more(0,data,size); }

uint32_t nm_save_crc32(const uint8_t *data,size_t size) {
    uint32_t crc=UINT32_C(0xffffffff);
    while(size--) {
        crc^=*data++;
        for(unsigned bit=0;bit<8;++bit) crc=crc&1u ? (crc>>1)^UINT32_C(0xedb88320) : crc>>1;
    }
    return ~crc;
}

static void put16(uint8_t *p,uint32_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p,uint32_t v) { put16(p,v);put16(p+2,v>>16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8; }
static uint32_t get32(const uint8_t *p) { return get16(p)|get16(p+2)<<16; }

/* Payload: 0 "DNRS", 4 version, 6 size, 8 difficulty, 9 advertise sound, 10..12 selected, 13..15 zero,
 * 16 the three tables (24 slots of six words), 1744 the time-attack words, 1808 CRC-32 of bytes 0..1807. */
static const uint8_t payload_magic[4]={'D','N','R','S'};
_Static_assert(sizeof(nm_save_t)==8+sizeof(nm_race_record_t)*NM_SAVE_TABLES*NM_SAVE_SLOTS+4*NM_SAVE_TIME_ATTACK_WORDS,
    "nm_save_t without padding");
enum { PAYLOAD_RECORDS=16, PAYLOAD_TIME_ATTACK=PAYLOAD_RECORDS+NM_SAVE_TABLES*NM_SAVE_SLOTS*24,
       PAYLOAD_CRC=PAYLOAD_TIME_ATTACK+4*NM_SAVE_TIME_ATTACK_WORDS };
_Static_assert(PAYLOAD_CRC+4==NM_SAVE_PAYLOAD_BYTES,"payload layout");

size_t nm_save_encode(const nm_save_t *save,uint8_t out[NM_SAVE_PAYLOAD_BYTES]) {
    if(!nm_save_valid(save)) return 0;
    nm_memset(out,0,NM_SAVE_PAYLOAD_BYTES);
    nm_memcpy(out,payload_magic,4);
    put16(out+4,NM_SAVE_PAYLOAD_VERSION);put16(out+6,NM_SAVE_PAYLOAD_BYTES);
    out[8]=save->settings.difficulty;out[9]=save->settings.advertise_sound;
    nm_memcpy(out+10,save->selected,NM_SAVE_TABLES);
    uint8_t *p=out+PAYLOAD_RECORDS;
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) for(unsigned i=0;i<NM_SAVE_SLOTS;++i,p+=24) {
        const nm_race_record_t *r=&save->records[t][i];
        put32(p,r->rank);put32(p+4,r->time);put32(p+8,r->view);
        put32(p+12,r->flags);put32(p+16,r->time_copy);put32(p+20,r->name);
    }
    for(unsigned i=0;i<NM_SAVE_TIME_ATTACK_WORDS;++i) put32(out+PAYLOAD_TIME_ATTACK+4*i,save->time_attack[i]);
    put32(out+PAYLOAD_CRC,nm_save_crc32(out,PAYLOAD_CRC));
    return NM_SAVE_PAYLOAD_BYTES;
}

int nm_save_decode(const uint8_t *in,size_t size,nm_save_t *save) {
    if(!in || !save || size<NM_SAVE_PAYLOAD_BYTES || memcmp(in,payload_magic,4) ||
       get16(in+4)!=NM_SAVE_PAYLOAD_VERSION || get16(in+6)!=NM_SAVE_PAYLOAD_BYTES ||
       get32(in+PAYLOAD_CRC)!=nm_save_crc32(in,PAYLOAD_CRC) || in[13] || in[14] || in[15]) return 0;
    nm_save_t s;
    s.settings=(nm_settings_t){.difficulty=in[8],.advertise_sound=in[9]};
    nm_memset(s.zero,0,sizeof(s.zero));
    nm_memcpy(s.selected,in+10,NM_SAVE_TABLES);
    const uint8_t *p=in+PAYLOAD_RECORDS;
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) for(unsigned i=0;i<NM_SAVE_SLOTS;++i,p+=24)
        s.records[t][i]=(nm_race_record_t){.rank=get32(p),.time=get32(p+4),.view=get32(p+8),
            .flags=get32(p+12),.time_copy=get32(p+16),.name=get32(p+20)};
    for(unsigned i=0;i<NM_SAVE_TIME_ATTACK_WORDS;++i) s.time_attack[i]=get32(in+PAYLOAD_TIME_ATTACK+4*i);
    if(!nm_save_valid(&s)) return 0;
    *save=s;return 1;
}

/* The VMS header (KOS dc/vmu_pkg.h vmu_hdr_t): 0 short description (16, blank padded), 16 long description (32,
 * blank padded), 48 application id (16, NUL padded), 64 icon count, 66 icon animation speed, 68 eyecatch type,
 * 70 CRC, 72 data length (32 bits), 76 reserved (20 zero), 96 the icon palette (16 ARGB4444), 128 the icons. */
static const char vms_short[]="DAYTONA USA";
static const char vms_long[]="RECORDS AND SETTINGS";
static const char vms_app[]="DAYTONA93NATIVE";

/* The icon, drawn here (this project's own): a chequered flag on its pole over a blue field with a red stripe. */
enum { ICON_FIELD, ICON_WHITE, ICON_BLACK, ICON_POLE, ICON_RED, ICON_GOLD };
static const uint16_t icon_palette[16]={0xf03a,0xffff,0xf000,0xf999,0xfe21,0xffc2};
static unsigned icon_pixel(unsigned x,unsigned y) {
    if(x>=2 && x<=5 && y>=1 && y<=2) return ICON_GOLD;
    if(x>=3 && x<=4 && y>=3 && y<=30) return ICON_POLE;
    const unsigned wave=x>=5 && (x-5)%12>=6 ? 1 : 0;
    if(x>=5 && x<=28 && y>=3+wave && y<19+wave)
        return (((x-5)>>2)+((y-3-wave)>>2))&1u ? ICON_BLACK : ICON_WHITE;
    if(y>=23 && y<=26) return ICON_RED;
    return ICON_FIELD;
}

size_t nm_save_file_build(const nm_save_t *save,uint8_t out[NM_SAVE_FILE_BYTES]) {
    nm_memset(out,0,NM_SAVE_FILE_BYTES);
    if(nm_save_encode(save,out+NM_SAVE_VMS_HEADER_BYTES+NM_SAVE_ICON_BYTES)!=NM_SAVE_PAYLOAD_BYTES) return 0;
    nm_memset(out,' ',48);
    nm_memcpy(out,vms_short,sizeof(vms_short)-1);nm_memcpy(out+16,vms_long,sizeof(vms_long)-1);
    nm_memcpy(out+48,vms_app,sizeof(vms_app)-1);
    put16(out+64,1);put16(out+66,0);put16(out+68,0);put32(out+72,NM_SAVE_PAYLOAD_BYTES);
    for(unsigned i=0;i<16;++i) put16(out+96+2*i,icon_palette[i]);
    uint8_t *icon=out+NM_SAVE_VMS_HEADER_BYTES;
    for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;x+=2)
        *icon++=(uint8_t)(icon_pixel(x,y)<<4|icon_pixel(x+1,y));
    put16(out+70,nm_save_crc16(out,NM_SAVE_FILE_BYTES));
    return NM_SAVE_FILE_BYTES;
}

int nm_save_file_parse(const uint8_t *file,size_t size,nm_save_t *save) {
    if(!file || !save || size<NM_SAVE_VMS_HEADER_BYTES || memcmp(file+48,vms_app,sizeof(vms_app)) ||
       get16(file+64)!=1 || get16(file+68)!=0 || get32(file+72)!=NM_SAVE_PAYLOAD_BYTES ||
       size<NM_SAVE_FILE_BYTES) return 0;
    /* The CRC over the file with its CRC field zero, without writing to the caller's buffer. */
    uint8_t header[NM_SAVE_VMS_HEADER_BYTES];
    nm_memcpy(header,file,sizeof(header));header[70]=header[71]=0;
    const uint16_t crc=crc16_more(nm_save_crc16(header,sizeof(header)),file+sizeof(header),
        NM_SAVE_FILE_BYTES-sizeof(header));
    if(crc!=get16(file+70)) return 0;
    return nm_save_decode(file+NM_SAVE_VMS_HEADER_BYTES+NM_SAVE_ICON_BYTES,NM_SAVE_PAYLOAD_BYTES,save);
}
